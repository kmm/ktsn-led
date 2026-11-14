# Changelog

One version (`version.txt`) covers the firmware, the DSL, the console and HTTP API, and the IDE. Rules for bumping it
are in CLAUDE.md ("Versioning"). Newest first; versions before 0.10.0 were not recorded here.

## 0.26.0 (2026-11-13)
- Audio: a noise gate, so a quiet room stays dark. Before, the automatic gain turned room noise up until it filled the
  bands and level within ~10 s of quiet. The gate is an RMS (a block's average loudness) in ADC counts: blocks below it
  are silence (outputs fall to 0, no beats, nothing learned), and sound just above it shows dim instead of being turned
  up to full. `audio gate learn [s]` listens to the quiet room (5 s by default) and sets the gate a little above its
  noise (90th percentile + ~3.5 dB, so a click doesn't count); `audio gate <rms>` / `audio gate off` set it by hand;
  `save` keeps it. Off by default.
- `audio` and `audio watch` show each block's `rms` and mark gated blocks; API status `audio.rms`, config `audio_gate`
  (GET and PATCH). IDE: the Board panel shows rms and the gate, with a Learn button.

## 0.25.0 (2026-11-13)
- New: lists. `(list a b ...)` holds up to 255 values of one type (numbers, colours, buffers such as palettes,
  strings, t/nil), past the usual 10-argument limit; keep one in a variable with `def`. `(list-len l)`,
  `(list-nth l i)` (negative counts from the end). Entries are worked out only when picked.
- New: `(list-pick l period)` gives the entry for now and changes every `period` seconds. The entries go round in
  shuffled rounds (each once per round, never the same twice in a row), different per channel and per list. It needs
  no memory, so it carries on after a reboot. For example, a new palette every 5 minutes:
  `(fill-grad (buf O) (list-pick pals 300))`.
- New (automation): `(play-pick ch (list "fire" "rain" ...) [period])` plays one of the listed library programs, picked
  the same way. Without a period it keeps the channel's program if it is from the list, otherwise it plays a random
  one. `(play-library ch "party-" [period])` does the same over the library programs whose names start with the
  prefix, read from the library each time it runs.
- `play` also takes the name as a string, which may be made while the program runs: `(play 0 "fire")`,
  `(play 0 (list-nth l 2))`. A bare name works as before.
- Programs with their own functions named `list` keep working (the list form is then theirs).
- IDE: the preview runs `list-pick` on its clock; reference category Lists.

## 0.24.0 (2026-11-13)
- New build variants `esp32c6-xiao` and `esp32c6-xiao-parlio` for the Seeed XIAO ESP32-C6: the same as `esp32c6` /
  `esp32c6-parlio` plus its RF switch between the built-in ceramic antenna and the U.FL connector (preset
  `sdkconfig.xiaoc6`). `antenna internal|external` on the console, `antenna` in the API config, a selector in the IDE
  Board panel. It applies at once, `save` keeps it, and it is set at boot before WiFi starts (default: built-in).
  GPIO3 and GPIO14 drive the switch in these builds, so channels and io slots refuse them (they aren't on the XIAO's
  pins).

## 0.23.1 (2026-11-13)
- Fixed (IDE): adding a GPIO slot on a board with none set up offered only binary and pwm; the Board panel now offers
  the modes the board's firmware has (input and analog from 0.20.0, audio from 0.21.0). IDE only: no need to flash.

## 0.23.0 (2026-11-12)
- IDE: programs that read the audio values get an Audio row in the preview: a fake beat at a chosen tempo (40..240
  bpm, default 120; a kick in the bass on every beat, hats in the top bands between beats), or off (no audio input).
- Changed: `(env beatms)` before the first beat or without an audio input is 2147483647 (was -1 in 0.21.0 and 0.22.0),
  so `(lt (env beatms) 100)` stays false instead of flashing all the time.
- API: `GET /api/status` has `audio` (bands, level, beats, beatms, swing, analysis time per block, overflows), or null
  without an audio input.

## 0.22.0 (2026-11-12)
- New: `audio bench` times the audio analysis on a built-in test signal (fastest, average, slowest per 32 ms block),
  with no audio input or microphone needed.
- Faster: the audio analysis is compiled with -O2.

## 0.21.0 (2026-11-12)
- New (experimental, a prototype to measure on hardware): an audio input. `io config <n> <gpio> audio` on an ADC1 pin
  (one at most) with an analog microphone module. The board samples it at 8 kHz and every 32 ms runs a 256-point
  integer FFT: `(env band0)` .. `(env band7)` (about an octave each, 31 Hz .. 4 kHz) and `(env level)` are 0..1 with
  an automatic gain, `(env beats)` counts beats (sudden bass), `(env beatms)` is the ms since the latest one (-1
  before the first or without an audio input). All are 0 without one.
- New console command `audio` (rate, analysis time per block, bands, level, beats, swing), `audio watch [s]`,
  `audio reset`; `env` prints the audio values. The API and the Board panel take the mode `audio` (status value: the
  level). The IDE preview has no sound yet: the values are 0 there.

## 0.20.0 (2026-11-12)
- New: GPIO inputs. `io config <n> <gpio> input` sets up a digital input (pull up by default, or `pull=down|none`;
  `debounce=<ms>`, default 20; `invert`; `name=`), `io config <n> <gpio> analog` an analog one (ADC1 pins only:
  ESP32 GPIO 32..39, C6 GPIO 0..6). They share the 8 slots and names with the outputs.
- New: `(in button)` -> t / nil and `(analog knob [samples])` -> 0..1 (the latest 1..256 readings averaged, one per
  ms, default 16). Unlike the outputs they work in channel programs too, not only in automation.
- New: an input marked `trigger` runs the automation program right after it changes (a button can switch programs at
  once).
- `io` lists input states and analog values; the API config and patch take the new modes and `pull`, `debounce`,
  `trigger`; status shows each slot's mode. `io-set` and `io` refuse inputs.
- IDE: the Board panel's Outputs became "Inputs and outputs"; the preview has Inputs controls (a checkbox per digital
  input, a slider per analog one) for the board's inputs, or offline for the names the program reads; reference
  category Inputs. Outputs saved by 0.19.0 and older load unchanged.

## 0.19.0 (2026-11-12)
- New: date and time comparisons for scheduling. `(time-gt t)`, `time-ge`, `time-lt`, `time-le`, `time-eq` compare the
  time of day with `"HH:MM"`, `"HH:MM:SS"` or a number of minutes after midnight (so `(env sunset)` fits), at the
  precision the time is written in. `(time-in from to)` is a span that wraps past midnight (end not included).
- New: `(date-gt d)`, `date-ge`, `date-lt`, `date-le`, `date-eq` with `"MM-DD"` (every year) or `"YYYY-MM-DD"`, and
  `(date-in from to)` (both days included; `"MM-DD"` spans wrap past New Year).
- All of them are false until the clock is set. Times and dates written in a program are checked when it compiles.
  IDE: a new reference category, Time.

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
