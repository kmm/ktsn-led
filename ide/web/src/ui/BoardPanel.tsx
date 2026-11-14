import { Fragment, useEffect, useRef, useState } from 'react';
import { buildRestorePatch, type ConfigFile, DEFAULT_RESTORE, exportConfig, fileName, parseConfigFile, type RestoreOptions } from '../devices/configFile';
import type { Connection } from '../devices/useConnection';
import { parseAppImage, updateProblems, type AppImage } from '../devices/firmware';
import { ApiError, type BoardStatus, type LibList } from '../devices/types';
import { OutputsSection } from './OutputsSection';

interface Props {
  conn: Connection;
  deviceName: string;
  visible: boolean;
  onOpenProgram(ch: number, src: string): void;
  onOpenAutoexec(src: string): void;
  /** Open the USB flashing dialog. */
  onFlash(): void;
  /** Open a library program in a tab. */
  onOpenLibrary(name: string, src: string): void;
  /** Changes when the IDE stored something in the library: list it again. */
  libVersion: number;
  /** Open the board's automation program in a tab. */
  onOpenAutomation(src: string): void;
  /** Save the board's programs into the IDE's local library (dialog; preselect e.g. "lib:wave" ticks only that one). */
  onSaveToLocal(preselect?: string): void;
}

const RESTORE_LABELS: [keyof RestoreOptions, string][] = [
  ['layout', 'Channel layout (GPIOs, pixel counts; applies after reboot)'], ['programs', 'Channel programs'],
  ['library', 'Library programs (same names are replaced)'],
  ['autoexec', 'Autoexec'], ['automation', 'Automation program and its period'], ['intensity', 'Intensity'], ['output', 'Output: gamma and dither per channel, refresh, parallel sending'], ['time', 'Time zone and NTP server'],
  ['hostname', 'Hostname (careful: two boards with one name collide)'], ['wifiSsid', 'WiFi SSID (keeps the board\'s current password)'],
  ['save', 'Save to the board\'s flash'],
];

function errText(e: unknown): string {
  return e instanceof ApiError || e instanceof Error ? e.message : String(e);
}

/** Gamma 1.0..3.0 (1 = off): edited as text, sent on Enter or when leaving the field. */
function GammaInput({ value, onCommit }: { value: number; onCommit(g: number): void }) {
  const [text, setText] = useState(String(value));
  useEffect(() => setText(String(value)), [value]);
  const commit = () => {
    const g = Math.round(Number(text) * 100) / 100;
    if (text.trim() === '' || !(g >= 1 && g <= 3)) { setText(String(value)); return; }
    if (g !== value) onCommit(g);
  };
  return <input className="gamma" type="number" min={1} max={3} step={0.1} value={text} title="1 = off; typical LEDs 2.2..2.8"
    onChange={(e) => setText(e.target.value)} onBlur={commit} onKeyDown={(e) => { if (e.key === 'Enter') commit(); }} />;
}

/** One channel's intensity: a slider (0..100 %), sent when let go; shows where a fade is now. */
function ChanIntensity({ ch, value, now, onCommit }: { ch: number; value: number; now?: number; onCommit(v: number): void }) {
  const [v, setV] = useState(Math.round(value * 100));
  useEffect(() => setV(Math.round(value * 100)), [value]);
  const commit = () => { if (v / 100 !== value) onCommit(v / 100); };
  const fading = now !== undefined && Math.abs(now - value) > 0.005;
  return (
    <label title={`Channel ${ch} intensity, multiplied with the global one`}>ch{ch}
      <input type="range" min={0} max={100} value={v} onChange={(e) => setV(+e.target.value)} onPointerUp={commit} onKeyUp={commit} />
      <span className="num">{v}%{fading && <span className="muted"> (now {Math.round(now * 100)}%)</span>}</span>
    </label>
  );
}

