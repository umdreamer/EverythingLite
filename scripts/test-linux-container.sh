#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -z "${EL_TEST_PLATFORM:-}" ]]; then
    case "$(docker info --format '{{.Architecture}}')" in
        aarch64|arm64) EL_TEST_PLATFORM=linux/arm64 ;;
        x86_64|amd64) EL_TEST_PLATFORM=linux/amd64 ;;
        *) echo '请设置 EL_TEST_PLATFORM 为目标 Linux 架构。' >&2; exit 1 ;;
    esac
fi
EL_TEST_IMAGE="${EL_TEST_IMAGE:-everythinglite-dev:0.4.12-${EL_TEST_PLATFORM##*/}}"
EL_ARTIFACT_DIR="${EL_ARTIFACT_DIR:-$ROOT_DIR/dist/linux-${EL_TEST_PLATFORM##*/}}"
mkdir -p "$EL_ARTIFACT_DIR"
EL_SOURCE_SNAPSHOT="$(mktemp -d "${TMPDIR:-/tmp}/everything-lite-source.XXXXXX")"
trap 'rm -rf -- "$EL_SOURCE_SNAPSHOT"' EXIT
"$ROOT_DIR/scripts/export-build-source.sh" "$EL_SOURCE_SNAPSHOT"
docker build --platform "$EL_TEST_PLATFORM" -f "$EL_SOURCE_SNAPSHOT/Dockerfile.linux-dev" -t "$EL_TEST_IMAGE" "$EL_SOURCE_SNAPSHOT"
docker run --rm --platform "$EL_TEST_PLATFORM" \
    --mount "type=bind,src=$EL_SOURCE_SNAPSHOT,dst=/src,readonly" \
    --mount "type=bind,src=$EL_ARTIFACT_DIR,dst=/artifacts" \
    -e HOME=/tmp/el-home -e XDG_CONFIG_HOME=/tmp/el-home/.config \
    -e XDG_RUNTIME_DIR=/tmp/el-runtime -e QT_QPA_PLATFORM=offscreen \
    "$EL_TEST_IMAGE" bash -euc '
        mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_RUNTIME_DIR"
        chmod 700 "$XDG_RUNTIME_DIR"
        EL_BUILD_DIR=/tmp/el-debug /src/scripts/build-linux.sh
        python3 /src/tests/check_cli_parity.py /tmp/el-debug/everything-lite-cli --output /artifacts/cli-parity.json
        EL_RELEASE_DIR=/tmp/el-release EL_PACKAGE_DIR=/artifacts /src/scripts/package-linux.sh
        printf "synthetic protected index\n" > /tmp/el-protected-index
        cp /tmp/el-protected-index /tmp/el-protected-index.expected
        for package in /artifacts/*.deb; do
            EVERYTHING_LITE_DB=/tmp/el-protected-index EVERYTHING_LITE_SEARCH_TRACE=1 \
                EL_CHECK_NATIVE_QPA=1 /src/scripts/check-linux-package.sh "$package"
            cmp /tmp/el-protected-index /tmp/el-protected-index.expected
        done
        install -m 755 /tmp/el-release/EverythingLite /artifacts/EverythingLite
        install -m 755 /tmp/el-release/everything-lite-cli /artifacts/everything-lite-cli
    '
