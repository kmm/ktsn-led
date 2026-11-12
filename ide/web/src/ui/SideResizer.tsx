import { useCallback, useRef, useState, type RefObject } from 'react';

const KEY = 'ktsn-led.ide.sideWidth';
export const SIDE_DEFAULT = 400, SIDE_MIN = 300, SIDE_MAX_FRACTION = 0.7;

/** Clamp a sidebar width to [SIDE_MIN, 70% of the window area it shares with the editor]. */
export function clampSide(w: number, total: number): number {
  return Math.round(Math.max(SIDE_MIN, Math.min(w, Math.max(SIDE_MIN, total * SIDE_MAX_FRACTION))));
}

export interface SideWidth { width: number; set(w: number): void; commit(): void }

export function useSideWidth(): SideWidth {
  const [width, setWidth] = useState(() => {
    try { const v = Number(localStorage.getItem(KEY)); return v >= SIDE_MIN ? v : SIDE_DEFAULT; } catch { return SIDE_DEFAULT; }
  });
  const latest = useRef(width);
  latest.current = width;
  const commit = useCallback(() => { try { localStorage.setItem(KEY, String(latest.current)); } catch { /* not remembered */ } }, []);
  const set = useCallback((w: number) => { latest.current = w; setWidth(w); }, []);
  return { width, set, commit };
}

/** Drag handle between the editor and the side pane. Arrow keys move it, double-click resets it. */
export function SideResizer({ state, container }: { state: SideWidth; container: RefObject<HTMLElement | null> }) {
  const dragging = useRef(false);
  const total = () => container.current?.clientWidth ?? window.innerWidth;
  const fromPointer = (x: number) => {
    const r = container.current?.getBoundingClientRect();
    return clampSide((r ? r.right : window.innerWidth) - x, total());
  };

  return (
    <div className="resizer" role="separator" aria-orientation="vertical" aria-label="Resize the side pane" tabIndex={0}
      aria-valuenow={state.width} aria-valuemin={SIDE_MIN} title="Drag to resize (double-click resets)"
      onPointerDown={(e) => { dragging.current = true; try { e.currentTarget.setPointerCapture(e.pointerId); } catch { /* pointer already gone */ } e.preventDefault(); }}
      onPointerMove={(e) => { if (dragging.current) state.set(fromPointer(e.clientX)); }}
      onPointerUp={() => { dragging.current = false; state.commit(); }}
      onDoubleClick={() => { state.set(clampSide(SIDE_DEFAULT, total())); state.commit(); }}
      onKeyDown={(e) => {
        const d = e.key === 'ArrowLeft' ? 20 : e.key === 'ArrowRight' ? -20 : 0;
        if (!d) return;
        e.preventDefault();
        state.set(clampSide(state.width + (e.shiftKey ? d * 4 : d), total()));
        state.commit();
      }} />
  );
}
