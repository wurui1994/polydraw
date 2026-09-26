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

# ── 整个 polydraw（出图 + GUI）──────────────────────────────────────────────────
# 与 arm64 那一份**同一批源码**，只有三处按平台分岔（都在文件里 `#if defined(__APPLE__)`）：
#   * GL 头：`OpenGL/gl.h` -> `GL/gl.h` + `GL/glext.h`（要 GL_GLEXT_PROTOTYPES）；
#   * 上下文：CGL -> GLFW 的**不可见窗口**（`pd_gui_open_offscreen`）；
#   * `wglGetProcAddress` 的退路：dlsym 之后再问一次 `glfwGetProcAddress`。
# `PD_X64_GL=0` 只编 eval（没有 mesa/glfw 的机器上也能过一半判据）。
if [ "${PD_X64_GL:-1}" = 0 ]; then echo "== 跳过 polydraw（PD_X64_GL=0）"; exit 0; fi

case "$(uname -s)" in
Darwin) GLLIB=(-framework OpenGL -L/opt/homebrew/lib -lglfw); GLINC=(-I/opt/homebrew/include);;
*)      GLLIB=(-lGL -lglfw -ldl);                             GLINC=();;
esac

echo "== kplib"
$CC "${ARCH[@]}" $OPT -I polydraw_src -include port/pd_port.h -w -c polydraw_src/kplib.stitch.c -o "$OUT/kplib.o"

echo "== polydraw（只到 .o）"
# 那四个 `-Wno-`：原文是 C89，而 clang 16 起把 implicit-int / implicit-decl /
# int-conversion / incompatible-pointer-types 提成了**错误**（`-w` 压不住错误）。
$CC "${ARCH[@]}" $OPT -fms-extensions -w \
	-Wno-implicit-int -Wno-implicit-function-declaration \
	-Wno-int-conversion -Wno-incompatible-pointer-types \
	-I polydraw_src -I port/a64/winshim -include port/pd_port.h \
	-c polydraw_src/polydraw.a64.stitch.c -o "$OUT/polydraw.o"

echo "== eval.o（给 polydraw 连的那份，不带 main）"
$CC "${ARCH[@]}" $OPT -w -I polydraw_src -include port/pd_port.h -DCOMPILE=0 \
	-c polydraw_src/eval.a64.stitch.c -o "$OUT/eval.o"
for f in pd_win_a64 pd_gl_cgl pd_main_a64; do
	$CC "${ARCH[@]}" $OPT -w -I polydraw_src -I port/a64/winshim -include port/pd_port.h \
		-c "port/a64/$f.c" -o "$OUT/$f.o"
done
# GUI 与攒批那两份不要假 windows.h（它们用真 GLFW / 真 GL 头）
$CC "${ARCH[@]}" $OPT -w "${GLINC[@]}" -c port/a64/pd_gui_glfw.c -o "$OUT/pd_gui_glfw.o"
$CC "${ARCH[@]}" $OPT -w -c port/a64/pd_gl_imm.c -o "$OUT/pd_gl_imm.o"

echo "== polydraw_x64"
$CC "${ARCH[@]}" "$OUT/polydraw.o" "$OUT/kplib.o" "$OUT/eval.o" \
	"$OUT/pd_win_a64.o" "$OUT/pd_gl_cgl.o" "$OUT/pd_main_a64.o" "$OUT/pd_gui_glfw.o" \
	"$OUT/pd_gl_imm.o" "${GLLIB[@]}" -lm -o "$OUT/polydraw_x64"
echo "-> $OUT/polydraw_x64"

