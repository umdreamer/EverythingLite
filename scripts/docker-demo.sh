#!/usr/bin/env bash
set -euo pipefail

SEARCH_ROOT="${1:-$HOME/Documents}"
QUERY="${2:-pdf}"
export SEARCH_ROOT

echo "1) 构建容器"
docker compose build

echo "2) 索引：$SEARCH_ROOT"
docker compose run --rm everything-lite index /search

echo "3) 搜索：$QUERY"
docker compose run --rm everything-lite search "$QUERY" --limit 30
