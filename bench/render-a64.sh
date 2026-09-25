#!/usr/bin/env bash
# bench/render-a64.sh —— 出图那一轴的判据：几份 .pss 各出一张 PNG，**不许是空画面**。
#
# "不空"的口径：抽样到的**不同颜色数 >= 2**。全透明黑（0,0,0,0）一种色就是没画上去。
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${BIN:-bench/out/polydraw_a64}
OUT=${OUT:-bench/out/png}
[ -x "$BIN" ] || { echo "先跑 bench/build-a64.sh"; exit 1; }
mkdir -p "$OUT"

pass=0; fail=0
one () {
	local f=$1 nm
	nm=$(basename "$f" .pss)
	if ! timeout 60 "$BIN" "$f" --frames 3 --out "$OUT/$nm.png" >"$OUT/$nm.log" 2>&1; then
		fail=$((fail+1)); printf '  FAIL %-22s 跑挂了（rc=%d，看 %s）\n' "$nm" "$?" "$OUT/$nm.log"; return
	fi
	local n
	n=$(python3 - "$OUT/$nm.png" <<'PY'
import sys,zlib,struct
d=open(sys.argv[1],'rb').read(); i=8; idat=b''; w=h=0
while i<len(d):
    n=struct.unpack('>I',d[i:i+4])[0]; tag=d[i+4:i+8]; body=d[i+8:i+8+n]; i+=12+n
    if tag==b'IHDR': w,h=struct.unpack('>II',body[:8])
    elif tag==b'IDAT': idat+=body
raw=zlib.decompress(idat); stride=w*4+1; c=set()
for y in range(0,h,3):
    row=raw[y*stride+1:y*stride+1+w*4]
    for x in range(0,w,3): c.add(tuple(row[x*4:x*4+4]))
print(len(c))
PY
)
	if [ "${n:-0}" -ge 2 ]; then pass=$((pass+1)); printf '  ok   %-22s %s 种颜色\n' "$nm" "$n"
	else fail=$((fail+1)); printf '  FAIL %-22s 空画面（%s 种颜色）\n' "$nm" "${n:-?}"; fi
}

for f in ken/ceilflor2.pss ken/texture.pss ken/orthoglobe.pss tigrou/clock.pss; do
	[ -f "$f" ] && one "$f"
done

# 不计分的那一族：**本机 GL 的上限挡住的**，不是移植的洞。
#   * ken/gspiral.pss —— 片元着色器用了 `&` / `>>`（整数位运算），那是 GLSL 1.30 起才有的；
#     macOS 的 legacy profile 最高到 GL 2.1 / GLSL 1.20，编译期就报
#     `'&' does not operate on 'int' and 'int'`。要它得换 core profile（3.2+），
#     可是 core 里没有固定管线，而 polydraw 的 glBegin/glEnd 一族要固定管线 ——
#     这是一整条另外的路，不在这一轴里。
echo "  skip gspiral                GLSL 1.20 没有整数位运算（macOS legacy GL 的上限）"

echo
echo "$pass passed, $fail failed（出图：PNG 不是空画面）"
[ "$fail" = 0 ]
