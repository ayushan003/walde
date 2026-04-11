#!/bin/bash
# Download ARC S3 trace for benchmarking.
# Source: dgraph-io/benchmarks (originally from ARC paper, Megiddo & Modha 2003)
#
# Usage: ./traces/download.sh

set -e
cd "$(dirname "$0")"

URL="https://raw.githubusercontent.com/dgraph-io/benchmarks/master/cachebench/ristretto/trace/arc_s3.txt"
FILE="s3_arc.txt"

if [ -f "$FILE" ]; then
    echo "Trace already exists: $FILE ($(wc -l < "$FILE") lines)"
    exit 0
fi

echo "Downloading ARC S3 trace..."
if command -v curl &> /dev/null; then
    curl -L -o "$FILE" "$URL"
elif command -v wget &> /dev/null; then
    wget -O "$FILE" "$URL"
else
    echo "ERROR: Neither curl nor wget found. Download manually from:"
    echo "  $URL"
    exit 1
fi

echo "Downloaded: $FILE ($(wc -l < "$FILE") lines, $(du -h "$FILE" | cut -f1))"
echo "Unique keys: $(awk '{print \$1}' "$FILE" | sort -u | wc -l)"
echo ""
echo "Run benchmark:"
echo "  ./build/walde_comparison --trace traces/$FILE"
