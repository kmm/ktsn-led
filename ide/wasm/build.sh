#!/bin/sh
# Build the DSL core (components/dsl) to WebAssembly for the IDE: ide/web/src/wasm/generated/ktsn-led.mjs
# (an ES module with the .wasm embedded, so it bundles into the web app and Electron alike).
# Uses emcc if it is on PATH (e.g. inside the emsdk image), otherwise runs the emsdk Docker image.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
EMSDK_VERSION=4.0.15
OUT=ide/web/src/wasm/generated

run_emcc() {
    mkdir -p "$OUT"
    emcc -O2 -std=c11 -Wall -Wextra -Werror \
        -I components/dsl/include -I components/dsl/src -I main \
        main/gamma.c main/wire.c main/astro.c components/dsl/src/arena.c components/dsl/src/compile.c components/dsl/src/natives.c components/dsl/src/channel.c \
        ide/wasm/ktsn_led_wasm.c \
        -o "$OUT/ktsn-led.mjs" \
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sSINGLE_FILE=1 -sENVIRONMENT=web,node \
        -sALLOW_MEMORY_GROWTH=1 -sFILESYSTEM=0 \
        -sEXPORTED_FUNCTIONS=_malloc,_free \
        -sEXPORTED_RUNTIME_METHODS=cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU32,HEAPU8
    echo "built $OUT/ktsn-led.mjs ($(wc -c < "$OUT/ktsn-led.mjs") bytes)"
}

cd "$ROOT"
if command -v emcc >/dev/null 2>&1; then
    run_emcc
else
    case "$(uname -m)" in arm64|aarch64) TAG="$EMSDK_VERSION-arm64" ;; *) TAG="$EMSDK_VERSION" ;; esac
    docker run --rm -v "$ROOT":/src -w /src -u "$(id -u):$(id -g)" "emscripten/emsdk:$TAG" sh ide/wasm/build.sh
fi
