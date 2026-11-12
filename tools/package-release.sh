#!/bin/sh
# Package built variants for a release: dist/ktsn-led-<version>-<variant>.zip + dist/SHA256SUMS.
# Run tools/build-variants.sh first. Each zip holds one folder with what `idf.py flash` writes (flasher_args.json,
# flash_args and the files they name), so it works with the IDE's "Flash firmware over USB…" and with esptool.
set -e
cd "$(dirname "$0")/.."
VERSION=$(cat version.txt)
rm -rf dist && mkdir -p dist

tools/build-variants.sh --list | while read -r name; do
    dir=$(tools/build-variants.sh --dir "$name")
    [ -f "$dir/flasher_args.json" ] || { echo "$name: $dir is not built (run tools/build-variants.sh $name)" >&2; exit 1; }
    strings "$dir/ktsn-led.bin" | grep -qx "$VERSION" || { echo "$name: $dir/ktsn-led.bin is not version $VERSION (rebuild it)" >&2; exit 1; }
    pkg="ktsn-led-$VERSION-$name"
    mkdir -p "dist/$pkg"
    cp "$dir/flasher_args.json" "$dir/flash_args" "dist/$pkg/"
    python3 -c 'import json,sys; print("\n".join(json.load(open(sys.argv[1]))["flash_files"].values()))' \
        "$dir/flasher_args.json" | while read -r f; do
        mkdir -p "dist/$pkg/$(dirname "$f")" && cp "$dir/$f" "dist/$pkg/$f"
    done
    chip=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["extra_esptool_args"]["chip"])' "$dir/flasher_args.json")
    cat > "dist/$pkg/FLASHING.txt" <<EOF
KTSN LED $VERSION, variant $name ($chip)

From the IDE (Chrome): Devices -> Flash firmware over USB... -> pick this folder.
With esptool, from this folder:
    esptool --chip $chip -b 460800 write-flash @flash_args
Saved settings and programs are kept. Network update (4 MB variants with WiFi): use ktsn-led.bin.
EOF
    (cd dist && zip -qr "$pkg.zip" "$pkg" && rm -rf "$pkg")
    echo "dist/$pkg.zip"
done

(cd dist && shasum -a 256 ./*.zip | sed 's| \./| |' > SHA256SUMS)
cat dist/SHA256SUMS
