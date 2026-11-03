#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

mkdir -p build
cmake -B build -S .. -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

echo "Build complete! Executable is at _build_/build/SPSculpt"
