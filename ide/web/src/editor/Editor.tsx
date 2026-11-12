import { closeBrackets, closeBracketsKeymap, completionKeymap } from '@codemirror/autocomplete';
import { defaultKeymap, history, historyKeymap, indentWithTab, toggleComment } from '@codemirror/commands';
import { bracketMatching, indentOnInput } from '@codemirror/language';
import { forceLinting, lintKeymap } from '@codemirror/lint';
import { highlightSelectionMatches, searchKeymap } from '@codemirror/search';
import { Compartment, EditorState, type Extension } from '@codemirror/state';
import { oneDark } from '@codemirror/theme-one-dark';
import { EditorView, highlightActiveLine, highlightActiveLineGutter, keymap, lineNumbers } from '@codemirror/view';
import { useEffect, useRef, useState } from 'react';
import { ColorPicker } from '../color/ColorPicker';
import type { CompileError, Core, DslContext } from '../core/ktsnLed';
import { type ColorClick, ktsnLedLanguage } from './dslLanguage';
import type { PaletteSpan } from '../palette/inCode';

interface Props {
  core: Core;
  dark: boolean;
  /** Which document to show (the tab id). Each keeps its own undo history, cursor and scroll position. */
  docKey: string;
  /** Text for a document the editor hasn't seen yet. */
  docText: string;
  /** Documents still open: the editor forgets the others. */
  openKeys: string[];
  onChange(key: string, src: string): void;
  onCompile(err: CompileError | null): void;
  onSave(src: string): void;                     // Ctrl/Cmd+S
  /** Replace a document's text when `seq` changes (e.g. reloaded from a board). */
  replace?: { key: string; text: string; seq: number };
  /** Open the language reference at a function ("" = the index). */
  onOpenRef(name: string): void;
  /** A palette's ▦ marker was clicked: its place and contents in the shown document. */
  onPalette?(p: PaletteSpan): void;
  /** Replace `expect` (found at from..to, or elsewhere if it moved) in document `key` when `seq` changes; reports where
   *  the new text went, or null if `expect` is gone (the code was changed meanwhile). The document must be shown. */
  rangeEdit?: { key: string; from: number; to: number; expect: string; text: string; seq: number };
  onRangeEdited?(r: { from: number; to: number; text: string } | null): void;
  /** Insert text at the cursor of the shown document when `seq` changes (e.g. palette code). */
  insert?: { text: string; seq: number };
  /** What the shown document compiles as (automation tabs: 'automation'). */
  context?: DslContext;
  /** Check the document again when this changes (what it compiles against changed, e.g. the libraries it uses). */
  recheck?: number;
}

const lightTheme = EditorView.theme({}, { dark: false });

