// Minimal Web Serial typings (Chromium only; not in TypeScript's DOM lib).
interface SerialPortInfo { usbVendorId?: number; usbProductId?: number }
interface SerialOptions { baudRate: number; bufferSize?: number }
interface SerialPort extends EventTarget {
  readonly readable: ReadableStream<Uint8Array> | null;
  readonly writable: WritableStream<Uint8Array> | null;
  open(options: SerialOptions): Promise<void>;
  close(): Promise<void>;
  getInfo(): SerialPortInfo;
}
interface Serial extends EventTarget {
  requestPort(options?: { filters?: SerialPortInfo[] }): Promise<SerialPort>;
  getPorts(): Promise<SerialPort[]>;
}
interface Navigator { readonly serial?: Serial }
