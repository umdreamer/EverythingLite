#!/usr/bin/env bash
set -euo pipefail
[[ "$(uname -s)" == Linux ]] || { echo '此脚本需要 Linux。' >&2; exit 1; }
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EL_BUILD_DIR="${EL_BUILD_DIR:-$ROOT_DIR/build/linux-debug}"
EL_GUI_BIN="$EL_BUILD_DIR/EverythingLite"
[[ -x "$EL_GUI_BIN" ]] || { echo '未找到 GUI，请先执行 scripts/build-linux.sh。' >&2; exit 1; }
exec "$EL_GUI_BIN" "$@"
