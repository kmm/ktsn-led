import { useState } from 'react';
import { chosenPorts } from '../devices/useConnection';
import { describe, type DeviceSpec, newDeviceId } from '../devices/store';

interface Props {
  devices: DeviceSpec[];
  onChange(list: DeviceSpec[]): void;
  onClose(): void;
  /** Flash firmware over USB (a new board, or one that doesn't start). */
  onFlash(): void;
}

const blank = (): DeviceSpec => ({ id: newDeviceId(), name: '', kind: 'http', host: '', token: '' });

export function DeviceManager({ devices, onChange, onClose, onFlash }: Props) {
  const [edit, setEdit] = useState<DeviceSpec | null>(devices.length ? null : blank());
  const [showToken, setShowToken] = useState(false);
  const [err, setErr] = useState('');
  const hasSerial = !!navigator.serial;

  const choosePort = async () => {
    setErr('');
    try {
      const port = await navigator.serial!.requestPort();
      const info = port.getInfo();
      chosenPorts.set(edit!.id, port);
      setEdit({ ...edit!, usbVendorId: info.usbVendorId, usbProductId: info.usbProductId });
    } catch (e) {
      if ((e as Error).name !== 'NotFoundError') setErr((e as Error).message);   // NotFoundError = chooser cancelled
    }
  };

  const save = () => {
    const d = edit!;
    if (!d.name.trim()) return setErr('give it a name');
    if (d.kind === 'http' && (!d.host?.trim() || !d.token)) return setErr('address and token are needed (set the token on the board: token <secret>)');
    if (d.kind === 'serial' && d.usbVendorId === undefined) return setErr('choose the serial port');
    const clean: DeviceSpec = d.kind === 'http'
      ? { id: d.id, name: d.name.trim(), kind: 'http', host: d.host!.trim(), token: d.token }
      : { id: d.id, name: d.name.trim(), kind: 'serial', usbVendorId: d.usbVendorId, usbProductId: d.usbProductId };
    onChange(devices.some((x) => x.id === d.id) ? devices.map((x) => (x.id === d.id ? clean : x)) : [...devices, clean]);
    setEdit(null);
    setErr('');
  };

  return (
    <div className="modal-backdrop" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className="modal" role="dialog" aria-label="Devices">
        <div className="modal-head"><h2>Devices</h2><button onClick={onClose} aria-label="Close">✕</button></div>
        {!edit && (
          <>
            {devices.length === 0 && <p className="muted">No devices yet.</p>}
            <ul className="device-list">
              {devices.map((d) => (
                <li key={d.id}>
                  <span><b>{d.name}</b> <span className="muted">{describe(d)}</span></span>
                  <span>
                    <button onClick={() => { setEdit(d); setErr(''); }}>Edit</button>{' '}
                    <button onClick={() => confirm(`Remove ${d.name}?`) && onChange(devices.filter((x) => x.id !== d.id))}>Remove</button>
                  </span>
                </li>
              ))}
            </ul>
            <button className="primary" onClick={() => setEdit(blank())}>+ Add device</button>
          </>
        )}
        {edit && (
          <div className="form">
            <label>Name <input value={edit.name} autoFocus placeholder="Kitchen strip" onChange={(e) => setEdit({ ...edit, name: e.target.value })} /></label>
            <div className="radio">
              <label><input type="radio" checked={edit.kind === 'http'} onChange={() => setEdit({ ...edit, kind: 'http' })} /> Network (HTTP)</label>
              <label><input type="radio" checked={edit.kind === 'serial'} onChange={() => setEdit({ ...edit, kind: 'serial' })} /> USB serial</label>
            </div>
            {edit.kind === 'http' ? (
              <>
                <label>Address <input value={edit.host ?? ''} placeholder="ktsn-led.local or 10.0.0.42" onChange={(e) => setEdit({ ...edit, host: e.target.value })} /></label>
                <label>Token
                  <span className="row">
                    <input type={showToken ? 'text' : 'password'} value={edit.token ?? ''} autoComplete="off"
                      onChange={(e) => setEdit({ ...edit, token: e.target.value })} />
                    <button onClick={() => setShowToken(!showToken)}>{showToken ? 'Hide' : 'Show'}</button>
                  </span>
                </label>
                <p className="muted small">The token is set on the board's serial console with <code>token &lt;secret&gt;</code>.
                  It is remembered in this browser. The first request may make the browser ask to access your local network.</p>
              </>
            ) : hasSerial ? (
              <>
                <p>Port: <b>{edit.usbVendorId === undefined ? 'none chosen' : describe(edit)}</b>{' '}
                  <button onClick={choosePort}>Choose port…</button></p>
                <p className="muted small">The connection doesn't reset the board, and needs no token (serial is physical access).</p>
              </>
            ) : (
              <p className="bad">This browser has no Web Serial. Brave can turn it off; Chrome or Edge have it.</p>
            )}
            {err && <p className="bad">{err}</p>}
            <div className="row">
              <button className="primary" onClick={save}>Save</button>
              <button onClick={() => { setEdit(null); setErr(''); }}>Cancel</button>
            </div>
          </div>
        )}
        <p className="flash-hint muted">New board, or one without KTSN LED? <button onClick={onFlash}
          title="Write firmware to a board over USB with esptool; no connection to the board needed">Flash firmware over USB…</button></p>
      </div>
    </div>
  );
}
