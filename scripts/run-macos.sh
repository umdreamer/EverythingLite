#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-macos}"
APP="$BUILD_DIR/EverythingLite.app"
BIN="$APP/Contents/MacOS/EverythingLite"

if [[ ! -x "$BIN" ]]; then
  echo "未找到可执行文件：$BIN" >&2
  echo "请先执行：./scripts/build-macos.sh" >&2
  exit 1
fi

exec "$BIN"
