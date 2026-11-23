#!/usr/bin/env bash
# 用 Android NDK 交叉编译 aarch64 目标（API 29，静态链接）。
# 用法：ANDROID_NDK_HOME=/path/to/ndk bash scripts/build_targets.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${ANDROID_NDK_HOME:?please set ANDROID_NDK_HOME}"
CC="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
OUT="$ROOT/prebuilt"
mkdir -p "$OUT"

build() {
  echo "[CC] $2"
  "$CC" -O2 -static -o "$OUT/$2" "$1"
}

build "$ROOT/exploit/ghost1.c"  ghost1
build "$ROOT/service/glsrv.c"   glsrv
build "$ROOT/service/gl-su.c"   gl-su
build "$ROOT/service/keyprobe.c" keyprobe
echo "[OK] binaries in $OUT"