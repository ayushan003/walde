#!/bin/bash
# Download ARC S3 trace for benchmarking.
# Source: moka-rs/cache-trace submodule (via moka-rs/mokabench)
# Original dataset: Megiddo & Modha, "ARC: A Self-Tuning, Low Overhead
#   Replacement Cache", USENIX FAST 2003.
# Format: one integer key per line (WALDE TraceFormat::SIMPLE)
#
# Usage: bash traces/download.sh

set -e
cd "$(dirname "$0")"

FILE="s3_arc.txt"

if [ -f "$FILE" ] && [ "$(wc -l < "$FILE")" -gt 1000000 ]; then
    echo "Trace already exists: $FILE ($(wc -l < "$FILE") lines)"
    exit 0
fi

if ! command -v zstd &>/dev/null; then
    echo "ERROR: zstd not found. Install it first:"
    echo "  Ubuntu/Debian: sudo apt install zstd"
    echo "  macOS:         brew install zstd"
    exit 1
fi

if ! command -v git &>/dev/null; then
    echo "ERROR: git not found."
    exit 1
fi

echo "Cloning moka-rs/mokabench (shallow) to fetch cache-trace submodule..."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

git clone --depth=1 https://github.com/moka-rs/mokabench.git "$TMP"
cd "$TMP"
git submodule init
git submodule update --depth=1

COMPRESSED="cache-trace/arc/S3.lis.zst"
if [ ! -f "$COMPRESSED" ]; then
    echo "ERROR: $COMPRESSED not found in submodule."
    echo "The submodule layout may have changed — check:"
    echo "  https://github.com/moka-rs/cache-trace"
    exit 1
fi

echo "Decompressing $COMPRESSED ..."
zstd -d "$COMPRESSED" -o "$OLDPWD/$FILE"
cd "$OLDPWD"

LINES=$(wc -l < "$FILE")
UNIQUE=$(sort -u "$FILE" | wc -l)
SIZE=$(du -h "$FILE" | cut -f1)

echo ""
echo "Downloaded : $FILE"
echo "Size       : $SIZE"
echo "Lines      : $LINES"
echo "Unique keys: $UNIQUE"
echo ""
echo "Run benchmark:"
echo "  ./build/walde_comparison --trace traces/$FILE --cache-size 65536"
