# Building KTSN LED

Every supported board and build variant, plus the host tests and the IDE. Run everything from the repository root
(`ktsn-led/`) unless noted.

## Toolchain

ESP-IDF v6.1 (installed with EIM). Activate it once per shell:

    source ~/.espressif/tools/activate_idf_v6.1.sh

Serial ports used below (adjust to yours; `ls /dev/cu.*` lists them):

| Board | Port |
|---|---|
| ESP32 dev board (USB-UART bridge) | `/dev/cu.usbserial-0001` |
| Seeed XIAO ESP32-C6 (native USB) | `/dev/cu.usbmodem2101` |

## How variants work

Each variant has its own build directory and its own `sdkconfig`, made from `sdkconfig.defaults` plus presets:

| Preset | Effect |
|---|---|
| `sdkconfig.defaults` | always first: 2 MB flash, one 1.5 MB app partition, WiFi + NTP, network CLI, HTTP API, mDNS |
| `sdkconfig.flash4mb` | 4 MB flash, two 1.9 MB app slots: network firmware updates (`/api/ota`) with rollback |
| `sdkconfig.nowifi` | no WiFi, NTP, network CLI, HTTP API, mDNS (much smaller image; serial console and serial API only) |
| `sdkconfig.usbjtag` | console on the chip's own USB port (XIAO C6/C3 and other boards without a USB-UART chip) |
| `sdkconfig.parlio` | C6: drive all strings with one parallel DMA transfer (PARLIO) instead of RMT |

Presets only apply when a build directory's `sdkconfig` is created. After changing the preset list of an existing
variant, delete its `sdkconfig` (or the whole build directory) first.

All layouts keep the settings (NVS) at the same flash address, so saved settings survive switching variants. Switching
between the 2 MB layout and the 4 MB OTA layout needs one full USB flash (below), not a network update.

Every layout ends with `progs`, a 256 KB partition for program text (channel programs, autoexec, the program library);
added 2026-10-18 (the 4 MB app slots shrank from 1.94 to 1.81 MB for it). A board flashed before that needs one full
USB flash to get it; settings are kept and its stored programs move over at the first boot. Until then (e.g. after a
network update or `app-flash`) programs stay in the settings NVS and the library has little room.

## All variants at once, and release packages

    tools/build-variants.sh                    # every variant below (or name some: esp32c6 esp32-4mb ...; --list)
    tools/package-release.sh                   # dist/ktsn-led-<version>-<variant>.zip + dist/SHA256SUMS

`build-variants.sh` is the one list of variants (name, build directory, target, presets); CI
(`.github/workflows/ci.yml`: host tests, the IDE Docker build, every variant in `espressif/idf:v6.1`) uses it too.
Each zip holds what `idf.py flash` writes, so it flashes from the IDE or with `esptool ... write-flash @flash_args`.

Releasing: bump `version.txt` + CHANGELOG, commit, check a fresh clone builds (`git clone . /tmp/x`, then the two
scripts above, `make -C host`, `docker build -f ide/Dockerfile .`), tag `v<version>`, push the tag, and
`gh release create v<version> dist/*.zip dist/SHA256SUMS` with the CHANGELOG entry as notes.

## ESP32 (Xtensa, e.g. WROOM-32 dev board)

### 2 MB flash, WiFi (default) — verified on hardware

    idf.py build
    idf.py -p /dev/cu.usbserial-0001 flash monitor