export function BoardPanel({ conn, deviceName, visible, onOpenProgram, onOpenAutoexec, onFlash, onOpenLibrary, libVersion, onOpenAutomation, onSaveToLocal }: Props) {
  const { transport: t, config } = conn;
  const [status, setStatus] = useState<BoardStatus | null>(null);
  const [msg, setMsg] = useState('');
  const [intensity, setIntensity] = useState(256);
  const [fade, setFade] = useState(0);
  const [restore, setRestore] = useState<{ file: ConfigFile; opts: RestoreOptions } | null>(null);
  const [cmdLine, setCmdLine] = useState('');
  const [update, setUpdate] = useState<{ image: Uint8Array; app: AppImage; problems: string[]; sent?: number } | null>(null);
  const otaInput = useRef<HTMLInputElement>(null);
  const [lib, setLib] = useState<LibList | null>(null);
  const [libErr, setLibErr] = useState('');

  const loadLib = async () => {
    if (!t) return;
    try { setLib(await t.libList()); setLibErr(''); }
    catch (e) { setLib(null); setLibErr(e instanceof ApiError && e.status === 404 ? 'this firmware has no program library (update it)' : errText(e)); }
  };
  useEffect(() => { if (t && visible) void loadLib(); }, [t, visible, libVersion]);  // eslint-disable-line react-hooks/exhaustive-deps
  const fileInput = useRef<HTMLInputElement>(null);
  const logEnd = useRef<HTMLDivElement>(null);

  useEffect(() => { if (config) setIntensity(config.intensity); }, [config]);

  // Status every 2 s while the panel is visible.
  useEffect(() => {
    if (!t || !visible) return;
    let stop = false;
    const poll = async () => {
      try { const s = await t.status(); if (!stop) { setStatus(s); conn.pollResult(null); } }
      catch (e) { if (!stop) conn.pollResult(e); }        // repeated network failures drop the connection
      if (!stop) timer = setTimeout(poll, 2000);
    };
    let timer = setTimeout(poll, 0);
    return () => { stop = true; clearTimeout(timer); };
  }, [t, visible]);  // eslint-disable-line react-hooks/exhaustive-deps -- conn.pollResult is stable

  useEffect(() => { logEnd.current?.scrollIntoView({ block: 'nearest' }); }, [conn.logs.length]);

  if (!t || !config) {
    return (
      <div className="board">
        <p className="muted">{conn.status === 'connecting' ? 'Connecting…' : 'Not connected. Pick a device and press Connect.'}</p>
        {conn.status !== 'connecting' && <p className="muted">No KTSN LED on it yet, or it doesn't start?{' '}
          <button onClick={onFlash}>Flash over USB…</button></p>}
      </div>
    );
  }

  const act = async (label: string, f: () => Promise<unknown>) => {
    setMsg(`${label}…`);
    try { await f(); setMsg(`${label}: done`); } catch (e) { setMsg(`${label} failed: ${errText(e)}`); }
  };

  const download = async () => {
    const cfg = await conn.refreshConfig() ?? config;
    let library: { name: string; src: string }[] | undefined;
    try {                                                 // firmware without a library: none in the file
      const l = await t.libList();
      library = [];
      for (const p of l.programs) library.push({ name: p.name, src: await t.libGet(p.name) });
    } catch { library = undefined; }
    const blob = new Blob([JSON.stringify(exportConfig(cfg, deviceName, new Date(), library), null, 2)], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = fileName(cfg);
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  };

  const pickRestore = async (f: File | undefined) => {
    if (!f) return;
    try { setRestore({ file: parseConfigFile(await f.text()), opts: { ...DEFAULT_RESTORE } }); }
    catch (e) { setMsg(`restore: ${errText(e)}`); }
    if (fileInput.current) fileInput.current.value = '';
  };

  const applyRestore = async () => {
    if (!restore) return;
    const { removals, patch, skipped, library } = buildRestorePatch(restore.file, config, restore.opts);
    setRestore(null);
    await act('restore', async () => {
      for (const r of removals) await t.patch(r);
      for (const p of library) await t.libPut(p.name, p.src);   // first: the automation program's play names are checked
      if (library.length) await loadLib();
      const res = await t.patch(patch);
      await conn.refreshConfig();
      const notes = [res.reboot_needed && 'reboot to apply the channel layout', ...skipped].filter(Boolean);
      if (notes.length) setTimeout(() => setMsg(`restore: done. Note: ${notes.join('; ')}`), 0);
    });
  };

  const fw = status?.firmware;
  const hasW = config.channels.some((c) => /W/.test(c.wire ?? ''));

  const pickUpdate = async (f: File | undefined) => {
    if (otaInput.current) otaInput.current.value = '';
    if (!f) return;
    try {
      const image = new Uint8Array(await f.arrayBuffer());
      const app = parseAppImage(image);
      setUpdate({ image, app, problems: fw ? updateProblems(app, fw) : [] });
    } catch (e) { setMsg(`update: ${errText(e)}`); }
  };

  /** Upload, then the board restarts: reconnect once it is back (WiFi takes a few seconds). If the new firmware doesn't
   *  come up, the board goes back to the old one by itself (rollback), and the reconnect finds that instead. */
  const install = async () => {
    if (!update || !t.ota) return;
    const u = update;
    setUpdate({ ...u, sent: 0 });
    try {
      const v = await t.ota(u.image, (sent, total) => setUpdate((x) => (x ? { ...x, sent: sent / total } : x)));
      setUpdate(null);
      setMsg(`update: ${v} installed, the board is restarting`);
      await conn.reconnectAfterRestart(`restarting with ${v}…`, 10000);
    } catch (e) {
      setUpdate({ ...u, sent: undefined });
      setMsg(`update failed: ${errText(e)} (the board keeps its current firmware)`);
    }
  };

  const runCmd = async () => {
    const line = cmdLine.trim();
    if (!line) return;
    setCmdLine('');
    conn.addLog(`led> ${line}`);                     // one console: board logs and command output in order
    try { const r = await t.cmd(line); for (const l of r.output.replace(/\n$/, '').split('\n')) conn.addLog(l); }
    catch (e) { conn.addLog(`(error) ${errText(e)}`); }
  };

  return (
    <div className="board">
      {config.reboot_needed && <div className="banner">The channel layout changed: save to flash, then reboot to apply it.</div>}

      <h3>Status</h3>
      {status ? (
        <dl className="stats">
          <dt>fps</dt><dd>{status.fps.toFixed(1)}{status.output_hz !== undefined && `, output ${status.output_hz.toFixed(0)} Hz`} <span className="muted">(missed {status.missed})</span></dd>
          <dt>frame</dt><dd>avg {status.frame_us.avg} µs, max {status.frame_us.max} µs</dd>
          <dt>uptime</dt><dd>{Math.floor(status.uptime_s / 3600)}h {Math.floor(status.uptime_s / 60) % 60}m {status.uptime_s % 60}s</dd>
          <dt>time</dt><dd>{status.time.replace('T', ' ')} <span className="muted">{status.tz}</span></dd>
          {config.loc !== undefined && <><dt>location</dt><dd>{config.loc ? `${config.loc.lat}, ${config.loc.lon}` : <span className="muted">not set (console: loc &lt;lat&gt; &lt;lon&gt;)</span>}</dd></>}
          <dt>wifi</dt><dd>{status.wifi ? (status.wifi.connected ? `${status.wifi.ssid} ${status.wifi.ip} (${status.wifi.rssi} dBm)` : `not connected (${status.wifi.ssid || 'off'})`) : 'not in this build'}</dd>
          {config.antenna !== undefined && <><dt>antenna</dt><dd>
            <select value={config.antenna} title="XIAO ESP32-C6 RF switch: the built-in ceramic antenna or one on the U.FL connector (applies at once; save to keep it)"
              onChange={(e) => { const a = e.target.value as 'internal' | 'external'; void act('antenna', async () => { await t.patch({ antenna: a }); await conn.refreshConfig(); }); }}>
              <option value="internal">built-in (ceramic)</option>
              <option value="external">external (U.FL)</option>
            </select></dd></>}
          {status.audio && config.audio_gate !== undefined && <><dt>audio</dt><dd>
            <span title="The microphone's average loudness in the latest 32 ms (ADC counts); blocks below the gate count as silence">rms {status.audio.rms ?? '?'}</span>
            {' '}<label title="Noise gate (rms in ADC counts, 0 = off): room noise below it stays dark. Applies at once; save to keep it">gate
              <input className="gamma" type="number" min={0} max={2000} defaultValue={config.audio_gate} key={config.audio_gate}
                onBlur={(e) => { const v = Math.round(+e.target.value); if (v >= 0 && v <= 2000 && v !== config.audio_gate) void act('audio gate', async () => { await t.patch({ audio_gate: v }); await conn.refreshConfig(); }); }} /></label>
            {' '}<button title="Listen to the quiet room for 5 s and set the gate a little above its noise (keep it quiet)"
              onClick={() => act('audio gate learn', async () => {
                const r = await t.cmd('audio gate learn 5');
                for (const l of r.output.replace(/\n$/, '').split('\n')) conn.addLog(l);
                await conn.refreshConfig();
              })}>Learn (5 s of quiet)</button></dd></>}
          <dt>heap</dt><dd>{Math.round(status.heap_free / 1024)} KB free</dd>
          {status.channels.map((c, i) => (
            <Fragment key={i}><dt>ch{i}</dt>
              <dd className={c.faults ? 'bad' : ''}>{c.npix} px, render {c.render_us.avg} µs, out {c.output_us.avg} µs,
                faults {c.faults}{c.last_error ? ` (${c.last_error})` : ''}</dd></Fragment>
          ))}
        </dl>
      ) : <p className="muted">…</p>}

      <h3>Channels</h3>
      <table className="channels">
        <thead><tr><th>ch</th><th>gpio</th><th>px</th><th title="Bytes per pixel on the wire (chan <n> <gpio> <npix> <wire>; save + reboot)">wire</th><th title="Gamma correction at output, 1 = off (applies at once)">gamma</th><th title="Temporal dither bits at output, 0 = off (applies at once)">dither</th>{hasW && <th title="White limit in percent (100 = every W output at full; applies at once)">W max</th>}<th>program</th><th /></tr></thead>
        <tbody>
          {config.channels.map((c, i) => (
            <tr key={i}>
              <td>{i}</td><td>{c.gpio}</td><td>{c.npix}</td><td><code>{c.wire ?? 'RGB'}</code></td>
              <td>{c.gamma === undefined ? <span className="muted" title="firmware without gamma support">–</span>
                : <GammaInput value={c.gamma} onCommit={(g) => act(`ch${i} gamma`, async () => { await t.patch({ channels: { [i]: { gamma: g } } }); await conn.refreshConfig(); })} />}</td>
              <td>{c.dither === undefined ? <span className="muted" title="firmware without dither support">–</span>
                : <select className="dither" value={c.dither} title="0 = off; more bits = finer dark steps, slower cycle (2^bits frames)"
                    onChange={(e) => { const d = +e.target.value; void act(`ch${i} dither`, async () => { await t.patch({ channels: { [i]: { dither: d } } }); await conn.refreshConfig(); }); }}>
                    {[0, 1, 2, 3, 4].map((d) => <option key={d} value={d}>{d || 'off'}</option>)}
                  </select>}</td>
              {hasW && <td>{c.wmax === undefined || !/W/.test(c.wire ?? '') ? null
                : <select className="dither" value={c.wmax} title="White limit (percent)"
                    onChange={(e) => { const v = +e.target.value; void act(`ch${i} white max`, async () => { await t.patch({ channels: { [i]: { wmax: v } } }); await conn.refreshConfig(); }); }}>
                    {[...new Set([100, 75, 50, 33, 25, 10, 0, c.wmax])].sort((a, b) => b - a).map((v) => <option key={v} value={v}>{v}%</option>)}
                  </select>}</td>}
              <td className="prog" title={c.prog ?? ''}>{c.running ? (c.prog?.split('\n').find((l) => l.trim() && !l.trim().startsWith(';')) ?? <span className="muted">(none)</span>) : <span className="muted">not running yet</span>}</td>
              <td>{c.running && <button onClick={() => onOpenProgram(i, c.prog ?? '')} title="Edit in a tab">Open</button>}</td>
            </tr>
          ))}
        </tbody>
      </table>

      <h3>Firmware</h3>
      <p className="fw">{fw ? <>KTSN LED <b>{fw.version}</b> · {fw.target} {fw.driver} · built {fw.built} {fw.time}
        <span className="muted"> · IDF {fw.idf} · slot {fw.slot}</span>
        {fw.state === 'pending' && <span className="bad" title="Kept once it renders for 30 s; a reset before then goes back to the other slot"> · new, not confirmed yet</span>}
        {fw.other && <div className="muted small">other slot {fw.other.slot}: KTSN LED {fw.other.version} (built {fw.other.built} {fw.other.time})</div>}</>
        : <span className="muted">version not reported (older firmware)</span>}</p>
      <div className="controls">
        {t.ota && fw?.ota && <button onClick={() => otaInput.current?.click()} disabled={!!update}
          title="Install a build/ktsn-led.bin over the network; the old firmware comes back if the new one doesn't run">Update over the network…</button>}
        <button onClick={onFlash} title="Write a whole build folder with esptool over USB (like idf.py flash)">Flash over USB…</button>
        {t.kind === 'http' && fw && !fw.ota && <span className="muted">network updates need the 4 MB OTA layout (flash it over USB once)</span>}
        <input ref={otaInput} type="file" accept=".bin,application/octet-stream" hidden onChange={(e) => pickUpdate(e.target.files?.[0])} />
      </div>
      {update && (
        <div className="update">
          <div>Install KTSN LED <b>{update.app.version}</b> (built {update.app.date} {update.app.time}, {update.app.target})
            {fw && <> replacing <b>{fw.version}</b></>}?</div>
          {update.problems.map((p) => <div key={p} className="bad">✖ {p}</div>)}
          {update.sent !== undefined
            ? <><progress max={1} value={update.sent} /> <span className="muted">{update.sent < 1 ? 'uploading…' : 'checking and writing…'}</span></>
            : <div className="controls">
                <button className="primary" disabled={update.problems.length > 0} onClick={install}>Install and restart</button>
                <button onClick={() => setUpdate(null)}>Cancel</button>
              </div>}
        </div>
      )}

      {hasW && config.channels.some((c) => c.wmode !== undefined) && (
        <>
          <h3>White (W)</h3>
          <table className="channels white">
            <thead><tr><th>ch</th>
              <th title="direct: W only from the program; extract: the white in RGB moves to the W LED (same colour); boost: it is added (brighter)">mode</th>
              <th title="White point: what full W looks like, as an RGB colour (tune by eye in direct mode: W full next to this colour in RGB)">point</th>
              <th title="How bright full W is compared to the point colour in RGB (0.1..4)">ratio</th></tr></thead>
            <tbody>{config.channels.map((c, i) => c.wmode === undefined || !/W/.test(c.wire ?? '') ? null : (
              <tr key={i}><td>{i}</td>
                <td><select value={c.wmode} onChange={(e) => { const v = e.target.value as 'direct'; void act(`ch${i} white mode`, async () => { await t.patch({ channels: { [i]: { wmode: v } } }); await conn.refreshConfig(); }); }}>
                  {(['direct', 'extract', 'boost'] as const).map((m) => <option key={m}>{m}</option>)}</select></td>
                <td><input type="color" value={(c.wpoint ?? '#FFB46B').toLowerCase()}
                  onChange={(e) => { const v = e.target.value.toUpperCase(); void act(`ch${i} white point`, async () => { await t.patch({ channels: { [i]: { wpoint: v } } }); await conn.refreshConfig(); }); }} />
                  {' '}<code>{c.wpoint}</code></td>
                <td><input className="gamma" type="number" min={0.1} max={4} step={0.1} defaultValue={c.wratio ?? 1} key={c.wratio}
                  onBlur={(e) => { const r = Math.round(+e.target.value * 100) / 100; if (r >= 0.1 && r <= 4 && r !== c.wratio) void act(`ch${i} white ratio`, async () => { await t.patch({ channels: { [i]: { wratio: r } } }); await conn.refreshConfig(); }); }} /></td>
              </tr>))}</tbody>
          </table>
        </>
      )}

      {config.automation !== undefined && (
        <>
          <h3>Automation</h3>
          <div className="automation">
            <div>{config.automation
              ? <><code className="prog">{config.automation.split('\n').find((l) => l.trim() && !l.trim().startsWith(';')) ?? ''}</code></>
              : <span className="muted">none: + in the tab bar → New automation, then Send</span>}</div>
            {status?.automation && config.automation && <div className="muted small">
              {status.automation.runs} runs{status.automation.last_run ? `, last ${status.automation.last_run.replace('T', ' ')}`
                : status.automation.last_run_uptime_s !== undefined ? `, last at uptime ${status.automation.last_run_uptime_s} s (clock not set)` : ''}
              {status.automation.faults > 0 && <span className="bad"> · {status.automation.faults} faults, last: {status.automation.last_error}</span>}
            </div>}
            <div className="controls">
              <button onClick={() => onOpenAutomation(config.automation ?? '')}>Open</button>
              {config.automation && <button onClick={() => act('automation run', async () => { await t.cmd('automation run'); })}>Run now</button>}
              <label title="How often it runs (seconds); aligned to the clock">every
                <input className="gamma" type="number" min={1} max={3600} defaultValue={config.automation_every ?? 10} key={config.automation_every}
                  onBlur={(e) => { const v = Math.round(+e.target.value); if (v >= 1 && v <= 3600 && v !== config.automation_every) void act('automation every', async () => { await t.patch({ automation_every: v }); await conn.refreshConfig(); }); }} /> s</label>
            </div>
          </div>
        </>
      )}

      {config.io !== undefined && (
        <>
          <h3>Inputs and outputs</h3>
          <OutputsSection t={t} config={config} status={status} act={act} refresh={conn.refreshConfig}
            pollNow={() => { t.status().then(setStatus, () => {}); }} />
        </>
      )}

      <h3>Library</h3>
      {libErr ? <p className="muted">{libErr}</p> : !lib ? <p className="muted">…</p> : (
        <>
          {lib.programs.length === 0 && <p className="muted">No library programs yet: + in the tab bar → New library program, or <code>lib save &lt;name&gt; &lt;ch&gt;</code>.</p>}
          <table className="channels library">
            <tbody>{[...lib.programs].sort((a, b) => a.name.localeCompare(b.name)).map((p) => (
              <tr key={p.name}>
                <td><code>{p.name}</code></td><td className="muted">{p.size} chars</td>
                <td>
                  <button onClick={() => act(`open ${p.name}`, async () => onOpenLibrary(p.name, await t.libGet(p.name)))}>Open</button>{' '}
                  <button onClick={() => onSaveToLocal(`lib:${p.name}`)} title="Keep a copy in the IDE's local library">To local…</button>{' '}
                  <button onClick={() => confirm(`Delete ${p.name} from ${deviceName}'s library?`) && act(`delete ${p.name}`, async () => { await t.libDel(p.name); await loadLib(); })}>Delete</button>
                </td>
              </tr>))}</tbody>
          </table>
          <p className="muted small">Storage: {lib.used} of {lib.total} entries used
            {!lib.separate && ' (shared with the settings: flash the current layout over USB for room for the library)'}</p>
          <p><button onClick={() => onSaveToLocal()} title="Copy library and channel programs into the IDE's local library">Save to local library…</button>
            <span className="muted small"> library and channel programs; to upload, select programs in the Library tab</span></p>
        </>
      )}

      <h3>Board</h3>
      <div className="controls">
        <label title="Global intensity (all channels)">Intensity <input type="range" min={0} max={256} value={intensity} onChange={(e) => setIntensity(+e.target.value)}
          onPointerUp={() => act('intensity', () => t.patch({ intensity, ...(fade ? { fade } : {}) }))} onKeyUp={() => act('intensity', () => t.patch({ intensity, ...(fade ? { fade } : {}) }))} />
          <span className="num">{intensity}</span></label>
        {config.channels[0]?.intensity !== undefined && <label title="Intensity changes from these sliders fade in over this many seconds (0 = at once)">fade
          <input type="number" min={0} max={3600} step={0.5} value={fade} onChange={(e) => setFade(Math.max(0, Math.min(3600, +e.target.value || 0)))} /> s</label>}
      </div>
      {config.channels[0]?.intensity !== undefined && (
        <div className="controls chan-int">
          {config.channels.map((c, i) => c.running && c.intensity !== undefined && (
            <ChanIntensity key={i} ch={i} value={c.intensity} now={status?.channels[i]?.intensity}
              onCommit={(v) => act(`ch${i} intensity`, async () => { await t.patch({ channels: { [i]: { intensity: v } }, ...(fade ? { fade } : {}) }); await conn.refreshConfig(); })} />))}
        </div>
      )}
      {config.output && (
        <div className="controls">
          <label title="Sends per rendered frame: programs run at 60 Hz, the LEDs are refreshed 1-4 times per frame (dither steps every send). Applies at once.">Refresh
            <select value={config.output.refresh} onChange={(e) => { const r = +e.target.value; void act('refresh', async () => { await t.patch({ output: { refresh: r } }); await conn.refreshConfig(); }); }}>
              {[1, 2, 3, 4].map((r) => <option key={r} value={r}>{r}× ({60 * r} Hz)</option>)}
            </select></label>
          {config.output.driver === 'rmt'
            ? <label className="check" title="Send all strings at the same time instead of one after another (RMT). Applies at once.">
                <input type="checkbox" checked={config.output.parallel}
                  onChange={(e) => { const p = e.target.checked; void act('parallel sending', async () => { await t.patch({ output: { parallel: p } }); await conn.refreshConfig(); }); }} /> parallel strings</label>
            : <span className="muted" title="PARLIO sends every string in one transfer">strings sent together (PARLIO)</span>}
        </div>
      )}
      <div className="controls">
        <details className="grow">
          <summary>Autoexec {config.autoexec ? '' : <span className="muted">(none)</span>}</summary>
          {config.autoexec && <pre className="code">{config.autoexec}</pre>}
        </details>
        <button onClick={() => onOpenAutoexec(config.autoexec ?? '')} title="Edit in a tab">Open</button>
      </div>
      <div className="controls">
        <button onClick={() => act('save to flash', () => t.save())} title="Store programs and settings so they survive a reboot">Save to flash</button>
        <button onClick={() => confirm(`Reboot ${deviceName}?`) && act('reboot', () => conn.reboot())}>Reboot</button>
        <button onClick={() => act('download config', download)}>Download config</button>
        <button onClick={() => fileInput.current?.click()}>Restore config…</button>
        <input ref={fileInput} type="file" accept=".json,application/json" hidden onChange={(e) => pickRestore(e.target.files?.[0])} />
      </div>
      {msg && <p className={/failed|error/.test(msg) ? 'bad' : 'muted'}>{msg}</p>}

      {restore && (
        <div className="restore">
          <p>Restore <b>{restore.file.device}</b> ({restore.file.config.channels.length} channel{restore.file.config.channels.length > 1 ? 's' : ''},
            exported {new Date(restore.file.exported).toLocaleString()}) to <b>{deviceName}</b>:</p>
          {RESTORE_LABELS.map(([k, label]) => (
            <label key={k} className="check"><input type="checkbox" checked={restore.opts[k]}
              onChange={(e) => setRestore({ ...restore, opts: { ...restore.opts, [k]: e.target.checked } })} /> {label}</label>
          ))}
          <div className="row"><button className="primary" onClick={applyRestore}>Restore</button><button onClick={() => setRestore(null)}>Cancel</button></div>
        </div>
      )}

      <h3>Console</h3>
      <div className="console">
        {conn.logs.map((l, i) => <div key={i} className={l.startsWith('led> ') ? 'echo' : /^[EW] \(/.test(l) ? 'bad' : ''}>{l}</div>)}
        <div ref={logEnd} />
      </div>
      <form className="row" onSubmit={(e) => { e.preventDefault(); void runCmd(); }}>
        <input className="cmd" value={cmdLine} placeholder="console command, e.g. stat" onChange={(e) => setCmdLine(e.target.value)} />
        <button type="submit">Run</button>
      </form>
    </div>
  );
}
