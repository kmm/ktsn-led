import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { BUFFER_NAMES, type BufferName, type CompileError, type Core, DEFAULT_WHITE, type WhiteSettings } from '../core/ktsnLed';
import { averageSends, envAt, FRAME_HZ, type Location, pixelCss, pixelCssVia, pixelCssWhite, screenTable, skyClock } from '../core/env';

interface Props {
  core: Core;
  program: string | null;        // what to run (set by Run / Ctrl+S)
  programVersion: number;        // bump to (re)load `program`
  onLoad(err: CompileError | null): void;
  /** Pixel count of the board channel being edited, if any: the preview follows it. */
  npixHint?: number;
  /** Gamma of that board channel (1 = off), if known: the preview follows it. */
  gammaHint?: number;
  /** Dither bits of that board channel (0 = off), if known. */
  ditherHint?: number;
  /** The board's output refresh (sends per frame, 1..4), if known. */
  refreshHint?: number;
  /** That board channel's own intensity (0..1), multiplied with the preview's intensity at output. */
  chanIntensityHint?: number;
  /** Wire format and white handling of that board channel, if known. */
  whiteHint?: WhiteSettings;
  /** That board's location (null = none set; undefined = no board, or firmware without it). */
  locHint?: Location | null;
  /** Autoexec the preview uses: run on Reset, and at once whenever `version` changes (an autoexec tab was saved). */
  autoexec: { src: string; from: string; version: number } | null;
}

type ClockMode = 'real' | 'sim';
const SPEEDS = [0.25, 0.5, 1, 2, 4];

