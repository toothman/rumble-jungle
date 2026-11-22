#!/usr/bin/env bash
# Builds the web UI (web/) into the gzipped assets embedded in the firmware
# (main/www/). Uses the Tailwind CSS standalone CLI - no Node.js needed.
# The generated files are committed, so building the firmware does not
# require this script; run it after changing anything in web/.
set -euo pipefail

TAILWIND_VERSION="v4.3.5"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WEB="$ROOT/web"
OUT="$ROOT/main/www"
TMP="$WEB/.build"
BIN="${TAILWIND_BIN:-$ROOT/tools/bin/tailwindcss-$TAILWIND_VERSION}"

if [ ! -x "$BIN" ]; then
    case "$(uname -s)-$(uname -m)" in
        Darwin-arm64)  asset="tailwindcss-macos-arm64" ;;
        Darwin-x86_64) asset="tailwindcss-macos-x64" ;;
        Linux-x86_64)  asset="tailwindcss-linux-x64" ;;
        Linux-aarch64) asset="tailwindcss-linux-arm64" ;;
        *) echo "Unsupported platform, set TAILWIND_BIN to a tailwindcss binary" >&2; exit 1 ;;
    esac
    echo "Downloading Tailwind CSS $TAILWIND_VERSION ($asset)"
    mkdir -p "$(dirname "$BIN")"
    curl -fsSL -o "$BIN" \
        "https://github.com/tailwindlabs/tailwindcss/releases/download/$TAILWIND_VERSION/$asset"
    chmod +x "$BIN"
fi

mkdir -p "$TMP" "$OUT"
"$BIN" -i "$WEB/app.css" -o "$TMP/app.css" --minify

gzip -9 -n -c "$WEB/index.html" > "$OUT/index.html.gz"
gzip -9 -n -c "$WEB/app.js"     > "$OUT/app.js.gz"
gzip -9 -n -c "$TMP/app.css"    > "$OUT/app.css.gz"

ls -l "$OUT"
