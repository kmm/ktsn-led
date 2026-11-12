// The device list, remembered in this browser. Tokens are stored as typed (a local tool on your own machine).
// Serial ports can't be stored: the browser remembers which ports were granted, and we match them by USB ids.

export interface DeviceSpec {
  id: string;
  name: string;
  kind: 'http' | 'serial';
  host?: string;                                   // http
  token?: string;                                  // http
  usbVendorId?: number; usbProductId?: number;     // serial: to find the granted port again after a reload
}

const KEY = 'ktsn-led.ide.devices';

export function loadDevices(): DeviceSpec[] {
  try {
    const v = JSON.parse(localStorage.getItem(KEY) ?? '[]');
    return Array.isArray(v) ? v.filter((d) => d && typeof d.id === 'string' && (d.kind === 'http' || d.kind === 'serial')) : [];
  } catch { return []; }
}

export function saveDevices(list: DeviceSpec[]) {
  try { localStorage.setItem(KEY, JSON.stringify(list)); } catch { /* private mode: not remembered */ }
}

export function newDeviceId(): string {
  return Math.random().toString(36).slice(2, 10);
}

export function describe(d: DeviceSpec): string {
  if (d.kind === 'http') return d.host ?? '';
  const hex = (n?: number) => (n === undefined ? '?' : n.toString(16).padStart(4, '0'));
  return d.usbVendorId === undefined ? 'serial (no port chosen)' : `serial ${hex(d.usbVendorId)}:${hex(d.usbProductId)}`;
}
