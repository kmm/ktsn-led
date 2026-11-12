# KTSN LED

Addressable-LED controller for ESP32 (ESP-IDF, C) driven by small LISP-like render programs.

## Download
Ready-to-flash firmware is on the [releases page](https://github.com/kmm/ktsn-led/releases): one zip per variant
(ESP32 2 MB, 2 MB without WiFi, 4 MB; ESP32-C6 RMT, PARLIO, without WiFi). Unzip it, then either pick the folder in
the IDE (Devices → *Flash firmware over USB…*, Chrome) or run `esptool --chip <chip> -b 460800 write-flash @flash_args`
from inside it (`FLASHING.txt` in each zip has the exact command). Saved settings are kept. Check a download against
`SHA256SUMS`. The IDE runs locally: `ide/run.sh` (Docker) serves it at http://localhost:8080.

## Layout
- `components/dsl/`  portable core (arena, pixel math, parser/compiler, natives, channel runtime). No ESP-IDF dependencies.
- `main/`            ESP-IDF glue: LED output (RMT via `led_strip`), render task + frame timer, console, config.
- `host/`            unit tests for the core: `cd host && make`

## Build / flash
    source ~/.espressif/tools/activate_idf_v6.1.sh
    idf.py build flash monitor

[BUILD.md](BUILD.md) has the commands for every board and variant, flashing, updates, tests and the IDE.

### Build options
Defaults (`sdkconfig.defaults`): 2 MB flash, one 1.5 MB app partition, WiFi + NTP on.
Presets can be combined; defaults only apply to a fresh `sdkconfig`, so build a variant in its own directory:

    idf.py -B build-nowifi -D SDKCONFIG=build-nowifi/sdkconfig \
           -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.nowifi" build flash

| Preset              | Effect                                                        |
|---------------------|---------------------------------------------------------------|
| `sdkconfig.nowifi`  | no WiFi/NTP: image ~255 KB instead of ~880 KB                 |
| `sdkconfig.flash4mb`| 4 MB flash, two 1.9 MB app slots for network updates + rollback (`partitions_4mb.csv`) |
| `sdkconfig.usbjtag` | console on the chip's own USB port (XIAO C6/C3, most C6/S3 boards) |
| `sdkconfig.parlio`  | C6: drive all strings with one parallel DMA transfer (PARLIO) instead of RMT |

Seeed XIAO ESP32-C6 (4 MB flash, native USB), WiFi build:

    idf.py -B build-c6 -D SDKCONFIG=build-c6/sdkconfig -D IDF_TARGET=esp32c6 \
           -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.flash4mb;sdkconfig.usbjtag" build flash monitor

XIAO ESP32-C6 with PARLIO output (two strings on D4/D5 by default, immune to WiFi timing):

    idf.py -B build-c6p -D SDKCONFIG=build-c6p/sdkconfig -D IDF_TARGET=esp32c6 \
           -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.flash4mb;sdkconfig.usbjtag;sdkconfig.parlio" build flash monitor

Default LED pin: GPIO 25 on the ESP32, GPIO 22 (XIAO D4) on the C6, GPIO 0 elsewhere; C6 + PARLIO defaults to
two channels on GPIO 22 and 23.
All layouts keep NVS at 0x9000, so saved settings survive switching.

## Firmware updates

- **Over the network** (4 MB boards built with `sdkconfig.flash4mb`; not 2 MB boards, the image doesn't fit twice):
  IDE Board panel → *Update over the network…* with `build…/ktsn-led.bin`, or
  `curl $AUTH --data-binary @build-c6p/ktsn-led.bin http://<board>/api/ota`. The board checks the image (a KTSN LED build
  for its chip), writes it to its other app slot while it keeps running (a progress bar on the strings), and restarts.
  The new firmware keeps itself only if it renders for 30 s; otherwise the board goes back to the previous one.
- **Over USB from the IDE**: *Flash over USB…* (Board panel or Devices) and pick the build folder; it writes what
  `idf.py flash` would (bootloader, partition table, app; settings kept). The first switch to the OTA layout needs
  this (or `idf.py flash`) once.
- The version is `version.txt` (bump it for a release). `version` on the console (and `GET /api/status` → `firmware`,
  the IDE's Board panel) shows what runs, from which slot, whether a fresh update is still unconfirmed, and what is in
  the other slot.
- Testing rollback: bump `version.txt`, rebuild, update over the network, and run `version` right after the restart: it
  says "NEW, not confirmed yet". Power-cycle (or `reboot`) within 30 s: `version` shows the previous version again, and
  the update sits unconfirmed in the other slot. Updating again and waiting 30 s keeps it ("confirmed").

The build options are also in `idf.py menuconfig` (KTSN LED -> WiFi; Partition Table; Serial flasher config -> Flash size).
Edit `main/config.h` for the default GPIO and pixel count; color order is per channel (`chan`, below).

## Console
    prog 0 (fill (buf O) #00FF00 5 -5) (scale (buf O) 0.75)
    list | env | env intensity 128 | env time 2026-10-13 14:30:00 | buf 0 O 0 8 | stat | stat reset

Longer programs can be typed or pasted over several lines (serial console or telnet): `prog 0` (or `autoexec paste`)
on its own, then the program, then a line with only `.`. Comments and indentation are kept; Ctrl-C cancels, and nothing
changes unless the whole program compiles. Up to 3999 characters, 511 per line.

    led> prog 0
    enter the program for ch0, then a line with only '.' to finish (Ctrl-C cancels):
    ... ; two-color chase
    ... (fill-pat (buf O) (set (slice (buf G0) 0 2) 0 #FF0000 #0000FF) (/ (env frame) 4) 5)
    ... .
    ok (takes effect next frame)

Program library: programs stored on the board by name (a-z 0-9 - _, up to 15 characters), kept in their own 256 KB
flash partition (`progs`) next to the channel programs and the autoexec:

    lib                      list them, with the storage used
    lib set sunset (fill (buf O) #FF6000)    store one (or `lib set sunset` alone: several lines, end with .)
    lib save chase 0         store channel 0's current program as "chase"
    lib run sunset 1         put "sunset" on channel 1 now (save keeps it there)
    lib show sunset | lib rm sunset          (refused if a running program uses it and would break)

Automation: one program that runs every few seconds to schedule things: which library program each channel runs,
and intensities (with fades). It may use G0..G7 and `env`, plus `(play ch name)`, `(stop ch)`, `(intensity [ch])` and
`(set-intensity <ch|_> <0..1> [fade seconds])` (`_` = global), which only it may use:

    automation (if (and (ge (env hour) 17) (lt (env hour) 23)) (play 0 evening) (stop 0))
    automation paste         (several lines, end with .)
    automation every 30      run every 30 s (1..3600, default 10; aligned to the clock), also at boot and when set
    automation               the program, runs, last run, faults; `automation run` runs it now, `automation clear`

GPIO outputs (relays, PWM): up to 8, each a pin in binary (on/off) or PWM mode, optionally inverted and named. They
are always off at boot; the console, the API and the automation program switch them (`(io-set pump t)`). The C6
build starts with GPIO 19 and 20 as binary outputs 0 and 1.

    io                       list them, with what they are doing now
    io 0 on | io pump off | io fan 0.4      switch, or set a PWM level 0..1
    io config 2 4 pwm freq=500 name=fan     output 2 on GPIO 4, PWM at 500 Hz (also: invert); applies at once
    io config 2 off          remove it (its pin is let go)

A segment display wired as one LED strip: `seg7`, `seg9` and `seg14` draw a number or text as a mask (lit segments
white, the rest black) to combine with colours, e.g. a clock: `(seg7 (buf X) (str-time "%k%M") abcdefg 3 2 4)` then
`(fill-grad (buf O) pal)` and `(multiply (buf O) (buf X))`. The layout names the order the segments are wired in
(A top, clockwise, G middle, optional p point); horizontal segments are 3 px, vertical 2 px here. Strings are written
in double quotes (`(seg14 (buf X) "HELLO")`) and work on one console line too.

Settings change immediately in RAM; `save` stores them in flash so they survive a reboot:

    autoexec (set (slice (buf G0) 0 3) 0 #FF0000 #00FF00 #0000FF)   runs once now and at every boot
    chan 1 26 100            add/change a channel (gpio 26, 100 px); applies after save + reboot
    chan 0 22 15 "BRG WWW"   wire format: bytes per pixel in order (RGB default, GRB, GRBW, "BRG WWW" = Govee S14)
    intensity 0 0.5 10       channel 0 to half over 10 s (all = the global intensity; no fade time = at once);
                             the output uses global x channel; `intensity` alone shows them, mid-fade too
    white 0 max 60           white limit on channel 0 (percent); several W outputs per pixel ("BRG WWW") are
                             driven together, with 3x finer steps at the dark end
    white 0 mode extract     RGB programs use the white LED: the white in RGB moves to W (boost: added on top;
                             direct: W only from the program, the default)
    white 0 point #FFB46B 1.5  what full W looks like in RGB (white point) and how bright (ratio); tune it in direct
                             mode with full W next to that colour in RGB
    gamma 0 2.2              gamma correction on channel 0's output (1.0..3.0, or off); applies at once
    dither 0 3               temporal dither on channel 0: 3 extra bits below 8 (0..4, 0 = off); applies at once
    output refresh 4         send to the LEDs 4x per frame (240 Hz; programs still run at 60 Hz); 1..4, at once
    output parallel on       send all strings at the same time (RMT builds); at once; `output` shows the settings
    wifi MyNetwork secret    connect (quote an SSID with spaces: wifi "My Net" secret); wifi off
    tz PST8PDT,M3.2.0,M11.1.0    POSIX time zone for env hour/day/...; ntp <server> to change NTP
    loc 40.71 -74.01             location for env sunrise etc. (north and east positive); loc off to clear
    save | config | config erase | reboot
    token <secret>           enable network access (token off disables it again)
    hostname <name>          network name, reachable as <name>.local (default ktsn-led.local)

Gamma and dither: gamma correction maps each channel's output through `(in/255)^gamma`, so fades look even to the eye
but dark values fall into few 8-bit steps. Dither keeps 1..4 more bits: each pixel cycles through 2^bits frames whose
average lands between the steps (smooth dark fades). Black stays black. Few bits flicker least; 4 bits cycle every 16
frames (~4 Hz), which can show at the very darkest levels. `output refresh` sends 2..4 times per frame, and dither steps
with every send: at 4x, 3 bits cycle at 30 Hz and 2 bits at 60 Hz. The board refuses a refresh its strings can't keep up
with (about 30 us per pixel per send). On RMT builds without DMA (ESP32, C6 RMT) more sends mean more chances for a
WiFi interrupt to cause a glitch, so both settings are off by default; PARLIO (C6) sends by DMA.

Over WiFi the same commands work via telnet or netcat once a token is set:

    nc ktsn-led.local 23
    led> auth <secret>
    led> prog 0 (fill (buf O) #0000FF)

## Serial JSON API
For tools (the IDE uses it): type `api` on the serial console, then send one JSON request per line. Each response is
one line starting with the byte 0x1E (log lines never do); `exit` returns to the prompt.

    {"id": 1, "op": "status"}                               -> \x1e{"id":1,"status":200,"body":{...}}
    {"id": 2, "op": "prog", "ch": 0, "src": "(fill (buf O) #FF0000)"}
    {"id": 3, "op": "patch", "body": {"intensity": 128, "save": true}}
    {"id": 4, "op": "cmd", "line": "stat"}

Ops: `ping status config patch prog cmd save reboot exit`, the same operations and bodies as the HTTP API.
No token: serial is physical access. `python3 tools/apitest.py <port>` runs a quick self-test.

## HTTP API
Every request needs the token: `-H "Authorization: Bearer <secret>"` (abbreviated `$AUTH` below).
`<board-ip>` can be `ktsn-led.local` (or whatever `hostname` is set to).

    curl $AUTH http://<board-ip>/api/status                        fps, timing, faults, wifi, time
    curl $AUTH http://<board-ip>/api/config                        running configuration (no secrets)
    curl $AUTH -X PUT --data-binary @wave.lisp http://<board-ip>/api/prog/0
    curl $AUTH -X PATCH -d '{"intensity": 128, "channels": {"0": {"prog": "(fill (buf O) #FF0000)"}}, "save": true}' \
         http://<board-ip>/api/config
    curl $AUTH -d 'stat' http://<board-ip>/api/cmd                  any console command; output as text
    curl $AUTH -X POST http://<board-ip>/api/save
    curl $AUTH -X POST http://<board-ip>/api/reboot

`PATCH /api/config` fields (all optional): `intensity`, `tz`, `ntp`, `loc` (`{lat, lon}` or null), `autoexec` (string or null),
`wifi` (`{"ssid", "password"}` or null; omitted password = keep), `token`, `save` (bool), and
`channels` keyed by number: `{"1": {"gpio": 26, "npix": 100}}` adds/changes a channel (after save + reboot),
`{"0": {"wire": "GRB"}}` sets the wire format (after save + reboot), `{"0": {"prog": "..."}}` sets a program, `{"0": {"gamma": 2.2}}` sets gamma correction (1.0..3.0, 1 = off; at once), `{"0": {"dither": 3}}` dither bits (0..4), `{"0": {"wmax": 60}}` white limit (percent), `wmode` ("direct" / "extract" / "boost"), `wpoint` ("#RRGGBB"), `wratio`, `{"1": null}` removes the last channel. Nothing is applied unless every
field is valid; errors come back per field, e.g. `{"errors": {"channels.0.prog": "line 2 col 7: ..."}}`.

`PATCH /api/config` with `{"channels": {"0": {"intensity": 0.5}}, "fade": 10}` sets a channel's intensity (0..1;
`intensity` at the top level is the global one, 0..256); `fade` (seconds) makes the patch's intensity changes fade.

`PATCH /api/config` with `{"automation": "<source>", "automation_every": 30}` sets the automation program (compiled
first; null clears it); `GET /api/status` → `automation` shows runs, the last run and faults.

GPIO outputs: `PATCH /api/config` with `{"io": {"2": {"gpio": 4, "mode": "pwm", "freq": 500, "invert": false,
"name": "fan"}}}` sets one up (`null` removes it), `{"io_set": {"pump": true, "2": 0.4}}` switches or dims outputs by
number or name. `GET /api/config` → `io` lists them, `GET /api/status` → `io` gives each one's value (0..1).

`GET /api/lib` lists the library (`{programs: [{name, size}], used, total}`), `GET /api/lib/<name>` gives
`{name, src}`, `PUT /api/lib/<name>` stores the body (compiled first), `DELETE /api/lib/<name>` removes it (409 with
the error if a running program uses it and would break).

Programs sent over HTTP may be up to 3999 characters, span several lines and contain `;` comments.

## DSL (v1)
```
    (buf <id>)                                  id: X Y Z O  or G0..G7        -> buffer
    (slice <buf> [start] [end] [step])          every step-th of [start, end) -> buffer
    (fill <buf> <#RRGGBB[WW]> [start] [end])    fills [start, end)            -> buf
    (scale <buf> <0.0..1.0> [start] [end])      multiply brightness (rounds down, so fades reach black) -> buf
    (invert <buf> [start] [end])                each channel x -> 255 - x (e.g. a seg mask) -> buf
    (copy <dst> <src> [start] [end])            dst[i] = src[i]               -> dst
    (screen <dst> <src> [start] [end])          screen blend src onto dst     -> dst
    (add <dst> <src> [start] [end])             saturating add                -> dst
    (subtract <dst> <src> [start] [end])        dst - src, floor 0            -> dst
    (lighten <dst> <src> [start] [end])         per-channel max               -> dst
    (darken <dst> <src> [start] [end])          per-channel min               -> dst
    (multiply <dst> <src> [start] [end])        multiply (mask)               -> dst
    (mix <dst> <src> <0.0..1.0> [start] [end])  crossfade dst -> src (repeated, it arrives) -> dst
    (grad-map <dst> <src> <palette> [start] [end])  src luma -> palette color -> dst
    (fill-pat <dst> <pattern> [phase] [stretch] [mirror] [start] [end])  repeat pattern -> dst
    (fill-grad <buf> <palette> [start] [end])   gradient through palette      -> buf
    (fill-rand <buf> <color> <min> <max> [start] [end])  random brightness per pixel -> buf
    (fill-pick <buf> <color1> <color2> <p> [start] [end]) color2 with probability p  -> buf
    (set <buf> <index> <color>...)              up to 8 colors from index on  -> buf
    (get <buf> <index>)                         pixel color (black outside)   -> color
    (rotate <buf> <amount> [start] [end])       + right, - left; fractional = sub-pixel -> buf
    (if <cond> <then> [else])                   runs one branch; its value    -> branch type
    (and a b ...) (or a b ...) (not a)          short-circuit logic           -> boolean
    (lt a b) (gt a b) (le a b) (ge a b)         compare numbers               -> boolean
    (eq a b) (ne a b)                           numbers/colors/booleans/bufs  -> boolean
    (+ a b ...) (- a [b]) (* a b ...) (/ a b)   arithmetic                    -> int or number
    (mod a b) (abs x) (clamp x lo hi)           (mod -1 60) is 59             -> int or number
    (min a b ...) (max a b ...)                 2..10 numbers                 -> int or number
    (floor x) (round x)                         number -> int                 -> int
    (sin deg) (cos deg)                         degrees -> -1..1              -> number
    (tri deg)                                   triangle wave, sin's period/phase -> number
    (lerp a b t)                                a + (b-a)*t; two colors: blend    -> number or color
    (rand) (rand hi) (rand lo hi)               random: 0..1, [0, hi), [lo, hi)   -> int or number
    (env <name>)                                frame hour minute second ms year month day doy weekday
                                                daymin daysec timeset intensity npix sunrise sunset
                                                dawn dusk daylight moon moonlit -> int
                                                dayfrac hourfrac minfrac -> number 0..<1
    (def <name> <value>)                        a variable (program or function body top level)
    (defn <name> (<p:type>...) <body>...)       a function; types int decimal color bool buf str
    (fn (<p:type>...) <body>...)                a function written where one is passed
    (use <name>)                                the functions of a library program (top level only)
    (map-px <dst> <src> <f> [start] [end])      dst[i] = (f src[i] i)          -> dst
    (repeat <n> <f>)                            f with i = 0 .. n-1
    "text"                                      a string (one line, up to 255 characters; \" and \\ escapes)
    (str-num n [width] [decimals] [zero])       number as text, padded         -> string
    (str-time "%H:%M")                          the clock as text (H k I l M S p Y y m d e j %) -> string
    (str-cat a b ...) (str-sub s [start] [end]) (str-len s)   join / part / length  -> string, string, int
    (seg7 <buf> <num|text> [layout] [hstride] [vstride] [digits])  7-segment display mask -> buf
    (seg9 <buf> <num|text> [layout] [hstride] [vstride] [digits])  7 + centre verticals j m -> buf
    (seg14 <buf> <num|text> [layout] [hs] [vs] [ds] [gs] [digits])  14-segment (a..f g1 g2 h j k l m n) -> buf
    automation program only:
    (play ch name) (stop ch)                    library program on a channel / dark
    (intensity [ch]) (set-intensity ch|_ v [fade s])  read / set intensity 0..1, fading
    (io-set out|name t|nil|0..1) (io out|name)  switch / dim a GPIO output; read it back (0..1)
Ranges work the same everywhere: `[start] [end]` are absolute indices, `end` is exclusive, defaults are the whole
buffer, and negative indices count from the end. `(fill (buf O) #FF0000 -5)` is the last five pixels,
`(fill (buf O) #FF0000 2 -2)` all but the first and last two.
A negative `slice` step reverses: `(slice (buf O) 0 10 -2)` is 8 6 4 2 0.
Two-buffer functions pair `dst[i]` with `src[i]` over the shorter of the two; position with `slice`:
`(copy (slice (buf O) 10) (buf X))` puts X at pixel 10. Overlap is safe: `(copy (slice (buf O) 1) (buf O))` shifts right by one.
Use `_` to skip an optional argument: `(slice (buf O) _ _ -1)` is O reversed.
`fill`/`scale` return their buffer so calls can nest: `(scale (fill (buf X) #00FF00) 0.5)`.
A palette is any buffer view. `set` returns its buffer, so a palette can be written and used in one go:
`(fill-grad (buf O) (set (slice (buf G0) 0 3) 0 #FF0000 #00FF00 #0000FF))`.
`set`/`get` indices may be negative (from the end); pixels outside the buffer are skipped, so
`(set (buf O) (env second) #FF0000)` is a dot that moves once a second.
`fill-rand` and `fill-pick` pick new random values every frame.
`rotate` wraps inside the range. Amounts within 0.01 of a whole number move whole pixels; others blend neighbours,
which softens the pattern, so for continuous sub-pixel motion rotate a fresh copy each frame rather than the same
buffer over and over.
`fill-pat` repeats a pattern: `mirror` (t/nil) plays it ABCCBA, `stretch` repeats each pixel (2: AABBCC), `phase`
shifts it (positive = right, so `(env frame)` marches it along). Reverse or space it out with slices:
`(fill-pat (slice (buf O) 0 _ 3) (slice pal _ _ -1) (env frame) 2 t)`.
Variables: `(def name value)` at the top level of a program stores a buffer/slice, color, number or boolean for the
rest of that run (recomputed every frame, not kept between frames). A variable keeps its type:
`(def pal (slice (buf G0) 0 3)) (def w (* 2 (env second))) (fill-pat (slice (buf O) 0 w) pal)`.
Functions: `(defn dim (c:color k:decimal) (lerp #000000 c k))` can be called anywhere in the program, also above its
defn: `(fill (buf O) (dim #FF8000 0.5))`. The last form is the result; `def` in the body makes a local. A function
sees only its parameters (not the program's variables). It may call itself as its very last step, which loops:
`(defn wrap (x:int n:int) (if (ge x n) (wrap (- x n) n) x))`; other recursion is a compile error. `fn` writes a
function in place and sees the variables around it: `(map-px (buf O) (buf O) (fn (c:color i:int) (if (lt i 5) c #000000)))`,
`(def n 0) (repeat 4 (fn (i:int) (def n (+ n i))))`. Each map-px pixel is a call, so watch `stat` on long strings.
To share functions, put only `defn`s (and `use`s) in a library program, say `shades`, and write `(use shades)` at the
top level of the programs that want them. The functions are copied in when the program compiles. Saving a library
program re-compiles the running programs that use it; if one would break, the save is refused with its error.
Arithmetic keeps ints as ints; any decimal makes the result a decimal (`(/ 7 2)` is 3, `(/ 7 2.0)` is 3.5).
Use `floor`/`round` where an int is needed. A brightness wave: `(scale (buf O) (+ 0.5 (* 0.5 (sin (* (env frame) 3)))))`.
Sun and moon: `(env sunrise)`, `sunset`, `dawn` and `dusk` are minutes after local midnight, computed from the `loc`
setting; compare them with `(env daymin)`. `(env daylight)` is 1 while the sun is up, 0 when down (checked each
second, -1 without a location). `(env moon)` is the phase angle (0 new, 180 full) and
`(env moonlit)` the percent lit. All are -1 when they aren't known or don't happen that day (no location, clock not
set, midnight sun), so test for that first.
Booleans are `t` and `nil`; numbers aren't true or false by themselves, so compare them:
`(if (lt (env hour) 7) (scale (buf O) 0.1))` dims the string before 7 am. `if` can also pick a value:
`(fill (buf O) (if (gt (env second) 29) #FF0000 #0000FF))`.
`env` values can be used wherever an int goes: `(fill (buf O) #FF0000 0 (env second))` is a bar that grows each second.
month is 1..12, day 1..31, doy (day of year) 1..366, weekday 0..6 (0 Sunday), ms 0..999. The clock starts at
1970-01-01 on boot; set it with `env time`; `(env timeset)` is 1 once it is set. Time of day: `daymin` minutes since
midnight, `daysec` seconds, and `dayfrac`, `hourfrac`, `minfrac` how far through the day, hour and minute it is
(decimals 0 up to just below 1, smooth with the ms): `(set (buf O) (floor (* (env minfrac) (env npix))) #0000FF)` is a
dot going round once a minute.
Decimals are converted at compile time (Q16.16, then Q8 for scale args); no floats at run time.
```

## License
MIT, see [LICENSE](LICENSE).
