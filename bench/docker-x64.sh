#!/usr/bin/env bash
# bench/docker-x64.sh —— 在 linux/amd64 容器里跑一条命令（这台机器是 Apple Silicon，
# 所以那一档是 **qemu 转译**：验对错可以，量速度不行）。
#
#   bash bench/docker-x64.sh build              # 编（eval + polydraw）
#   bash bench/docker-x64.sh test               # eval 判据（PD_JIT=0 / 1 各一趟）
#   bash bench/docker-x64.sh ops                # 按指令族那 83 行，落到 bench/out-x64/
#   bash bench/docker-x64.sh render             # 出图判据（Xvfb 当显示）
#   bash bench/docker-x64.sh gui ken/balls.pss  # 真窗口 —— 走 **XQuartz**（见下）
#   bash bench/docker-x64.sh sh                 # 进去自己敲
#
# ## 显示从哪儿来（两档，别混）
#
#   * **出图/判据** 用容器自己的 `Xvfb`（`:99`）—— 与宿主无关，所以判据不会因为
#     XQuartz 没开而红。为什么出图也要 X：macOS 上上下文走 CGL（不要窗口），
#     linux 上走的是 GLFW 的不可见窗口，而 GLFW 要一个显示才给上下文；
#   * **GUI** 用宿主的 **XQuartz**（`DISPLAY=host.docker.internal:0`）。要三步：
#       1. XQuartz 允许 TCP：`defaults write org.xquartz.X11 nolisten_tcp -bool false`
#          然后**退出再开** XQuartz（这一项是持久设置，改完记得知道自己改了什么）；
#       2. `xhost +` 放行（跑完可以 `xhost -` 收回）；
#       3. 这个脚本把 DISPLAY 递进去。
#     `gui` 这一档会先拿 `xdpyinfo` 探一次，通不了就把上面三步印出来、不往下跑。
set -uo pipefail
cd "$(dirname "$0")/.."

IMG=${IMG:-pd_x64}
PLAT=--platform=linux/amd64
MNT=(-v "$PWD:/src" -w /src)
CMD=${1:-sh}; shift || true

# 出图/判据那一档：容器里起一格 Xvfb，DISPLAY=:99。
# `XDG_RUNTIME_DIR` 要有：GLFW 3.4 会先探 Wayland，没有这一格就先报一句错
# （踩过：`glfwInit 失败` 的真原因就是它，代码那侧也点明了 `GLFW_PLATFORM_X11`）。
XVFB='export XDG_RUNTIME_DIR=/tmp/xdg; mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR;
      Xvfb :99 -screen 0 1280x960x24 >/tmp/xvfb.log 2>&1 & sleep 2; export DISPLAY=:99;
      xdpyinfo >/dev/null 2>&1 || { echo "Xvfb 没起来："; cat /tmp/xvfb.log; exit 1; };'

case "$CMD" in
build)
	exec docker run --rm $PLAT "${MNT[@]}" "$IMG" bash -lc \
		'bash bench/build-x64.sh 2>&1 | grep -vE "^In file|warning:|note:|^ *\||^ *\^|^ *~"'
	;;
test)
	exec docker run --rm $PLAT "${MNT[@]}" "$IMG" bash -lc \
		'for m in 0 1; do echo "=== PD_JIT=$m"; PD_JIT=$m BIN=bench/out-x64/eval_x64 bash bench/test-a64.sh | tail -2; done'
	;;
ops)
	exec docker run --rm $PLAT "${MNT[@]}" "$IMG" bash -lc \
		'for m in 0 1; do PD_JIT=$m BIN=bench/out-x64/eval_x64 bash bench/test-ops.sh > bench/out-x64/ops.x64.$m.txt; done; \
		 diff bench/out-x64/ops.x64.0.txt bench/out-x64/ops.x64.1.txt && echo "x64：jit == 解释器，逐行相同"'
	;;
render)
	exec docker run --rm $PLAT "${MNT[@]}" -e OUT=bench/out-x64/png -e BIN=bench/out-x64/polydraw_x64 \
		"$IMG" bash -lc "$XVFB bash bench/render-a64.sh"
	;;
gui)
	exec docker run --rm -it $PLAT "${MNT[@]}" -e DISPLAY=host.docker.internal:0 \
		"$IMG" bash -lc 'xdpyinfo >/dev/null 2>&1 || {
			echo "连不上 $DISPLAY —— XQuartz 那三步：";
			echo "  1) defaults write org.xquartz.X11 nolisten_tcp -bool false（然后退出再开 XQuartz）";
			echo "  2) xhost +（跑完 xhost - 收回）";
			echo "  3) 再跑这一条";
			exit 1; }
		 bench/out-x64/polydraw_x64 --gui '"${1:-ken/balls.pss}"
	;;
sh)
	exec docker run --rm -it $PLAT "${MNT[@]}" "$IMG" bash -l
	;;
*)
	echo "不认识的子命令：$CMD（build/test/ops/render/gui/sh）"; exit 1
	;;
esac
