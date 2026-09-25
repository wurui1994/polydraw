#!/usr/bin/env bash
# bench/build-a64.sh —— 在 arm64 macOS 上编原版 polydraw 的 eval 核心。
#
# 三条规矩：
#   1. **原文一个字节都不动** —— 缺的 MSVC 关键字走 `-include ../port/pd_port.h`；
#      `kasm87` 那一份原文在 COMPILE==0 上写的是 `??? not implemented`，所以
#      arm64 的缝合文件（`eval.a64.stitch.c`）把那一格换成 `port/a64/kasm_main_a64.c`，
#      差别只有十几行，`diff -u` 看得见；
#   2. `COMPILE=0` 走纯 C 解释器（`kasm87c`）。真的 arm64 JIT 是下一步（ADR-0045）；
#   3. `-O2`：性能的尺子必须是真优化编译器。
set -euo pipefail

cd "$(dirname "$0")/.."
OUT=${OUT:-bench/out}
mkdir -p "$OUT"

CC=${CC:-clang}
OPT=${OPT:--O2}

echo "== eval（COMPILE=0，纯 C 解释器）"
$CC -arch arm64 $OPT -I polydraw_src \
	-include port/pd_port.h \
	-DCOMPILE=0 -DEVALTEST \
	-Wno-deprecated-non-prototype -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
	polydraw_src/eval.a64.stitch.c -o "$OUT/eval_a64" -lm

echo "-> $OUT/eval_a64"
"$OUT/eval_a64" "1+2*3" | tail -1
