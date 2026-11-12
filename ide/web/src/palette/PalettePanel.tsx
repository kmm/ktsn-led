import { useEffect, useMemo, useState } from 'react';
import { ColorPicker } from '../color/ColorPicker';
import { parseHex, swatchCss, useFavorites } from '../color/colors';
import { gradient, loadPalettes, MAX_STOPS, paletteCode, type PaletteTarget, pattern, type SavedPalette, savePalettes, VAR_NAME } from './palette';

interface Props {
  /** Put the generated code at the cursor of the active tab. */
  onInsert(code: string): void;
  /** A palette opened from the code (▦ in the editor): load it (when seq changes). */
  request?: { seq: number; stops: string[]; target: PaletteTarget };
  /** Where the loaded palette came from (a tab's title), while it is linked to that code. */
  linked: string | null;
  /** Write the palette back over the code it came from. */
  onApply(code: string): void;
  onUnlink(): void;
  /** What happened on the last apply. */
  applyMsg: string;
}

const CURRENT = 'ktsn-led.ide.palette.current';
const START: { stops: string[]; target: PaletteTarget } = { stops: ['#FF0000', '#FFFF00', '#0000FF'], target: { buf: 0, start: 0, name: 'pal' } };

function loadCurrent(): typeof START {
  try {
    const v = JSON.parse(localStorage.getItem(CURRENT) ?? 'null');
    if (Array.isArray(v?.stops) && v.stops.every((s: unknown) => typeof s === 'string' && parseHex(s)) && v.target) return v;
  } catch { /* fresh */ }
  return START;
}

/** The Palette tab: build a palette from colour stops (same picker and favourites as the editor), see it as a gradient
 *  (fill-grad) or a repeating pattern (fill-pat), and get the code that stores it in a G buffer. */
