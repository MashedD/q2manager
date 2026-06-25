#!/usr/bin/env -S bash -Eeuo pipefail
cd "$(dirname -- "$(readlink -f -- "$0")")"

cmake -S . -B build
cmake --build build
sstrip build/q2manager

