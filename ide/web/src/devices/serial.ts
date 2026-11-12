// The board's serial JSON API (firmware main/serial_api.c) over Web Serial.
// Protocol: send `api` on the console; then one JSON request per line, and each response is a line starting with
// 0x1E. Every other line (logs, prompt, echo) is passed to `onLog`.
// Reset safety: ESP32 dev boards and the C6's USB-Serial/JTAG reset the chip while RTS is asserted and DTR is not.
// Opening the port asserts both together (harmless), so the port is opened and its control lines are never touched.
import { apiError, ApiError, type BoardConfig, type BoardStatus, type ConfigPatch, type PatchResult, type Transport, type LibList } from './types';

const RS = '\x1e';
const ANSI = /\x1b\[[0-9;?]*[A-Za-z]/g;

export interface Frame { id: number | null; status: number; body: unknown }

/** Splits decoded serial text into lines: 0x1E lines become frames, the rest log lines (terminal escapes removed). */
export class LineFramer {
  private buf = '';
  constructor(private onFrame: (f: Frame) => void, private onLine: (line: string) => void) {}
  push(text: string) {
    this.buf += text;
    for (let i = this.buf.indexOf('\n'); i >= 0; i = this.buf.indexOf('\n')) {
      const line = this.buf.slice(0, i).replace(/\r$/, '');
      this.buf = this.buf.slice(i + 1);
      const rs = line.indexOf(RS);
      if (rs >= 0) {                               // a frame (the echo/prompt may precede it on the same line)
        try { this.onFrame(JSON.parse(line.slice(rs + 1)) as Frame); continue; }
        catch { /* not a frame after all: show it */ }
      }
      const text2 = line.replace(ANSI, '').trimEnd();
      if (text2) this.onLine(text2);
    }
  }
}

/** Byte stream to and from the board: a Web Serial port, or a fake one in tests. */
export interface Link {
  write(text: string): Promise<void>;
  /** Starts delivering received text; resolves when the link closes. */
  run(onText: (t: string) => void): Promise<void>;
  close(): Promise<void>;
}

/** Open a Web Serial port without touching DTR/RTS (see top of file). */
export async function openSerialLink(port: SerialPort): Promise<Link> {
  await port.open({ baudRate: 115200, bufferSize: 65536 });
  const enc = new TextEncoder();
  let reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  let closing = false;
  return {
    async write(text) {
      const w = port.writable!.getWriter();
      try { await w.write(enc.encode(text)); } finally { w.releaseLock(); }
    },
    async run(onText) {
      const dec = new TextDecoder();
      while (!closing && port.readable) {
        reader = port.readable.getReader();
        try {
          for (;;) {
            const { value, done } = await reader.read();
            if (done) break;
            onText(dec.decode(value, { stream: true }));
          }
        } catch { if (!closing) break; } finally { reader.releaseLock(); reader = null; }
      }
    },
    async close() {
      closing = true;
      try { await reader?.cancel(); } catch { /* already gone */ }
      try { await port.close(); } catch { /* unplugged */ }
    },
  };
}

export class SerialTransport implements Transport {
  readonly kind = 'serial';
  private nextId = 1;
  private pending = new Map<number, { resolve(f: Frame): void; reject(e: Error): void; timer: ReturnType<typeof setTimeout> }>();
  private ready: ((f: Frame) => void) | null = null;
  private queue: Promise<unknown> = Promise.resolve();
  private closed = false;
  /** Resolves when the link closes (unplugged, board reset over USB, or close()). */
  readonly done: Promise<void>;

  private constructor(private link: Link, private onLog: (line: string) => void, private timeoutMs: number) {
    const framer = new LineFramer((f) => this.onFrame(f), (l) => this.onLog(l));
    this.done = link.run((t) => framer.push(t)).finally(() => this.fail(new ApiError(0, 'serial connection closed')));
  }

  /** Enter the board's api mode on an open link. */
  static async connect(link: Link, onLog: (line: string) => void, timeoutMs = 10000): Promise<SerialTransport> {
    const t = new SerialTransport(link, onLog, timeoutMs);
    try { await t.handshake(); } catch (e) { await t.close(); throw e; }
    return t;
  }

  private async handshake() {
    const ready = new Promise<Frame>((resolve, reject) => {
      this.ready = resolve;
      setTimeout(() => reject(new ApiError(0, 'the board did not answer `api` (is this a KTSN LED board with the serial API?)')), 5000);
    });
    // A previous session may have left the console in api mode: `exit` leaves it (and is harmless at the prompt).
    await this.link.write('\nexit\n');
    await new Promise((r) => setTimeout(r, 300));
    await this.link.write('api\n');
    await ready;
  }

  private onFrame(f: Frame) {
    if (f.id === null && this.ready && (f.body as { ready?: boolean })?.ready) { this.ready(f); this.ready = null; return; }
    if (f.id === null) return;
    const p = this.pending.get(f.id);
    if (p) { clearTimeout(p.timer); this.pending.delete(f.id); p.resolve(f); }
  }

  private fail(e: Error) {
    this.closed = true;
    for (const p of this.pending.values()) { clearTimeout(p.timer); p.reject(e); }
    this.pending.clear();
  }

  /** One request at a time: the board handles them in order anyway, and it keeps the line simple. */
  private frame(op: string, params: Record<string, unknown> = {}): Promise<Frame> {
    const run = async (): Promise<Frame> => {
      if (this.closed) throw new ApiError(0, 'serial connection closed');
      const id = this.nextId++;
      const answer = new Promise<Frame>((resolve, reject) => {
        const timer = setTimeout(() => { this.pending.delete(id); reject(new ApiError(0, `no answer to "${op}" within ${this.timeoutMs / 1000} s`)); }, this.timeoutMs);
        this.pending.set(id, { resolve, reject, timer });
      });
      await this.link.write(JSON.stringify({ id, op, ...params }) + '\n');
      return answer;
    };
    const p = this.queue.then(run, run);
    this.queue = p.catch(() => undefined);
    return p;
  }

  private async request<T>(op: string, params: Record<string, unknown> = {}): Promise<T> {
    const f = await this.frame(op, params);
    if (f.status >= 300) throw apiError(f.status, f.body);
    return f.body as T;
  }

  status() { return this.request<BoardStatus>('status'); }
  config() { return this.request<BoardConfig>('config'); }
  patch(body: ConfigPatch) { return this.request<PatchResult>('patch', { body }); }
  async prog(ch: number, src: string) { await this.request('prog', { ch, src }); }
  async save() { await this.request('save'); }
  libList() { return this.request<LibList>('lib'); }
  async libGet(name: string) { return (await this.request<{ src: string }>('lib_get', { name })).src; }
  async libPut(name: string, src: string) { await this.request('lib_put', { name, src }); }
  async libDel(name: string) { await this.request('lib_del', { name }); }
  async reboot() { await this.request('reboot'); }

  /** Commands that fail come back as status 400 but still carry their output: return it, like HTTP does. */
  async cmd(line: string) {
    const f = await this.frame('cmd', { line });
    const b = f.body as { result?: number; output?: string };
    if (typeof b?.output === 'string') return { result: b.result ?? (f.status === 200 ? 0 : 1), output: b.output };
    throw apiError(f.status, f.body);
  }

  async close() {
    if (!this.closed) {
      try { await Promise.race([this.frame('exit'), new Promise((r) => setTimeout(r, 500))]); } catch { /* gone */ }
    }
    this.fail(new ApiError(0, 'serial connection closed'));
    await this.link.close();
  }
}
