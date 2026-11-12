import { ESPLoader, Transport } from 'esptool-js';
import { useRef, useState } from 'react';
import { chipKey, parseAppImage, planFromFolder, type AppImage, type FlashPlan } from '../devices/firmware';

interface Props {
  onClose(): void;
  /** The port of the serial device in use, if known: flash it without the browser's port chooser. */
  knownPort?: SerialPort | null;
  /** Release the IDE's own connection to the board first (the port can only be open once). */
  beforeFlash(): Promise<void>;
  /** Flashing finished and the board was reset. */
  afterFlash?(): void;
}

/** Restart into the new firmware: pulse RTS (EN) with DTR (IO0/boot) released, like Python esptool's hard reset.
 *  esptool-js 0.7.0's after('hard_reset') only drops RTS, so when RTS is already low after flashing nothing happens
 *  and the board needed a power cycle (seen on the XIAO C6). Works for UART auto-reset circuits and USB-Serial/JTAG. */
async function hardReset(t: Transport) {
  const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
  await t.setDTR(false);
  await t.setRTS(true);
  await sleep(200);
  await t.setRTS(false);
  await sleep(100);
}

const fmtSize = (n: number) => (n >= 1024 * 1024 ? `${(n / 1024 / 1024).toFixed(2)} MB` : `${Math.ceil(n / 1024)} KB`);

/** USB flashing with Espressif's esptool-js over Web Serial: writes a build folder's files (flasher_args.json) like
 *  `idf.py flash`. Saved settings (NVS) are kept unless "erase everything" is ticked. */
export function FlashDialog({ onClose, knownPort, beforeFlash, afterFlash }: Props) {
  const [plan, setPlan] = useState<FlashPlan | null>(null);
  const [app, setApp] = useState<AppImage | null>(null);
  const [err, setErr] = useState('');
  const [eraseAll, setEraseAll] = useState(false);
  const [busy, setBusy] = useState(false);
  const [done, setDone] = useState(false);
  const [progress, setProgress] = useState(0);
  const [log, setLog] = useState('');
  const logRef = useRef<HTMLPreElement>(null);

  const append = (s: string) => {
    setLog((l) => (l + s).slice(-20000));
    requestAnimationFrame(() => logRef.current?.scrollTo(0, logRef.current.scrollHeight));
  };

  const pick = async (files: FileList | null) => {
    setErr(''); setPlan(null); setApp(null); setDone(false);
    if (!files?.length) return;
    try {
      const p = await planFromFolder([...files]);
      setPlan(p);
      if (p.app) setApp(parseAppImage(new Uint8Array(await p.app.file.arrayBuffer())));
    } catch (e) { setErr((e as Error).message); }
  };

  const flash = async () => {
    if (!plan) return;
    if (!navigator.serial) { setErr('this browser has no Web Serial (Chrome has it; Brave may have it switched off)'); return; }
    setErr(''); setLog(''); setProgress(0); setDone(false);
    let port: SerialPort;
    try { port = knownPort ?? await navigator.serial.requestPort(); }   // the chooser needs the click: ask first
    catch { return; }                                                     // chooser cancelled
    setBusy(true);
    let transport: Transport | null = null;
    try {
      await beforeFlash();
      transport = new Transport(port, false);
      const loader = new ESPLoader({
        transport, baudrate: 460800, romBaudrate: 115200,
        terminal: { clean: () => setLog(''), writeLine: (s) => append(`${s}\n`), write: append },
      });
      append('Connecting (the board is reset into its bootloader)…\n');
      await loader.main();
      const chip = chipKey(loader.chip.CHIP_NAME);
      if (chip !== plan.chip) throw new Error(`the board is an ${loader.chip.CHIP_NAME}, but this build is for ${plan.chip}: nothing was written`);
      const fileArray = await Promise.all(plan.files.map(async (f) => ({ address: f.address, data: new Uint8Array(await f.file.arrayBuffer()) })));
      const total = fileArray.reduce((n, f) => n + f.data.length, 0);
      const before = fileArray.map((_, i) => fileArray.slice(0, i).reduce((n, f) => n + f.data.length, 0));
      await loader.writeFlash({
        fileArray, flashMode: 'keep', flashFreq: 'keep', flashSize: 'keep', eraseAll, compress: true,
        reportProgress: (i, written, size) => setProgress((before[i] + (written / size) * fileArray[i].data.length) / total),
      });
      append('Resetting the board…\n');
      await hardReset(transport);
      setProgress(1);
      setDone(true);
      afterFlash?.();
    } catch (e) {
      setErr((e as Error).message || String(e));
    } finally {
      await transport?.disconnect().catch(() => {});
      setBusy(false);
    }
  };

  return (
    <div className="modal-backdrop" onMouseDown={(e) => e.target === e.currentTarget && !busy && onClose()}>
      <div className="modal flash" role="dialog" aria-label="Flash firmware over USB">
        <div className="modal-head"><h2>Flash firmware over USB</h2><button onClick={onClose} disabled={busy} aria-label="Close">✕</button></div>
        <p className="muted">Pick the build folder (e.g. <code>ktsn-led/build</code>, <code>build-c6p</code>) after <code>idf.py build</code>.
          Everything <code>idf.py flash</code> would write is written; saved settings are kept.</p>
        <input type="file" {...{ webkitdirectory: '', directory: '' }} multiple disabled={busy} onChange={(e) => pick(e.target.files)} />
        {plan && (
          <div className="plan">
            <div><b>{plan.chip}</b>, {plan.flashSize} flash{app && <> · KTSN LED <b>{app.version}</b> built {app.date} {app.time}</>}</div>
            <table className="channels">
              <tbody>{plan.files.map((f) => <tr key={f.address}><td><code>0x{f.address.toString(16)}</code></td><td>{f.path}</td><td>{fmtSize(f.file.size)}</td></tr>)}</tbody>
            </table>
            <label className="check"><input type="checkbox" checked={eraseAll} disabled={busy} onChange={(e) => setEraseAll(e.target.checked)} />
              erase everything first (also clears saved settings, programs and WiFi)</label>
          </div>
        )}
        {err && <p className="bad">{err}</p>}
        {(busy || done) && <progress max={1} value={progress} />}
        {done && <p className="good">Flashed and reset. {afterFlash ? 'Reconnecting…' : 'Connect to it again when it is up.'}</p>}
        {log && <pre className="log" ref={logRef}>{log}</pre>}
        <div className="controls">
          <button className="primary" disabled={!plan || busy} onClick={flash}>{busy ? 'Flashing…' : knownPort ? 'Flash' : 'Choose port and flash'}</button>
          <button disabled={busy} onClick={onClose}>{done ? 'Done' : 'Cancel'}</button>
        </div>
      </div>
    </div>
  );
}
