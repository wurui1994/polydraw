#!/usr/bin/env bash
# bench/gui-a64.sh —— GUI 那条腿的判据（`--gui`：真窗口 + 实时循环）。
#
# 看不见窗口也要能判，所以判两件**能量到**的事（`PD_GUIDBG=1` 印在 stderr 上）：
#   1. 窗口那张帧缓冲里**非黑像素的个数** > 0 —— 说明这一帧真画到窗口上了
#      （读在 swap 之前，swap 之后后台缓冲未定义）。抽样点靠不住：
#      `tigrou/clock.pss` 是黑底细线，3x3 那九个点一个都碰不上；
#   2. 标题栏那一行有 fps 且 > 0 —— 原文每秒往标题写一次（`pd_win.c:768`），
#      我们把 `SetWindowText` 转给 `glfwSetWindowTitle`，于是它同时是"帧在推进"的证据。
#
# 每份跑 SECS 秒就 kill（GUI 是跑到关窗为止，没有自己退出的口）。
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${BIN:-bench/out/polydraw_a64}
SECS=${SECS:-5}
[ -x "$BIN" ] || { echo "先跑 bench/build-a64.sh"; exit 1; }

pass=0; fail=0
for f in ken/ceilflor2.pss ken/texture.pss tigrou/clock.pss; do
	nm=$(basename "$f" .pss)
	log=$(mktemp /tmp/pd-gui-XXXXXX)
	PD_GUIDBG=1 timeout "$SECS" "$BIN" "$f" --gui --size 640x480 >"$log" 2>&1
	sum=$(grep -o "非黑像素 [0-9]*" "$log" | sed 's/[^0-9]//g' | sort -n | tail -1)
	sum=${sum:-0}
	fps=$(grep -o "([0-9.]* fps)" "$log" | tail -1 | tr -dc '0-9.')
	if [ "$sum" -gt 0 ] && [ -n "${fps:-}" ] && [ "${fps%%.*}" -gt 0 ]; then
		printf '  ok   %-14s 非黑像素=%s  %s fps\n' "$nm" "$sum" "$fps"; pass=$((pass+1))
		rm -f "$log"
	else
		printf '  FAIL %-14s 非黑像素=%s  fps=%s（日志 %s）\n' "$nm" "$sum" "${fps:-无}" "$log"; fail=$((fail+1))
	fi
done
echo
echo "$pass passed, $fail failed（GUI：窗口真画了 + 标题有 fps）"
[ "$fail" = 0 ]
