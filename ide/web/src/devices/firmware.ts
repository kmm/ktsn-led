// Firmware files: what an ESP-IDF app image says about itself, and a build folder's flash plan (flasher_args.json,
// written by idf.py build). Used by the network update (one .bin) and USB flashing (the whole build folder).

/** esp_image_header_t chip_id values (ESP-IDF esp_app_format.h) -> target names as in flasher_args.json. */
const CHIP_IDS: Record<number, string> = {
  0: 'esp32', 2: 'esp32s2', 5: 'esp32c3', 9: 'esp32s3', 12: 'esp32c2', 13: 'esp32c6', 16: 'esp32h2', 18: 'esp32p4',
  20: 'esp32c61', 23: 'esp32c5',
};

/** App image project names a board accepts: the current one and the name before the rename. */
export const PROJECT_NAMES = ['ktsn-led', 'ledctl'];

export interface AppImage {
  target: string;            // e.g. "esp32c6" (or "chip 42" if unknown)
  project: string;           // "ktsn-led" ("ledctl" before 0.18.0)
  version: string;
  date: string; time: string;
  idf: string;
}

/** Read the image header and app description (esp_app_desc_t right after the first segment header). Throws if the
 *  file isn't an ESP-IDF app image. */
export function parseAppImage(b: Uint8Array): AppImage {
  const IMG_HDR = 24, SEG_HDR = 8, DESC = IMG_HDR + SEG_HDR;
  if (b.length < DESC + 256 || b[0] !== 0xe9) throw new Error('not an ESP-IDF app image (pick build/ktsn-led.bin)');
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  if (dv.getUint32(DESC, true) !== 0xabcd5432) throw new Error('not an ESP-IDF app image (no app description)');
  const str = (off: number, len: number) => {
    const s = b.subarray(DESC + off, DESC + off + len), z = s.indexOf(0);
    return new TextDecoder().decode(z < 0 ? s : s.subarray(0, z));
  };
  const chip = dv.getUint16(12, true);
  return {
    target: CHIP_IDS[chip] ?? `chip ${chip}`,
    version: str(16, 32), project: str(48, 32), time: str(80, 16), date: str(96, 16), idf: str(112, 32),
  };
}

/** Problems that should stop an update of a board running `board` (from /api/status firmware) with `img`. */
export function updateProblems(img: AppImage, board: { target: string }): string[] {
  const p: string[] = [];
  if (!PROJECT_NAMES.includes(img.project)) p.push(`this is a "${img.project}" image, not a KTSN LED build`);
  if (img.target !== board.target) p.push(`built for ${img.target}, but the board is an ${board.target}`);
  return p;
}

export interface FlashFile { address: number; path: string; file: File }
export interface FlashPlan {
  chip: string;              // "esp32c6"
  flashSize: string;         // "4MB"
  files: FlashFile[];
  app: FlashFile | null;     // the app image (to show its version)
}

/** Build a flash plan from a picked build folder (input webkitdirectory): flasher_args.json at its root names the
 *  files and offsets, exactly what `idf.py flash` writes. Throws with a readable reason. */
export async function planFromFolder(files: File[]): Promise<FlashPlan> {
  const byPath = new Map<string, File>();
  for (const f of files) byPath.set(f.webkitRelativePath.split('/').slice(1).join('/'), f);   // drop the folder name
  const argsFile = byPath.get('flasher_args.json');
  if (!argsFile) throw new Error('no flasher_args.json in that folder: pick a build folder (e.g. ktsn-led/build-c6p) after idf.py build');
  let args: { flash_files?: Record<string, string>; flash_settings?: { flash_size?: string }; extra_esptool_args?: { chip?: string }; app?: { file?: string } };
  try { args = JSON.parse(await argsFile.text()); } catch { throw new Error('flasher_args.json is not valid JSON'); }
  const chip = args.extra_esptool_args?.chip;
  if (!chip || !args.flash_files) throw new Error('flasher_args.json has no chip or flash files');
  const plan: FlashPlan = { chip, flashSize: args.flash_settings?.flash_size ?? '?', files: [], app: null };
  const missing: string[] = [];
  for (const [addr, path] of Object.entries(args.flash_files)) {
    const file = byPath.get(path);
    if (!file) { missing.push(path); continue; }
    const f = { address: parseInt(addr, 16), path, file };
    plan.files.push(f);
    if (path === args.app?.file) plan.app = f;
  }
  if (missing.length) throw new Error(`missing from the folder: ${missing.join(', ')} (build it first)`);
  plan.files.sort((a, b) => a.address - b.address);
  return plan;
}

/** "ESP32-C6" (esptool-js chip name) -> "esp32c6" (flasher_args.json). */
export const chipKey = (name: string) => name.toLowerCase().replace(/[^a-z0-9]/g, '');
