import { useState } from 'react';
import { type BoardConfig, type BoardIo, type BoardStatus, isOutputMode, type Transport } from '../devices/types';

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
const MODES: { mode: BoardIo['mode']; title: string }[] = [
  { mode: 'binary', title: 'Output: on or off (relay)' },
  { mode: 'pwm', title: 'Output: a level 0..1 (dimmer, fan)' },
  { mode: 'input', title: 'Digital input: (in name) -> t / nil' },
  { mode: 'analog', title: 'Analog input: (analog name) -> 0..1 (ADC1 pins only)' },
  { mode: 'audio', title: 'Audio input (microphone module, ADC1 pin, one at most): (env band0) .. (env band7), level, beats, beatms' },
];

/** The Board panel's GPIO slots: outputs to switch / dim, inputs to watch, and their setup (pin, mode, frequency,
 *  pull, debounce, trigger, inverted, name). Output values aren't saved (always off at boot); the setup applies at
 *  once, Save keeps it. */
export function OutputsSection({ t, config, status, act, refresh, pollNow }: Props) {
  const ios = config.io ?? [];
  const max = config.io_max ?? 8;
  const [level, setLevel] = useState<Record<number, number>>({});
  const [add, setAdd] = useState({ gpio: '', mode: 'binary' as BoardIo['mode'] });
  const free = Array.from({ length: max }, (_, i) => i).find((i) => !ios.some((o) => o.n === i));
  const inputs = !!status?.io?.some((s) => s.mode !== undefined) || ios.some((o) => o.pull !== undefined);   // firmware with inputs

  const cfg = (o: BoardIo, label: string, change: Partial<Omit<BoardIo, 'n'>>) =>
    act(`slot ${o.n} ${label}`, async () => { await t.patch({ io: { [o.n]: change } }); await refresh(); pollNow(); });
  const set = (o: BoardIo, v: boolean | number) =>
    act(`output ${o.name || o.n}`, async () => { await t.patch({ io_set: { [o.n]: v } }); pollNow(); });
  const modes = MODES.filter((m) => inputs || isOutputMode(m.mode));

  return (
    <>
      {ios.length > 0 && (
        <table className="channels outputs">
          <thead><tr><th>slot</th><th title="Used by name in programs: (io-set pump t), (in button), (analog knob)">name</th><th>gpio</th><th>mode</th>
            <th title="Outputs: active low (on = pin low). Digital inputs: a low pin reads as t. Analog: 1 - value.">inv</th><th>now</th><th /></tr></thead>
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
                    {modes.map((m) => <option key={m.mode} value={m.mode} title={m.title}>{m.mode}</option>)}
                  </select>
                  {o.mode === 'pwm' && <> <input className="gamma" type="number" min={1} max={20000} defaultValue={o.freq} key={`f${o.freq}`} title="PWM frequency (Hz)"
                    onBlur={(e) => { const f = Math.round(+e.target.value); if (f !== o.freq && f >= 1 && f <= 20000) void cfg(o, 'frequency', { freq: f }); }} /> Hz</>}
                  {o.mode === 'input' && <>
                    {' '}<select value={o.pull ?? 'up'} title="Pull resistor" onChange={(e) => void cfg(o, 'pull', { pull: e.target.value as BoardIo['pull'] })}>
                      <option value="up">pull up</option><option value="down">pull down</option><option value="none">no pull</option>
                    </select>
                    {' '}<input className="gamma" type="number" min={0} max={1000} defaultValue={o.debounce ?? 20} key={`d${o.debounce}`} title="Debounce: ms a new level must hold"
                      onBlur={(e) => { const d = Math.round(+e.target.value); if (d !== o.debounce && d >= 0 && d <= 1000) void cfg(o, 'debounce', { debounce: d }); }} /> ms
                    {' '}<label title="Run the automation program right after this input changes"><input type="checkbox" checked={!!o.trigger}
                      onChange={(e) => void cfg(o, 'trigger', { trigger: e.target.checked })} /> trigger</label>
                  </>}
                </td>
                <td><input type="checkbox" checked={o.invert} onChange={(e) => void cfg(o, 'invert', { invert: e.target.checked })} /></td>
                <td className="io-now">
                  {st && !st.ok ? <span className="bad" title="Not set up: see the board's log">error</span>
                    : o.mode === 'input' ? <span className={v ? 'good' : 'muted'} title="Read by (in …); refreshed with the status">{v ? 'on' : 'off'}</span>
                    : o.mode === 'analog' ? <span className="num" title="Read by (analog …); refreshed with the status">{v.toFixed(2)}</span>
                    : o.mode === 'audio' ? <span className="num" title="The level, (env level); refreshed with the status">{v.toFixed(2)}</span>
                    : o.mode === 'binary'
                      ? <button className={v ? 'primary' : ''} onClick={() => void set(o, !v)} title="Switch it">{v ? 'on' : 'off'}</button>
                      : <><input type="range" min={0} max={100} value={level[o.n] ?? Math.round(v * 100)}
                          onChange={(e) => setLevel({ ...level, [o.n]: +e.target.value })}
                          onPointerUp={() => { const l = level[o.n]; if (l !== undefined) void set(o, l / 100).then(() => setLevel(({ [o.n]: _, ...r }) => r)); }}
                          onKeyUp={() => { const l = level[o.n]; if (l !== undefined) void set(o, l / 100).then(() => setLevel(({ [o.n]: _, ...r }) => r)); }} />
                        <span className="num">{level[o.n] ?? Math.round(v * 100)}%</span></>}
                </td>
                <td><button onClick={() => confirm(`Remove slot ${o.n}${o.name ? ` (${o.name})` : ''}? Its pin is let go.`) && void act(`remove slot ${o.n}`, async () => { await t.patch({ io: { [o.n]: null } }); await refresh(); })}
                  title="Remove this slot">✕</button></td>
              </tr>
            );
          })}</tbody>
        </table>
      )}
      {ios.length === 0 && <p className="muted">Nothing configured.</p>}
      {free !== undefined && (
        <div className="controls">
          <label>Add slot {free} on gpio <input className="gamma" type="number" min={0} max={63} value={add.gpio} onChange={(e) => setAdd({ ...add, gpio: e.target.value })} /></label>
          <select value={add.mode} onChange={(e) => setAdd({ ...add, mode: e.target.value as BoardIo['mode'] })}>
            {modes.map((m) => <option key={m.mode} value={m.mode} title={m.title}>{m.mode}</option>)}
          </select>
          <button disabled={add.gpio === ''} onClick={() => void act(`add slot ${free}`, async () => {
            await t.patch({ io: { [free]: { gpio: Math.round(+add.gpio), mode: add.mode } } }); await refresh(); setAdd({ gpio: '', mode: 'binary' });
          })}>Add</button>
        </div>
      )}
      <p className="muted small">Outputs are always off at boot. Inputs: digital ones pull up by default; analog ones need
        an ADC1 pin (ESP32 GPIO 32..39, C6 GPIO 0..6). Setup changes apply at once (slots you didn't change keep
        running); Save keeps them.</p>
    </>
  );
}
