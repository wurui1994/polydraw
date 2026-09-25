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

# 量尺那一份：把 Ken 自带的 main 换成 port/a64/eval_bench.c。
# 为什么要换：自带那个的 `rdtsc64()` 在非 x86 上是 `return(LL(0))`
# （eval_test.c:35），所以它印的永远是 `0 cc` —— 量不出东西。
echo "== eval_bench（量尺）"
$CC -arch arm64 $OPT -I polydraw_src \
	-include port/pd_port.h -DCOMPILE=0 -w \
	-DPD_EVAL_BENCH polydraw_src/eval.a64.stitch.c -o "$OUT/eval_bench" -lm
echo "-> $OUT/eval_bench"
"$OUT/eval_bench" '(x){s=0;for(i=0;i<x;i++)s=s+i*i;s}' 1000 | tail -1

# kplib：**零错误**直接编过（原文什么都不缺）。
echo "== kplib"
$CC -arch arm64 $OPT -I polydraw_src -include port/pd_port.h \
	-w -c polydraw_src/kplib.stitch.c -o "$OUT/kplib.o"
echo "-> $OUT/kplib.o"

# polydraw：靠 port/a64/winshim/ 那几份**假头文件**（windows.h / process.h / gl/gl.h）
# 编过。原文 18KB 的 pd_head.h 一个字节都没改。
#   * `-fms-extensions`：`10000000000000I64` 这种 MSVC 整数后缀（pd_host_gl.c:722）；
#   * 那三个 `-Wno-`：原文是 C89，而 clang 16 起把 implicit-int / implicit-decl /
#     int-conversion 提成了错误。
# 这一步只到 .o —— 连成可执行还差那 78 个 win32 函数的实现（下一步）。
echo "== polydraw（只到 .o）"
$CC -arch arm64 $OPT -fms-extensions \
	-Wno-implicit-int -Wno-implicit-function-declaration -Wno-int-conversion -w \
	-I polydraw_src -I port/a64/winshim -include port/pd_port.h \
	-c polydraw_src/polydraw.a64.stitch.c -o "$OUT/polydraw.o"
echo "-> $OUT/polydraw.o"

# 连成可执行：polydraw.o + kplib.o + eval.o + 那三份 port/a64 的实现。
#   * pd_win_a64.c  —— 81 个 win32 函数（计时/ini 是真的，窗口/菜单/对话框空壳）
#   * pd_gl_cgl.c   —— wgl* 走 CGL 离屏上下文 + FBO；SwapBuffers 是"一帧画完"的钩子
#                      （数帧、到点 glReadPixels 写 PNG，PNG 写出器也在里头）
#   * pd_main_a64.c —— main()：读 .pss 进一格全局，GetWindowText 回它（假编辑框），
#                      然后交给原文的 WinMain，`/bench:N` 让它自己计时并跑满退出
#   * pd_gui_glfw.c  —— `--gui` 那条腿：GLFW 开真窗口（legacy 2.1 上下文），
#                      每帧把 FBO 那一块 blit 到窗口，键鼠喂回 polydraw
echo "== eval.o（给 polydraw 连的那份，不带 main）"
$CC -arch arm64 $OPT -w -I polydraw_src -include port/pd_port.h -DCOMPILE=0 \
	-c polydraw_src/eval.a64.stitch.c -o "$OUT/eval.o"
for f in pd_win_a64 pd_gl_cgl pd_main_a64; do
	$CC -arch arm64 $OPT -w -I polydraw_src -I port/a64/winshim -include port/pd_port.h \
		-c "port/a64/$f.c" -o "$OUT/$f.o"
done
# GUI 那一份不要假 windows.h（它用真 GLFW 头），也不要 -include pd_port.h
$CC -arch arm64 $OPT -w -I /opt/homebrew/include \
	-c port/a64/pd_gui_glfw.c -o "$OUT/pd_gui_glfw.o"
echo "== polydraw_a64"
$CC -arch arm64 "$OUT/polydraw.o" "$OUT/kplib.o" "$OUT/eval.o" \
	"$OUT/pd_win_a64.o" "$OUT/pd_gl_cgl.o" "$OUT/pd_main_a64.o" "$OUT/pd_gui_glfw.o" \
	-framework OpenGL -L/opt/homebrew/lib -lglfw -o "$OUT/polydraw_a64"
echo "-> $OUT/polydraw_a64"
