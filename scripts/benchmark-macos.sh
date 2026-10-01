#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

TARGET="${1:-$HOME/Documents}"
shift || true

CLI="build-macos/everything-lite-cli"
if [[ ! -x "$CLI" ]]; then
  echo "未找到 $CLI，先执行 ./scripts/build-macos.sh" >&2
  exit 1
fi

if [[ "$#" -eq 0 ]]; then
  exec "$CLI" benchmark "$TARGET"
else
  exec "$CLI" benchmark "$TARGET" "$@"
fi
