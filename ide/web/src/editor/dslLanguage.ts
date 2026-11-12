// CodeMirror support for the KTSN LED DSL: highlighting, color swatches, autocomplete, and live compile errors from the
// real compiler (WebAssembly).
import { autocompletion, type CompletionContext, type CompletionResult } from '@codemirror/autocomplete';
import { StreamLanguage, HighlightStyle, syntaxHighlighting } from '@codemirror/language';
import { linter, type Diagnostic } from '@codemirror/lint';
import { type Extension, RangeSetBuilder } from '@codemirror/state';
import { Decoration, type DecorationSet, EditorView, hoverTooltip, keymap, ViewPlugin, type ViewUpdate, WidgetType } from '@codemirror/view';
import { tags as t } from '@lezer/highlight';
import type { Core, CompileError, DslContext, UseSources } from '../core/ktsnLed';
import { usedNames } from '../library/uses';
import { ENTRY_BY_NAME, KEYWORDS, signature } from '../reference/entries';
import { findPalettes, type PaletteSpan } from '../palette/inCode';

/* ---------------- tokens ---------------- */

interface St { afterParen: boolean }
const SYM = /^[^\s();"]+/;

const dsl = StreamLanguage.define<St>({
  name: 'ktsn-led',
  startState: () => ({ afterParen: false }),
  token(stream, st) {
    if (stream.eatSpace()) return null;
    if (stream.match(/^"(?:[^"\\]|\\.)*"/)) { st.afterParen = false; return 'string'; }
    if (stream.match(/^".*/)) { st.afterParen = false; return 'invalid'; }   // unclosed: strings end on their line
    if (stream.match(/^;.*/)) return 'comment';
    if (stream.eat('(')) { st.afterParen = true; return 'paren'; }
    if (stream.eat(')')) { st.afterParen = false; return 'paren'; }
    const head = st.afterParen;
    st.afterParen = false;
    if (stream.match(/^#[0-9a-fA-F]{6}(?:[0-9a-fA-F]{2})?(?=[\s();]|$)/)) return 'color';
    if (stream.match(/^-?(?:\d+\.?\d*|\.\d+)(?=[\s();]|$)/)) return 'number';
    const m = stream.match(SYM) as RegExpMatchArray | null;
    if (!m) { stream.next(); return 'invalid'; }
    if (head) return KEYWORDS.includes(m[0]) ? 'def' : 'function';
    if (m[0] === 't' || m[0] === 'nil' || m[0] === '_') return 'atom';
    return 'name';
  },
  tokenTable: {
    paren: t.paren, color: t.color, number: t.number, function: t.function(t.variableName), def: t.definitionKeyword,
    atom: t.atom, name: t.variableName, comment: t.lineComment, invalid: t.invalid, string: t.string,
  },
  languageData: { commentTokens: { line: ';' }, closeBrackets: { brackets: ['('] } },
});

const highlight = HighlightStyle.define([
  { tag: t.lineComment, color: 'var(--syn-comment)', fontStyle: 'italic' },
  { tag: t.function(t.variableName), color: 'var(--syn-function)' },
  { tag: t.definitionKeyword, color: 'var(--syn-keyword)', fontWeight: '600' },
  { tag: t.number, color: 'var(--syn-number)' },
  { tag: t.color, color: 'var(--syn-color)' },
  { tag: t.string, color: 'var(--syn-string)' },
  { tag: t.atom, color: 'var(--syn-atom)' },
  { tag: t.variableName, color: 'var(--syn-name)' },
  { tag: t.paren, color: 'var(--syn-paren)' },
  { tag: t.invalid, color: 'var(--syn-invalid)' },
]);

/* ---------------- color swatches next to #RRGGBB literals ---------------- */

class Swatch extends WidgetType {
  constructor(readonly css: string) { super(); }
  eq(o: Swatch) { return o.css === this.css; }
  toDOM() {
    const s = document.createElement('span');
    s.className = 'cm-swatch';
    s.style.background = this.css;
    s.title = 'Click to edit the colour';
    return s;
  }
  ignoreEvent() { return false; }                      // let clicks reach the editor's handler below
}

/* ---------------- palettes in the code: a marker to open them in the Palette tab ---------------- */

class PaletteMark extends WidgetType {
  constructor(readonly n: number) { super(); }
  eq(o: PaletteMark) { return o.n === this.n; }
  toDOM() {
    const s = document.createElement('span');
    s.className = 'cm-pal-btn';
    s.textContent = '▦';
    s.title = `Edit this palette (${this.n} colour${this.n === 1 ? '' : 's'}) in the Palette tab`;
    return s;
  }
  ignoreEvent() { return false; }
}

function paletteMarks(onPalette: (p: PaletteSpan) => void): Extension {
  const plugin = ViewPlugin.fromClass(class {
    decorations: DecorationSet;
    spans: PaletteSpan[] = [];
    constructor(v: EditorView) { this.decorations = this.build(v); }
    update(u: ViewUpdate) { if (u.docChanged) this.decorations = this.build(u.view); }
    build(v: EditorView) {
      this.spans = findPalettes(v.state.doc.toString());
      const b = new RangeSetBuilder<Decoration>();
      for (const s of this.spans) b.add(s.from, s.from, Decoration.widget({ widget: new PaletteMark(s.stops.length), side: -1 }));
      return b.finish();
    }
  }, { decorations: (v) => v.decorations });
  return [plugin, EditorView.domEventHandlers({
    mousedown(e, view) {
      const el = e.target as HTMLElement;
      if (!el.classList?.contains('cm-pal-btn')) return false;
      const pos = view.posAtDOM(el);
      const span = view.plugin(plugin)?.spans.find((s) => s.from === pos);
      if (!span) return false;
      e.preventDefault();
      onPalette(span);
      return true;
    },
  })];
}

/** A colour literal clicked in the editor: its place in the document and where it is on screen. */
export interface ColorClick { from: number; to: number; value: string; rect: { left: number; top: number; bottom: number } }

/** Clicking a swatch reports the #RRGGBB[WW] literal right after it. */
function swatchClicks(onColor: (c: ColorClick) => void): Extension {
  return EditorView.domEventHandlers({
    mousedown(e, view) {
      const el = e.target as HTMLElement;
      if (!el.classList?.contains('cm-swatch')) return false;
      const pos = view.posAtDOM(el);
      const m = /^#[0-9a-fA-F]{6}(?:[0-9a-fA-F]{2})?/.exec(view.state.doc.sliceString(pos, pos + 9));
      if (!m) return false;
      e.preventDefault();
      const r = el.getBoundingClientRect();
      onColor({ from: pos, to: pos + m[0].length, value: m[0], rect: { left: r.left, top: r.top, bottom: r.bottom } });
      return true;
    },
  });
}

function swatches(view: EditorView): DecorationSet {
  const b = new RangeSetBuilder<Decoration>();
  for (const { from, to } of view.visibleRanges) {
    const text = view.state.doc.sliceString(from, to);
    for (const m of text.matchAll(/#([0-9a-fA-F]{6})(?:[0-9a-fA-F]{2})?(?=[\s();]|$)/g)) {
      b.add(from + m.index!, from + m.index!, Decoration.widget({ widget: new Swatch('#' + m[1]), side: -1 }));
    }
  }
  return b.finish();
}

const swatchPlugin = ViewPlugin.fromClass(class {
  decorations: DecorationSet;
  constructor(v: EditorView) { this.decorations = swatches(v); }
  update(u: ViewUpdate) { if (u.docChanged || u.viewportChanged) this.decorations = swatches(u.view); }
}, { decorations: (v) => v.decorations });

/* ---------------- user functions ---------------- */

export interface UserFn { name: string; params: string; line: number; doc: string; lib?: string }

/** The program's own functions, then those of the libraries it uses (directly or not). */
export function visibleFunctions(src: string, uses: UseSources): UserFn[] {
  const out = userFunctions(src), seen = new Set<string>(), todo = usedNames(src);
  for (let n = todo.pop(); n !== undefined; n = todo.pop()) {
    const lib = uses.get(n);
    if (seen.has(n) || typeof lib !== 'string') continue;
    seen.add(n);
    out.push(...userFunctions(lib).map((f) => ({ ...f, lib: n })));
    todo.push(...usedNames(lib));
  }
  return out;
}
const fnWhere = (f: UserFn) => (f.lib ? `from library ${f.lib}` : `defined on line ${f.line}`);

/** The program's (defn name (params) …) forms: name, parameter list as written, line (1-based), and the comment
 *  lines right above it (its description). Found by text, so it works while the program doesn't compile. */
export function userFunctions(src: string): UserFn[] {
  const out: UserFn[] = [];
  for (const m of src.matchAll(/\(\s*defn\s+([^\s();]+)\s*\(([^()]*)\)/g)) {
    const before = src.slice(0, m.index!);
    const lines = before.split('\n');
    const doc: string[] = [];
    for (let k = lines.length - 2; k >= 0 && /^\s*;/.test(lines[k]); k--) doc.unshift(lines[k].replace(/^\s*;+\s?/, ''));
    if (lines[lines.length - 1].trim()) doc.length = 0;         // defn not at the start of its line: no doc comment
    out.push({ name: m[1], params: m[2].trim().split(/\s+/).filter(Boolean).join(' '), line: lines.length, doc: doc.join(' ') });
  }
  return out;
}

/** Parameter names of every (defn …) and (fn …) in the program (for completion). */
function paramNames(src: string): string[] {
  const out: string[] = [];
  for (const m of src.matchAll(/\(\s*(?:defn\s+[^\s();]+|fn)\s*\(([^()]*)\)/g))
    for (const p of m[1].trim().split(/\s+/)) if (p.includes(':')) out.push(p.slice(0, p.indexOf(':')));
  return out;
}

/* ---------------- autocomplete ---------------- */

const BUFFER_IDS = ['X', 'Y', 'Z', 'O', 'G0', 'G1', 'G2', 'G3', 'G4', 'G5', 'G6', 'G7'];
const ENV_NAMES = ['frame', 'hour', 'minute', 'second', 'year', 'month', 'day', 'doy', 'intensity', 'npix'];

function completer(core: Core) {
  const natives = core.natives();
  return (ctx: CompletionContext): CompletionResult | null => {
    const word = ctx.matchBefore(/[^\s();]*/);
    if (!word || (word.from === word.to && !ctx.explicit)) return null;
    const before = ctx.state.doc.sliceString(Math.max(0, word.from - 12), word.from);
    const doc = ctx.state.doc.toString();
    const fns = visibleFunctions(doc, core.useSources());
    if (/\(\s*$/.test(before)) {                     // function position
      return {
        from: word.from,
        options: [...KEYWORDS, ...natives.map((n) => n.name)].map((name) => {
          const e = ENTRY_BY_NAME.get(name);
          return { label: name, type: KEYWORDS.includes(name) ? 'keyword' : 'function', detail: e ? signature(e).slice(name.length + 1, -1) : '', info: e?.summary };
        }).concat(fns.map((f) => ({ label: f.name, type: 'function', detail: f.params, info: f.doc || fnWhere(f) }))),
      };
    }
    if (/\(use\s+$/.test(before)) return { from: word.from, options: [...new Set([...(core.libraryNames() ?? []), ...core.useSources().keys()])].map((n) => ({ label: n, type: 'module' })) };
    if (/\(buf\s+$/.test(before)) return { from: word.from, options: BUFFER_IDS.map((b) => ({ label: b, type: 'constant' })) };
    if (/\(env\s+$/.test(before)) return { from: word.from, options: ENV_NAMES.map((e) => ({ label: e, type: 'property' })) };
    const vars = [...doc.matchAll(/\(def\s+([^\s();]+)/g)].map((m) => m[1]).concat(paramNames(doc));
    return { from: word.from, options: [...new Set(vars)].map((v) => ({ label: v, type: 'variable' }))
      .concat(fns.map((f) => ({ label: f.name, type: 'function', detail: f.params, info: f.doc || fnWhere(f) })))
      .concat(['t', 'nil', '_'].map((a) => ({ label: a, type: 'constant' }))) };
  };
}

/* ---------------- live compile errors ---------------- */

/** Document position of a compiler "line L col C" (columns count bytes; fine for ASCII programs). */
export function errorPos(doc: EditorView['state']['doc'], e: CompileError): number {
  const line = doc.line(Math.min(Math.max(e.line, 1), doc.lines));
  return Math.min(line.from + Math.max(e.col - 1, 0), line.to);
}

function compileLinter(core: Core, onResult: (e: CompileError | null) => void, context: () => DslContext) {
  return linter((view): Diagnostic[] => {
    const err = core.check(view.state.doc.toString(), context());
    onResult(err);
    if (!err) return [];
    const from = errorPos(view.state.doc, err);
    const tok = view.state.doc.sliceString(from, from + 40).match(/^[^\s()]*/)?.[0].length ?? 0;
    return [{ from, to: Math.min(from + Math.max(tok, 1), view.state.doc.length), severity: 'error', message: err.message }];
  }, { delay: 200 });
}

/* ---------------- reference: hover help and F1 ---------------- */

/** The function name at `pos`: a reference entry in function position (right after "("), or a user function
 *  (defn) wherever its name appears, e.g. passed to map-px. */
function functionAt(doc: EditorView['state']['doc'], pos: number, core: Core): { name: string; from: number; to: number; user?: UserFn } | null {
  const line = doc.lineAt(pos);
  const text = line.text, at = pos - line.from;
  let a = at, b = at;
  while (a > 0 && /[^\s();]/.test(text[a - 1])) a--;
  while (b < text.length && /[^\s();]/.test(text[b])) b++;
  if (a === b) return null;
  const name = text.slice(a, b);
  const user = visibleFunctions(doc.toString(), core.useSources()).find((f) => f.name === name);
  if (user) return { name, from: line.from + a, to: line.from + b, user };
  if (!/\(\s*$/.test(text.slice(0, a))) return null;
  return ENTRY_BY_NAME.has(name) ? { name, from: line.from + a, to: line.from + b } : null;
}

function referenceHelp(core: Core, onOpenRef: (name: string) => void): Extension {
  return [
    hoverTooltip((view, pos) => {
      const f = functionAt(view.state.doc, pos, core);
      if (!f) return null;
      const u = f.user, e = ENTRY_BY_NAME.get(f.name);
      return {
        pos: f.from, end: f.to, above: true,
        create() {
          const dom = document.createElement('div');
          dom.className = 'cm-ref-tip';
          const sig = document.createElement('code');
          sig.textContent = u ? `(${[u.name, u.params].filter(Boolean).join(' ')})` : signature(e!);
          const sum = document.createElement('div');
          sum.textContent = u ? [u.doc, u.lib ? `From library ${u.lib}.` : `Defined on line ${u.line}.`].filter(Boolean).join(' ') : e!.summary;
          const more = document.createElement('a');
          more.href = '#';
          more.textContent = u ? 'About defn (F1) →' : 'Reference (F1) →';
          more.onclick = (ev) => { ev.preventDefault(); onOpenRef(u ? 'defn' : f.name); };
          dom.append(sig, sum, more);
          return { dom };
        },
      };
    }, { hoverTime: 350 }),
    keymap.of([{ key: 'F1', run: (view) => {
      const f = functionAt(view.state.doc, view.state.selection.main.head, core);
      onOpenRef(f ? (f.user ? 'defn' : f.name) : '');
      return true;
    } }]),
  ];
}

/** `context`: what the shown document compiles as (render programs, or the automation program), asked on every check.
 *  `onColor`: a colour swatch was clicked (the editor opens its colour picker). `onPalette`: a palette's ▦ marker was
 *  clicked (the IDE opens it in the Palette tab). */
export function ktsnLedLanguage(core: Core, onCompile: (e: CompileError | null) => void, onOpenRef: (name: string) => void,
                               context: () => DslContext = () => 'render', onColor?: (c: ColorClick) => void,
                               onPalette?: (p: PaletteSpan) => void): Extension {
  return [dsl, syntaxHighlighting(highlight), swatchPlugin, autocompletion({ override: [completer(core)] }), compileLinter(core, onCompile, context),
          referenceHelp(core, onOpenRef), onColor ? swatchClicks(onColor) : [], onPalette ? paletteMarks(onPalette) : []];
}
