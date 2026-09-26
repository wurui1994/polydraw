#!/usr/bin/env bash
# bench/docker-x64.sh —— 在 linux/amd64 容器里跑一条命令（这台机器是 Apple Silicon，
# 所以那一档是 **qemu 转译**：验对错可以，量速度不行）。
#
#   bash bench/docker-x64.sh build              # 编（eval + polydraw）
#   bash bench/docker-x64.sh test               # eval 判据（PD_JIT=0 / 1 各一趟）
#   bash bench/docker-x64.sh ops                # 按指令族那 83 行，落到 bench/out-x64/
#   bash bench/docker-x64.sh render             # 出图判据（Xvfb 当显示）
#   bash bench/docker-x64.sh gui                # GUI 判据（Xvfb + 非黑像素/fps）
#   bash bench/docker-x64.sh vnc ken/balls.pss  # **看得见的窗口**：VNC 到宿主
#   bash bench/docker-x64.sh x11 xeyes          # 往宿主 XQuartz 发 X11（**GL 不行**，见下）
#   bash bench/docker-x64.sh sh                 # 进去自己敲
#
# ## 显示从哪儿来（三档，别混）
#
#   * **判据（出图 / GUI）** 用容器自己的 `Xvfb`（`:99`）+ mesa 的 llvmpipe。
#     为什么出图也要 X：macOS 上上下文走 CGL（不要窗口），linux 上走的是 GLFW 的
#     不可见窗口，而 GLFW 要一个显示才给上下文；
#   * **要用眼睛看** 走 **VNC**：容器里 `x11vnc` 把 `:99` 那张屏送出来，宿主
#     `open vnc://127.0.0.1:5900`（macOS 自带"屏幕共享"）。**这是能看见动画的那条路**；
#   * **XQuartz**（`x11` 那一档）：X11 本身**通**（`xeyes` 能显示），但 **GLX 不通** ——
#     容器里的 mesa 要一格 `drisw` 软件屏，而 XQuartz 不给对应的 fbConfig：
#         No matching fbConfigs or visuals found / glx: failed to create drisw screen
#     `enable_iglx` 也没用（试过）。所以 polydraw 的窗口**走不了 XQuartz**，用 VNC 那一档。
#     宿主那侧要先：`defaults write org.xquartz.X11 nolisten_tcp -bool false`（退出再开
#     XQuartz）+ `xhost +`（跑完 `xhost -` 收回）。

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
scan)
	# 整份语料（53 份）过一遍，落 bench/out-x64/scan.tsv。qemu + llvmpipe，所以
	# **只看分类、别看毫秒**。TMO 放宽一点（软件光栅慢）。
	exec docker run --rm $PLAT "${MNT[@]}" -e OUT=bench/out-x64 -e BIN=bench/out-x64/polydraw_x64 \
		-e TMO="${TMO:-60}" "$IMG" bash -lc "$XVFB bash bench/scan-a64.sh ${1:-10}"
	;;
gui)
	exec docker run --rm $PLAT "${MNT[@]}" -e BIN=bench/out-x64/polydraw_x64 \
		"$IMG" bash -lc "$XVFB bash bench/gui-a64.sh"
	;;
vnc)
	# 看得见的那一档：Xvfb 画、x11vnc 送出来。宿主上 `open vnc://127.0.0.1:5900`。
	echo "窗口在 VNC 里：宿主执行  open vnc://127.0.0.1:5900  （密码没设，只听 127.0.0.1）"
	exec docker run --rm -it $PLAT "${MNT[@]}" -p 127.0.0.1:5900:5900 \
		"$IMG" bash -lc "$XVFB x11vnc -display :99 -forever -shared -nopw -quiet -listen 0.0.0.0 >/tmp/vnc.log 2>&1 &
		 sleep 1; bench/out-x64/polydraw_x64 --gui ${1:-ken/balls.pss}"
	;;
x11)
	# 往宿主 XQuartz 发 X11。**GL 不行**（见头注），所以这一档只给非 GL 的客户端用。
	exec docker run --rm -it $PLAT "${MNT[@]}" -e DISPLAY=host.docker.internal:0 \
		"$IMG" bash -lc 'xdpyinfo >/dev/null 2>&1 || {
			echo "连不上 $DISPLAY —— XQuartz 那两步：";
			echo "  1) defaults write org.xquartz.X11 nolisten_tcp -bool false（然后退出再开 XQuartz）";
			echo "  2) xhost +（跑完 xhost - 收回）";
			exit 1; }
		 '"${*:-xeyes}"
	;;
sh)
	exec docker run --rm -it $PLAT "${MNT[@]}" -e DISPLAY=host.docker.internal:0 "$IMG" bash -l
	;;
*)
	echo "不认识的子命令：$CMD（build/test/ops/render/gui/vnc/x11/sh）"; exit 1
	;;
esac
