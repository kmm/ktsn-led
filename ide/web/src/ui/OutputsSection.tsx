import { useState } from 'react';
import type { BoardConfig, BoardIo, BoardStatus, Transport } from '../devices/types';

interface Props {
  t: Transport;
  config: BoardConfig;
  status: BoardStatus | null;
  /** Run a board action with a status message (BoardPanel's act). */
  act(label: string, f: () => Promise<unknown>): Promise<void>;
  refresh(): Promise<unknown>;
  /** Ask for a fresh status (values just changed). */
  pollNow(): void;
}

const NAME = /^[a-z][a-z0-9_-]{0,14}$/;

/** The Board panel's GPIO outputs: switch / dim them, and their setup (pin, binary or PWM, frequency, inverted,
 *  name). Values aren't saved (outputs are always off at boot); the setup applies at once, Save keeps it. */
export function OutputsSection({ t, config, status, act, refresh, pollNow }: Props) {
  const ios = config.io ?? [];
  const max = config.io_max ?? 8;
  const [level, setLevel] = useState<Record<number, number>>({});
  const [add, setAdd] = useState({ gpio: '', mode: 'binary' as BoardIo['mode'] });
  const free = Array.from({ length: max }, (_, i) => i).find((i) => !ios.some((o) => o.n === i));

  const cfg = (o: BoardIo, label: string, change: Partial<Omit<BoardIo, 'n'>>) =>
    act(`output ${o.n} ${label}`, async () => { await t.patch({ io: { [o.n]: change } }); await refresh(); pollNow(); });
  const set = (o: BoardIo, v: boolean | number) =>
    act(`output ${o.name || o.n}`, async () => { await t.patch({ io_set: { [o.n]: v } }); pollNow(); });

  return (
    <>
      {ios.length > 0 && (
        <table className="channels outputs">
          <thead><tr><th>out</th><th title="Used by name in the automation program: (io-set pump t)">name</th><th>gpio</th><th>mode</th>
            <th title="Active low: on = pin low">inv</th><th>now</th><th /></tr></thead>
          <tbody>{ios.map((o) => {
            const st = status?.io?.find((s) => s.n === o.n);
            const v = st?.value ?? 0;
            return (
              <tr key={o.n}>
                <td>{o.n}</td>
                <td><input className="gamma io-name" defaultValue={o.name} key={`n${o.name}`} placeholder="—" spellCheck={false}
                  onBlur={(e) => { const n = e.target.value.trim(); if (n !== o.name) { if (n && !NAME.test(n)) { e.target.value = o.name; return; } void cfg(o, 'name', { name: n }); } }}
                  onKeyDown={(e) => { if (e.key === 'Enter') (e.target as HTMLInputElement).blur(); }} /></td>
                <td><input className="gamma" type="number" min={0} max={63} defaultValue={o.gpio} key={`g${o.gpio}`}
                  onBlur={(e) => { const g = Math.round(+e.target.value); if (g !== o.gpio && g >= 0) void cfg(o, 'gpio', { gpio: g }); }} /></td>
                <td>
                  <select value={o.mode} onChange={(e) => void cfg(o, 'mode', { mode: e.target.value as BoardIo['mode'] })}>
                    <option value="binary">binary</option><option value="pwm">pwm</option>
                  </select>
                  {o.mode === 'pwm' && <> <input className="gamma" type="number" min={1} max={20000} defaultValue={o.freq} key={`f${o.freq}`} title="PWM frequency (Hz)"
                    onBlur={(e) => { const f = Math.round(+e.target.value); if (f !== o.freq && f >= 1 && f <= 20000) void cfg(o, 'frequency', { freq: f }); }} /> Hz</>}
                </td>
                <td><input type="checkbox" checked={o.invert} onChange={(e) => void cfg(o, 'invert', { invert: e.target.checked })} /></td>
                <td className="io-now">
                  {st && !st.ok ? <span className="bad" title="Not set up: see the board's log">error</span>
                    : o.mode === 'binary'
                      ? <button className={v ? 'primary' : ''} onClick={() => void set(o, !v)} title="Switch it">{v ? 'on' : 'off'}</button>
                      : <><input type="range" min={0} max={100} value={level[o.n] ?? Math.round(v * 100)}
                          onChange={(e) => setLevel({ ...level, [o.n]: +e.target.value })}
                          onPointerUp={() => { const l = level[o.n]; if (l !== undefined) void set(o, l / 100).then(() => setLevel(({ [o.n]: _, ...r }) => r)); }}
                          onKeyUp={() => { const l = level[o.n]; if (l !== undefined) void set(o, l / 100).then(() => setLevel(({ [o.n]: _, ...r }) => r)); }} />
                        <span className="num">{level[o.n] ?? Math.round(v * 100)}%</span></>}
                </td>
                <td><button onClick={() => confirm(`Remove output ${o.n}${o.name ? ` (${o.name})` : ''}? Its pin is let go.`) && void act(`remove output ${o.n}`, async () => { await t.patch({ io: { [o.n]: null } }); await refresh(); })}
                  title="Remove this output">✕</button></td>
              </tr>
            );
          })}</tbody>
        </table>
      )}
      {ios.length === 0 && <p className="muted">No outputs configured.</p>}
      {free !== undefined && (
        <div className="controls">
          <label>Add output {free} on gpio <input className="gamma" type="number" min={0} max={63} value={add.gpio} onChange={(e) => setAdd({ ...add, gpio: e.target.value })} /></label>
          <select value={add.mode} onChange={(e) => setAdd({ ...add, mode: e.target.value as BoardIo['mode'] })}>
            <option value="binary">binary</option><option value="pwm">pwm</option>
          </select>
          <button disabled={add.gpio === ''} onClick={() => void act(`add output ${free}`, async () => {
            await t.patch({ io: { [free]: { gpio: Math.round(+add.gpio), mode: add.mode } } }); await refresh(); setAdd({ gpio: '', mode: 'binary' });
          })}>Add</button>
        </div>
      )}
      <p className="muted small">Always off at boot. Setup changes apply at once (outputs you didn't change keep running); Save keeps them.</p>
    </>
  );
}
