import { useCallback, useEffect, useRef, useState } from 'react';
import { HttpTransport } from './http';
import { openSerialLink, SerialTransport } from './serial';
import type { DeviceSpec } from './store';
import { ApiError, type BoardConfig, type Transport } from './types';

export type ConnStatus = 'idle' | 'connecting' | 'connected' | 'error';

/** Serial ports chosen in this page session (SerialPort objects can't be stored). */
export const chosenPorts = new Map<string, SerialPort>();

/** Find the port for a serial device: chosen this session, or a previously granted one with the same USB ids. */
async function findPort(d: DeviceSpec): Promise<SerialPort> {
  if (!navigator.serial) throw new Error('this browser has no Web Serial (Brave may have it disabled; Chrome has it)');
  const chosen = chosenPorts.get(d.id);
  const granted = await navigator.serial.getPorts();
  if (chosen && granted.includes(chosen)) return chosen;
  const same = granted.filter((p) => { const i = p.getInfo(); return i.usbVendorId === d.usbVendorId && i.usbProductId === d.usbProductId; });
  if (same.length === 1) { chosenPorts.set(d.id, same[0]); return same[0]; }
  throw new Error(same.length ? 'several matching serial ports: choose the port again in Devices' : 'serial port not available: plug the board in, or choose the port again in Devices');
}

export interface Connection {
  status: ConnStatus;
  message: string;
  transport: Transport | null;
  config: BoardConfig | null;
  logs: string[];
  connect(): Promise<void>;
  disconnect(): Promise<void>;
  refreshConfig(): Promise<BoardConfig | null>;
  reboot(): Promise<void>;
  /** The board is restarting on its own (after a firmware update): drop the link, then reconnect, retrying a few times. */
  reconnectAfterRestart(why: string, firstDelayMs?: number): Promise<void>;
  addLog(line: string): void;
  /** Report the result of a periodic status poll: after LOST_AFTER network failures in a row the board counts as
   *  gone (powered off, out of WiFi) and the connection is dropped instead of silently retrying forever. */
  pollResult(err: unknown | null): void;
}

const LOST_AFTER = 3;

const MAX_LOGS = 300;

export function useConnection(device: DeviceSpec | null): Connection {
  const [status, setStatus] = useState<ConnStatus>('idle');
  const [message, setMessage] = useState('');
  const [config, setConfig] = useState<BoardConfig | null>(null);
  const [logs, setLogs] = useState<string[]>([]);
  const tRef = useRef<Transport | null>(null);
  const [transport, setTransport] = useState<Transport | null>(null);
  const deviceRef = useRef(device);
  deviceRef.current = device;
  const failures = useRef(0);

  const addLog = useCallback((line: string) => setLogs((l) => (l.length >= MAX_LOGS ? [...l.slice(-MAX_LOGS + 1), line] : [...l, line])), []);

  const drop = useCallback(async (why: string, st: ConnStatus = 'idle') => {
    const t = tRef.current;
    tRef.current = null;
    setTransport(null);
    setConfig(null);
    setStatus(st);
    setMessage(why);
    if (t) await t.close().catch(() => {});
  }, []);

  const refreshConfig = useCallback(async () => {
    const t = tRef.current;
    if (!t) return null;
    const c = await t.config();
    setConfig(c);
    return c;
  }, []);

  const connect = useCallback(async () => {
    const d = deviceRef.current;
    if (!d) return;
    await drop('', 'connecting');
    setStatus('connecting');
    setMessage(`connecting to ${d.name}…`);
    try {
      let t: Transport;
      if (d.kind === 'http') {
        if (!d.host || !d.token) throw new Error('set the address and token in Devices');
        t = new HttpTransport(d.host, d.token);
      } else {
        const st = await SerialTransport.connect(await openSerialLink(await findPort(d)), addLog);
        st.done.then(() => { if (tRef.current === st) drop('serial connection closed (unplugged or board reset)', 'error'); });
        t = st;
      }
      const c = await t.config();                    // proves the connection (and the token)
      if (deviceRef.current?.id !== d.id) { await t.close(); return; }
      tRef.current = t;
      failures.current = 0;
      setTransport(t);
      setConfig(c);
      setStatus('connected');
      setMessage(`connected to ${d.name} (${d.kind === 'http' ? d.host : 'serial'})`);
    } catch (e) {
      setStatus('error');
      setMessage((e as Error).message);
    }
  }, [drop, addLog]);

  const disconnect = useCallback(() => drop('disconnected'), [drop]);

  /** Reboot, then reconnect once the board is back (serial needs a new handshake; USB may re-enumerate). */
  const reboot = useCallback(async () => {
    const t = tRef.current;
    if (!t) return;
    await t.reboot();
    await drop('rebooting…', 'connecting');
    setStatus('connecting');
    await new Promise((r) => setTimeout(r, 4000));
    await connect();
  }, [drop, connect]);

  const reconnectAfterRestart = useCallback(async (why: string, firstDelayMs = 8000) => {
    await drop(why, 'connecting');
    setStatus('connecting');
    await new Promise((r) => setTimeout(r, firstDelayMs));
    for (let i = 0; i < 4; i++) {
      await connect();
      if (tRef.current) return;
      setStatus('connecting');
      setMessage(`${why} (waiting for the board, try ${i + 2} of 5)`);
      await new Promise((r) => setTimeout(r, 4000));
    }
    await connect();
  }, [drop, connect]);

  const pollResult = useCallback((err: unknown | null) => {
    if (!err) { failures.current = 0; return; }
    if (!(err instanceof ApiError) || err.status !== 0) return;     // the board answered (e.g. 401): not a lost link
    if (++failures.current >= LOST_AFTER && tRef.current) {
      void drop(`lost connection to ${deviceRef.current?.name ?? 'the board'}: ${err.message}`, 'error');
    }
  }, [drop]);

  // Switching devices closes the old connection.
  useEffect(() => { void drop(''); setLogs([]); }, [device?.id, drop]);
  useEffect(() => () => { void tRef.current?.close(); }, []);

  return { status, message, transport, config, logs, connect, disconnect, refreshConfig, reboot, reconnectAfterRestart, addLog, pollResult };
}
