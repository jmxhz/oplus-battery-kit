#!/bin/sh
# obk 构建：host 用于测试，target 为 Android arm64 静态
set -e
cd "$(dirname "$0")"
SRC="src/common.c src/fdt.c src/dtbo.c src/crypto.c src/avb.c src/profile.c src/cfg.c src/detect.c src/batt.c src/main.c"
mkdir -p build dist
case "${1:-all}" in
host|all)
  cc -std=c11 -D_GNU_SOURCE -O1 -Wall -Wextra -Wno-unused-parameter -o build/obk $SRC
  echo "host: build/obk"
  ;;
esac
case "${1:-all}" in
target|all)
  PATH="/opt/homebrew/bin:$PATH" zig cc -target aarch64-linux-musl -static -Os -D_GNU_SOURCE \
    -std=c11 -Wall -Wextra -Wno-unused-parameter -s -o dist/obk $SRC
  echo "target: dist/obk ($(wc -c < dist/obk) 字节)"
  ;;
esac
