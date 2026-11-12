#!/bin/sh
# Build firmware variants (the same list as BUILD.md). Needs ESP-IDF v6.1 activated (idf.py on PATH).
#   tools/build-variants.sh                 every variant
#   tools/build-variants.sh esp32c6 ...     only these
#   tools/build-variants.sh --list          print the variant names
#   tools/build-variants.sh --dir <name>    print a variant's build directory
# Each variant builds in its own directory (build-<dir>), which also holds its sdkconfig.
set -e
cd "$(dirname "$0")/.."

# name | build dir | target | presets after sdkconfig.defaults
VARIANTS='esp32-2mb|build|esp32|
esp32-2mb-nowifi|build-nowifi|esp32|sdkconfig.nowifi
esp32-4mb|build-4mb|esp32|sdkconfig.flash4mb
esp32c6|build-c6|esp32c6|sdkconfig.flash4mb;sdkconfig.usbjtag
esp32c6-parlio|build-c6p|esp32c6|sdkconfig.flash4mb;sdkconfig.usbjtag;sdkconfig.parlio
esp32c6-nowifi|build-c6-nowifi|esp32c6|sdkconfig.flash4mb;sdkconfig.usbjtag;sdkconfig.nowifi'

if [ "$1" = "--list" ]; then echo "$VARIANTS" | cut -d'|' -f1; exit 0; fi
if [ "$1" = "--dir" ]; then echo "$VARIANTS" | grep "^$2|" | cut -d'|' -f2; exit 0; fi

for n in "$@"; do
    echo "$VARIANTS" | cut -d'|' -f1 | grep -qx "$n" || { echo "unknown variant: $n (see --list)" >&2; exit 1; }
done

# EIM's activate script defines idf.py as a shell function, which a script doesn't inherit; export.sh puts it on PATH.
if command -v idf.py >/dev/null 2>&1; then IDF="idf.py"
elif [ -n "$IDF_PATH" ]; then IDF="python3 $IDF_PATH/tools/idf.py"
else echo "activate ESP-IDF v6.1 first (BUILD.md, Toolchain)" >&2; exit 1; fi

want=" $* "
echo "$VARIANTS" | while IFS='|' read -r name dir target presets; do
    [ $# -eq 0 ] || case "$want" in *" $name "*) ;; *) continue ;; esac
    defaults="sdkconfig.defaults${presets:+;$presets}"
    echo "=== $name ($dir: $target, $defaults)"
    # The root variant keeps the usual build/ + ./sdkconfig, so plain `idf.py build` stays the same build.
    if [ "$dir" = build ]; then cfg=sdkconfig; else cfg="$dir/sdkconfig"; fi
    $IDF -B "$dir" -D SDKCONFIG="$cfg" -D IDF_TARGET="$target" -D SDKCONFIG_DEFAULTS="$defaults" build || exit 1
done
