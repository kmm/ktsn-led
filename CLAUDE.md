# KTSN LED: project context for Claude Code

Named `ledctl` until 0.18.0. The short name `ktsn-led` is used where spaces can't go (CMake project and image name
`ktsn-led.bin`, default hostname, file formats, IDE storage keys `ktsn-led.ide.*`, Docker project), `KTSN_` for Kconfig.
**Deliberately not renamed:** the NVS namespace `ledctl` (settings, programs; renaming would erase every board's
saved state). Kept for compatibility: boards and the IDE accept app images named `ledctl` (OTA, update check), the IDE
imports `ledctl-config` / `ledctl-library` files, copies `ledctl.ide.*` storage to the new keys on load
(`src/storageRename.ts`, imported first), and the HTTP API sends the result code as both `X-Ktsn-Result` and
`X-Ledctl-Result`. Boards on 0.17.0 or older refuse an OTA image with another project name, so moving them to 0.18.0
takes one USB flash (decided with the user).

Addressable-LED controller for ESP32 (ESP-IDF, C). Render programs are written in a small LISP-like DSL,
compiled to a cached linked list, and run once per frame (60 Hz) per channel.

## Targets
- First target: plain ESP32 (WROOM-32 dev board), WS2811 RGB string, white channel ignored for now.
- Must stay compatible with ESP32-C3 / C6 (RISC-V, no FPU). Hence: **no floating point anywhere at run time**.
- Second target being brought up: Seeed XIAO ESP32-C6 (4 MB flash, native USB-Serial/JTAG, single core). Build dir
  `build-c6` with presets `sdkconfig.defaults;sdkconfig.flash4mb;sdkconfig.usbjtag` (see README). **Verified working
  on the XIAO ESP32-C6**, 2026-10-15 (WiFi build, 1.1 MB image). RMT on C3/C6 has no DMA (only S3/P4 do), 48-symbol
  blocks, 2 TX channels; WiFi builds default to 96 symbols = 1 channel, accepted by the user for the C6. Single core,
  so WiFi and LED output share the CPU; the RMT build looked glitch-resistant enough to keep as an option.
  **PARLIO output** (`CONFIG_KTSN_OUTPUT_PARLIO`, preset `sdkconfig.parlio`, build dir `build-c6p`): all strings in one
  DMA transfer, one data line each (lane = channel number, width 1/2/4/8), no refill interrupts. Encoder `ws_parallel.c`
  is portable and host-tested (decodes every lane back): 5 samples per bit at 3.333 MHz (240 MHz/72), 0 = 10000,
  1 = 11100, same high times as led_strip's WS2812 RMT timing. Frame limit 65535 bytes = 15 B x px x width.
  C6 PARLIO can't end a transfer mid 32-bit word (no TRANS_BIT_ALIGN): frames are padded to whole words with
  `ws_par_frame_bytes` (zero samples = extra low time). Unpadded 750-byte frames (1 lane x 50 px) strobed red/green.
  C6 + PARLIO defaults to two channels on GPIO 22/23 (D4/D5). Only one SPI host (GPSPI2) is free on the C6, which is
  why the SPI backend can't do two strings. **Verified working on the XIAO ESP32-C6**, 2026-10-15: one string
  (width 1, after the 32-bit padding fix), then two strings on GPIO 22/23 (width 2, channel 1 confirmed good).
  Default LED GPIO is per target (`DEFAULT_LED_GPIO`): 25 on ESP32, 22 on C6 (XIAO D4, the user's carrier board),
  0 elsewhere. C6 GPIO 24..30 are flash pins.
  Boot validates every channel GPIO with `settings_check_gpio` before touching it.
- Networking: WiFi station + SNTP, token-protected TCP CLI (telnet/netcat), HTTP/JSON API, mDNS (`<hostname>.local`).

## Layout
- `components/dsl/`  portable core, no ESP-IDF includes. Built for the ESP and by `host/Makefile`.
  - `arena.c`    bump allocator (per-channel scratch arena is reset before every run)
  - `compile.c`  tokenizer, parser (temp AST), type-checked compile into `node_t` list; constant folding
  - `natives.c`  native function table (`buf`, `slice`, fills, `scale`, `copy`, blends, `set`/`get`, `rotate`, logic, arithmetic, `env`) + `dsl_resolve_buf`
  - `channel.c`  channel runtime, evaluator, atomic program swap, `stat_record`
- `main/`  ESP-IDF glue: `led_out.c` (RMT via led_strip, or PARLIO; stage with `led_out_write`, send with
  `led_out_flush`), `ws_parallel.c` (portable parallel WS281x encoder, host-tested), `render.c` (timer + render task + perf), `console.c`, `config.h`,
  `settings.c` (NVS persistence, `g_set`), `net.c` (WiFi STA, SNTP), `main.c` (boot order, `autoexec_run`),
  `cmd.c` (transport-neutral command layer), `netcli.c` (TCP CLI), `api.c` (transport-neutral JSON API),
  `http_api.c` (HTTP transport + CORS), `serial_api.c` (serial JSON transport, `api` command)
- `ide/`  web IDE (see ide/README.md): `wasm/` (core -> WebAssembly glue + emcc flags), `web/` (Vite + React + TS +
  CodeMirror 6), `Dockerfile` (emsdk -> node test+build -> nginx), `run.sh` (http://localhost:8080, 127.0.0.1 only).
- `tools/apitest.py`  exercises the serial JSON API on a connected board (pyserial from the IDF venv).
- `host/`  unit tests for the core. `cd host && make` (gcc, ASan+UBSan, -Werror). Run after every core change.

## Versioning (dev process, agreed with the user 2026-11-04; the sequence starts at 0.10.0)
- One version, `version.txt` (`MAJOR.MINOR.PATCH`, PROJECT_VER), covers the firmware, DSL, console/HTTP API and IDE.
  `ide/web/package.json` is not a release version; leave it.
- Bump once per piece of work handed to the user (when it is built and ready to flash), not per edit. The highest
  kind of change in that work decides; lower fields reset to 0. Docs-only or test-only work doesn't bump.
  - **PATCH**: bug fixes (including making a native do what its docs say, e.g. the `scale` rounding fix), speed-ups and
    internal changes with no intended change in behaviour.
  - **MINOR**: additions that keep every existing program, saved setting, command and API call working the same: new
    natives, new optional arguments, new commands, API fields and settings (with defaults), IDE features.
  - **MAJOR**: anything that can break what a user already has: a saved program stops compiling or means something
    else (a native renamed/removed, argument order or defaults changed), saved settings not carried over on upgrade
    (NVS layout change without migration), an incompatible console or API change, or a partition table change (needs
    a USB flash, can't be done over OTA). Ask the user before making a breaking change. The first one takes 0.x to 1.0.0.
- The user runs the IDE from the Docker container (http://localhost:8080). When handing over any IDE or DSL change, run
  `ide/run.sh` to rebuild it (it also runs the IDE tests); a reload of the page alone shows the old build.
- Every bump gets a line per change in `CHANGELOG.md` (newest first), and the final report to the user says the new
  version so they can check it with `version` after flashing. Status entries below may cite the version.
- Releases (from 0.18.0, 2026-11-11): public repo github.com/kmm/ktsn-led, MIT, commits as `kmm`
  `<52089+kmm@users.noreply.github.com>` (repo-local git config; never the user's real email). Release = tag
  `v<version>` + GitHub release with the CHANGELOG entry and the zips from `tools/package-release.sh`, built from a
  fresh clone (steps in BUILD.md). Only release a version the user has flashed and confirmed. CI
  (`.github/workflows/ci.yml`) runs host tests, the IDE Docker build and every variant from `tools/build-variants.sh`;
  a new variant goes in that script's list, the CI matrix and BUILD.md. Keep personal data (the user's location, IPs,
  serial numbers) out of committed files. **Verified by the user**: the v0.18.0 release zips flash and run for
  `esp32c6` (2026-11-11), `esp32-4mb`, `esp32-2mb` and `esp32-2mb-nowifi` (2026-11-12). Not flashed from a zip:
  `esp32c6-parlio`, `esp32c6-nowifi` (built and packaged the same way).

## Design decisions (keep these unless there is a reason to change them)
- Pixel layout `0xWWRRGGBB`. Color order is applied only at output (`led_out.c`, per channel wire format). Global intensity is applied at output, not baked into buffers.
- **Wire formats** (2026-10-18, `main/wire.c`, portable, host-tested): per channel, the bytes sent per pixel in order,
  letters R G B W 0, spaces only group chips: `RGB` (default, the user's WS2811 modules), `GRB`, `GRBW` (SK6812 RGBW),
  `BRG WWW` (Govee S14). Up to 12 bytes per pixel. Part of the channel layout (`led_chan_cfg_t.wire`, NVS `wire<N>`
  only when not RGB, `chan <n> <gpio> <npix> [wire]`, JSON `channels.N.wire`), so it applies after save + reboot;
  `reboot_needed` compares it with what runs (`led_out_wire`). npix stays pixels (bulbs); the string gets npix x bytes.
  RMT: led_strip is a plain byte pipe (RGB "pixels" = 3 wire bytes each, the last one zero-padded; padding reaches no
  chip). PARLIO: `ws_par_encode` with n bytes per pixel, frame sized in 3-byte slots. Send-time estimates count bytes.
  W over several outputs (phase 2, 2026-10-18, `wire_encode_mapped` in wire.h): with more than one W in the wire, or a
  white limit, W goes through the 8.8 gamma table, intensity and `wmax`, is scaled to 0..nw x 255, dithered (or
  rounded) to whole steps and spread: each W output gets total / nw, the first (total % nw) one more. W 255 = every W
  output at 255; the dark end gets nw times finer steps. One W and no limit = exactly the old path (out_map_px). The
  8.8 gamma tables are now always built (not only with dither). White limit: `white <ch> max <0..100>` (percent, at
  once), NVS `wmax<N>` when not 100, JSON `channels.N.wmax`, IDE "W max" column (only for wires with W).
  White modes (phase 3, 2026-10-18, `wire_encode_white`): `white <ch> mode direct|extract|boost`, `white <ch> point
  #RRGGBB [ratio]`; NVS `wmod<N>`, `wpt<N>`, `wrat<N>` (only when not default: direct, #FFB46B ~3000 K, 1.00); JSON
  `wmode` / `wpoint` / `wratio`. The white point is what full W looks like in RGB, ratio how bright full W is next to
  that colour (0.1..4); `wire_white_factors` turns them into `out_map_t.wf` (RGB light of full W, Q8). In linear 8.8
  (after the gamma table) the white contained in RGB is a = min over channels of lin_c / wf_c, capped so W stays under
  the white limit; extract subtracts a x wf from RGB, both add a to W; then intensity, dither (or rounding), W spread.
  Only used when the wire has W and mode != direct (direct = the phase 2 path, unchanged). Host test: extract
  preserves light (RGB + W x point = input within 1) over many colours. Calibrate the point in direct mode: full W next
  to the point colour in RGB (e.g. `(fill (buf O) #000000FF) (fill (slice (buf O) 1 _ 2) #FFB46B)` on an S14).
  IDE: Board panel "White (W)" section (mode, point colour, ratio); the preview runs the same mapping in WebAssembly
  (`lc_output` takes wire + white settings, returns RGB as sent + W level) and draws W as point colour x ratio x level
  added to RGB (`pixelCssWhite`), following the board channel or set by hand (Wire, White, point, max).
- **Gamma correction** (2026-10-17): per channel, at output only: `led_out_write` maps each byte through the channel's
  256-entry table, then applies intensity (gamma first, so intensity stays a linear power scale). `main/gamma.c` builds
  the tables with integer math only (Q28 log2 by squaring, exp2 by a Q30 table + linear tail), portable and host-tested:
  all 201 x 255 entries for gamma 1.00..3.00 match libm's `round(255*(i/255)^g)`. The IDE's WASM core compiles the same
  file (`lc_gamma_lut`), so the preview's tables are byte-identical. Stored as hundredths (`g_set.gamma[]`, NVS
  `gam<N>`, only when not off), 100 = off; `render_gamma_update(ch)` rebuilds the table (call it after changing
  `g_set.gamma`). Not part of the channel layout: applies at once, no reboot.
- **Dither** (2026-10-17): per channel, 0..4 bits (`g_set.dither[]`, NVS `dith<N>`, 0 = off), ordered temporal dither
  at output (`main/dither.h`, `out_map_px`, used by both drivers, host tests and the WASM preview). With dither on, the
  pixel goes through an 8.8 gamma table (`gamma_lut16`; identity = in << 8), is scaled by intensity in 8.8, then gets
  an offset that steps through 2^bits evenly spaced values over 2^bits frames (bit-reversed order, per-pixel phase
  from a Fibonacci hash of the index), and is truncated to 8 bits: the cycle average is the 8.8 value to within half
  a dither step (host-tested for every input, gamma and intensity sample). The offset is < 256, so a 0 byte stays 0:
  dither never lights a dark channel. Without gamma or intensity there is nothing to dither (output unchanged).
  Dither 0 is exactly the previous path (8-bit gamma table, px_scale). Same R/G/B offset per pixel, so no colour
  shimmer. Interpreted from the user's "0-4 LSBs of dither": bits of extra resolution below the 8-bit output.
- Numbers: DSL literals are parsed by hand with integers. Ints stay ints; decimals become Q16.16 (`V_FIX`);
  `A_UNIT` args (0.0..1.0) are folded to Q8 0..256 at compile time (`UNIT_ONE` = 256).
  `A_NUM` args (any number, e.g. `rotate` amount) arrive as Q16.16; ints are converted (saturating at +-32767).
  `px_scale()` in `pixel.h` scales two bytes per multiply and is tested exhaustively against a reference.
- Natives are declared in a table with arg types, so the compiler rejects bad calls with `line L col C: message`.
  `NF_FOLD` natives with all-constant args are evaluated at compile time (`(buf O)` becomes a direct pointer).
  Programs are compiled per channel because buffer pointers are bound at compile time.
- `fill`/`scale` return their buffer, so calls nest: `(scale (fill (buf X) #00FF00) 0.5)`.
- `scale` rounds **down** (`px_scale_down`), 2026-11-04: with round-to-nearest a repeated fade stuck (the user's
  `(scale (buf O) 0.9)` trail held every pixel at 4 forever, since x*(256-s) < 128 means no change). `px_scale` (rounded)
  is still used for output brightness and `fill-random`. **Verified by the user on hardware 2026-11-04** (trails fade out).
- The buffer `mix` uses `px_mix_toward` (2026-11-04, user's choice): rounded, but each differing byte moves at least
  1 toward src when amount > 0, so a repeated mix arrives (plain rounding stalled 4 levels away at 0.1; rounding down
  would stall further away when fading up). Only the buffer `mix`: `fill-grad`, `grad-map`, colour `lerp` and
  fractional `rotate` are one-shot and keep `px_mix` (nearest). Host-tested (exhaustive, both directions arrive); **verified by the user on hardware
  2026-11-04** (orange/black fade test: the exact-arrival indicator pixel turns green).
- `bufref_t` is a view `{px, len, stride}`: pixel i is `px[i*stride]` (use `buf_at`). Natives must loop by stride, never
  index `px[i]` directly. Slices of slices multiply strides. A negative step selects the same pixels as the positive
  step, reversed (`(slice b 0 10 -2)` = 8 6 4 2 0). Constant slices fold to a fixed view at compile time.
- **Arena memory is freed through `arena_t.mem`, never `base`** (2026-10-18): `arena_init` rounds `base` up to 8 bytes,
  and ESP heaps return blocks that are only 4-aligned. `channel_free` freed `scratch.base`, which corrupted the heap
  whenever the scratch block happened to be 4- but not 8-aligned; it only showed on the C6 no-WiFi build (crash in
  `tlsf_free` from `autoexec_run`'s `channel_free` at boot). Host and WASM mallocs are 16-aligned, so tests can't catch
  it by running; `t_arena` checks the contract with deliberately 4-aligned memory.
- Compile is two-pass: the first into a `DSL_PROG_ARENA_MAX` (32 KB) block finds errors and measures, the second
  compiles into an exactly sized block (typical programs: a few hundred bytes). AST scratch is reset per top-level form.
- Hot swap: `channel_set_source` compiles into a new malloc block, then atomically publishes it in `ch->pending`.
  Only the render task frees/replaces `ch->prog`. A failed compile changes nothing.
- `DSL_MAX_ARGS` is 10 (`set` with 8 colors). Arg arrays in the evaluator and constant folder are VLAs sized by the call's
  actual `nargs`, so the limit costs no stack; deep nesting of small calls stays cheap on the render task stack (8 KB since 0.14.0).
- Runtime safety: node budget per run (`DSL_NODE_BUDGET`), nesting limit, program size limit. Faults are counted, not fatal.
- Render task is woken by an `esp_timer` callback via task notify; nothing renders in the timer context.
  It is pinned to core 1 (dual-core chips) and creates the LED outputs itself, so RMT interrupts stay off the WiFi core.
  `ulTaskNotifyTake` count > 1 means a frame was missed (`g_perf.missed`).
- Channel buffers X, Y, Z, O persist between runs. 8 global buffers (G0..G7) are reachable via `(buf G3)`.

## System (settings, boot, network)
- Console changes apply in RAM; `save` writes everything to NVS (namespace `ledctl`): channel layout (`nch`, `gpio<N>`, gamma `gam<N>`, dither `dith<N>`,
  `npix<N>`), programs as source (`prog<N>`), `auto` (autoexec), `intens`, `ssid`, `pass`, `ntp`, `tz`.
  `config` dumps what is stored (password masked), `config erase` clears it. WiFi password is stored unencrypted.
- **Boot must never loop on bad stored config:** invalid values fall back to defaults; a channel whose memory or output
  can't be set up is logged and disabled; a stored program that no longer compiles is logged and skipped.
- Boot order: NVS load, TZ, globals, autoexec (temp channel with GLOBAL_BUF_PIXELS-long X Y Z O, run once, freed),
  channels + stored programs, render task (creates outputs), WiFi, console.
- Channel layout changes (`chan`) only apply after save + reboot, because buffers are sized at boot.
- **Commands are transport-neutral:** each is `int fn(cmd_out_t *o, int argc, char **argv)` in the `g_cmds` table
  (`console.c`) and prints only via `out_printf`/`out_puts` (never `printf`). Serial registers every entry with
  esp_console (`func_w_context`); network transports call `cmd_exec`. One mutex (`cmd_lock`) serializes all commands
  across transports; anything else touching `g_set` or channel sources from another task must take it too.
- **Output stage: oversampled refresh and parallel sending** (2026-10-17), board-wide, both off by default, apply at
  once (`output` command, JSON `output: {refresh, parallel}`, NVS `refresh`/`outpar`, IDE Board panel):
  - `refresh` 1..4 (REFRESH_MAX): the frame timer ticks at 60 x refresh Hz; the render task renders once per `refresh`
    ticks (counting missed ticks, so programs stay at 60 Hz) and sends buffer O on every tick. No snapshot needed: only
    the render task writes O. The dither counter is `g_perf.sends`, so dither steps with every send (refresh 4 makes a
    3-bit cycle 30 Hz instead of 7.5 Hz). Capped at 4x because LED chips' own PWM (a few hundred Hz to ~2 kHz) makes
    faster updates pointless. `render_check_output` refuses settings whose estimated send time (`led_out_send_us`:
    RMT 30 us/px + 300, PARLIO 36 us/px + 100; summed for sequential RMT, max when concurrent) exceeds 90% of a tick;
    boot falls back to 1x if a stored refresh no longer fits. Glitch risk: on RMT without DMA (ESP32, C6 RMT) more
    sends per second = proportionally more chances for WiFi interrupt latency to delay a refill (each glitch lasts
    only one tick); PARLIO (DMA) has no refills.
  - `parallel` (RMT only): `led_strip_refresh_async` for every string, then `led_strip_refresh_async_done` in
    `led_out_flush` (in-flight strings are tracked, so toggling mid-frame is safe). PARLIO always sends together.
  - Stats: `frames` (renders), `sends`, `missed` = timer ticks missed; `stat` and JSON status show the output rate.
  - IDE preview: `refresh` sends per frame, each with the next dither step; the drawn frame is their per-byte average
    (`averageSends`, what the eye integrates), so the preview shows the smoother dither.
- **Program storage** (2026-10-18, `main/progs.c`): program text lives in its own NVS partition `progs` (256 KB at the
  end of flash in both layouts: partitions_2mb.csv replaces ESP-IDF's singleapp_large with the same nvs/phy/factory
  offsets; the 4 MB OTA slots shrank to 0x1D0000 each), because the 24 KB settings NVS holds only ~630 entries and a
  3999-char string takes a whole 126-entry page. Namespaces: `ledctl` (prog<N>, auto; later automation) and `lib`
  (library, key = name). `progs_init` (in settings_init) moves prog<N>/auto out of the settings NVS once; without a
  progs partition (old table, e.g. after OTA or app-flash) it falls back to the settings NVS so nothing breaks.
  `config erase` clears settings, channel programs and autoexec, not the library.
- **Intensity: global x per channel, with fades** (2026-10-20): `render_set_intensity(ch | INTENSITY_GLOBAL, 0..256,
  fade_ms)` is the only way to change intensity (console `intensity <ch|all> <0..1> [fade s]` and `env intensity`,
  JSON top-level `intensity` (0..256) / `channels.N.intensity` (0..1) with `fade` (seconds) for the patch's changes,
  IDE Board panel sliders + fade, later automation). Faders (`main/fader.h`, portable, host-tested) are 16.16 fixed
  point (an hour-long fade still moves every frame; step magnitude rounded up so it arrives on time), stepped once per
  rendered frame in the render task under a spinlock. Output intensity = global x channel (Q8). `env intensity` is the
  global value now (0..256). `save` stores the targets (`intens`, NVS `cint<N>` when not full); status shows the
  values now (mid-fade), config the targets.
  **16-bit output intensity** (0.13.0, 2026-11-09; user's choice: fades only): targets, settings, API, console and the
  DSL (`env intensity`, `(intensity)`) stay 0..256, but the output stage takes `out_map_t.intensity16` 0..OUT_ONE
  (65536, dither.h): `fader_value16` (the fader's 16.16 value rounded to 1/65536 of full) for global and channel,
  multiplied with `intensity16_mul` (64-bit, rounded), so mid-fade values are 256 times finer (a 1-minute fade from
  4/256 to black: ~1000 output steps instead of 4). Dither path and wire.h: `(v * k) >> 16` (fits 32 bits). Without
  dither `px_scale16`: per-byte rounding, and for whole Q8 values (k & 0xFF == 0) the old two-lane `px_scale`, so steady
  settings give exactly the old bytes and cost the same. One intended difference: global and channel both below full
  now multiply exactly (e.g. 128 x 3 = 1.5/256) instead of rounding the product to 1/256. The progress bar uses
  OUT_ONE. WASM `lc_output` takes intensity16; the IDE's `core.output` takes 0..256 with fractions (x 256), the preview
  passes intensity x channel intensity unrounded. Visible gain is mostly with dither on (the bits below the byte
  follow the finer value); without dither the output is whole bytes either way.
- **IDE local library** (step 1 2026-11-04, confirmed by the user; step 2 2026-11-09; `src/library/`; plan in "Next steps"): `library.ts` (pure, throws Errors
  with user-facing messages) holds `{folders, programs: [{id, folder, name, source, modified}]}` in localStorage
  `ktsn-led.ide.library` as `{version: 1, ...}`; an unreadable value is copied to `ktsn-led.ide.library.unreadable`, never
  overwritten silently. `folders` lists every folder with its ancestors (sorted segment-wise), root "" not listed.
  App owns the state (`changeLib(f)`: apply, store, report the error or a full storage). Tab kind `local` (`localId`;
  `boardSource` = what the library holds, so the dot means unsaved); an effect keeps local tabs in step with the
  library (renamed/moved: retitled; deleted: becomes a preview-only "(deleted)" tab; changed by an import: the dot
  shows the tab differs). Save to local library converts a preview-only program tab into the local tab (closing an
  older tab of the same program); board tabs only store a copy. Folder open/closed: `ktsn-led.ide.library.closed`.
  File format `{format: "ktsn-led-library", version: 1, exported, folders, programs: [{folder, name, source}]}`;
  `parseLibraryFile` rejects the whole file on the first problem (with where); identical programs import as unchanged.
  Step 2 (0.12.0, 2026-11-09, `library/board.ts` pure + `BoardDialogs.tsx`; dialogs owned by App, shown only while
  connected): upload = `planUpload` (selected programs, board names, local compile check; the same name ticked twice is
  refused, board names are flat) + `runUpload` (one `libPut` at a time; identical code = unchanged, not sent; an
  ApiError fails that program only, any other error stops and marks the rest "not sent"); afterwards App bumps
  `libVersion` and sets open board library tabs' `boardSource`. Save from a board = `readBoardLibrary` (404 = no
  library: channels only) + `boardEntries` (library programs, then running channels' programs as `ch<N>`, uniqueName
  on a clash) fed through planImport/applyImport (`entriesAsFile`), default folder `boardFolder(name)` =
  `boards/<name>`. URL import: `fetchLibraryUrl` (http/https only, `credentials: 'omit'`, 8 MB cap, parsed before it
  is accepted; a network failure is explained as unreachable or no CORS; no proxy in the container).
- **IDE palette tab** (2026-10-22, `src/palette/`; option C agreed with the user: a Palette tab first, then a shortcut
  from `set` calls in the code into it): stops -> `paletteCode` (one `(set ...)` per 8 colours on a G slice, optional
  `(def name ...)`, compiled and checked on the real core in the tests), preview via `gradient` (n_fill_grad's maths
  and px_mix rounding) or `pattern` (fill-pat), Insert (Editor `insert` prop: at the cursor, on its own line) / Copy,
  saved palettes in localStorage `ktsn-led.ide.palettes`, the current one in `ktsn-led.ide.palette.current`.
  In-code shortcut (2026-10-22, `src/palette/inCode.ts`): `findPalettes` recognises exactly the shapes paletteCode
  writes (colours filling the G slice, sets at 0, 8, ...; with/without def; split sets), so writing back can't lose
  code; the editor shows a ▦ widget at each (`paletteMarks`), the click opens it in the Palette tab (App `palLink`), and
  *Apply to code* replaces that text via the Editor `rangeEdit` prop (found at its range or, if moved, by its text;
  refused if it changed).
- **IDE colour picker** (2026-10-22, `src/color/`): clicking an editor swatch (`swatchClicks` in dslLanguage.ts) opens
  `ColorPicker` (fixed position, per the popup rule) on that literal; edits are dispatched live with userEvent
  `input.color`, Esc restores the original. Literals keep their 8-digit form (#RRGGBBWW) once they had or get a W.
  Its outside-click listener is added on the next tick: with a real click React mounted the picker while the opening
  mousedown was still bubbling to the document, so that same event closed it again (synthetic dispatchEvent tests
  passed; check popups with real clicks, `computer left_click`, not only dispatched events).
  Favourites: 16 slots in localStorage `ktsn-led.ide.favorites`, `useFavorites()` keeps every editor in sync (and other
  IDE tabs via the storage event); meant to be shared with the palette editor (planned).
- **IDE tab bar `+` menu** (fixed 2026-10-20, confirmed by the user): the tab bar scrolls sideways (`overflow-x: auto`),
  which also clips vertically, so the absolutely placed menu was hidden inside it. The menu is now `position: fixed`
  at the button's screen position (closes on outside click, resize or scroll). Rule: popups inside scrolling boxes
  must be fixed (or portaled), never absolute.
- **Automation** (2026-10-20, `main/automation.c`; core: `dsl_host_t`, `DSL_CTX_AUTOMATION`, `NF_AUTOMATION`, `NF_OPT0`):
  the automation program runs in its own 1-px channel with `ctx = DSL_CTX_AUTOMATION`, in its own task, with
  `cmd_lock` held: at boot (after render_start), when set, on `automation run`, and whenever wall-clock seconds cross a
  multiple of `auto_every` (1..3600, default 10; uptime until the clock is set, i.e. before 2023). In that context
  `(buf X|Y|Z|O)` is a compile error (n_buf), and only there the NF_AUTOMATION natives compile: `(intensity [ch])`
  -> decimal 0..1 now, `(set-intensity <ch|_> <0..1> [fade s])` (NF_OPT0: `_` allowed as the required first arg =
  global), `(play ch name)` (bind_play checks the name and `lib_exists` at compile time), `(stop ch)` (channel runs
  `(fill (buf O) #000000)`). They reach the system through `dsl_set_host` (firmware: automation.c; host tests: a fake;
  WASM: a stand-in that remembers intensities and checks names against the list the IDE sets from the connected
  board's library). play/stop skip work when the channel still runs what automation put there (same source pointer,
  library generation `g_lib_gen` unchanged), so calling them every run is cheap; anyone else setting a program makes
  the next run re-apply it (automation wins). Faults (deleted library program, bad channel) are counted on the
  automation channel and shown in status. Stored: progs `automat`, settings `autoevery`; a stored program that no
  longer compiles at boot is logged and not run. Console `automation`, JSON `automation` / `automation_every` (config,
  patch: compile-checked first) and status `automation {active, every, runs, last_run, faults, last_error}`; IDE tab
  kind `automation` (Editor `context` prop -> core.check(src, 'automation')), Board panel Automation section, config
  files (restore puts the library first so play names resolve), reference category Automation (`context` field).
- **GPIO outputs** (2026-10-30, `main/io.c`; core: `NF_IDARG`, `dsl_host_t` io hooks): up to `IO_MAX` 8 slots in
  `g_set.io` (`io_cfg_t`: gpio (-1 = unused), mode binary/pwm, invert, freq 1..20000 Hz (default 1 kHz), name 1..15 of
  a-z 0-9 - _ starting with a letter). Binary = plain GPIO; PWM = LEDC low-speed, 10-bit duty (full on = 1024 = 2^bits; written with
  `ledc_set_duty` + `ledc_update_duty`, never `ledc_set_duty_and_update`, which needs the fade service), one timer per distinct
  frequency (4 timers; channels: 6 on the C6, 8 on the ESP32), allocated and reference-counted in io.c. `io_check`
  validates a whole table (pin usable, not an LED channel's pin, no duplicate pins or names, PWM resources);
  `chan` and the patch refuse an output's pin for a channel. Always off at boot (`io_apply` right after
  settings_init; the level is set before the pin becomes an output); values are never saved. `io_apply` only tears
  down slots whose pin, mode, polarity or frequency changed, so reconfiguring one output (or renaming any) doesn't
  glitch a running relay; released pins are left floating. Stored as one NVS blob `io` (erased when equal to the
  defaults; size-checked and validated on load). Defaults: C6 GPIO 19 + 20 binary (`IO_DEFAULTS_INIT` in config.h),
  none elsewhere. Automation `(io-set out v)` / `(io out)`: NF_IDARG lets the first (A_INT) arg be a bare name (not a
  def variable, t, nil or `_`), passed to `bind_io` as a symbol and rewritten to the slot via `io_find` at compile time
  (unknown name = compile error; constant numbers are checked with `io_used`). Value: t/nil or a number (A_ANY, type
  checked in bind); a tiny positive level never rounds to off. Console `io`; JSON config `io` [{n, gpio, mode, freq,
  invert, name}] + `io_max`, patch `io` {"n": {...} | null} (all-or-nothing, the final table is checked) and `io_set`
  {"n or name": bool | 0..1}, status `io` [{n, name, value, ok}]. WASM: `lc_set_io` ("n:name" lines from the connected
  board's config; offline every slot 0..7 and any name is accepted). IDE: Board panel Outputs (`ui/OutputsSection.tsx`).
- **Strings** (2026-10-31; plan agreed 2026-10-30): `V_STR` / `A_STR`, value `strref_t {p, len}`, a view (not
  NUL-terminated; `str-sub` returns part of another string without copying). Literals `"..."` (one line, at most
  `DSL_STR_MAX` 255, escapes only `\"` and `\\`; the parser counts first, then copies into the AST arena: no stack
  buffer in the recursive parser) are copied into the program arena by compile_arg. Strings made at run time
  (`str-num`, `str-time`, `str-cat`) live in the per-run scratch (`str_alloc`; out of scratch = fault), so they last
  one run, like variables. `def`, `if` and `eq`/`ne` (same text) take strings. Only `str-sub` and `str-len` fold
  (they allocate nothing). `str-time` format codes are checked at compile time when constant (`time_fmt_check`);
  `str-num` width 0..32 and decimals 0..4 likewise (clamped at run time). Integers saturate symmetrically, so the
  smallest is -2147483647. Console: `esp_console_split_argv` drops the quotes of a word that starts with `"`, so
  one-line source (`prog`, `autoexec`, `lib set`, `automation`) is taken from the raw line (`cmd_out_t.line`,
  `source_arg` in console.c): cmd_exec keeps a copy; the serial REPL's line is captured by linking with
  `--wrap=esp_console_split_argv` (cmd.c; cmd_exec calls `__real_`). `chan`'s wire format still re-joins argv.
- **Segment displays** (2026-10-31, one engine `seg_draw` for `seg7` / `seg9` / `seg14`, `segdisp_t`): a layout name
  is the segment names run together in wiring order (7: a..g; 9: a..g j m; 14: a b c d e f g1 g2 h j k l m n; p
  optional point; case-insensitive), rewritten in place by bind into segment numbers + 1. Lengths: a d hstride; b c e
  f j m vstride; g hstride (7/9); 14 only: h k l n `dstride` (default vstride), g1 g2 `gstride` (default
  (hstride+1)/2) — a deliberate change from the plan's "single-letter legend": the datasheet names g1/g2 parse
  unambiguously. Value: int (right-aligned, blanks, dashes when too long) or string (left-aligned, cut off; `.`/`,`
  after a character joins that digit's point, also past the last digit; a leading or second point takes a digit).
  Fonts (`dsl_seg_glyph(kind, ch)`): k_seg7; k_seg9 = our own overrides using the centre verticals (B D I K M Q R T
  W X Y, i m w, + | ! $), else the 7-segment glyph (exact case first, then the other case); k_seg14 = our own table
  in the usual Adafruit-style bit order (a..f g1 g2 h j k l m n p), upper case + symbols, lower case shown as upper.
- **Firmware info** (2026-10-22, `main/firmware.c` `fw_info`): version, build, running slot, its OTA state ("pending" =
  a new update not confirmed yet: a reset before the 30 s health check boots the other slot), the other slot's app
  version, the next boot slot. Used by the `version` command, /api/status `firmware.state` / `firmware.other`, and the
  IDE Board panel; made for testing rollback.
- **Program library** (2026-10-18): names 1..15 chars `[a-z][a-z0-9_-]*` (NVS key limit; also valid DSL names, for
  automation's `play`). `lib_put` compiles against channel 0 before storing. Console `lib`, JSON API `api_lib_*`
  (HTTP GET/PUT/DELETE /api/lib[/<name>], serial ops lib / lib_get / lib_put / lib_del), IDE tab kind `library`
  (Send = store on the board), Board panel Library section, config files `library` (restore option).
- **Multi-line entry** (2026-10-17): `prog <ch>` alone and `autoexec paste` read lines until one that is only `.`
  (`read_source` in console.c), up to SOURCE_MAX, comments and indentation kept, compiled as a whole. Interactive
  transports give `cmd_out_t.read_line` (serial: `stdin_read_line` in cmd.c, polling stdin with its own echo while the
  command runs in the REPL task, like `api`; telnet: `next_line` in netcli.c, the session's own line reader). The JSON
  API's `cmd` has no read_line, so multi-line entry is refused there. `read_source` releases `cmd_lock` while waiting
  (slow typing must not block the HTTP API); after an overlong line/program it keeps reading to the `.` and drops it
  all, so the rest of a paste never runs as commands.
- **Network CLI** (`CONFIG_KTSN_NET_CLI`, port `CONFIG_KTSN_NET_CLI_PORT`, default 23): one session at a time
  (others get "busy"), telnet IAC bytes stripped, CRLF output, 10 min idle timeout. **Refuses everyone until a token is
  set**; sessions start with `auth <token>` (constant-time compare, 1 s delay per failure, closed after 3). The token is
  stored in NVS (`token` key), never printed by any command, unencrypted on the wire (LAN only).
- **HTTP API** (`CONFIG_KTSN_HTTP_API`, port `CONFIG_KTSN_HTTP_PORT`, default 80; esp_http_server + registry
  `espressif/cjson`, ~54 KB): every endpoint needs `Authorization: Bearer <token>` (403 if no token set, 401 + 0.5 s delay
  if wrong). `PATCH /api/config` is all-or-nothing: work on copies, compile programs with `dsl_compile`, run autoexec
  last, apply only if `errors` is empty, then publish with `channel_set_program`; WiFi changes apply after the response.
  Never return the WiFi password or token. cJSON's doubles are fine in this config path, never in the render path.
- **JSON API layer** (`api.c`): status, config get/patch, prog, cmd, save, reboot as functions returning
  `{status, body, after}`; transports only do auth/framing and call `after` once the response is sent (WiFi apply,
  restart). Add new operations here, then expose them in both transports.
- **Serial JSON API** (`CONFIG_KTSN_SERIAL_API`, default y, all builds): `api` on the serial console (registered
  directly with esp_console, not in `g_cmds`, because it must not hold `cmd_lock`; not available over telnet).
  One JSON request per line `{"id", "op", ...}`; each response is one line `0x1E{"id","status","body"}` written with a
  single fwrite so log lines can't interleave inside it. Ops: ping status config patch prog cmd save reboot exit. No
  token (physical access). Lines up to API_BODY_MAX + 4 KB, so programs up to SOURCE_MAX go over serial too.
- **USB-Serial/JTAG RX (C6 etc.):** the REPL installs the driver with a 256-byte RX buffer and the driver silently
  drops what doesn't fit (`xRingbufferSendFromISR` result ignored, no USB backpressure), so long serial-API requests
  got truncated (found on the C6, 2026-10-15). `console_start` swaps in a driver with `API_LINE_MAX + 1 KB` RX
  (nonblocking VFS during the swap). The ESP32's UART at 115200 is slow enough for the stock buffer.
- **Reset on serial open (ESP32 and C6, confirmed 2026-10-15 by uptime):** the dev board's auto-reset circuit resets the chip
  while RTS is asserted and DTR is not. Opening the port asserts both together (harmless); *releasing* them afterwards
  goes DTR first in pyserial, giving a brief RTS=1/DTR=0 = reset. Rule for every client (apitest, the IDE): open the
  port and leave DTR/RTS as they are; never change one line without the other. `apitest.py --release-lines` reproduces
  the reset. The C6's USB-Serial/JTAG behaves the same (it emulates that circuit). Solved for clients that follow the rule.
- **CORS:** every HTTP response has `Access-Control-Allow-Origin: *`; `OPTIONS /api/*` preflights are answered without a
  token (incl. `Access-Control-Allow-Private-Network: true`). The IDE runs on localhost, never on the board.
- **Reserved pins** (fixed 2026-10-31): `settings_check_gpio` checks a snapshot of `esp_gpio_is_reserved` taken at
  the start of `settings_init`, before any of our drivers run. gpio_config, RMT and LEDC reserve the pins they drive,
  so a live check refused a pin we already use ourselves (found with `io config 0 19 binary name=test` on the C6:
  "is reserved (flash/PSRAM)"); the same would have hit `chan`/patch on a running LED channel's pin.
- Validation (`settings_check_gpio/npix/wifi/token`) and the constant-time `settings_token_ok` live in `settings.c` and
  are shared by every transport; add new checks there, not in a transport.
- **mDNS** (`CONFIG_KTSN_MDNS`, registry `espressif/mdns` ^1.14, ~41 KB): `<hostname>.local`, advertises `_http._tcp`
  (TXT path=/api/status, auth=bearer) and `_telnet._tcp`. `hostname` setting (NVS key `host`, default `ktsn-led`,
  `settings_check_hostname`: a-z 0-9 -) also goes to DHCP; changes apply to mDNS at once, DHCP on the next connect.
- Registry dependencies are re-solved whenever `idf_component.yml` changes: adding cjson/mdns moved led_strip from 3.0.0
  to 3.1.0 (within `^3.0.0`). Check `dependencies.lock` after editing the manifest.
- Program sources: up to `SOURCE_MAX` (3999, NVS string limit) via HTTP; console lines stay at 511.
- New Kconfig-conditional sources go in `main/CMakeLists.txt` SRCS (that works); only REQUIRES must stay unconditional.
- **Sun and moon** (0.16.0): `main/astro.c` is portable (host tests, WASM) and has no tables; `main/sky.c` is the firmware
  glue, called every 100 ms from the automation task (low priority, so it can't cost a frame) and once at boot before
  autoexec; it recomputes when the minute or location changes and writes `g_env`. Local days use the time zone through
  `astro_local_day`, so changeover days are 23 or 25 hours and values are clock minutes; the preview mirrors this in
  `core/env.ts` `skyClock` with the computer's zone. Location is `g_set.has_loc/lat/lon` (1e-5 degrees, NVS `lat`, `lon`).
  `host/astro_cases.h` holds reference values from the US Naval Observatory API, generated once; its illumination is for
  12:00 UTC.
- **Time-of-day env values** (0.17.0): `env_t` stores only what the clock gives (`ms` and `weekday` and `timeset` from
  `render.c` `env_tick` via gettimeofday, `daylight` from `sky_tick` each second as sun altitude > ASTRO_SUNRISE, so it
  is right through polar day/night and doesn't depend on rounded minutes). `daymin`, `daysec` and the fractions are
  derived in `n_env` when read; `bind_env` sets the node's return type to decimal for the names from `ENV_FIRST_FRAC`
  on, so the fractions are typed at compile time. Fractions are floor(x * 65536 / whole), capped at 65535 (never 1.0).
  All time values follow the clock even before it is set (like hour), `timeset` says whether to trust them. The
  env-name error suggests the closest name by edit distance (<= 2): the name list outgrew DSL_ERR_LEN (96).
- Time: SNTP after each WiFi connect; `env` fields use local time via POSIX `TZ` (`tz` command). Without WiFi, `env time`.
- Build options: `CONFIG_KTSN_WIFI` (main/Kconfig.projbuild) guards `net.c`, the `wifi`/`ntp` commands and boot connect.
  Code testing it must `#include "sdkconfig.h"` first. CONFIG_ values are unknown when IDF resolves REQUIRES, so
  `main/CMakeLists.txt` always requires esp_wifi/esp_netif/esp_event and only compiles `net.c` conditionally; with
  `MINIMAL_BUILD ON` the unreferenced stack drops out (~255 KB image vs ~876 KB). Stored wifi/ntp keys are kept either way.
- Partitions: default 2 MB flash + `partitions_singleapp_large` (1.5 MB app). Presets `sdkconfig.nowifi`,
  `sdkconfig.flash4mb` (custom `partitions_4mb.csv`: OTA, two 0x1F0000 app slots, otadata 0xf000, phy 0x11000).
  Every layout keeps nvs at 0x9000 (settings survive); keep it that way in any new layout.
- **OTA** (2026-10-17, `CONFIG_KTSN_OTA`, on in the flash4mb preset with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`;
  decided with the user: no OTA on 2 MB boards (the ~1 MB image doesn't fit twice), loose health check, no signing):
  `POST /api/ota` (token) streams the body in 4 KB pieces into `ota.c`: the first 288 bytes (image header, segment
  header, esp_app_desc_t) are checked before anything is written (ESP image, `CONFIG_IDF_FIRMWARE_CHIP_ID`, project
  name `ktsn-led` or the old `ledctl`), then `esp_ota_begin(OTA_WITH_SEQUENTIAL_WRITES)` + writes, `esp_ota_end` (SHA-256) and
  `esp_ota_set_boot_partition`; restart after the response. Any failure leaves the running firmware alone. While an
  update runs, the render loop shows a progress bar on every string instead of the programs (`render_set_progress`).
  Health check (`ota_health_check`, main.c): a first-boot image (PENDING_VERIFY) marks itself valid after 30 s if it
  has rendered frames, else `esp_ota_mark_app_invalid_rollback_and_reboot`; a crash/reset before then rolls back in
  the bootloader. Version: `version.txt` (PROJECT_VER, bumped per "Versioning"), shown in status `firmware`.
- **USB flashing from the IDE** (esptool-js 0.7.0 pinned, `ui/FlashDialog.tsx`): the user picks a build folder
  (`webkitdirectory`); `flasher_args.json` gives chip, files and offsets (`planFromFolder`), so it writes exactly what
  `idf.py flash` writes (NVS untouched unless "erase everything"). Chip checked after connecting, before writing.
  The IDE's own serial connection is closed first (a port opens once) and reconnected afterwards. Unlike the IDE's
  serial transport, esptool resets the board via DTR/RTS on purpose (that's how it enters the bootloader).

## DSL conventions (every new native must follow these)
- **Ranges:** optional `[start] [end]` pair, always parsed with `range()` in `natives.c`. Absolute indices into the
  buffer/view, `end` exclusive, defaults `0` and `len`. Negative indices count from the end (`-1` = last pixel), then
  clamp to `[0, len]`. Empty ranges (`start >= end`) are silent no-ops, not faults.
  No other range form (lengths, offsets-from-end, inclusive ends) unless there is a strong reason; if so, document it here.
- **Single positions** (`set`/`get` index) count negatives from the end once, then **clip**: pixels outside the view are
  skipped and `get` returns black. Unlike ranges they never clamp, so a dot moving off an edge disappears instead of sticking.
- **In place, no work buffers:** prefer in-place algorithms (e.g. `rotate`: three reversals + one saved pixel) over
  scratch or user-supplied work buffers. Use per-run scratch only when unavoidable (overlapping views with different strides).
- **Optional args:** `_` stands in for any optional argument and means "the default" (compiled to `V_NIL`). Read optional
  ints with `opt_int`. Required args reject `_` at compile time.
- **Argument order:** buffer first, then the function's own parameters, then the optional range last.
- **Two-buffer natives:** `(op <dst> <src> [params] [start] [end])` (src/dst model, matches the user's prototype),
  implemented as a per-pixel `pair_fn` in `pixel.h`/`natives.c` passed to `pairwise()`; the op's own parameters go in
  a `pair_arg_t` (`k` amount, `pal` palette view), e.g. `mix`, `grad-map`.
  `dst[i]` pairs with `src[i]`; the range is taken over the shorter view; placement/offset is done with `slice`, not
  extra args. Overlapping views behave as if `src` were read first (same stride: walk direction; otherwise staged in scratch).
- **Return value:** natives that modify a buffer return that buffer (the same view they got), so calls nest.
- **Names as arguments** (like `env` fields) are resolved at compile time by the native's `bind` hook, so a typo is a
  compile error pointing at the name, never a run-time lookup or fault.
- **Values used as indices** must not go negative by accident (negatives index from the end); e.g. `env frame` is masked.
- **Lists of colors** (palettes) are passed as a buffer view, not variadic args: `(fill-grad (buf O) (slice (buf G0) 0 3))`.
- **Randomness** comes from the per-channel xorshift32 `ch->rng` (seeded from `esp_random()` in `main.c`, fixed seed on
  host so tests are repeatable). Never call platform RNGs from the core.
- **Booleans** are their own type `V_BOOL`, literals `t` / `nil`. Conditions (`if`/`and`/`or`/`not`) take only booleans;
  numbers are never truthy (compile error with a hint to compare). Comparisons take `A_NUMERIC` (int or Q16.16, unchanged)
  and compare ints exactly / mixed in 64-bit, so large `env frame` values work. `eq`/`ne` also take colors/booleans/buffers.
- **Arithmetic types:** ints stay ints; any decimal argument makes the result a decimal (Q16.16), set at compile time by
  `bind_arith`. Intermediates are 64-bit and results saturate (never wrap). `/` on two ints floors; `mod` is floor modulo
  (sign of the divisor) for ints and decimals alike. Decimals never silently become ints: `floor`/`round` (compile error
  hint says so). Decimals are limited to about +-32767 by Q16.16; ints keep full int32 range.
- **Trig:** `sin`/`cos` take degrees (int or decimal; ints reduced mod 360 exactly, so `(env frame)` works forever) and
  return decimal -1..1. Quarter-wave table, 1 degree steps, linear interpolation; within ~5/65536 of libm (tested).
- Names: blends use standard blend-mode words (`screen`, `multiply`, `add`, `subtract`, `lighten`, `darken`, `mix`); arithmetic uses
  symbols (`+ - * /`) and math words (`min`, `max`, `mod`, `clamp`). Renamed 2026-10-14: `fmul` -> `scale`,
  `max` blend -> `lighten` (freed `min`/`max` for numbers); 2026-10-17: `sub` -> `subtract`. Old names are unknown functions now.
- **Variables:** `(def name value)` only at the top level of the program or of a function body (always executed in
  order, so a use always follows a set). The compiler maps names to slots (`cc_t.vars`, max `DSL_MAX_VARS` 64 incl.
  function params/locals) and emits hidden `dsl_var_set`/`dsl_var_get` nodes (not in the native table). A slot keeps
  the type of its first def (redef must match), so uses are type-checked at compile time. Slots live in the program
  block (`program_t.slots`, `exec_ctx_t.vars`), cleared every run: no state across frames.
  A bare symbol where a value goes must be a defined variable (or `t`/`nil`); `A_SYM` positions (buffer ids, env
  names) never look up variables.
- **User functions** (0.14.0, plan agreed with the user 2026-11-09; step 1 = within one program):
  `(defn name (p:type ...) body...)` top level only; types int decimal(A_NUM: ints convert) color bool buf str; up to
  `DSL_MAX_ARGS` params, `DSL_MAX_FNS` 32 functions. Each function is a `ufn_t` (dsl_internal.h) in the program arena:
  `def` (NF_LAZY native_def_t, args = param types, ret = the body's last form) for calls, `tdef` for self tail calls.
  **No call stack:** params and locals are slots of the program like `def` variables (`c->vars`, now
  `program_t.slots` in the program block, cleared every run, `DSL_MAX_VARS` 64 total); that works because a function
  is never active twice: circles in the call graph are compile errors and a self call is only allowed in tail position
  (the last body form, or a branch of an `if` in tail position), where it compiles to a `tdef` node that evaluates
  the new values, stores them and sets `c->tail`; `dsl_call` then runs the body again (a loop; the node budget stops
  endless ones). The tail node's type is `V_TAILCALL` until the body's type is known; `bind_if` takes the other branch.
  **Compile order** (`compile_pass`): 1 parse every form, read defn headers (`declare_fn`); 2 `fn_refs` collects who
  calls whom (heads, and names in `A_FN` positions); 3 `order_fns` depth-first (callees first, a cycle = error at the
  call); 4 bodies in that order; 5 the program. No on-demand recursion, so compiling needs no more stack than before.
  **Scopes** (`scope_t`): top level, defn body (a barrier: sees only its params/locals, env, buffers; never the
  program's variables, so a library function is self-contained) and `fn` bodies (see everything around them up to the
  barrier). `def` assigns a visible variable of that name (type must match) or makes a local. Function names and
  variable/param names can't overlap. `fn` exists only as an `A_FN` argument (`V_FN` constant, never stored), so the
  slots it reads are always live. **Stack (0.14.1):** `node_t.stack` = estimated evaluator bytes incl. called bodies,
  from the `STK_*` frame costs in compile.c (ESP32/C6 -O2 disassembly: a call 176 + 16 per arg, a lazy native 288, a
  user call 304 + 16 per arg; a fixed 16 per value so host/IDE/board agree); `DSL_MAX_EVAL_STACK` 6400 checked at
  compile time, `program_t.stack` / `perf.stack` for `stat`. A level count (0.14.0) was not enough: 32 levels of `if`
  need 9 KB. Re-check `STK_*` when the evaluator changes. **Time (0.14.1):** `dsl_set_clock` + `channel_t.deadline_us`:
  `dsl_call` reads the clock every 32nd call / tail round and faults past the deadline, so only function programs can
  hit it. render.c: `LOOP_LIMIT_US` 12 ms per frame shared by the channels, at least `LOOP_MIN_US` 2 ms each. Needed
  because a node costs ~2.4 us at -Og (measured), so the 65536 node budget alone is ~150 ms a frame. The IDE and the
  automation program have no clock limit, only the node budget. The core is built with -O2 (components/dsl/CMakeLists.txt).
  `map-px` / `repeat` give back the scratch a call used after each call, unless the fn stores a string in an enclosing
  variable (`UF_KEEP`, set by `compile_def`). A body form that is a constant becomes a `dsl_const` node.
  Render/automation stacks raised to 8 KB (`RENDER_STACK`, `AUTOMATION_STACK`).
- **Libraries** (`use`, 0.15.0, step 2 of user functions): `(use name)` is top level only; a used program may contain
  only defn/use. compile.c: `unit_t` per source (0 = the program; `DSL_MAX_USES` 8 libraries), `scan_unit` walks the uses
  depth first through `dsl_host_t.lib_load` (malloc'd, the core frees it; optional `lib_why` says why a name can't be
  used). The functions are copied into the program when it compiles: no pointers between programs. All of the program's
  own functions are compiled, of the libraries only those reached (`DSL_MAX_FNS` 64, uint64 masks). An error in a library
  reads "line L col C: in <lib> line l: msg" at the `(use ...)`. Board (`progs.c`): `lib_put`/`lib_del` go through
  `lib_recheck` (channels, autoexec, automation whose source contains the text "use"; `lib_source` serves the proposed
  change as an override), refuse with that program's error, then `lib_publish` swaps the programs. Library programs are
  checked as channel programs when saved, so one with automation-only natives can't be stored. IDE: `library/uses.ts`
  (`usedNames`, `localSources`, `loadUses`, `usesFirst`), App effect feeds `core.setUseSources` (connected: board programs
  reachable from the open tabs; offline: local library), `Editor.recheck` re-lints when they change, upload compiles with
  `core.checkUsing` against board + selection.
- **Lazy natives** (`NF_LAZY`: `if`, `and`, `or`) get `args == NULL`, read `c->node` on entry and evaluate their own args
  with `dsl_eval_arg`. Use this for anything that must not evaluate all args (short-circuit, branches).
- **Result types:** each node has `ret` (= `def->ret` unless the bind hook refines it, e.g. `if` takes its branches' common
  type; int + decimal -> decimal; no else or a nothing-branch -> statement). Type checks use `node->ret`, not `def->ret`.
- **Pure natives** (no side effects, no env access) get `NF_FOLD` so constant calls cost nothing at run time.
- **Bad values:** out-of-range indices clamp; values that make the call meaningless (e.g. `slice` step 0) set `c->err`,
  which is a compile error when folded and a counted fault at run time. Never crash, never write out of bounds.
- Each new native gets host tests for defaults, `_`, negative indices, empty ranges, and slices with stride != 1.

## DSL v1
    (buf <id>)                                 id = X Y Z O or G0..G7
    (slice <buf> [start] [end] [step])         view of every step-th pixel of the range; step<0 reverses
    (fill <buf> <#RRGGBB[WW]> [start] [end])   set the range to a color
    (scale <buf> <0.0..1.0> [start] [end])     multiply brightness of the range
    (invert <buf> [start] [end])               x -> 255 - x on all four channels (inverts a seg mask)
    (copy <dst> <src> [start] [end])           dst[i] = src[i]
    (screen <dst> <src> [start] [end])         255 - (255-d)(255-s)/255 per channel
    (add <dst> <src> [start] [end])            min(255, d + s)
    (subtract <dst> <src> [start] [end])       max(0, d - s)
    (lighten <dst> <src> [start] [end])        max(d, s) per channel
    (darken <dst> <src> [start] [end])         min(d, s) per channel
    (multiply <dst> <src> [start] [end])       d*s/255 (white neutral, black masks)
    (mix <dst> <src> <0.0..1.0> [start] [end]) crossfade d -> s
    (grad-map <dst> <src> <palette> [s] [e])   gradient map: src luma (77R+150G+29B)/256 picks a color along the
                                               palette (first at 0, last at 255, linear blend); dst's value ignored
    (fill-pat <dst> <pat> [phase] [stretch] [mirror] [s] [e])  repeat pattern across the range: mirror = ping-pong
                                               (ABCCBA), stretch k = each pixel k times, phase + moves right (like rotate)
    (fill-grad <buf> <palette> [start] [end])  lerp through palette colors across the range (0 colors no-op, 1 = fill)
    (fill-rand <buf> <color> <min> <max> [s] [e])  color scaled by random [min, max] per pixel, every frame
    (fill-pick <buf> <c1> <c2> <p> [s] [e])    per pixel: c2 with probability p else c1, every frame
    (set <buf> <index> <color>...)             write 1..8 colors from index on; returns buf
    (get <buf> <index>)                        color at index (black outside the view)
    (rotate <buf> <amount> [start] [end])      rotate range right (+) / left (-); within 0.01 of an int: whole
                                               pixels, else sub-pixel (linear blend). In place.
    (if <cond> <then> [else])                  lazy; value of the taken branch (statement without else)
    (and <a> <b>...) (or <a> <b>...) (not <a>) booleans, short-circuit, 2..10 args
    (lt a b) (gt a b) (le a b) (ge a b)        numbers -> boolean
    (eq a b) (ne a b)                          numbers, colors, booleans or buffers -> boolean
    t  nil                                     boolean literals
    (+ a b ...) (- a [b]) (* a b ...) (/ a b)  2..10 args for + and *; (- x) negates; int/int floors
    (mod a b) (abs x) (clamp x lo hi)          ints and decimals; mod takes the divisor's sign
    (min a b ...) (max a b ...)                2..10 numbers
    (floor x) (round x)                        decimal -> int (for indices/ranges)
    (sin deg) (cos deg)                        degrees -> decimal -1..1
    (tri deg)                                  triangle wave, same period/phase as sin, exact (no table)
    (lerp a b t)                               numbers: a + (b-a)*t, t unclamped (int only if all ints);
                                               two colors: px_mix with t clamped 0..1 -> color (bind_lerp)
    (rand) (rand hi) (rand lo hi)              decimal [0,1) / [0,hi) / [lo,hi), limits in either order; int if
                                               all ints; channel rng, not folded; `_` rejected at compile time
    (env <name>)                               int: frame hour minute second year month day doy intensity npix
                                               (npix: pixels of the channel the program runs on; automation: 1)
    (def <name> <value>)                       top level / function body top level; usable as a value afterwards
    (defn <name> (<p:type>...) <body>...)      function; types int decimal color bool buf str; last form = result;
                                               callable anywhere in the program; self call only as the last step
    (fn (<p:type>...) <body>...)               function in place, only where one is passed; sees the variables around it
    (map-px <dst> <src> <f> [start] [end])     dst[i] = (f src[i] i); f: (c:color) or (c:color i:int) -> color
    (repeat <n> <f>)                           f with i = 0..n-1 (f: (i:int) or ()); nothing if n <= 0
    (seg7 <buf> <num> [layout] [hstride] [vstride] [digits])  7-segment mask: a display wired as one strip, segments in
                                               layout order (default abcdefg; optional p = 1-px point), a d g = hstride
                                               px, b c e f = vstride px; number right-aligned with blanks, dashes if it
                                               doesn't fit; lit = #FFFFFFFF, off = black; pixels after the last digit
                                               untouched; up to 16 digits; num may also be a string (text: left-aligned,
                                               points join the digit before); glyphs dsl_seg_glyph
    (seg9 <buf> <num|str> [layout] [hstride] [vstride] [digits])  7 segments + centre verticals j (top) m (bottom);
                                               default layout abcdefgjm; j m are vstride px
    (seg14 <buf> <num|str> [layout] [hs] [vs] [ds] [gs] [digits])  starburst: a b c d e f g1 g2 h j k l m n (p);
                                               ds = diagonals h k l n (default vs), gs = g1 g2 (default (hs+1)/2)
    "text"                                     string literal (one line, <= 255, escapes \" \\)
    (str-num <n> [width] [decimals] [zero])    int/decimal -> string; right-aligned to width (0..32) with blanks or
                                               zeros; decimals 0..4 (default 0 for ints, 2 for decimals), rounded
    (str-time <format>)                        env time: %H %k %I %l %M %S %p %Y %y %m %d %e %j %%
    (time-gt|ge|lt|le|eq <t>) (time-in <from> <to>)  time of day vs "HH:MM[:SS]" or minutes after midnight (int:
                                               minute, decimal: second; wraps at 24 h; negative = nil), at the
                                               precision given; time-in end exclusive, wraps past midnight
    (date-gt|ge|lt|le|eq <d>) (date-in <from> <to>)  today vs "MM-DD" (every year) / "YYYY-MM-DD"; date-in both
                                               ends included, MM-DD wraps past New Year; all nil while timeset is 0
    (str-cat <a> <b>...)                       2..10 strings or numbers (numbers as str-num's default); cut at 255
    (str-sub <s> [start] [end])                part of s, range rules; (str-len <s>) -> int
  Automation program only (compile error elsewhere; it may use G0..G7 and env, not X Y Z O):
    (intensity [ch])                           global / channel intensity now, decimal 0..1
    (set-intensity <ch|_> <0..1> [fade s])     _ = global; fades frame by frame on the board
    (play <ch> <name>)                         run a library program on the channel (name checked at compile time)
    (stop <ch>)                                the channel goes dark
    (io-set <out|name> <t|nil|0..1>)           GPIO output: switch it, or a PWM level (binary: > 0 = on)
    (io <out|name>)                            where the output is now, decimal 0..1
`_` = use the default for an optional argument, e.g. `(slice (buf O) _ _ -1)` reverses O.
Console: `version` (what runs, slot, unconfirmed update?, other slot), `prog <ch> <source>` (or `prog <ch>` alone: several lines, end with `.`), `list`, `env [intensity N | time YYYY-MM-DD HH:MM:SS]`, `buf <ch> <id> [start] [count]`, `stat [reset]`,
`autoexec [run | clear | paste | <source>]`, `lib [show|rm <name> | set <name> [source] | save|run <name> <ch>]`, `intensity [<ch|all> <0..1> [fade s]]`, `automation [<source> | paste | clear | run | every <s>]`, `chan [<n> <gpio> <npix> [wire] | <n> off]`, `gamma [<ch> <1.0..3.0 | off>]`, `dither [<ch> <0..4>]`, `white [<ch> max <0..100> | <ch> mode <direct|extract|boost> | <ch> point <#RRGGBB> [ratio]]`, `output [refresh <1..4> | parallel <on|off>]`, `io [<n|name> <on|off|0..1> | config <n> <gpio> <binary|pwm> [freq=Hz] [invert] [name=x] | config <n> off]`, `wifi [<ssid> [pass] | off]`, `ntp [server]`,
`tz [POSIX TZ]`, `loc [<lat> <lon> | off]`, `save`, `config [erase]`, `reboot`.

## Status / known unverified parts
Host tests: 4483 checks, 0 failures (macOS clang and gcc). **Builds cleanly with ESP-IDF v6.1 for esp32, flashed and running; RGB color order confirmed on the WS2811 string.**
- Toolchain: `source ~/.espressif/tools/activate_idf_v6.1.sh` (EIM install).
- `led_strip` is pinned to `^3.0.0` (3.x uses `color_component_format`; 2.x used `led_pixel_format`). Color order is the per-channel wire format (it was `LED_COLOR_FORMAT` in `config.h` until 2026-10-18).
- `main` requires `esp_driver_rmt` (not the legacy `driver` component).
- Clock: SNTP once WiFi is up; otherwise 1970-01-01 at boot until set with `env time`.
- WiFi station connect + SNTP time sync: **verified working on the original ESP32 (Xtensa)**, 2026-10-14, and on the
  XIAO ESP32-C6 (RISC-V), 2026-10-15.
- `save`, `config` and `reboot`: **verified working on the original ESP32**, 2026-10-14.
- `chan` and `tz` (e.g. `tz MST7MDT,M3.2.0,M11.1.0` for US Mountain): **verified working**, 2026-10-15.
- Autoexec and `config erase`: **verified working**, 2026-10-15.
- `fill-pat` with all options (phase, stretch, mirror, range) and variables (`def`): **verified on hardware**, 2026-10-15.
- Serial JSON API (incl. 1460-char multi-line program, after the USB RX buffer fix): **verified on the XIAO ESP32-C6**,
  2026-10-15, `tools/apitest.py` 11/11, and on the ESP32 over UART (11/11). HTTP API after the `api.c` refactor and the
  CORS preflight (`OPTIONS` -> 204 with Access-Control-Allow-*): **verified on the ESP32**, 2026-10-15.
- All BUILD.md variants **verified on hardware** by the user, 2026-10-18 (ESP32 2 MB WiFi / no WiFi / 4 MB OTA, C6 RMT,
  C6 PARLIO), and C6 no WiFi after the arena fix (it crashed in app_main: heap corruption, `tlsf_free`; see the arena
  rule): **verified**, 2026-10-18.
- Wire formats (RGBW phase 1, 2026-10-18): host-tested, builds for every variant, **verified on hardware**, 2026-10-18
  (WS2811 strings unchanged with the default `RGB`; Govee S14 with `BRG WWW`).
- W spread + white limit (RGBW phase 2, 2026-10-18): host-tested (all W values with gamma: exact totals, even spread,
  monotonic; limit; dither averages; one-W path unchanged), builds for every variant, IDE checked against the mock
  board; **verified on hardware** (Govee S14), 2026-10-18.
- White modes (RGBW phase 3, 2026-10-18): host-tested (extract/boost, white point, ratio, limit, light preserved),
  builds for every variant, IDE and preview checked against the mock board; **verified on hardware** (Govee S14),
  2026-10-18.
- SK6812 RGBW (RGBW phase 4, 2026-10-22): wire `GRBW` and other 4-letter orders **verified on hardware** by the user.
- `grad-map` (2026-10-16): host-tested, in the IDE reference/preview, **verified on hardware**, 2026-10-17.
- Gamma correction (2026-10-17): host-tested, builds for esp32 / C6 RMT / C6 PARLIO, IDE checked against the mock board;
  `tools/apitest.py` has gamma checks; **verified on hardware**, 2026-10-17.
- Dither (2026-10-17): host-tested, builds for esp32 / C6 RMT / C6 PARLIO, IDE checked against the mock board;
  `tools/apitest.py` has dither checks; **verified on hardware**, 2026-10-17.
- `subtract` blend (2026-10-17, added as `sub`, renamed the same day to the standard blend-mode name): host-tested
  (exhaustive byte math), in the IDE reference/preview, **verified on hardware**, 2026-10-17 (as `sub`; same code).
- `tri`, `lerp`, `rand` (2026-10-17): host-tested (values, types, saturation, rand bounds and spread), in the IDE
  reference/preview, **verified on hardware**, 2026-10-17.
- Multi-line console entry (2026-10-17): builds for esp32 / C6 RMT / C6 PARLIO; **verified on hardware**, 2026-10-17
  (serial UART, serial USB-JTAG and telnet each have their own line reader).
- OTA + IDE firmware updates (2026-10-17): network update (OTA) **verified on the XIAO C6**, 2026-10-17. IDE USB
  flashing worked on hardware but the board needed a manual power cycle: esptool-js 0.7.0's after('hard_reset') only
  drops RTS (no pulse when it is already low), so FlashDialog does its own reset (DTR low, RTS high 200 ms, RTS low,
  like Python esptool); **verified on the C6**, 2026-10-17 (restarts into the new firmware by itself). Flashing a blank
  board from Devices confirmed reachable by the user. Rollback (power-cycle within 30 s) and USB flashing of the ESP32
  (UART auto-reset) not tried yet.
  Flashing a blank board needs no connection: Devices dialog footer (also on the Add form) and the Board panel's
  not-connected state open the flash dialog (before, the button only showed in the device list, which a first run
  skips).
- Program storage + library (phase 1 of automation, 2026-10-18): builds for every variant (all partition tables have
  `progs`), IDE checked against the mock board, `tools/apitest.py` has library checks; **verified on the XIAO C6**,
  2026-10-20 (full USB flash to the new table; ESP32 not tried yet).
- Per-channel intensity + fades (phase 2 of automation, 2026-10-20): faders host-tested, builds for every variant, IDE
  checked against the mock board, `tools/apitest.py` checks a 1 s fade; **verified on hardware**, 2026-10-20.
- `version` command, firmware state/other slot in status, `(env npix)` (2026-10-22): host-tested (npix), builds for
  every variant; OTA rollback **verified by the user on hardware 2026-11-04**; `version`, the status fields and
  `(env npix)` **confirmed working by the user on hardware 2026-11-09**.
- `seg7` and `seg9` mask generation **verified by the user on hardware 2026-11-04** (host-tested: layouts, strides,
  digits, right-aligned blanks, overflow dashes, points, glyphs, text; in the IDE reference; builds for every variant).
- Strings, `str-*`, `seg14` (2026-10-31): host-tested (literals, escapes, errors, views, formatting, str-time codes,
  scratch faults; 14 layouts, strides, font, points), fonts checked as ASCII art, in the IDE reference (examples compile
  and run), string highlighting, builds for every variant. All `str-*` functions (`str-num`, `str-time`, `str-cat`,
  `str-sub`, `str-len`) and quoted string literals (including one-line console source with strings, the raw-line wrap)
  **verified by the user on hardware 2026-11-04**. `seg14` **verified by the user on hardware 2026-11-09 with a single
  digit** (multi-digit displays not tried yet).
- Sun and moon (0.16.0, 2026-11-10): host-tested against the US Naval Observatory almanac (7 places x 7 dates incl. polar
  days, 150 moon phases): worst sun event 37 s, illumination 0.32%, phase angle 0.03 degrees; DST changeover days,
  mismatched time zone, poles. WASM + IDE tests, checked by Claude in the IDE preview, builds for every variant.
  **Verified by the user on an ESP32 2026-11-11**: sunrise/sunset for the user's location match almanac.com, location
  survives reboot, a program using `(env sunset)`/`(env sunrise)` switches correctly with the time zone. One
  calculation takes ~18 ms on the ESP32 (once a minute, in the low-priority automation task, so no frame cost).
  Not run on hardware: the API `loc` field (`tools/apitest.py` check only syntax-checked).
- Date/time comparisons (0.19.0, 2026-11-12, `natives.c` "date and time comparisons", `bind_time_cmp`): host-tested
  (`t_time_cmp`: precision, numbers incl. sunset -1 and 24 h wrap, time-in/date-in wrapping, leap days, run-time
  strings as faults, nil without a clock, compile errors), reference entries (category Time) whose examples compile
  and run, builds for every variant. **Not run on hardware.**
- Rename to KTSN LED (0.18.0, 2026-11-11): host tests, IDE tests (incl. `storageRename.test.ts`, old config/library
  files, old image name), all six variants build from the renamed folder with configs identical to before apart from
  option names; Docker IDE checked by Claude (title, saved state copied to `ktsn-led.ide.*`). **Verified by the user**
  (2026-11-11) on the ESP32 2 MB nowifi, ESP32 4 MB WiFi and C6: flashed, new name reported. Not separately reported:
  OTA of a `ktsn-led` image, `X-Ktsn-Result`.
- Time-of-day env values (0.17.0, 2026-11-11): `ms weekday timeset daylight daymin daysec dayfrac hourfrac minfrac`.
  Host-tested (`t_env_time`, sun altitude at every almanac event within 0.002 degrees and on the right side 2 minutes
  either way, one side all day on polar days), WASM + IDE tests, builds for every variant. **Verified by the user on an
  ESP32 2026-11-11**: `env` prints the new values, the minfrac/daylight/weekday test program shows the expected output,
  `loc off` gives daylight -1, moving `loc` to the opposite longitude flips daylight. `daylight` ignores the time zone
  by design (it is the real sun). Known, accepted by the user: after a `tz` change, sunrise/sunset/dawn/dusk keep the
  old clock minutes until the next minute (sky_tick only recomputes on minute or location change); not worth fixing.
- User functions step 2 / `use` (0.15.0, 2026-11-09): host-tested (`t_use`), WASM + IDE tests (`uses.test.ts`, core `use`
  tests, reference example), checked by Claude in the IDE offline (local library resolves, unknown name and name in two
  folders give the right errors, autocomplete of library names and library functions), builds for every variant.
  **Confirmed by the user on an ESP32 2026-11-09 (0.15.0 firmware):** a channel using a library shows the right colour,
  changing the library on the board updates the running channel without resending it, a breaking change and `lib rm` of
  a library in use are both refused. Not tested on a C6. **IDE connected path confirmed by the user 2026-11-10** on the
  rebuilt Docker IDE: `use` against the board's library, unknown library error, upload of a library with a program using
  it.
- User functions step 1 (0.14.0, 2026-11-09): host-tested (`t_functions`: calls, hoisting, every param type, locals,
  tail loops incl. nested ifs and the budget stop, map-px ranges/strides/overlap/in place, fn capture of defn params
  and top-level vars, repeat with outer accumulators and nesting, scratch give-back and UF_KEEP, depth limit, ~45
  compile errors), WASM + IDE tests (reference examples compile and run, userFunctions), checked by Claude in the IDE
  (preview runs a defn + map-px + repeat program, hover on a user function, error for a non-tail self call, reference
  page), builds for every variant. **Run by the user on an ESP32 2026-11-09** (0.14.0, -Og): both test programs, 0
  faults; a map-px program of 369 nodes on 32 px took ~870 us (~2.4 us/node, 27 us/px); the depth-32 program used
  ~5.7 KB of stack, matching the frame-size model. 0.14.1 (stack estimate, loop time limit, -O2): host-tested, builds
  for every variant; **run by the user on an ESP32 2026-11-09**: the map-px program ~720 us (~1.95 us/node, 22 us/px,
  -O2 gained ~17%), 1980 B of stack used against an estimate of 1792 + 160 B task frames; the depth program compiles
  with the predicted `stack est 6384 B`. `map-px`/`repeat`/`defn` output on the LEDs **confirmed right by the user on an ESP32 2026-11-09**. The time limit has not been triggered on hardware.
- 16-bit output intensity (0.13.0, 2026-11-09): host-tested (px_scale16 against a per-byte reference and against
  px_scale for whole Q8 values; ordering; dither averages at fine values; W path; global x channel; a 1-minute fade near
  black takes >= 1000 output steps), WASM and IDE tests, builds for every variant; **verified by the user on hardware
  2026-11-09** (slow fades near black are smooth).
- `invert` (0.10.0, 2026-11-04): host-tested (all four channels, range, `_`, negative, empty, strides, seg mask),
  in the IDE reference, builds for every variant; **verified by the user on hardware 2026-11-04**.
- GPIO outputs (2026-10-30): io/io-set host-tested (names, slots, values, compile errors, faults), builds for every
  variant, IDE Outputs section checked against the mock board; after two fixes (reserved-pin snapshot, LEDC duty
  calls) **verified on hardware by the user** (XIAO C6: binary, PWM, automation), 2026-10-31.
- IDE local library step 1 (0.11.0, 2026-11-04): unit-tested (folders, programs, search, export/import, storage),
  checked by Claude in the browser (save a tab into a new folder, edit + Ctrl/Cmd+S, search, import with keep both,
  move a selection with the open tab following, reload); **confirmed working by the user 2026-11-09**.
- IDE local library step 2 (0.12.0, 2026-11-09): unit-tested (`board.test.ts`: upload plan/outcomes/stop on lost
  connection, board entries, default folder, URL fetch errors), checked by Claude in the browser against a mock HTTP
  board (upload with duplicate/broken/refused/same/different, skip and replace; From board into boards/mock and the
  states at the top level; Board panel To local… preselects one; URL import incl. 401 and unreachable messages);
  **confirmed working by the user on hardware 2026-11-09**.
- IDE colour picker + favourites (2026-10-22): the user saw no picker on swatch clicks: it opened and closed at once
  (see "IDE colour picker"); fixed and checked with real mouse clicks in the browser; **confirmed working by the user**,
  2026-10-22.
- IDE palette tab (2026-10-22): **confirmed working by the user**, 2026-10-22.
- IDE palette in-code shortcut (2026-10-22): **confirmed working by the user**, 2026-10-22.
- Automation (phase 3, 2026-10-20): host-tested (context rules, G-only, compile-time library names, `_` = global, fades,
  faults), reference examples compile and run as automation, builds for every variant, IDE checked against the mock
  board, `tools/apitest.py` checks refusal and periodic runs; **verified on hardware**, 2026-10-20.
- Output refresh (1..4x) and parallel RMT sending (2026-10-17): builds for esp32 / C6 RMT / C6 PARLIO, IDE checked against
  the mock board, `tools/apitest.py` has output checks. Output refresh **verified on hardware**, 2026-10-17; parallel
  RMT sending (`output parallel on`) **verified on hardware**, 2026-10-18.
- Command layer refactor (`cmd.c`), `token`, and the network CLI: **verified working on the ESP32**, 2026-10-14.
- HTTP API, two-pass exact-size program compile, shared validators: **verified working on the ESP32**, 2026-10-14.
- mDNS + hostname: **verified working on the ESP32**, 2026-10-15. Image with everything on: ~992 KB of 1.5 MB.
- **WiFi LED glitching: solved** (ESP32, 2026-10-14). Cause: WiFi interrupt latency delaying RMT refill interrupts
  (the original ESP32 has no RMT DMA; `nowifi` builds never glitched). Fix, both needed and both default on ESP32 WiFi builds
  (`nowifi` defaults to 64 symbols = 8 channels):
  `CONFIG_KTSN_WIFI_PS_NONE` (WiFi power save off) and `CONFIG_KTSN_RMT_MEM_SYMBOLS=256` (4 of 8 RMT blocks per
  channel, refilled in 128-symbol halves). Confirmed glitch-free with smooth scrolling. `CONFIG_RMT_TX_ISR_CACHE_SAFE`
  made no difference. Cost: max 2 LED channels per WiFi build (512 / 256), accepted by the user; for more channels the plan
  is a WiFi-less build bridged over serial to a second ESP32 that hosts WiFi. Fallback if it ever returns: led_strip's
  SPI backend (SPI DMA, no refill interrupts; max 2 SPI channels on ESP32).
- Console REPL works on hardware. `esp_console` splits lines into words and silently drops words past
  `max_cmdline_args` (default 32), so `console.c` sets it from the line length so it can't truncate.
Still unverified on hardware:
1. `led_strip_refresh` blocks until the frame is sent (~30 us/pixel). With several long strings the sum can exceed the 16.6 ms
   frame budget. Watch `stat` (output time, missed ticks). `output parallel on` sends RMT strings at the same time.
2. `led_model` is `LED_MODEL_WS2812` (800 kHz). led_strip 3.x also has `LED_MODEL_WS2811`; try it if the string misbehaves.

## Next steps (planned)
- Time comparisons, inputs, audio (plan agreed with the user 2026-11-12; **don't push to GitHub or run CI until all
  three phases are done and tested by the user**). One handover + bump per phase:
  - Phase 1, 0.19.0 (done 2026-11-12, see Status): `time-gt/ge/lt/le/eq <t>`, `(time-in <from> <to>)` (wraps midnight, end exclusive);
    `date-gt/ge/lt/le/eq <d>`, `(date-in <from> <to>)` (wraps the year end, end inclusive). Times: a `"HH:MM[:SS]"`
    literal (checked at compile time) or a number = minutes after midnight (decimal for seconds; negative, e.g.
    `(env sunset)` without a location, makes the test nil). `time-eq` matches at the precision given (the minute for
    "HH:MM" and numbers, the second for "HH:MM:SS"). Dates: `"MM-DD"` (every year) or `"YYYY-MM-DD"`. All of them
    are **nil while the clock isn't set** (`timeset` 0). Channel programs and automation; not folded.
  - Phase 2, 0.20.0: io modes `input` (pull=up|down|none, invert, debounce, opt-in `trigger` = run automation right
    after a debounced change) and `analog` (ADC1 only: ESP32 GPIO 32..39, C6 GPIO 0..6; continuous/DMA sampling in a
    background task, ring of recent readings). `(in <name>)` -> bool, `(analog <name> [samples 1..256, default 16])` ->
    decimal 0..1 (average of the latest readings), in channel programs and automation; cached, cheap per call.
    Console/API/IDE Board panel; IDE preview gets a toggle per input and a slider per analog input.
  - Phase 3, 0.21.0 (nice to have; prototype first, measure CPU on both chips, stop if it doesn't fit): io mode
    `audio` on an analog pin (analog mic module, MAX9814/MAX4466): ~8 kHz, 256-point integer FFT every 32 ms, a few bands
    with automatic gain; env `band0..band7` (0..1), `level`, `beatms` (ms since the last beat), `beats` (count). IDE
    preview: a fake beat at a chosen BPM.
- Sun and moon in `env` (done 2026-11-10, 0.16.0, see "Sun and moon" and Status; decisions made with the user, no ephemeris
  tables, computed from NOAA solar equations and Meeus's low-accuracy lunar terms): `loc <lat> <lon>` console command,
  saved in settings and in the API config. New `env` values: `sunrise`, `sunset`, civil `dawn`, `dusk` (minutes after
  local midnight), `moon` (phase angle 0..359), `moonlit` (percent). `-1` when there is no location or no such event that
  day (not a fault). Computed about once a minute and cached, never per frame; code shared with the WASM build so the
  preview matches; host tests against published almanac times; cover polar days, DST changeover days, unset clock.
  Possible future enhancement, deliberately skipped for now: moonrise and moonset.
- User functions (plan agreed with the user 2026-11-09): global function table per program + shared library, declared
  param types, self tail calls only, first application natives map-px + fn + repeat (reduce/fold not chosen).
  - Step 1 (done 2026-11-09, 0.14.0, see "User functions"): defn, fn, locals, tail calls, cycle and depth checks,
    map-px, repeat, IDE reference/autocomplete/hover. Hardware checkpoint done 2026-11-09 (see Status); it led to the
    stack estimate and loop time limit of 0.14.1.
  - Step 2 (done 2026-11-09, 0.15.0, see "Libraries"; confirmed on an ESP32, see Status): `(use name)` pulls the defns of a board library program (only defn/use forms; nested use, cycles
    are errors) into the program at compile time (copy, no cross-program pointers; host hook to read library source).
    Saving/deleting a library program re-compiles the running channel programs that use it and is refused with that
    program's error if any would break. IDE resolves `use` against the board's library (board tabs / connected) or the
    local library by name (a name in two folders = error); uploads send used libraries first.
- Automation, library, per-channel intensity (plan agreed with the user 2026-10-18; all three phases done and verified):
  - Phase 1 (done, verified on the C6): `progs` partition + program library (see "Program storage", "Program library").
  - Phase 2 (done and verified 2026-10-20, see "Intensity"): per-channel intensity with fades.
  - Phase 3 (done and verified 2026-10-20, see "Automation"): the automation program.
- Segment displays + strings + GPIO outputs (plan agreed with the user 2026-10-30):
  - Segment masks: a display wired as one strip, segments in the order a layout name gives (`abcdefg`, A top,
    clockwise, G middle; optional `p` decimal point), horizontal segments `hstride` px, vertical `vstride` px, digits
    one after another. Output is a mask (lit = #FFFFFFFF, off = black) to combine with blends (the user's wall clock).
    Numbers right-aligned with blanks; the full displayable 7-segment character set (for strings later).
  - Done 2026-10-30: `seg7` and the GPIO outputs (see "GPIO outputs"); outputs verified 2026-10-31; seg7/seg9 verified 2026-11-04, seg14 verified on a single digit 2026-11-09.
  - Done 2026-10-31: steps 2 and 3 together (the user asked for the other segment modes): strings, `str-num`,
    `str-time`, `str-cat`, `str-sub`, `str-len`, `seg9`, `seg14` (see "Strings", "Segment displays").
  - Order: 1 `seg7` with numbers; 2 a string type (literals; values are a pointer + length; runtime strings in the
    per-run scratch) + `seg14` (standard font, `dstride` for diagonals, single-letter layout legend); 3 formatting
    (`str-num`, `str-time`, ...) + `seg9` (custom approximate table).
  - GPIO outputs: up to 8, each gpio + mode binary/pwm (LEDC, freq default 1 kHz) + invert + optional name; checked
    like LED pins; apply at once; always off at boot; console `io`, JSON config/status/patch, IDE Board panel,
    automation `(io-set <n|name> <t/nil | 0..1>)` / `(io <n|name>)` (names checked at compile time; not in channel
    programs). C6 default: GPIO 19 and 20 binary (the user's relay drivers).
- IDE local library (plan agreed with the user 2026-11-04): named programs kept in the IDE (localStorage), in folders.
  - Decisions: names follow the board library's rules (`LIB_NAME`, unique per folder) so upload is 1:1 and names work
    with `play`; channel programs only (no automation/autoexec entries); two steps the user tries in between.
  - Step 1: storage (`ktsn-led.ide.library`, format version 1), folder tree (paths, empty folders allowed; create,
    rename, move, delete), editor tab kind `local` (Ctrl/Cmd+S = preview + save to the library, dot = unsaved),
    *Save to local library* from other tabs, search (case-insensitive substring of name, folder or code; flat results
    with the first matching line), multi-select (move, export, delete), export to JSON
    (`{format: "ktsn-led-library", version: 1, folders, programs: [{folder, name, source}]}`), import from a file with a
    preview dialog (target folder; conflicts overwrite / skip / keep both as name-2).
  - Step 2 (done 2026-11-09, 0.12.0, see "IDE local library"): upload a selection to the connected board's library (names checked, duplicates across folders refused,
    existing names: overwrite / skip / cancel, per-program results), save one or all board library programs and
    channel programs into the local library (default folder `boards/<board name>`), import from a URL (direct fetch:
    needs CORS on the server; no proxy in the container).
- RGBW / interlaced output (plan agreed 2026-10-18; all four phases done and verified):
  - Phase 1 (done 2026-10-18, see "Wire formats"): per-channel wire format, W passed through as is.
  - Phase 2 (done 2026-10-18, see "Wire formats"): W spread over several W outputs (nw x 255 steps; W 255 = all at
    255, decided with the user), dither on the fraction, per-channel `wmax`.
  - Phase 3 (done 2026-10-18, see "Wire formats"): white point + ratio, modes direct / extract / boost, preview draws W.
  - Phase 4 (done and verified 2026-10-22): SK6812 RGBW works with a 4-byte wire format (`GRBW`), and other byte
    orders of the same 4 letters work too (confirmed by the user). No code changes were needed beyond phases 1-3.
  - Govee S14 (user's string), measured by the user 2026-10-18: pairs of WS2811-compatible chips per bulb, wire
    `BRG WWW`: chip 1 drives RGB in B, R, G order; chip 2 drives white on all three outputs, each the same and additive
    (three identical LEDs or paralleled drivers; units are sealed).
- Web IDE (phases 1-5 done): localhost SPA in Docker; DSL core compiled to WebAssembly for exact compile errors and
  pixel preview; transports HTTP (CORS) and Web Serial (serial JSON API). Phase 1 (firmware: api.c, serial JSON API,
  CORS) done and verified on ESP32 + C6. Phase 2 (Docker build, WASM core, editor with live errors, preview with
  stepping and simulated env) done 2026-10-15, tried and approved by the user.
  Phase 3 (devices: HTTP + Web Serial transports, device manager, send-on-save, board panel, config download/restore)
  done 2026-10-15: unit-tested (simulated serial board incl. chunked frames, mocked fetch, restore rules) and checked in
  a browser against a mock HTTP board; **verified by the user on the ESP32 and the C6, over serial and HTTP, in Chrome
  and Brave**, 2026-10-15. Phase 4 (language reference: Reference tab, editor hover + F1, signatures in autocomplete)
  done 2026-10-15, checked in a browser by Claude; tried and approved by the user. Phase 5 (editor tabs, resizable
  side pane) done 2026-10-16, unit-tested and checked by Claude in a browser against the mock board; **verified by the
  user**, 2026-10-16.
- **IDE tabs** (`ide/web/src/tabs/tabs.ts`, localStorage `ktsn-led.ide.tabs`): a tab is `program` (deviceId + ch) or
  `autoexec` (deviceId), deviceId null = preview only. `boardSource` = what the board runs (set on send, and refreshed
  from every config read of the connected board); the unsent dot is `source !== boardSource`. Ctrl/Cmd+S saves the
  active tab (program: preview + send; autoexec: preview autoexec, remembered as `ktsn-led.ide.previewAutoexec`, + patch).
  Only one board is connected at a time (header); a tab for another board offers Connect, which switches the header.
  The editor keeps one CodeMirror `EditorState` per tab (undo history per tab). First load migrates the old single
  editor (`ktsn-led.ide.source`) and preview autoexec. Side-pane width: `ktsn-led.ide.sideWidth` (300 px to 70%).
- **Reference rules:** `ide/web/src/reference/entries.ts` is the manual. Every native needs an entry; its `args`
  (optional / `many`) must give the native's min/max arg counts; its example must compile and run 3 frames without a
  fault (all enforced by `reference.test.ts`). When adding or changing a native, update the entry in the same change.
- Preview autoexec (2026-10-16): the old preview-only box wasn't remembered or sent, which looked like "save doesn't
  send the autoexec". Replaced in phase 5 by autoexec tabs (see IDE tabs).
- HTTP connections are stateless: the Board panel's status poll reports to `conn.pollResult`; 3 network failures in a
  row drop the connection with the reason (found 2026-10-15: before that a vanished board looked connected forever). Defaults chosen with the user: React, no token over serial, send-on-save updates
  the running program only (separate "save to flash"), Brave/Chrome.
- **IDE rules:** the WebAssembly core must stay byte-identical in behaviour to the boards: it builds the same
  `components/dsl` sources, `main/gamma.c`, `main/dither.h` and `main/wire.*` (also listed in ide/Dockerfile and .dockerignore), and `ide/wasm/ktsn_led_wasm.c` repeats GLOBAL_BUF_PIXELS (256) and CHANNEL_SCRATCH (2048)
  from main/config.h (keep in sync). After a core change: host tests, `npm run wasm`, `npm test`. emsdk is pinned
  (4.0.15, `-arm64` tag on Apple Silicon); npm deps are pinned exactly in ide/web/package.json.
- **IDE transports** (`ide/web/src/devices`): `Transport` interface, HTTP and serial implementations must behave the
  same (failing console commands return their output, compile errors are ApiError 400, patch field errors in
  `ApiError.fields`). Serial: open the port and never touch DTR/RTS; handshake sends `exit` then `api`; one request at
  a time. Config restore: programs only for running channels; channel removals as separate patches, highest first
  (JS can't order integer keys descending in one object).
