#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-macos}"
APP="$BUILD_DIR/EverythingLite.app"
BIN="$APP/Contents/MacOS/EverythingLite"
LOG_DIR="${TRACE_LOG_DIR:-$ROOT_DIR/debug-logs}"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG_FILE="$LOG_DIR/search-trace-$STAMP.log"

if [[ ! -x "$BIN" ]]; then
  echo "未找到可执行文件：$BIN" >&2
  echo "请先执行：./scripts/build-macos.sh" >&2
  exit 1
fi

mkdir -p "$LOG_DIR"
echo "Everything Lite 搜索调试模式"
echo "程序：$BIN"
echo "日志：$LOG_FILE"
echo "请在 GUI 中输入出现问题的关键词（例如：砀例甲、示例工匠）。"
echo "终端会实时显示搜索全过程；测试完后关闭 GUI，分享前检查并脱敏日志。"
echo

set +e
EVERYTHING_LITE_SEARCH_TRACE=1 "$BIN" --debug-search 2>&1 | tee "$LOG_FILE"
APP_STATUS=${PIPESTATUS[0]}
set -e

echo
echo "调试日志已保存：$LOG_FILE"
exit "$APP_STATUS"
