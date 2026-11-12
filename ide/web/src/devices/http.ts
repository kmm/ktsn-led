// The board's HTTP API (firmware main/http_api.c). Needs the board's token; CORS is answered by the board.
import { apiError, ApiError, type BoardConfig, type BoardStatus, type ConfigPatch, type LibList, type PatchResult, type Transport } from './types';

const TIMEOUT_MS = 8000;

/** "ktsn-led.local", "10.0.0.5:8080" or a full URL -> base URL without a trailing slash. */
export function baseUrl(host: string): string {
  const h = host.trim().replace(/\/+$/, '');
  return /^https?:\/\//.test(h) ? h : `http://${h}`;
}

export class HttpTransport implements Transport {
  readonly kind = 'http';
  private base: string;

  constructor(host: string, private token: string, private fetchFn: typeof fetch = (...a) => fetch(...a),
              private xhrFn: () => XMLHttpRequest = () => new XMLHttpRequest()) {
    this.base = baseUrl(host);
  }

  private async request(method: string, path: string, body?: string, type = 'application/json'): Promise<Response> {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), TIMEOUT_MS);
    try {
      return await this.fetchFn(this.base + path, {
        method, body, signal: ctl.signal,
        headers: { Authorization: `Bearer ${this.token}`, ...(body !== undefined ? { 'Content-Type': type } : {}) },
      });
    } catch (e) {
      if (ctl.signal.aborted) throw new ApiError(0, `no answer from ${this.base} within ${TIMEOUT_MS / 1000} s`);
      throw new ApiError(0, `can't reach ${this.base} (${(e as Error).message}); check the address, that the board is on, `
        + 'and that the browser may access your local network');
    } finally {
      clearTimeout(timer);
    }
  }

  private async json<T>(method: string, path: string, body?: unknown): Promise<T> {
    const r = await this.request(method, path, body === undefined ? undefined : JSON.stringify(body));
    const data = await r.json().catch(() => ({}));
    if (!r.ok) throw apiError(r.status, data);
    return data as T;
  }

  status() { return this.json<BoardStatus>('GET', '/api/status'); }
  config() { return this.json<BoardConfig>('GET', '/api/config'); }
  patch(body: ConfigPatch) { return this.json<PatchResult>('PATCH', '/api/config', body); }
  async save() { await this.json('POST', '/api/save'); }
  async reboot() { await this.json('POST', '/api/reboot'); }
  async close() { /* stateless */ }

  libList() { return this.json<LibList>('GET', '/api/lib'); }
  async libGet(name: string) { return (await this.json<{ src: string }>('GET', `/api/lib/${encodeURIComponent(name)}`)).src; }
  async libDel(name: string) { await this.json('DELETE', `/api/lib/${encodeURIComponent(name)}`); }
  async libPut(name: string, src: string) {
    const r = await this.request('PUT', `/api/lib/${encodeURIComponent(name)}`, src, 'text/plain');
    if (!r.ok) throw apiError(r.status, await r.json().catch(() => ({})));
  }

  async prog(ch: number, src: string) {
    const r = await this.request('PUT', `/api/prog/${ch}`, src, 'text/plain');
    if (!r.ok) throw apiError(r.status, await r.json().catch(() => ({})));
  }

  /** Firmware update: POST /api/ota with the image. XMLHttpRequest rather than fetch, for upload progress. The board
   *  checks the image, writes it to its other app slot and restarts after answering; resolves with the new version. */
  ota(image: Uint8Array, onProgress: (sent: number, total: number) => void): Promise<string> {
    return new Promise((resolve, reject) => {
      const x = this.xhrFn();
      x.open('POST', `${this.base}/api/ota`);
      x.setRequestHeader('Authorization', `Bearer ${this.token}`);
      x.setRequestHeader('Content-Type', 'application/octet-stream');
      x.timeout = 180_000;                                   // ~1 MB over WiFi, plus flash erase/verify
      x.upload.onprogress = (e) => onProgress(e.loaded, e.total || image.length);
      x.onload = () => {
        let data: unknown = {};
        try { data = JSON.parse(x.responseText); } catch { /* not JSON */ }
        if (x.status >= 200 && x.status < 300) resolve((data as { version?: string }).version ?? '');
        else reject(apiError(x.status, data));
      };
      x.onerror = () => reject(new ApiError(0, `can't reach ${this.base} (upload failed)`));
      x.ontimeout = () => reject(new ApiError(0, 'the update took too long; the board keeps its current firmware unless it finished'));
      x.send(new Blob([image.slice()]));                    // a copy with a plain ArrayBuffer, as Blob wants
    });
  }

  /** Plain-text output; the command's result code is in X-Ktsn-Result (X-Ledctl-Result before 0.18.0) (non-zero results come back as 400). */
  async cmd(line: string) {
    const r = await this.request('POST', '/api/cmd', line, 'text/plain');
    const rc = (r.headers.get('X-Ktsn-Result') ?? r.headers.get('X-Ledctl-Result'));
    if (rc === null) throw apiError(r.status, await r.json().catch(() => ({})));
    return { result: Number(rc), output: await r.text() };
  }
}
