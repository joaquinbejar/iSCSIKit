#!/bin/sh
# Builds wipefs_probe. Usage: ./build.sh [OUTPUT_DIR]
# Default output directory: ./build next to this script (ignored by git).
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
OUT_DIR=${1:-"$SCRIPT_DIR/build"}
mkdir -p "$OUT_DIR"

# -lutil: the probe never calls libutil; it links it only so it can report
# which libutil build (UUID, source version) is installed on this Mac.
xcrun clang++ -std=c++17 -O1 -g -Wall -Wextra -Wno-missing-field-initializers \
    -o "$OUT_DIR/wipefs_probe" \
    "$SCRIPT_DIR/wipefs_probe.cpp" \
    -framework IOKit -framework CoreFoundation -lutil

echo "built $OUT_DIR/wipefs_probe"
