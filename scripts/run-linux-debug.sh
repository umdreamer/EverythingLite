#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EL_TRACE_DIR="${EL_TRACE_DIR:-$ROOT_DIR/debug-logs}"
mkdir -p "$EL_TRACE_DIR"
EL_TRACE_FILE="$EL_TRACE_DIR/search-trace-$(date +%Y%m%d-%H%M%S).log"
echo 'Trace 会记录路径、查询和结果。仅使用合成数据复现；分享前必须检查并脱敏日志。'
set +e
EVERYTHING_LITE_SEARCH_TRACE=1 "$ROOT_DIR/scripts/run-linux.sh" --debug-search "$@" 2>&1 | tee "$EL_TRACE_FILE"
EL_APP_STATUS=${PIPESTATUS[0]}
set -e
exit "$EL_APP_STATUS"