Output: `build/ktsn-led.bin`. No network firmware updates (the image doesn't fit twice in 2 MB).

### 2 MB flash, no WiFi — verified on hardware

    idf.py -B build-nowifi -D SDKCONFIG=build-nowifi/sdkconfig \
           -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.nowifi" build
    idf.py -B build-nowifi -p /dev/cu.usbserial-0001 flash monitor

Up to 8 RMT channels (64 symbols each) instead of 2.

### 4 MB flash, WiFi + network updates — verified on hardware

    idf.py -B build-4mb -D SDKCONFIG=build-4mb/sdkconfig \
           -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.flash4mb" build
    idf.py -B build-4mb -p /dev/cu.usbserial-0001 flash monitor

For ESP32 boards with 4 MB flash (most dev boards; `idf.py -p PORT flash-id` or esptool shows the size).

## Seeed XIAO ESP32-C6 (RISC-V, 4 MB flash, native USB)

All C6 variants use the 4 MB OTA layout and the USB console.

### RMT output, WiFi + network updates — verified on hardware

    idf.py -B build-c6 -D SDKCONFIG=build-c6/sdkconfig -D IDF_TARGET=esp32c6 \
           -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.flash4mb;sdkconfig.usbjtag" build
    idf.py -B build-c6 -p /dev/cu.usbmodem2101 flash monitor

One LED channel with WiFi (RMT memory), default GPIO 22 (D4).

### PARLIO output, WiFi + network updates — verified on hardware

    idf.py -B build-c6p -D SDKCONFIG=build-c6p/sdkconfig -D IDF_TARGET=esp32c6 \
           -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.flash4mb;sdkconfig.usbjtag;sdkconfig.parlio" build
    idf.py -B build-c6p -p /dev/cu.usbmodem2101 flash monitor

All strings in one DMA transfer (immune to WiFi timing); defaults to two channels on GPIO 22 and 23 (D4, D5).

### XIAO antenna switch (RMT or PARLIO, WiFi)

    idf.py -B build-c6x -D SDKCONFIG=build-c6x/sdkconfig -D IDF_TARGET=esp32c6 \
           -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.flash4mb;sdkconfig.usbjtag;sdkconfig.xiaoc6" build

Variants `esp32c6-xiao` (`build-c6x`) and `esp32c6-xiao-parlio` (`build-c6xp`, with `;sdkconfig.parlio` before
`;sdkconfig.xiaoc6`): the same builds plus the XIAO ESP32-C6's RF switch between its ceramic antenna and the U.FL
connector. `antenna internal|external` (console), `antenna` in the API config and a selector in the IDE Board panel;
applies at once, `save` keeps it, set at boot before WiFi starts. GPIO3 (switch power, driven low) and GPIO14
(selection) are reserved, so channels and io slots can't use them; neither is on the XIAO's pins. Only for the XIAO:
on another C6 board these pins may be wired to something else. A network update can move a board between these and
the plain `esp32c6` builds (the saved choice is kept and applies again in a XIAO build).

### No WiFi (RMT or PARLIO) — verified on hardware

    idf.py -B build-c6-nowifi -D SDKCONFIG=build-c6-nowifi/sdkconfig -D IDF_TARGET=esp32c6 \
           -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.flash4mb;sdkconfig.usbjtag;sdkconfig.nowifi" build

Add `;sdkconfig.parlio` for PARLIO output. Without WiFi there are no network updates (the slots stay, unused).

## Other chips

Not tested. Other ESP-IDF targets should build the same way: set `IDF_TARGET` (e.g. `esp32c3`, `esp32s3`), add
`sdkconfig.usbjtag` if the board's USB goes straight into the chip, and `sdkconfig.flash4mb` for 4 MB flash. The
default LED GPIO for unknown chips is 0 (change it with `chan` on the console, or `DEFAULT_LED_GPIO` in
`main/config.h`). PARLIO only works on chips that have it (ESP-IDF v6.1: C5, C6, H2, H4, H21, P4, S31).

## Flashing and updating

| Task | Command |
|---|---|
| Full flash (bootloader, partition table, app; settings kept) | `idf.py -B <dir> -p <port> flash` |
| Serial monitor (exit with Ctrl-]) | `idf.py -B <dir> -p <port> monitor` |
| App only (same layout already on the board) | `idf.py -B <dir> -p <port> app-flash` |
| Erase everything, including saved settings | `idf.py -p <port> erase-flash` |
| Network update (4 MB OTA variants) | IDE Board panel → *Update over the network…*, or `curl -H "Authorization: Bearer <token>" --data-binary @<dir>/ktsn-led.bin http://<board>/api/ota` |
| Flash from the browser | IDE → Devices → *Flash firmware over USB…* → pick the build directory (Chrome) |

Note that `app-flash` on an OTA board writes slot `ota_0`; if the board runs from `ota_1`, use `flash` (it resets
the boot selection) or a network update.

The version is in `version.txt` (bump rules: CLAUDE.md "Versioning", changes: `CHANGELOG.md`); it shows in the IDE,
in `GET /api/status` and with the `version` command.

## Host tests (DSL core)

    cd host && make

gcc or clang with ASan and UBSan, `-Werror`. Run after every change in `components/dsl` or `main/gamma.c`.

## API test against a board

    python3 tools/apitest.py /dev/cu.usbserial-0001      # or /dev/cu.usbmodem2101

Uses the serial JSON API (pyserial from the ESP-IDF environment). Restores what it changes.

## IDE

Docker (http://localhost:8080; the build runs the IDE tests):

    ide/run.sh

Development (http://127.0.0.1:5173, live reload):

    cd ide/web
    npm install
    npm run wasm          # the DSL core as WebAssembly: after any change in components/dsl, main/gamma.c, main/dither.h
    npm run dev
    npm test
    npm run typecheck