export function PalettePanel({ onInsert, request, linked, onApply, onUnlink, applyMsg }: Props) {
  const [{ stops, target }, setCur] = useState(loadCurrent);
  const [sel, setSel] = useState(0);
  const [picker, setPicker] = useState<{ i: number; original: string; rect: { left: number; top: number; bottom: number } } | null>(null);
  const [mode, setMode] = useState<'gradient' | 'pattern'>('gradient');
  const [len, setLen] = useState(50);
  const [stretch, setStretch] = useState(1);
  const [saved, setSaved] = useState<SavedPalette[]>(() => loadPalettes());
  const [fav] = useFavorites();
  const [msg, setMsg] = useState('');

  useEffect(() => { try { localStorage.setItem(CURRENT, JSON.stringify({ stops, target })); } catch { /* not remembered */ } }, [stops, target]);
  useEffect(() => {
    if (!request) return;
    setCur({ stops: [...request.stops], target: { ...request.target } });
    setSel(0);
    setMsg('');
  }, [request?.seq]);  // eslint-disable-line react-hooks/exhaustive-deps

  const setStops = (s: string[]) => setCur((c) => ({ ...c, stops: s }));
  const setTarget = (t: Partial<PaletteTarget>) => setCur((c) => ({ ...c, target: { ...c.target, ...t } }));
  const i = Math.min(sel, stops.length - 1);

  const add = (hex?: string) => {
    if (stops.length >= MAX_STOPS) return;
    const n = [...stops];
    n.splice(i + 1, 0, hex ?? stops[i] ?? '#FFFFFF');
    setStops(n);
    setSel(i + 1);
  };
  const remove = () => { if (stops.length > 1) { setStops(stops.filter((_, k) => k !== i)); setSel(Math.max(0, i - 1)); } };
  const move = (d: number) => {
    const j = i + d;
    if (j < 0 || j >= stops.length) return;
    const n = [...stops];
    [n[i], n[j]] = [n[j], n[i]];
    setStops(n);
    setSel(j);
  };

  const rgbw = useMemo(() => stops.map((s) => parseHex(s)!), [stops]);
  const shown = mode === 'gradient' ? gradient(rgbw, len) : pattern(rgbw, len, stretch);
  const code = paletteCode(stops, target);
  const overflow = target.start + stops.length > 256;
  const badName = target.name !== '' && !VAR_NAME.test(target.name);

  const saveAs = () => {
    const name = prompt('Save this palette as:', saved.find((p) => p.stops.join() === stops.join())?.name ?? '')?.trim();
    if (!name) return;
    const list = [...saved.filter((p) => p.name !== name), { name, stops }].sort((a, b) => a.name.localeCompare(b.name));
    setSaved(list);
    savePalettes(list);
    setMsg(`saved as "${name}"`);
  };
  const loadSaved = (name: string) => { const p = saved.find((x) => x.name === name); if (p) { setStops(p.stops); setSel(0); setMsg(`loaded "${name}"`); } };
  const deleteSaved = (name: string) => {
    if (!confirm(`Delete the saved palette "${name}"?`)) return;
    const list = saved.filter((p) => p.name !== name);
    setSaved(list);
    savePalettes(list);
  };

  return (
    <div className="palette">
      {linked && (
        <div className="pal-link">
          <span>Editing the palette in <b>{linked}</b></span>
          <div className="controls">
            <button className="primary" onClick={() => onApply(code.trimEnd())} disabled={overflow}
              title="Replace that palette code with this one (undo with Ctrl/Cmd+Z in the editor)">Apply to code</button>
            <button onClick={onUnlink} title="Stop editing that code; Insert / Copy still work">Unlink</button>
          </div>
          {applyMsg && <span className="muted small">{applyMsg}</span>}
        </div>
      )}
      <div className="pal-stops" role="listbox" aria-label="Colour stops">
        {stops.map((s, k) => (
          <button key={k} role="option" aria-selected={k === i} className={`pal-stop ${k === i ? 'sel' : ''}`} style={{ background: swatchCss(rgbw[k]) }}
            title={`${k}: ${s} (click to select, click again to edit)`}
            onClick={(e) => {
              if (k !== i) { setSel(k); return; }
              const r = e.currentTarget.getBoundingClientRect();
              setPicker({ i: k, original: s, rect: { left: r.left, top: r.top, bottom: r.bottom } });
            }}>
            {rgbw[k].w > 0 && <span className="pal-w" title={`W ${rgbw[k].w}`}>W</span>}
          </button>
        ))}
      </div>
      <div className="controls">
        <button onClick={() => add()} disabled={stops.length >= MAX_STOPS} title="Add a stop after the selected one (a copy of it)">+ Add</button>
        <button onClick={() => move(-1)} disabled={i <= 0} title="Move the selected stop left">◀</button>
        <button onClick={() => move(1)} disabled={i >= stops.length - 1} title="Move the selected stop right">▶</button>
        <button onClick={remove} disabled={stops.length <= 1} title="Remove the selected stop">✕</button>
        <button onClick={() => setStops([...stops].reverse())} title="Reverse the order">⇄</button>
        <span className="muted small">{stops.length} stop{stops.length === 1 ? '' : 's'} · <code>{stops[i]}</code></span>
      </div>
      {fav.some(Boolean) && (
        <div className="pal-favs" title="Favourites: click one to add it after the selected stop">
          {fav.map((f, k) => f && <button key={k} className="cp-fav" style={{ background: swatchCss(parseHex(f)!) }} title={`add ${f}`} onClick={() => add(f)} />)}
        </div>
      )}

      <h3>Preview</h3>
      <div className="controls">
        <label>as
          <select value={mode} onChange={(e) => setMode(e.target.value as 'gradient')}>
            <option value="gradient">gradient (fill-grad)</option>
            <option value="pattern">pattern (fill-pat)</option>
          </select></label>
        <label>pixels <input type="number" min={1} max={300} value={len} onChange={(e) => setLen(Math.max(1, Math.min(300, +e.target.value || 1)))} /></label>
        {mode === 'pattern' && <label>stretch <input type="number" min={1} max={20} value={stretch} onChange={(e) => setStretch(Math.max(1, Math.min(20, +e.target.value || 1)))} /></label>}
      </div>
      <div className="pal-preview">{shown.map((p, k) => <span key={k} style={{ background: swatchCss(p) }} />)}</div>

      <h3>Code</h3>
      <div className="controls">
        <label>buffer
          <select value={target.buf} onChange={(e) => setTarget({ buf: +e.target.value })}>
            {Array.from({ length: 8 }, (_, k) => <option key={k} value={k}>G{k}</option>)}
          </select></label>
        <label>from <input type="number" min={0} max={255} value={target.start} onChange={(e) => setTarget({ start: Math.max(0, Math.min(255, +e.target.value || 0)) })} /></label>
        <label title="A (def ...) name programs can use for the palette; empty: none">name <input className="pal-name" value={target.name} onChange={(e) => setTarget({ name: e.target.value.trim() })} /></label>
      </div>
      {overflow && <p className="bad small">G buffers have 256 pixels: from {target.start} + {stops.length} stops goes past the end.</p>}
      {badName && <p className="bad small">"{target.name}" isn't a usable name (a letter, then letters, digits, - or _): no (def ...) is written.</p>}
      <pre className="code pal-code">{code}</pre>
      <div className="controls">
        <button className="primary" onClick={() => { onInsert(code); setMsg('inserted at the cursor of the active tab'); }} disabled={overflow}
          title="Into the active tab at the cursor (G buffers persist: the autoexec is a good place; or keep it in the program)">Insert at cursor</button>
        <button onClick={() => { void navigator.clipboard?.writeText(code).then(() => setMsg('copied'), () => setMsg('could not copy')); }}>Copy</button>
        {msg && <span className="muted small">{msg}</span>}
      </div>

      <h3>Saved palettes</h3>
      <div className="controls">
        <button onClick={saveAs}>Save as…</button>
        {saved.length > 0 && (
          <select value="" onChange={(e) => loadSaved(e.target.value)} title="Load a saved palette">
            <option value="">Load…</option>
            {saved.map((p) => <option key={p.name} value={p.name}>{p.name} ({p.stops.length})</option>)}
          </select>
        )}
        {saved.length > 0 && (
          <select value="" onChange={(e) => e.target.value && deleteSaved(e.target.value)} title="Delete a saved palette">
            <option value="">Delete…</option>
            {saved.map((p) => <option key={p.name} value={p.name}>{p.name}</option>)}
          </select>
        )}
      </div>

      {picker && (
        <ColorPicker key={picker.i} value={stops[picker.i]} anchor={picker.rect}
          onChange={(hex) => setStops(stops.map((s, k) => (k === picker.i ? hex : s)))}
          onDone={() => setPicker(null)}
          onCancel={() => { setStops(stops.map((s, k) => (k === picker.i ? picker.original : s))); setPicker(null); }} />
      )}
    </div>
  );
}
