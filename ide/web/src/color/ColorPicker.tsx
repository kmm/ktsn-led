import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { addFavorite, formatHex, parseHex, type Rgbw, swatchCss, useFavorites } from './colors';

interface Props {
  value: string;                       // "#RRGGBB" or "#RRGGBBWW"
  /** Where to show it (screen coordinates of what was clicked). */
  anchor: { left: number; top: number; bottom: number };
  onChange(hex: string): void;         // live, while editing
  onDone(): void;                      // keep it (Enter, click outside)
  onCancel(): void;                    // put the original back (Esc)
}

const CHANNELS: [keyof Rgbw, string][] = [['r', 'R'], ['g', 'G'], ['b', 'B'], ['w', 'W']];

/** A colour editor for #RRGGBB[WW]: the browser's picker for RGB, sliders for R G B W, the hex text, and the shared
 *  favourites. Positioned against the window (fixed), so it is never clipped by a scrolling editor. */
export function ColorPicker({ value, anchor, onChange, onDone, onCancel }: Props) {
  const withW = value.length === 9;                     // an 8-digit literal stays 8 digits
  const [c, setC] = useState<Rgbw>(() => parseHex(value) ?? { r: 0, g: 0, b: 0, w: 0 });
  const [text, setText] = useState(value.toUpperCase());
  const [fav, setFav] = useFavorites();
  const box = useRef<HTMLDivElement>(null);
  const [pos, setPos] = useState({ left: anchor.left, top: anchor.bottom + 4 });

  // Below the swatch, or above it if there's no room; never off the side of the window.
  useLayoutEffect(() => {
    const r = box.current?.getBoundingClientRect();
    if (!r) return;
    const left = Math.max(4, Math.min(anchor.left, window.innerWidth - r.width - 4));
    const top = anchor.bottom + 4 + r.height > window.innerHeight ? Math.max(4, anchor.top - r.height - 4) : anchor.bottom + 4;
    setPos({ left, top });
  }, [anchor.left, anchor.top, anchor.bottom]);

  const cb = useRef({ onDone, onCancel });
  cb.current = { onDone, onCancel };
  useEffect(() => {
    const off = (e: MouseEvent) => { if (!box.current?.contains(e.target as Node)) cb.current.onDone(); };
    const key = (e: KeyboardEvent) => {
      if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); cb.current.onCancel(); }
      if (e.key === 'Enter' && (e.target as HTMLElement)?.tagName !== 'BUTTON') { e.preventDefault(); cb.current.onDone(); }
    };
    // Outside clicks close it, but only from the next event on: with a real click, React can mount the picker (and run
    // this effect) while the mousedown that opened it is still bubbling, and that same event would close it again at
    // once (it looked like the picker never opened). Synthetic test events didn't show this.
    const t = setTimeout(() => document.addEventListener('mousedown', off), 0);
    document.addEventListener('keydown', key, true);
    return () => { clearTimeout(t); document.removeEventListener('mousedown', off); document.removeEventListener('keydown', key, true); };
  }, []);

  const set = (n: Rgbw) => {
    setC(n);
    const hex = formatHex(n, withW);
    setText(hex);
    onChange(hex);
  };
  const hex = formatHex(c, withW);

  return (
    <div ref={box} className="color-picker" role="dialog" aria-label="Colour" style={{ left: pos.left, top: pos.top }}>
      <div className="cp-top">
        <span className="cp-preview" style={{ background: swatchCss(c) }} title="RGB with W mixed in (roughly)" />
        <input type="color" value={formatHex({ ...c, w: 0 }).toLowerCase()} title="Pick R G B"
          onChange={(e) => { const p = parseHex(e.target.value); if (p) set({ ...p, w: c.w }); }} />
        <input className="cp-hex" value={text} spellCheck={false} aria-label="Hex"
          onChange={(e) => { setText(e.target.value); const p = parseHex(e.target.value); if (p) { setC(p); onChange(formatHex(p, withW || e.target.value.length === 9)); } }} />
      </div>
      {CHANNELS.map(([k, label]) => (
        <label key={k} className={`cp-row cp-${k}`}>
          <span>{label}</span>
          <input type="range" min={0} max={255} value={c[k]} onChange={(e) => set({ ...c, [k]: +e.target.value })} />
          <input type="number" min={0} max={255} value={c[k]} onChange={(e) => set({ ...c, [k]: Math.max(0, Math.min(255, +e.target.value || 0)) })} />
        </label>
      ))}
      <div className="cp-fav-head">
        <span className="muted">Favourites</span>
        <button onClick={() => setFav(addFavorite(fav, hex))} disabled={fav.includes(hex) || !fav.includes(null)}
          title={fav.includes(null) ? 'Save this colour in the first free slot' : 'All 16 slots are used: right-click one to clear it'}>☆ Save</button>
      </div>
      <div className="cp-favs">
        {fav.map((f, i) => {
          const p = f ? parseHex(f) : null;
          return (
            <button key={i} className={`cp-fav ${f ? '' : 'empty'}`} style={p ? { background: swatchCss(p) } : undefined}
              title={f ? `${f} (click: use it; right-click: clear the slot)` : 'empty slot'}
              onClick={() => { if (p) set(p); }}
              onContextMenu={(e) => { e.preventDefault(); if (f) { const n = [...fav]; n[i] = null; setFav(n); } }} />
          );
        })}
      </div>
      <div className="cp-actions">
        <button onClick={onCancel}>Cancel</button>
        <button className="primary" onClick={onDone}>Done</button>
      </div>
    </div>
  );
}
