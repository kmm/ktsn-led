import { describe, expect, it } from 'vitest';
import { chipKey, parseAppImage, planFromFolder, updateProblems } from './firmware';
import { HttpTransport } from './http';

/** A minimal ESP-IDF app image head: image header, one segment header, app description. */
function fakeImage(chipId: number, project = 'ktsn-led', version = '0.9.0'): Uint8Array {
  const b = new Uint8Array(1024), dv = new DataView(b.buffer);
  b[0] = 0xe9;
  dv.setUint16(12, chipId, true);
  dv.setUint32(32, 0xabcd5432, true);
  const put = (off: number, s: string) => b.set(new TextEncoder().encode(s), 32 + off);
  put(16, version); put(48, project); put(80, '12:34:56'); put(96, 'Oct 17 2026'); put(112, 'v6.1');
  return b;
}

/** A File as an <input webkitdirectory> gives it: with its path inside the picked folder. */
function folderFile(path: string, content: string | Uint8Array): File {
  const f = new File([typeof content === 'string' ? content : content.slice()], path.split('/').pop()!);
  Object.defineProperty(f, 'webkitRelativePath', { value: path });
  return f;
}

describe('firmware images', () => {
  it('reads the app description and chip', () => {
    expect(parseAppImage(fakeImage(13))).toEqual({ target: 'esp32c6', project: 'ktsn-led', version: '0.9.0', date: 'Oct 17 2026', time: '12:34:56', idf: 'v6.1' });
    expect(parseAppImage(fakeImage(0)).target).toBe('esp32');
    expect(() => parseAppImage(new Uint8Array(1024))).toThrow(/not an ESP-IDF app image/);
    const noDesc = fakeImage(0); noDesc[32] = 0;
    expect(() => parseAppImage(noDesc)).toThrow(/no app description/);
  });
  it('refuses images for another chip or project', () => {
    expect(updateProblems(parseAppImage(fakeImage(13)), { target: 'esp32c6' })).toEqual([]);
    expect(updateProblems(parseAppImage(fakeImage(0)), { target: 'esp32c6' })).toEqual([expect.stringContaining('built for esp32')]);
    expect(updateProblems(parseAppImage(fakeImage(13, 'ledctl')), { target: 'esp32c6' })).toEqual([]);   // the name before 0.18.0
    expect(updateProblems(parseAppImage(fakeImage(13, 'blink')), { target: 'esp32c6' })).toEqual([expect.stringContaining('"blink"')]);
    expect(chipKey('ESP32-C6')).toBe('esp32c6');
  });
});

describe('build folders', () => {
  const args = JSON.stringify({
    flash_settings: { flash_size: '4MB' },
    flash_files: { '0x20000': 'ktsn-led.bin', '0x0': 'bootloader/bootloader.bin', '0x8000': 'partition_table/partition-table.bin', '0xf000': 'ota_data_initial.bin' },
    app: { file: 'ktsn-led.bin' },
    extra_esptool_args: { chip: 'esp32c6' },
  });
  const folder = (skip?: string) => [
    folderFile('build-c6p/flasher_args.json', args), folderFile('build-c6p/ktsn-led.bin', fakeImage(13)),
    folderFile('build-c6p/bootloader/bootloader.bin', 'b'), folderFile('build-c6p/partition_table/partition-table.bin', 'p'),
    folderFile('build-c6p/ota_data_initial.bin', 'o'), folderFile('build-c6p/esp-idf/whatever.o', 'x'),
  ].filter((f) => !f.webkitRelativePath.endsWith(skip ?? '\0'));

  it('plans what idf.py flash would write, in address order', async () => {
    const p = await planFromFolder(folder());
    expect(p.chip).toBe('esp32c6');
    expect(p.flashSize).toBe('4MB');
    expect(p.files.map((f) => [f.address, f.path])).toEqual([[0, 'bootloader/bootloader.bin'], [0x8000, 'partition_table/partition-table.bin'],
      [0xf000, 'ota_data_initial.bin'], [0x20000, 'ktsn-led.bin']]);
    expect(p.app?.path).toBe('ktsn-led.bin');
  });
  it('explains what is wrong with a folder', async () => {
    await expect(planFromFolder(folder('ota_data_initial.bin'))).rejects.toThrow(/missing from the folder: ota_data_initial.bin/);
    await expect(planFromFolder([folderFile('src/main.c', 'x')])).rejects.toThrow(/no flasher_args.json/);
    await expect(planFromFolder([folderFile('b/flasher_args.json', '{')])).rejects.toThrow(/not valid JSON/);
  });
});

describe('network update', () => {
  /** Just enough of XMLHttpRequest for HttpTransport.ota. */
  class FakeXhr {
    static last: FakeXhr;
    headers: Record<string, string> = {};
    method = ''; url = ''; timeout = 0; status = 0; responseText = '';
    body: Blob | null = null;
    upload: { onprogress?: (e: { loaded: number; total: number }) => void } = {};
    onload?: () => void; onerror?: () => void; ontimeout?: () => void;
    constructor(private reply: (x: FakeXhr) => void) { FakeXhr.last = this; }
    open(m: string, u: string) { this.method = m; this.url = u; }
    setRequestHeader(k: string, v: string) { this.headers[k] = v; }
    send(b: Blob) { this.body = b; this.upload.onprogress?.({ loaded: b.size / 2, total: b.size }); this.reply(this); }
  }
  const transport = (reply: (x: FakeXhr) => void) =>
    new HttpTransport('10.0.0.5', 'tok', undefined, () => new FakeXhr(reply) as unknown as XMLHttpRequest);

  it('uploads the image with the token and reports progress', async () => {
    const seen: number[] = [];
    const v = await transport((x) => { x.status = 200; x.responseText = '{"ok":true,"version":"0.9.1"}'; x.onload?.(); })
      .ota(fakeImage(13), (s, t) => seen.push(s / t));
    expect(v).toBe('0.9.1');
    expect(FakeXhr.last).toMatchObject({ method: 'POST', url: 'http://10.0.0.5/api/ota' });
    expect(FakeXhr.last.headers).toMatchObject({ Authorization: 'Bearer tok', 'Content-Type': 'application/octet-stream' });
    expect(FakeXhr.last.body?.size).toBe(1024);
    expect(seen).toEqual([0.5]);
  });
  it('turns a refusal into an ApiError with the board\'s reason', async () => {
    await expect(transport((x) => { x.status = 400; x.responseText = '{"error":"the image is for a different chip"}'; x.onload?.(); })
      .ota(fakeImage(0), () => {})).rejects.toMatchObject({ status: 400, message: 'the image is for a different chip' });
    await expect(transport((x) => x.onerror?.()).ota(fakeImage(0), () => {})).rejects.toMatchObject({ status: 0 });
  });
});