function localInputValue(d: Date): string {
  const p = (n: number) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}T${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

const LED_LIGHT_KEY = 'ktsn-led.ide.ledLight';
const LOCATION_KEY = 'ktsn-led.ide.previewLocation';

/** The location typed in the preview, or null unless both are valid numbers in range. */
function parseLocation(lat: string, lon: string): Location | null {
  const a = +lat, o = +lon;
  return lat.trim() && lon.trim() && Math.abs(a) <= 90 && Math.abs(o) <= 180 ? { lat: a, lon: o } : null;
}
const hhmm = (m: number) => (m < 0 ? 'none' : `${String(Math.floor(m / 60)).padStart(2, '0')}:${String(m % 60).padStart(2, '0')}`);

export function Preview({ core, program, programVersion, onLoad, npixHint, gammaHint, ditherHint, refreshHint, whiteHint, chanIntensityHint, locHint, autoexec }: Props) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const [npix, setNpix] = useState(50);
  const [playing, setPlaying] = useState(true);
  const [speed, setSpeed] = useState(1);
  const [intensity, setIntensity] = useState(256);
  const [gamma, setGamma] = useState(1);
  const [dither, setDither] = useState(0);
  const [refresh, setRefresh] = useState(1);
  const [white, setWhite] = useState<WhiteSettings>(DEFAULT_WHITE);
  const hasW = /W/i.test(white.wire);
  const [ledLight, setLedLight] = useState(() => { try { return localStorage.getItem(LED_LIGHT_KEY) !== 'false'; } catch { return true; } });
  const table = useMemo(() => screenTable(ledLight), [ledLight]);
  const [view, setView] = useState<BufferName>('O');
  const [clock, setClock] = useState<ClockMode>('real');
  const [simStart, setSimStart] = useState(() => localInputValue(new Date()));
  const [autoexecMsg, setAutoexecMsg] = useState('');
  const [latLon, setLatLon] = useState<[string, string]>(() => {
    try { const v = JSON.parse(localStorage.getItem(LOCATION_KEY) ?? 'null'); return Array.isArray(v) && v.length === 2 ? [String(v[0]), String(v[1])] : ['', '']; } catch { return ['', '']; }
  });
  const location = useMemo(() => parseLocation(latLon[0], latLon[1]), [latLon]);
  const sky = useMemo(() => skyClock(core), [core]);
  const [stats, setStats] = useState({ frame: 0, nodes: 0, faults: 0, lastError: '', scratch: 0, time: '', sun: '', moon: '' });

  // Everything the animation loop reads lives in a ref, so the loop never restarts on a state change.
  const chanInt = chanIntensityHint ?? 1;
  const live = useRef({ frame: 0, playing, speed, intensity, chanInt, gamma, dither, refresh, white, ledLight, view, clock, simStart, npix, table, location });
  live.current = { ...live.current, playing, speed, intensity, chanInt, gamma, dither, refresh, white, ledLight, view, clock, simStart, npix, table, location };
  const programRef = useRef(program);
  programRef.current = program;

  const draw = useCallback(() => {
    const c = canvas.current;
    if (!c) return;
    const L = live.current;
    const out = L.view === 'O';                      // gamma, intensity, dither and the LED-light view apply to the output only
    // Output: the board sends `refresh` times per frame, each with the next dither step; show what the eye averages.
    const px = out ? averageSends(Array.from({ length: L.refresh }, (_, j) => core.output(L.gamma, L.intensity * L.chanInt, L.dither, L.frame * L.refresh + j, L.white)))
      : core.buffer(L.view);
    const dpr = window.devicePixelRatio || 1;
    const w = c.clientWidth;
    const d = Math.max(6, Math.min(22, Math.floor((w - 8) / Math.min(px.length, 32)) - 3)), gap = Math.max(2, Math.round(d / 5));
    const cols = Math.max(1, Math.floor((w - 8) / (d + gap)));
    const rows = Math.ceil(px.length / cols);
    const h = rows * (d + gap) + 8;
    if (c.height !== Math.round(h * dpr) || c.width !== Math.round(w * dpr)) {
      c.width = Math.round(w * dpr); c.height = Math.round(h * dpr); c.style.height = `${h}px`;
    }
    const g = c.getContext('2d')!;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    const off = getComputedStyle(c).getPropertyValue('--led-off').trim() || '#2a2a2a';   // canvas can't read CSS vars
    for (let i = 0; i < px.length; i++) {
      const x = 4 + (i % cols) * (d + gap) + d / 2, y = 4 + Math.floor(i / cols) * (d + gap) + d / 2;
      g.beginPath();
      g.arc(x, y, d / 2, 0, Math.PI * 2);
      g.fillStyle = (out ? (px[i] >>> 24 ? pixelCssWhite(px[i], L.white.point, L.white.ratio, L.ledLight) : pixelCssVia(px[i], L.table))
        : (px[i] & 0xffffff) ? pixelCss(px[i], 256) : null) ?? off;
      g.fill();
    }
  }, [core]);

  const step = useCallback(() => {
    const L = live.current;
    L.frame += 1;                                   // like the board: frame counts up before each render
    const when = L.clock === 'real' ? new Date() : new Date(+new Date(L.simStart) + (L.frame * 1000) / FRAME_HZ);
    const s = sky(when, L.location);
    const nodes = core.render(envAt(L.frame, when, L.intensity, s));
    setStats({ frame: L.frame, nodes, faults: core.faults(), lastError: core.lastError(), scratch: core.scratchHigh(),
               time: when.toLocaleTimeString(),
               sun: L.location ? `dawn ${hhmm(s.dawn)}, rise ${hhmm(s.sunrise)}, set ${hhmm(s.sunset)}, dusk ${hhmm(s.dusk)} (${s.daylight > 0 ? 'up' : 'down'})` : 'no location',
               moon: `${s.moon}°, ${s.moonlit}% lit` });
  }, [core]);

  /** Fresh channel: clear all buffers (globals too), frame 0, fixed seed, autoexec, reload the program. */
  const reset = useCallback(() => {
    core.init(live.current.npix);
    core.clear();
    core.seed(0x2545f491);
    live.current.frame = 0;
    const src = autoexec?.src.trim() ? autoexec.src : '';
    const a = src ? core.autoexec(src) : '';
    setAutoexecMsg(a ? `autoexec failed: ${a}` : src ? `autoexec ran (${autoexec!.from})` : '');
    if (programRef.current !== null) onLoad(core.load(programRef.current));
    step();
    draw();
  }, [core, autoexec, onLoad, step, draw]);

  useEffect(() => { if (npixHint && npixHint > 0) setNpix(npixHint); }, [npixHint]);
  useEffect(() => { if (gammaHint !== undefined) setGamma(gammaHint); }, [gammaHint]);
  useEffect(() => { if (ditherHint !== undefined) setDither(ditherHint); }, [ditherHint]);
  useEffect(() => { if (refreshHint !== undefined) setRefresh(refreshHint); }, [refreshHint]);
  useEffect(() => { if (locHint !== undefined) setLatLon(locHint ? [String(locHint.lat), String(locHint.lon)] : ['', '']); }, [locHint?.lat, locHint?.lon, locHint === null]);  // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => { try { localStorage.setItem(LOCATION_KEY, JSON.stringify(latLon)); } catch { /* a convenience */ } }, [latLon]);
  useEffect(() => { if (whiteHint) setWhite(whiteHint); }, [whiteHint?.wire, whiteHint?.mode, whiteHint?.point, whiteHint?.ratio, whiteHint?.max]);  // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => { try { localStorage.setItem(LED_LIGHT_KEY, String(ledLight)); } catch { /* not remembered */ } }, [ledLight]);

  // Pixel count changes rebuild the channel.
  useEffect(() => { reset(); /* eslint-disable-next-line react-hooks/exhaustive-deps */ }, [npix]);

  // An autoexec tab was saved: run it now, like a board does when its autoexec is sent (global buffers change).
  useEffect(() => {
    if (!autoexec || autoexec.version === 0) return;
    const a = autoexec.src.trim() ? core.autoexec(autoexec.src) : '';
    setAutoexecMsg(a ? `autoexec failed: ${a}` : `autoexec ran (${autoexec.from})`);
  }, [core, autoexec?.version]);  // eslint-disable-line react-hooks/exhaustive-deps

  // New program: load it into the running channel (buffers persist, like `prog` on a board).
  useEffect(() => {
    if (program !== null && programVersion > 0) onLoad(core.load(program));
  }, [core, program, programVersion, onLoad]);

  // 60 Hz simulation driven by requestAnimationFrame, scaled by `speed`.
  useEffect(() => {
    let raf = 0, last = performance.now(), acc = 0;
    const tick = (now: number) => {
      const L = live.current;
      if (L.playing) {
        acc += Math.min(now - last, 250) * L.speed;
        let n = 0;
        while (acc >= 1000 / FRAME_HZ && n++ < 16) { step(); acc -= 1000 / FRAME_HZ; }
      } else acc = 0;
      last = now;
      draw();
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, [step, draw]);

  return (
    <div className="preview">
      <canvas ref={canvas} className="strip" />
      <div className="controls">
        <button onClick={() => setPlaying(!playing)} title="Play / pause">{playing ? '❚❚ Pause' : '▶ Play'}</button>
        <button onClick={() => { step(); draw(); }} disabled={playing} title="Advance one frame">Step</button>
        <button onClick={reset} title="Clear buffers, frame 0, run autoexec, reload the program">Reset</button>
        <label>Speed
          <select value={speed} onChange={(e) => setSpeed(+e.target.value)}>
            {SPEEDS.map((s) => <option key={s} value={s}>{s}×</option>)}
          </select>
        </label>
        <label>Show
          <select value={view} onChange={(e) => setView(e.target.value as BufferName)}>
            {BUFFER_NAMES.map((b) => <option key={b}>{b}</option>)}
          </select>
        </label>
      </div>
      <div className="controls">
        <label>Pixels <input type="number" min={1} max={2048} value={npix}
          onChange={(e) => setNpix(Math.max(1, Math.min(2048, +e.target.value || 1)))} /></label>
        <label>Intensity <input type="range" min={0} max={256} value={intensity} onChange={(e) => setIntensity(+e.target.value)} />
          <span className="num">{intensity}</span></label>
        {chanInt !== 1 && <span className="muted" title="The board channel's own intensity, applied too">× ch {Math.round(chanInt * 100)}%</span>}
      </div>
      <div className="controls">
        <label title="Gamma correction at output, like the board's (1 = off). Follows the board channel being edited.">Gamma
          <input type="number" min={1} max={3} step={0.1} value={gamma}
            onChange={(e) => { const g = Math.round(+e.target.value * 100) / 100; if (g >= 1 && g <= 3) setGamma(g); }} /></label>
        <label title="Temporal dither bits, like the board's (0 = off). Follows the board channel being edited. Pause and Step to see the cycle.">Dither
          <select value={dither} onChange={(e) => setDither(+e.target.value)}>
            {[0, 1, 2, 3, 4].map((d) => <option key={d} value={d}>{d ? `${d} bit${d > 1 ? 's' : ''}` : 'off'}</option>)}
          </select></label>
        <label title="Output sends per rendered frame, like the board's refresh setting: each frame shows the average of its sends (what the eye sees), so faster refresh smooths dither. Follows the connected board.">Refresh
          <select value={refresh} onChange={(e) => setRefresh(+e.target.value)}>
            {[1, 2, 3, 4].map((r) => <option key={r} value={r}>{r}× ({60 * r} Hz)</option>)}
          </select></label>
      </div>
      <div className="controls">
        <label title="The channel's wire format (bytes per pixel, e.g. RGB, GRBW, BRG WWW). Follows the board channel being edited.">Wire
          <input className="wire" value={white.wire} onChange={(e) => setWhite({ ...white, wire: e.target.value })} /></label>
        {hasW && <>
          <label title="direct: W only from the program; extract: the white in RGB moves to the W LED; boost: it is added">White
            <select value={white.mode} onChange={(e) => setWhite({ ...white, mode: e.target.value as WhiteSettings['mode'] })}>
              {(['direct', 'extract', 'boost'] as const).map((m) => <option key={m}>{m}</option>)}
            </select></label>
          <label title="White point: what full W looks like (RGB colour), and how bright compared to that colour">
            <input type="color" value={`#${white.point.toString(16).padStart(6, '0')}`} onChange={(e) => setWhite({ ...white, point: parseInt(e.target.value.slice(1), 16) })} />
            ×<input type="number" min={0.1} max={4} step={0.1} value={white.ratio} onChange={(e) => { const r = +e.target.value; if (r >= 0.1 && r <= 4) setWhite({ ...white, ratio: r }); }} /></label>
          <label title="White limit (percent)">max
            <input type="number" min={0} max={100} step={5} value={white.max} onChange={(e) => { const v = +e.target.value; if (v >= 0 && v <= 100) setWhite({ ...white, max: v }); }} /></label>
        </>}
        <label className="check" title="Show the output as the LEDs would look (their light is linear; a screen's isn't). Off: draw the bytes sent to the LEDs.">
          <input type="checkbox" checked={ledLight} onChange={(e) => setLedLight(e.target.checked)} /> LED light</label>
      </div>
      <div className="controls">
        <label>Clock
          <select value={clock} onChange={(e) => setClock(e.target.value as ClockMode)}>
            <option value="real">real time</option>
            <option value="sim">simulated from…</option>
          </select>
        </label>
        {clock === 'sim' && <input type="datetime-local" step={1} value={simStart} onChange={(e) => setSimStart(e.target.value)} />}
      </div>
      <div className="controls">
        <label title="For env sunrise, sunset, dawn and dusk: degrees, north positive. Follows the board when one is connected.">Lat
          <input type="number" min={-90} max={90} step="any" value={latLon[0]} onChange={(e) => setLatLon([e.target.value, latLon[1]])} /></label>
        <label title="Degrees, east positive.">Lon
          <input type="number" min={-180} max={180} step="any" value={latLon[1]} onChange={(e) => setLatLon([latLon[0], e.target.value])} /></label>
      </div>
      <dl className="stats">
        <dt>frame</dt><dd>{stats.frame}</dd>
        <dt>time</dt><dd>{stats.time}</dd>
        <dt>sun</dt><dd>{stats.sun}</dd>
        <dt>moon</dt><dd>{stats.moon}</dd>
        <dt>nodes</dt><dd>{stats.nodes}</dd>
        <dt>scratch</dt><dd>{stats.scratch} B</dd>
        <dt>faults</dt><dd className={stats.faults ? 'bad' : ''}>{stats.faults}{stats.lastError && ` — ${stats.lastError}`}</dd>
      </dl>
      <div className="autoexec-note">
        <span className="muted">Autoexec: </span>
        {autoexec?.src.trim() ? <span>{autoexec.from} <span className="muted">(runs on Reset and when that tab is saved)</span></span>
          : <span className="muted">none. Add an autoexec tab (+ in the tab bar) to set up global buffers.</span>}
        {autoexecMsg && <div className={autoexecMsg.startsWith('autoexec failed') ? 'bad' : 'muted'}>{autoexecMsg}</div>}
      </div>
    </div>
  );
}
