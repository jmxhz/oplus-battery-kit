#!/bin/sh
# 打包发布产物
set -e
cd "$(dirname "$0")"
./build.sh target
mkdir -p dist
cp dist/obk module/bin/obk 2>/dev/null || { mkdir -p module/bin; cp dist/obk module/bin/obk; }
chmod 755 module/bin/obk

find module restore -name '.DS_Store' -delete 2>/dev/null || true

rm -f dist/oplus-batt-kit.zip dist/obk-restore.zip
(cd module  && zip -qr9 ../dist/oplus-batt-kit.zip . -x '.*' -x '*/.*')
(cd restore && zip -qr9 ../dist/obk-restore.zip  . -x '.*' -x '*/.*')

echo "产物:"
ls -l dist/*.zip | awk '{printf "  %-28s %s 字节\n", $NF, $5}'
