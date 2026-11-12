#!/bin/sh
# Build and start the IDE container: http://localhost:8080. Picks the native Emscripten image for this CPU.
set -e
cd "$(dirname "$0")"
case "$(uname -m)" in arm64|aarch64) export EMSDK_TAG=4.0.15-arm64 ;; *) export EMSDK_TAG=4.0.15 ;; esac
docker compose up --build -d "$@"
echo "KTSN LED IDE: http://localhost:8080"
