# KTSN LED IDE

A browser IDE for KTSN LED programs. The DSL core (`components/dsl`) is compiled to WebAssembly, so compile errors and
the pixel preview come from the same compiler and runtime as the boards.

## Run it (Docker)

    ide/run.sh                 # builds and starts the container (native Emscripten image on Apple Silicon)
    open http://localhost:8080

`docker compose -f ide/docker-compose.yml down` stops it. The port is bound to `127.0.0.1` only: localhost is a secure
context (needed for Web Serial later) and the IDE isn't exposed to the network. The image build runs the tests; a
failing test fails the build.

## Develop

    cd ide/web
    npm install
    npm run wasm               # builds src/wasm/generated/ktsn-led.mjs (local emcc, or the emsdk Docker image)
    npm run dev                # http://127.0.0.1:5173 with live reload
    npm test                   # vitest: the core under Node
    npm run typecheck

Rebuild the WebAssembly (`npm run wasm`) after changing anything in `components/dsl` or `ide/wasm`.

## Layout

- `wasm/ktsn_led_wasm.c`  C glue: one simulated channel + the 8 global buffers, compile/check/load/render/env.
- `wasm/build.sh`       emcc flags (single-file ES module, wasm embedded), shared by Docker and local builds.
- `web/src/core/`       typed core wrapper and simulated env; no DOM, reusable from Electron.
- `web/src/editor/`     CodeMirror 6: DSL highlighting, color swatches, autocomplete, live compile errors.
- `web/src/preview/`    60 Hz simulation: play/pause/step/reset, speed, buffer view, clock, intensity, gamma, dither, autoexec.
- `web/src/devices/`    board transports (HTTP, Web Serial + the serial JSON API), device store, config files.
- `web/src/tabs/`       editor tabs (targets, unsent check, remembered in the browser).
- `web/src/library/`    the local library: programs in folders kept in the browser, search, export/import files.
- `web/src/ui/`         tab bar, side-pane resizer, device manager dialog, board panel, reference panel.
- `web/src/reference/`  the language reference (entries.ts: every function + guide topics).

## Editor

