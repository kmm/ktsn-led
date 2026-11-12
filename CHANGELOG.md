# Changelog

One version (`version.txt`) covers the firmware, the DSL, the console and HTTP API, and the IDE. Rules for bumping it
are in CLAUDE.md ("Versioning"). Newest first; versions before 0.10.0 were not recorded here.

## 0.18.0 (2026-11-11)
- Renamed the project from ledctl to **KTSN LED** (short name `ktsn-led`). The firmware image is `ktsn-led.bin`, new or
  reset boards answer as `ktsn-led.local` (a hostname already set is kept), and `version` reports "KTSN LED".
- Upgrading: boards on 0.17.0 or older refuse a network update to a differently named image, so each needs **one USB
  flash** to 0.18.0. Saved settings, programs and the library are kept. From 0.18.0 on, network updates accept both
  names.
- Build options are now `CONFIG_KTSN_*` (menuconfig "KTSN LED"). Delete a build folder's `sdkconfig` (or the folder)
  before rebuilding so the presets apply again.
- HTTP API: the plain-text result code is sent as `X-Ktsn-Result`; `X-Ledctl-Result` is still sent too.
- IDE: titled KTSN LED; saved state moves to new browser keys automatically on first load; exports are named
  `ktsn-led-config-…` / `ktsn-led-library-…`, and files exported before the rename still import.
- First public release: MIT license, GitHub Actions CI (host tests, IDE build and tests, all six firmware variants),
  and ready-to-flash zips per variant (`tools/build-variants.sh`, `tools/package-release.sh`).

## 0.17.0 (2026-11-11)
- New `env` values for the time of day: `daymin` (minutes since midnight), `daysec` (seconds), and `dayfrac`,
  `hourfrac`, `minfrac` (how far through the day, hour and minute, decimals 0 up to just below 1, including the ms).
- New `env` values: `ms` (0..999), `weekday` (0..6, 0 Sunday), `timeset` (1 once the clock is set), `daylight`
  (1 while the sun is up, 0 down, -1 without a location; checked every second).
- A misspelled `env` name now suggests the closest one (the full list no longer fits an error message).
- `env` on the console prints the new values. Needs a firmware flash; the IDE preview computes the same values.

## 0.16.0 (2026-11-10)
- New: sun and moon, computed on the board with no tables. `(env sunrise)`, `sunset`, `dawn` and `dusk` (civil twilight)
  are minutes after local midnight; `(env moon)` is the phase angle 0..359 and `(env moonlit)` the percent lit. Values
  are -1 when there's no location, the clock isn't set, or the event doesn't happen that day.
- New: `loc <lat> <lon>` / `loc off` console command, saved by `save`; `loc` in the API config (`{lat, lon}` or null).
  `env` prints the new values. Needs a firmware flash.
- IDE: the preview computes the same values, with Lat and Lon fields that follow a connected board; the board panel
  shows the location, and config export and restore include it.

## 0.15.0 (2026-11-09)
- New: user functions, step 2 (shared libraries). `(use name)` at the top level of a program makes the functions of a
  library program available in it. A library program may contain only `defn` and `use`; `use` can nest, a circle is an
  error. Only the library functions actually reached are compiled in, and errors in a library are reported at the
  `use` with the library's own line number. Up to 64 functions per program and 8 libraries.
- Board: saving or deleting a library program first compiles the running programs that use libraries (channels,
  autoexec, automation) against the change. If one would break, the change is refused with that program's error
  (`lib rm` prints it, `DELETE /api/lib/<name>` answers 409); otherwise they switch to the new functions at once.
  Needs a firmware flash.
- IDE: `use` reads the connected board's library, or the local library when offline (a name in two folders is an
  error). Uploading checks programs against the board's library plus the selection and sends used libraries first.
  Autocomplete offers library names after `(use ` and the functions of used libraries; hover shows where they come from.

## 0.14.1 (2026-11-09)
- Fixed: a program with many wide calls or `if`s nested through functions could pass the "32 levels" check and still
  overflow the 8 KB stack. The compiler now adds up an estimate of the stack bytes each call needs (limit 6400) and
  refuses a program over it. Programs without functions always fit.
- Fixed: a runaway `repeat`, `map-px` or tail loop could keep the render task busy for most of every frame, starving
  everything else on a single-core chip. On the board, function calls and loops now fault once a frame's 12 ms are
  used up (shared by the channels, at least 2 ms each). Programs without functions never see this limit.
- Faster: the language core is compiled with -O2.
- Changed: `stat` shows `stack est N B` instead of `depth N`; the status JSON has `channels[].stack_est` instead of
  `channels[].depth` (both new in 0.14.0). Needs a firmware flash.

## 0.14.0 (2026-11-09)
- New: user functions, step 1 (functions within one program). `(defn name (p:type ...) body...)` with declared
  parameter types (int decimal color bool buf str), usable anywhere in the program (also before its defn), checked
  at compile time; locals with `def` in the body; a function may call itself as its last step (a loop); functions
  can't call each other in a circle. `(fn (p:type ...) body...)` writes a function in place where one is passed.
  `(map-px <dst> <src> <f> [start] [end])` computes every pixel with f; `(repeat <n> <f>)` calls f n times.
- Changed (limits, nothing breaks): a frame may evaluate 65536 calls (was 4096); 64 variables per program (was 32,
  now shared with parameters and locals); calls may nest 32 deep counting function calls (checked when compiling).
  Variables no longer use the per-frame scratch memory.
- Firmware: render and automation task stacks 8 KB (were 4 and 6 KB); `stat` shows how much of the render stack was
  never used and each channel program's worst-case depth (status JSON: `render_stack_free`, `channels[].depth`).
  Needs a firmware flash; the IDE (preview, reference, autocomplete and hover for your functions) matches.

## 0.13.0 (2026-11-09)
- Improved: intensity at output is 16-bit. Fades (global and per channel) move in steps 256 times finer, so a slow
  fade near black no longer jumps visibly (most noticeable with dither on). Steady intensities give exactly the same
  output as before, except that global and channel intensity both below full now multiply exactly instead of rounding
  to 1/256. Values you set, saved settings, the API and `(env intensity)` are unchanged. Needs a firmware flash; the
  IDE preview matches.

## 0.12.0 (2026-11-09)
- New (IDE): local library, step 2. Upload selected programs to the connected board's library (names checked,
  programs that don't compile are not sent, names already there: skip or replace, a result per program); save a
  board's library and channel programs into the local library (default folder `boards/<board name>`; Library tab
  *From board…*, Board panel *To local…* / *Save to local library…*); import a library file from a web address
  (the server must allow cross-origin requests). IDE only: the firmware is unchanged (no need to flash).

## 0.11.0 (2026-11-04)
- New (IDE): local library, step 1. A Library tab keeps programs in this browser in folders, with search,
  multi-select (move, export, delete), editing in local tabs, *Save to local library* from other tabs, and export /
  import of library files. IDE only: the firmware is unchanged (no need to flash).

## 0.10.0 (2026-11-04)
- New: `(invert <buf> [start] [end])`, every channel x becomes 255 - x (e.g. the unlit segments of a seg mask).
