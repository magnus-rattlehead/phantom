#!/bin/sh
# Copies *.gguf files from $1 (source models dir) to $2 (dest dir).
# Skips files that already exist at the destination (-n).
set -e
src="$1"
dst="$2"
mkdir -p "$dst"
for f in "$src"/*.gguf; do
    [ -f "$f" ] && cp -n "$f" "$dst/" || true
done
