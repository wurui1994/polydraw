#!/usr/bin/env bash
# bench/docker-x64.sh —— 在 linux/amd64 容器里跑一条命令（这台机器是 Apple Silicon，
# 所以那一档是 **qemu 转译**：验对错可以，量速度不行）。
#
#   bash bench/docker-x64.sh build              # 编（eval + polydraw）
#   bash bench/docker-x64.sh test               # eval 判据（PD_JIT=0 / 1 各一趟）
#   bash bench/docker-x64.sh ops                # 按指令族那 83 行，落到 bench/out-x64/
#   bash bench/docker-x64.sh render             # 出图判据（Xvfb 当显示）
#   bash bench/docker-x64.sh gui                # GUI 判据（Xvfb + 非黑像素/fps）
#   bash bench/docker-x64.sh guix               # **验 GLFW**：窗口开到宿主 XQuartz 上
#   bash bench/docker-x64.sh vnc ken/balls.pss  # **看得见的窗口**：VNC 到宿主
#   bash bench/docker-x64.sh x11 xeyes          # 往宿主 XQuartz 发任意 X11 客户端

#   bash bench/docker-x64.sh sh                 # 进去自己敲
#
# ## 显示从哪儿来（三档，别混）
#
#   * **出图 / 扫描**：**无头** —— `port/x64/pd_gl_egl.c` 那格 EGL surfaceless 上下文
#     + mesa 的 llvmpipe。不要 X、不要窗口（这两档脚本里还故意 `unset DISPLAY`，
#     顺带把"出图不要 X"这件事判了）。`PD_EGL=0` 退回 GLFW 的不可见窗口（那条要 X）；
#   * **GUI 判据**：容器自己的 `Xvfb :99` —— 窗口那条腿走的是 GLFW，得有个 X 服务器；
#   * **XQuartz**（`x11` / `guix` 两档）：**用来验 GLFW 那条腿**能不能把窗口开到宿主上。
#     结论（量过，别再猜）：
#       * X11 通 —— `xeyes` 能显示；
#       * **GLX 也通，但是"间接"的** —— mesa 自己的 `drisw`（软件屏）建不起来
#         （`glx: failed to create drisw screen`，`glxinfo -B` 就死在这儿），
#         但它会**退到 indirect GLX**：`glxgears` 跑得起来，`GL_RENDERER = Apple M1`、
#         `GL_VERSION = 1.4 (2.1 Metal)` —— 真正渲染的是**宿主那颗 GPU**；
#       * 代价：间接 GLX **一个扩展都不报**（`GL_EXTENSIONS` 空、GLX 只到 1.4），
#         所以 FBO 与着色器那一族在这条路上没有 —— 用着色器的脚本会退化；
#       * **回读的像素不可信**：`gui-a64.sh` 那半判据（非黑像素数）在这条路上
#         `tigrou/clock.pss`（黑底细线）每一帧都报"整屏非黑"，明显是垃圾。
#         所以这一档只判得了"窗口开出来了 + 帧在推进（19~20fps）+ 没报错"，
#         画得对不对要用眼睛看，或者走 Xvfb / VNC 那两档。
#     宿主那侧要先：`defaults write org.xquartz.X11 nolisten_tcp -bool false`（退出再开
#     XQuartz）+ `xhost +`（跑完 `xhost -` 收回）。



set -uo pipefail
cd "$(dirname "$0")/.."

IMG=${IMG:-pd_x64}
PLAT=--platform=linux/amd64
MNT=(-v "$PWD:/src" -w /src)
CMD=${1:-sh}; shift || true

# 出图/扫描那一档：**无头**（EGL surfaceless + llvmpipe）。故意把 DISPLAY 抹掉 ——
# 判据顺带证明"出图不要 X"。
HEADLESS='unset DISPLAY;'
# GUI 那一档才要 X：容器里起一格 Xvfb（`XDG_RUNTIME_DIR` 要有，GLFW 3.4+ 会先探
# Wayland，没有这一格就先报一句错；代码那侧也点明了 GLFW_PLATFORM_X11）。
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
		"$IMG" bash -lc "$HEADLESS bash bench/render-a64.sh"
	;;
scan)
	# 整份语料（53 份）过一遍，落 bench/out-x64/scan.tsv。qemu + llvmpipe，所以
	# **只看分类、别看毫秒**。TMO 放宽一点（软件光栅慢）。
	exec docker run --rm $PLAT "${MNT[@]}" -e OUT=bench/out-x64 -e BIN=bench/out-x64/polydraw_x64 \
		-e TMO="${TMO:-60}" "$IMG" bash -lc "$HEADLESS bash bench/scan-a64.sh ${1:-10}"
	;;
gui)
	# SECS 默认给到 12（不是 gui-a64.sh 自己的 5）：fps 那半判据读的是**标题栏**，
	# 而原文每秒才写一次标题 —— qemu + llvmpipe 上 5 秒可能一次都没写上，
	# `texture` 就这么假红过一次（同一份 SECS=12 再跑就 145.9 fps）。
	exec docker run --rm $PLAT "${MNT[@]}" -e BIN=bench/out-x64/polydraw_x64 \
		-e SECS="${SECS:-12}" "$IMG" bash -lc "$XVFB bash bench/gui-a64.sh"
	;;
guix)
	# **验 GLFW 那条腿**：窗口开到宿主的 XQuartz 上（间接 GLX，宿主 GPU 渲染）。
	# 只信"窗口开了 + 帧在推进"那一半 —— 回读的像素在这条路上是垃圾（见头注）。
	echo '注意：这一档的「非黑像素」那半判据不可信（间接 GLX），只看 fps 与有没有报错'
	exec docker run --rm $PLAT "${MNT[@]}" -e DISPLAY=host.docker.internal:0 \
		-e BIN=bench/out-x64/polydraw_x64 -e SECS="${SECS:-12}" "$IMG" bash -lc \
		'export XDG_RUNTIME_DIR=/tmp/xdg; mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR
		 xdpyinfo >/dev/null 2>&1 || {
			echo "连不上 $DISPLAY —— XQuartz 那两步：";
			echo "  1) defaults write org.xquartz.X11 nolisten_tcp -bool false（然后退出再开 XQuartz）";
			echo "  2) xhost +（跑完 xhost - 收回）";
			exit 1; }
		 bash bench/gui-a64.sh'
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
	echo "不认识的子命令：$CMD（build/test/ops/render/scan/gui/guix/vnc/x11/sh）"; exit 1
	;;
esac
