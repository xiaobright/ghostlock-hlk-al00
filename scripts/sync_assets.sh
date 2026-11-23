#!/usr/bin/env bash
# 把 prebuilt 二进制和 glboot.sh 同步进管理器 App 的 assets。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DST="$ROOT/manager/app/src/main/assets/ghost"
mkdir -p "$DST"
cp "$ROOT/prebuilt/ghost1" "$ROOT/prebuilt/glsrv" "$ROOT/prebuilt/gl-su" "$DST/"
cp "$ROOT/scripts/glboot.sh" "$DST/"
echo "[OK] synced to $DST"