#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-macos}"
APP="$BUILD_DIR/EverythingLite.app"
BIN="$APP/Contents/MacOS/EverythingLite"

echo "== Environment =="
uname -a
sw_vers || true
printf 'arch: '; uname -m
printf 'cmake: '; cmake --version | head -n 1
printf 'qtbase: '; brew --prefix qtbase 2>/dev/null || true
printf 'Qt version: '; "$(brew --prefix qtbase)/bin/qtpaths6" --qt-version 2>/dev/null || true

echo
echo "== App =="
if [[ -x "$BIN" ]]; then
  file "$BIN"
  echo
echo "== Linked libraries =="
  otool -L "$BIN"
else
  echo "未找到：$BIN"
fi

echo
echo "== Code signature =="
codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 || true
