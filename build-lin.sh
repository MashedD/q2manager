#!/usr/bin/env -S bash -Eeuo pipefail
cd "$(dirname -- "$(readlink -f -- "$0")")"

mkdir -p dist
cmake -S . -B build
cmake --build build --config Release
sstrip build/q2manager
cp -f build/q2manager dist/q2manager

