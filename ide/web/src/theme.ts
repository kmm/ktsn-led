import { useEffect, useState } from 'react';

export type ThemePref = 'system' | 'light' | 'dark';
const KEY = 'ktsn-led.ide.theme';

function readPref(): ThemePref {
  try {
    const v = localStorage.getItem(KEY);
    return v === 'light' || v === 'dark' ? v : 'system';
  } catch { return 'system'; }
}

/** Theme preference (remembered per browser) and whether the result is dark. Sets data-theme on <html>. */
export function useTheme(): { pref: ThemePref; dark: boolean; cycle(): void } {
  const [pref, setPref] = useState<ThemePref>(readPref);
  const media = window.matchMedia('(prefers-color-scheme: dark)');
  const [systemDark, setSystemDark] = useState(media.matches);
  useEffect(() => {
    const on = (e: MediaQueryListEvent) => setSystemDark(e.matches);
    media.addEventListener('change', on);
    return () => media.removeEventListener('change', on);
  }, [media]);
  const dark = pref === 'dark' || (pref === 'system' && systemDark);
  useEffect(() => {
    document.documentElement.dataset.theme = dark ? 'dark' : 'light';
    try { localStorage.setItem(KEY, pref); } catch { /* private mode: not remembered */ }
  }, [pref, dark]);
  const cycle = () => setPref(pref === 'system' ? 'light' : pref === 'light' ? 'dark' : 'system');
  return { pref, dark, cycle };
}