export function Editor({ core, dark, docKey, docText, openKeys, onChange, onCompile, onSave, replace, insert, onOpenRef, context = 'render', onPalette, rangeEdit, onRangeEdited, recheck }: Props) {
  const host = useRef<HTMLDivElement>(null);
  const view = useRef<EditorView | null>(null);
  const theme = useRef(new Compartment());
  const states = useRef(new Map<string, EditorState>());
  const shown = useRef(docKey);
  const extensions = useRef<Extension[] | null>(null);
  const cb = useRef({ onChange, onCompile, onSave, onOpenRef, onPalette, onRangeEdited });
  cb.current = { onChange, onCompile, onSave, onOpenRef, onPalette, onRangeEdited };
  const darkRef = useRef(dark);
  darkRef.current = dark;
  const ctxRef = useRef(context);
  ctxRef.current = context;

  const makeState = (text: string) => EditorState.create({ doc: text, extensions: extensions.current! });

  // Colour picker on a clicked swatch: edits the literal in place while it is open (one undo step per pause), Esc puts
  // the original back. `edit` tracks where the literal is now (its length changes between 7 and 9 characters).
  const [picker, setPicker] = useState<ColorClick | null>(null);
  const edit = useRef<{ from: number; len: number; original: string } | null>(null);
  const openPicker = (c: ColorClick) => { edit.current = { from: c.from, len: c.to - c.from, original: c.value }; setPicker(c); };
  const replaceColor = (hex: string) => {
    const v = view.current, e = edit.current;
    if (!v || !e || e.from + e.len > v.state.doc.length) return;
    v.dispatch({ changes: { from: e.from, to: e.from + e.len, insert: hex }, userEvent: 'input.color' });
    e.len = hex.length;
  };
  const closePicker = (revert: boolean) => {
    if (revert && edit.current) replaceColor(edit.current.original);
    edit.current = null;
    setPicker(null);
    view.current?.focus();
  };

  useEffect(() => {
    extensions.current = [
      lineNumbers(), highlightActiveLineGutter(), highlightActiveLine(), history(), indentOnInput(),
      bracketMatching(), closeBrackets(), highlightSelectionMatches(),
      keymap.of([
        { key: 'Mod-s', preventDefault: true, run: (ed) => { cb.current.onSave(ed.state.doc.toString()); return true; } },
        { key: 'Mod-/', run: toggleComment },
        indentWithTab, ...closeBracketsKeymap, ...defaultKeymap, ...historyKeymap, ...searchKeymap, ...completionKeymap, ...lintKeymap,
      ]),
      ktsnLedLanguage(core, (e) => cb.current.onCompile(e), (n) => cb.current.onOpenRef(n), () => ctxRef.current, (c) => openPicker(c),
        (p) => cb.current.onPalette?.(p)),
      EditorView.updateListener.of((u) => { if (u.docChanged) cb.current.onChange(shown.current, u.state.doc.toString()); }),
      theme.current.of(darkRef.current ? oneDark : lightTheme),
      EditorView.theme({ '&': { height: '100%' }, '.cm-scroller': { fontFamily: 'var(--mono)' } }),
    ];
    const v = new EditorView({ parent: host.current!, state: makeState(docText) });
    view.current = v;
    shown.current = docKey;
    return () => { v.destroy(); states.current.clear(); };
    // the view is created once per core; documents are switched below
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [core]);

  useEffect(() => { if (view.current && recheck) forceLinting(view.current); }, [recheck]);

  // Switch documents: keep the old one's state (undo history, selection), restore or create the new one.
  useEffect(() => {
    const v = view.current;
    if (!v || shown.current === docKey) return;
    edit.current = null;
    setPicker(null);
    states.current.set(shown.current, v.state);
    shown.current = docKey;
    v.setState(states.current.get(docKey) ?? makeState(docText));
    v.dispatch({ effects: theme.current.reconfigure(darkRef.current ? oneDark : lightTheme) });   // theme may have changed meanwhile
    v.focus();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [docKey]);

  useEffect(() => {
    for (const k of [...states.current.keys()]) if (!openKeys.includes(k)) states.current.delete(k);
  }, [openKeys]);

  useEffect(() => {
    view.current?.dispatch({ effects: theme.current.reconfigure(dark ? oneDark : lightTheme) });
  }, [dark]);

  useEffect(() => {
    if (!replace) return;
    const v = view.current;
    if (v && replace.key === shown.current) {
      v.dispatch({ changes: { from: 0, to: v.state.doc.length, insert: replace.text }, selection: { anchor: 0 } });
      v.focus();
    } else {
      states.current.delete(replace.key);              // rebuilt from the new text when shown
    }
  }, [replace?.seq]);  // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    const v = view.current;
    if (!rangeEdit || !v || shown.current !== rangeEdit.key) return;
    const doc = v.state.doc.toString();
    const from = doc.slice(rangeEdit.from, rangeEdit.to) === rangeEdit.expect ? rangeEdit.from : doc.indexOf(rangeEdit.expect);
    if (from < 0 || !rangeEdit.expect) { cb.current.onRangeEdited?.(null); return; }
    const to = from + rangeEdit.expect.length;
    v.dispatch({ changes: { from, to, insert: rangeEdit.text }, userEvent: 'input.palette', scrollIntoView: true });
    cb.current.onRangeEdited?.({ from, to: from + rangeEdit.text.length, text: rangeEdit.text });
  }, [rangeEdit?.seq]);  // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    const v = view.current;
    if (!insert || !v) return;
    const { from, to } = v.state.selection.main;
    const line = v.state.doc.lineAt(from);
    const pre = from > line.from && v.state.doc.sliceString(from - 1, from) !== '\n' ? '\n' : '';   // start on its own line
    v.dispatch({ changes: { from, to, insert: pre + insert.text }, selection: { anchor: from + pre.length + insert.text.length },
                 userEvent: 'input.paste', scrollIntoView: true });
    v.focus();
  }, [insert?.seq]);  // eslint-disable-line react-hooks/exhaustive-deps

  return (
    <>
      <div className="editor" ref={host} />
      {picker && <ColorPicker key={`${picker.from}:${picker.value}`} value={picker.value} anchor={picker.rect}
        onChange={replaceColor} onDone={() => closePicker(false)} onCancel={() => closePicker(true)} />}
    </>
  );
}
