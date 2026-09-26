#!/usr/bin/env bash
# bench/build-x64.sh —— **x86-64** 那条腿：eval 核心（COMPILE=0 + 我们自己的 thunk
# + `port/x64/pd_x64_jitc.c` 那份 JIT）。
#
# 两个落点，同一份源码：
#   * linux x86-64 —— docker 里 `--platform linux/amd64`（见 bench/docker-x64.sh）；
#     这台机器是 Apple Silicon，所以那一档是 qemu 转译，能验**正确性**、量不了速度；
#   * osx x86-64 —— `clang -arch x86_64` 交叉编出来就是合法的 Mach-O，但**跑**要
#     Rosetta 2（本机没装：`arch -x86_64 /usr/bin/true` 回 Bad CPU type）。
#     装法是 `softwareupdate --install-rosetta --agree-to-license`。
#
# 三条规矩与 arm64 那一份一样：原文一个字节都不动、COMPILE=0、-O2。
set -euo pipefail

cd "$(dirname "$0")/.."
OUT=${OUT:-bench/out-x64}
mkdir -p "$OUT"

CC=${CC:-clang}
OPT=${OPT:--O2}
ARCH=()
case "$(uname -s)" in
Darwin) ARCH=(-arch x86_64);;
esac

echo "== eval_x64（COMPILE=0 + port/x64 的 JIT）"
$CC "${ARCH[@]}" $OPT -I polydraw_src \
	-include port/pd_port.h -DCOMPILE=0 -DEVALTEST -w \
	polydraw_src/eval.a64.stitch.c port/a64/pd_gl_imm_stub.c -o "$OUT/eval_x64" -lm
echo "-> $OUT/eval_x64"
"$OUT/eval_x64" "1+2*3" | tail -1

echo "== eval_bench_x64（量尺）"
$CC "${ARCH[@]}" $OPT -I polydraw_src \
	-include port/pd_port.h -DCOMPILE=0 -w \
	-DPD_EVAL_BENCH polydraw_src/eval.a64.stitch.c port/a64/pd_gl_imm_stub.c -o "$OUT/eval_bench_x64" -lm
echo "-> $OUT/eval_bench_x64"
"$OUT/eval_bench_x64" '(x){s=0;for(i=0;i<x;i++)s=s+i*i;s}' 1000 | tail -1
