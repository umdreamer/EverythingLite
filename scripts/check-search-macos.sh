#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CLI="$ROOT_DIR/build-macos/everything-lite-cli"
QUERY="${1:-砀例甲}"

if [[ ! -x "$CLI" ]]; then
  echo "未找到 CLI：$CLI"
  echo "请先运行 ./scripts/build-macos.sh"
  exit 1
fi

echo "=== everything-lite-cli 搜索诊断 ==="
echo "数据库：$($CLI db-path)"
echo
echo "--- stats ---"
$CLI stats
echo
echo "--- name search: $QUERY ---"
$CLI search "$QUERY" --limit 20
echo
echo "--- path search: path:$QUERY ---"
$CLI search "path:$QUERY" --limit 20
