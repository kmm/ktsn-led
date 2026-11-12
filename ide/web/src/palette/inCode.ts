// Palettes in program text: find the code the Palette tab writes (paletteCode), so the editor can offer to edit it.
// Recognised (G buffer slice, colours filling it exactly, `set` starting at 0, 8, 16 ...):
//   (set (slice (buf G0) 0 3) 0 #A #B #C)
//   (def pal (set (slice (buf G0) 0 3) 0 #A #B #C))
//   (def pal (slice (buf G0) 0 10)) (set pal 0 #.. x8) (set pal 8 #.. #..)
//   (set (slice (buf G0) 0 10) 0 #.. x8) (set (slice (buf G0) 0 10) 8 #.. #..)
// Only exact matches, so writing the palette back can't lose anything.
import { HEX_RE } from '../color/colors';
import { SET_MAX, type PaletteTarget } from './palette';

export interface PaletteSpan { from: number; to: number; text: string; stops: string[]; target: PaletteTarget }

type Sx = string | Sx[];
interface Form { from: number; to: number; sx: Sx }

/** Top-level forms with their positions (comments skipped). Unbalanced text ends the scan. */
export function topForms(src: string): Form[] {
  const out: Form[] = [];
  let i = 0;
  const skip = () => {
    for (;;) {
      while (i < src.length && /\s/.test(src[i])) i++;
      if (src[i] !== ';') return;
      while (i < src.length && src[i] !== '\n') i++;
    }
  };
  const parse = (): Sx | null => {
    skip();
    if (i >= src.length) return null;
    if (src[i] === '(') {
      i++;
      const list: Sx[] = [];
      for (;;) {
        skip();
        if (i >= src.length) return null;
        if (src[i] === ')') { i++; return list; }
        const x = parse();
        if (x === null) return null;
        list.push(x);
      }
    }
    if (src[i] === ')') return null;
    if (src[i] === '"') {                              // a string: kept whole, quotes included (may hold ( ) ;)
      const s = i++;
      while (i < src.length && src[i] !== '"' && src[i] !== '\n') i += src[i] === '\\' ? 2 : 1;
      if (src[i] !== '"') return null;
      return src.slice(s, ++i);
    }
    const s = i;
    while (i < src.length && !/[\s();]/.test(src[i])) i++;
    return src.slice(s, i);
  };
  for (;;) {
    skip();
    if (i >= src.length || src[i] !== '(') break;
    const from = i, sx = parse();
    if (sx === null) break;
    out.push({ from, to: i, sx });
  }
  return out;
}

const INT = /^\d+$/;
function slice(x: Sx): { buf: number; start: number; end: number } | null {
  if (!Array.isArray(x) || x.length !== 4 || x[0] !== 'slice') return null;
  const b = x[1];
  if (!Array.isArray(b) || b.length !== 2 || b[0] !== 'buf' || typeof b[1] !== 'string' || !/^[gG][0-7]$/.test(b[1])) return null;
  if (typeof x[2] !== 'string' || typeof x[3] !== 'string' || !INT.test(x[2]) || !INT.test(x[3])) return null;
  return { buf: +b[1][1], start: +x[2], end: +x[3] };
}
const sameSlice = (a: Sx, b: Sx) => JSON.stringify(a) === JSON.stringify(b);
const colors = (xs: Sx[]) => (xs.length > 0 && xs.length <= SET_MAX && xs.every((c) => typeof c === 'string' && HEX_RE.test(c)) ? xs as string[] : null);

/** (set <target> <k> #.. ...) -> colours if the target matches and k is the expected index. */
function setOf(x: Sx, target: Sx, k: number): string[] | null {
  if (!Array.isArray(x) || x[0] !== 'set' || x.length < 4 || !sameSlice(x[1], target) || x[2] !== String(k)) return null;
  return colors(x.slice(3));
}

/** Every recognised palette in `src`, in order. */
export function findPalettes(src: string): PaletteSpan[] {
  const forms = topForms(src);
  const out: PaletteSpan[] = [];
  const span = (a: number, b: number, stops: string[], t: PaletteTarget) => out.push({ from: forms[a].from, to: forms[b].to, text: src.slice(forms[a].from, forms[b].to), stops, target: t });
  /** Sets of `target` from form j on, at 0, 8, ...: [last form index, colours] filling exactly n, or null. */
  const run = (j: number, target: Sx, n: number, have: string[] = []): [number, string[]] | null => {
    let stops = have, last = j - 1;
    while (stops.length < n && last + 1 < forms.length) {
      const c = setOf(forms[last + 1].sx, target, stops.length);
      if (!c || (stops.length + c.length < n && c.length < SET_MAX)) return null;
      stops = [...stops, ...c];
      last++;
    }
    return stops.length === n ? [last, stops] : null;
  };
  for (let i = 0; i < forms.length; i++) {
    const x = forms[i].sx;
    if (!Array.isArray(x)) continue;
    // (def name (set slice 0 ...)) / (def name slice) + (set name 0 ...)...
    if (x[0] === 'def' && x.length === 3 && typeof x[1] === 'string') {
      const name = x[1], body = x[2];
      const sl = slice(body);
      if (sl) {
        const r = run(i + 1, name, sl.end - sl.start);
        if (r) { span(i, r[0], r[1], { buf: sl.buf, start: sl.start, name }); i = r[0]; }
        continue;
      }
      if (Array.isArray(body) && body[0] === 'set' && body.length >= 4) {
        const s2 = slice(body[1]), c = setOf(body, body[1], 0);
        if (s2 && c && c.length === s2.end - s2.start) span(i, i, c, { buf: s2.buf, start: s2.start, name });
      }
      continue;
    }
    // (set slice 0 ...) [ (set slice 8 ...) ... ]
    if (x[0] === 'set' && x.length >= 4) {
      const sl = slice(x[1]);
      if (!sl) continue;
      const r = run(i, x[1], sl.end - sl.start);
      if (r) { span(i, r[0], r[1], { buf: sl.buf, start: sl.start, name: '' }); i = r[0]; }
    }
  }
  return out;
}
