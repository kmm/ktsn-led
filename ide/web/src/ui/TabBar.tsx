import { useEffect, useRef, useState } from 'react';
import { isUnsent, type EditorTab, type TabKind } from '../tabs/tabs';

interface Props {
  tabs: EditorTab[];
  activeId: string;
  title(t: EditorTab): string;
  onSelect(id: string): void;
  onClose(id: string): void;
  onNew(kind: TabKind): void;
}

export function TabBar({ tabs, activeId, title, onSelect, onClose, onNew }: Props) {
  // The menu is positioned against the window (fixed), not inside the tab bar: the bar scrolls sideways, and a
  // scrolling box clips its children vertically too, so an absolutely placed menu ended up hidden inside it.
  const [menu, setMenu] = useState<{ top: number; left: number } | null>(null);
  const menuRef = useRef<HTMLDivElement>(null);
  const addRef = useRef<HTMLButtonElement>(null);

  const toggle = () => {
    const r = addRef.current?.getBoundingClientRect();
    setMenu(menu || !r ? null : { top: r.bottom + 2, left: Math.max(4, Math.min(r.left, window.innerWidth - 220)) });
  };

  useEffect(() => {
    if (!menu) return;
    const off = (e: MouseEvent) => { if (!menuRef.current?.contains(e.target as Node)) setMenu(null); };
    const close = () => setMenu(null);                    // the button moved: don't leave the menu floating elsewhere
    document.addEventListener('mousedown', off);
    window.addEventListener('resize', close);
    window.addEventListener('scroll', close, true);
    return () => { document.removeEventListener('mousedown', off); window.removeEventListener('resize', close); window.removeEventListener('scroll', close, true); };
  }, [menu]);

  return (
    <div className="tabbar" role="tablist">
      {tabs.map((t) => {
        const unsent = isUnsent(t);
        return (
          <div key={t.id} role="tab" aria-selected={t.id === activeId} className={`etab ${t.id === activeId ? 'active' : ''}`}
            title={`${title(t)}${t.kind === 'autoexec' ? ' (autoexec)' : t.kind === 'library' ? ' (library program: stored on the board when sent)' : t.kind === 'automation' ? ' (automation: runs on the board every few seconds)' : t.kind === 'local' ? ' (local library program)' : ''}${unsent ? (t.kind === 'local' ? ' (not saved)' : ' (not sent to the board)') : ''}`}
            onClick={() => onSelect(t.id)} onAuxClick={(e) => { if (e.button === 1) { e.preventDefault(); onClose(t.id); } }}>
            {t.kind === 'autoexec' && <span className="kind" aria-label="autoexec">⚙</span>}
            {t.kind === 'library' && <span className="kind" aria-label="library program">☰</span>}
            {t.kind === 'automation' && <span className="kind" aria-label="automation">⏱</span>}
            {t.kind === 'local' && <span className="kind" aria-label="local library program">❖</span>}
            <span className="label">{title(t)}</span>
            {unsent && <span className="unsent" aria-label={t.kind === 'local' ? 'not saved' : 'not sent'}>●</span>}
            <button className="close" aria-label={`Close ${title(t)}`} onClick={(e) => { e.stopPropagation(); onClose(t.id); }}>×</button>
          </div>
        );
      })}
      <div className="add-wrap" ref={menuRef}>
        <button ref={addRef} className="add" onClick={toggle} title="New tab" aria-label="New tab" aria-expanded={!!menu}>+</button>
        {menu && (
          <div className="menu" role="menu" style={{ top: menu.top, left: menu.left }}>
            <button onClick={() => { setMenu(null); onNew('program'); }}>New program</button>
            <button onClick={() => { setMenu(null); onNew('autoexec'); }}>New autoexec</button>
            <button onClick={() => { setMenu(null); onNew('library'); }}>New library program…</button>
            <button onClick={() => { setMenu(null); onNew('automation'); }}>New automation</button>
          </div>
        )}
      </div>
    </div>
  );
}
