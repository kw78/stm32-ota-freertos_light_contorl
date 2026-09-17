#!/usr/bin/env bash
# OTA 协议栈 host fuzz：构建（ASan/UBSan）+ 运行
# 用法: test/fuzz/run.sh [seconds] [seed]
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=/tmp/gcctest_fuzz
gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
    -Wall -Wextra -Wno-unused-parameter \
    -I test/fuzz/shim -I Core/Inc \
    test/fuzz/harness.c -o "$OUT"

"$OUT" --seconds "${1:-20}" ${2:+--seed "$2"}