- **Tabs.** Each tab has a target, shown in the bar under the tabs: *Preview only*, a board channel, or a board's
  autoexec (⚙). **+** opens a new program or autoexec tab; change a tab's target in its bar. A dot (●) marks a board tab
  whose text differs from what the board runs (checked whenever the board's config is read); closing such a tab asks
  first. Tabs, their text, the active tab and the side-pane width are remembered in this browser.
- Errors are underlined as you type, with the board's exact message (bottom bar).
- Ctrl/Cmd+S or **Run** saves the active tab:
  - program tab: loads it into the preview, and with *send on save* sends it to its board channel;
  - autoexec tab: becomes the preview's autoexec and runs at once, and with *send on save* is sent to its board
    (the board runs it at once; an empty tab clears the board's autoexec).
  **Send** in the tab's bar sends without saving to the preview (e.g. with *send on save* off).
- Switching to a program tab runs it in the preview. Buffers persist across runs, like `prog` on a board;
  **Reset** clears them, sets frame 0, runs the preview's autoexec (named under the stats) and reloads the program.
- Preview output (buffer O): the board's output stage (the same C code: gamma, intensity, dither for the frame),
  exactly the bytes a board sends. **Gamma** and **Dither** follow the board channel being edited; pause and **Step**
  to watch a dither cycle. **Refresh** (the board's output refresh, 1-4 sends per frame) shows the average of each
  frame's sends, which is what the eye sees, so faster refresh shows smoother dither. **Wire** and, for wire formats
  with W, **White** (mode), the white point colour × ratio and **max** follow the board channel too (or set them to
  simulate a string): W is drawn as light of the white point colour added to RGB, through the board's own mapping. **LED light** (on by default) shows those bytes the
  way the LEDs look: their light is linear and a screen's isn't, so an uncorrected board looks washed out here too,
  and gamma ~2.2 looks like the plain colour values. Turn it off to see the bytes sent. Other buffers show raw values.
- Drag the bar between the editor and the side pane to resize it (arrow keys when focused; double-click resets).
- Click a colour swatch in the code for a colour picker: the browser's picker for R G B, sliders and numbers for
  R G B **W**, the hex text, and 16 **favourite** slots shared by the whole IDE (☆ Save puts the colour in the first free
  slot; click a slot to use it, right-click to clear it). It edits the literal as you go (#RRGGBB, or #RRGGBBWW once W is
  set or if it already was); Enter or clicking elsewhere keeps it, Esc puts the original back.
- **Palette** tab: build a palette from colour stops (click a stop to select it, click again to edit it with the same
  picker; add / move / remove / reverse; favourites add a stop), see it as a gradient (exactly like `fill-grad` on a
  board) or a repeating pattern (`fill-pat`), and get the code that stores it in a G buffer: one `(set ...)` per 8
  colours on a slice, with an optional `(def name ...)`. *Insert at cursor* puts it in the active tab (the autoexec is a
  good home: G buffers persist), *Copy* copies it. Palettes can be saved by name (in this browser).
- Palettes already in the code get a **▦** marker (the shapes the Palette tab writes: `(set (slice (buf Gn) a b) 0 #…)`,
  with or without `(def name …)`, also split over several `set`s; the colours must fill the slice). Clicking it opens
  that palette in the Palette tab; *Apply to code* writes it back in place (the slice grows or shrinks with the stops;
  Ctrl/Cmd+Z undoes it), *Unlink* stops.
- Ctrl/Cmd+/ toggles a `;` comment. Ctrl+Space shows completions (functions, `(buf …)` ids, `(env …)` names,
  your `def` variables).
- Theme follows the system, or toggle System / Light / Dark (remembered per browser).

## Local library

- **Library** tab: programs kept in this browser, in folders (a path like `clocks/wall`; folders can be empty).
  Names follow the board library's rules (a-z 0-9 - _, starting with a letter, up to 15) and are unique per folder.
  **+ Program** / **+ Folder** make new ones; hover a row for rename ✎, move ⇢, duplicate ⧉ and delete ✕ (a folder's
  delete takes everything in it). Click a program to open it in its own tab (❖): Ctrl/Cmd+S saves it to the library
  and runs it in the preview; the dot means unsaved changes.
- **Save to local library…** in a program or board library tab's bar stores its text under a name and folder. A
  preview-only tab becomes the library program's tab; a board tab keeps its board and the library gets a copy.
- **Search** matches names, folders and code (ignoring case) and lists the hits with the first matching line.
- Tick programs (a folder's box ticks everything in it) to **Move to…**, **Export** or **Delete** them together.
- **Export all** / **Export** download a JSON file (`ktsn-led-library-<date>.json`); **Import…** reads one from another
  IDE: it lists what's in it (new, already there, different code, doesn't compile), puts it into a folder you pick, and
  for names already used skips them, replaces them or keeps both (the import becomes `name-2`). Instead of a file you
  can give a web address and **Fetch** it; the server must allow other sites to read it (CORS headers, as e.g.
  raw.githubusercontent.com does), otherwise download the file and choose it.
- **Upload to <board>…** (selection bar, when connected) stores the ticked programs in the board's library under their
  names (folders stay in the IDE). The dialog shows each one as new, already there, or different code; programs that
  don't compile aren't sent; two ticked programs with the same name (from different folders) must be narrowed to one;
  names already on the board with other code are skipped or replaced. Each program gets its own result; if the
  connection is lost the rest are marked not sent.
- **From board…** (Library tab) or **Save to local library…** / **To local…** (Board panel, Library section) copies the
  board's library programs and its channel programs (as `ch0`, `ch1`, ...) into a folder, `boards/<board name>` by
  default, with the same skip / replace / keep both choice as an import.

## Reference

- **Reference** tab: a guide (how programs run, values and types, ranges, `_`, booleans, faults) and every function
  with its signature, arguments, return value, details and an example. **Try it** loads the example into an *example* tab
  (reused, preview only) and runs it in the preview (never on a board).
- Hover a function name in the editor for its signature; **F1** opens its reference entry.
- Tests keep it honest: every compiler function must have an entry with the same argument counts, and every example
  must compile and run without faults. A new function without documentation fails the build.

## Boards

- **Devices…** adds a board: *Network* (address such as `ktsn-led.local`, plus the token set on the board with
  `token <secret>`) or *USB serial* (pick the port in the browser's chooser; no token). Devices are remembered in this
  browser; serial ports are found again by their USB ids after a reload.
- Pick the board in the header and **Connect** (or **Connect** in a board tab's bar). One board is connected at a time;
  tabs for other boards stay editable and show **Connect** instead of **Send**. Opening a serial port doesn't reset
  the board (DTR/RTS are left alone).
- Sending compiles locally first; the board runs the program at once but does **not** store it. **Save to flash**
  stores programs and settings on the board.
- **Board** tab: live status, channels (**Open** switches to that channel's tab, or opens one with the board's program; gamma, dither and
  (for wire formats with W) the white limit are editable and apply at once), intensity, output refresh and parallel strings,
  autoexec (**Open** likewise),
  reboot, **Download config** (JSON, never contains the WiFi password or token) and **Restore config…** (choose what to
  restore; programs for channels the board isn't running yet are skipped and listed), and a console for commands
  (over serial it also shows the board's log).
- **Intensity** (Board panel): the global slider, one slider per channel (multiplied with the global one), and a
  fade time for changes made with them; a channel mid-fade shows where it is now. The preview applies the edited
  channel's intensity on top of its own.
- **Automation** (Board panel): the board's automation program, its runs and faults, Open (an automation tab:
  checked as the automation program, with library names checked against the connected board's library; Send sets
  it on the board; not run in the preview), Run now, and how often it runs. + → *New automation* starts one.
- **Library** (Board panel): the board's stored programs, with Open (a library tab: Send stores it on the board, the
  preview runs it) and Delete; + in the tab bar → *New library program…* starts one. Config files include them.
- **Firmware** (Board panel): the running version, build date, chip and app slot. *Update over the network…* installs a
  `ktsn-led.bin` on a board with the 4 MB OTA layout (checked for chip and project first; progress, restart and
  reconnect; the board keeps the new firmware only if it runs). *Flash over USB…* (also at the bottom of Devices, and in the Board panel while not connected: for new boards
  or ones without KTSN LED, no connection needed)
  writes a whole build folder with esptool-js, like `idf.py flash`: pick e.g. `ktsn-led/build-c6p`; settings are kept
  unless "erase everything" is ticked.
- Web Serial needs Chrome or Edge (Brave may have it switched off). The first network request may make the browser
  ask for access to your local network.
