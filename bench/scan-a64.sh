#!/usr/bin/env bash
# bench/scan-a64.sh —— 把整份 .pss 语料在 arm64 上过一遍，给出**分类账**。
#
# 为什么要它：`render-a64.sh` 只压五份（当判据用，要快）。这一份是"摸家底"：
# 53 份脚本各跑一趟，分成 ok / 空画面 / 着色器报错 / 崩 / 超时，顺带记下 ms/帧。
# 有了这张表才知道下一刀该往哪儿使劲，而不是逐份猜。
#
# 用法：bench/scan-a64.sh [每份的帧数，默认 10]
#       结果落 bench/out/scan.tsv，每份的 polydraw 诊断落 bench/out/scanlog/。
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${BIN:-bench/out/polydraw_a64}
N=${1:-10}
TMO=${TMO:-20}
OUT=bench/out
LOGD=$OUT/scanlog
[ -x "$BIN" ] || { echo "先跑 bench/build-a64.sh"; exit 1; }
mkdir -p "$LOGD" "$OUT/scanpng"

colors () { # 抽样统计不同颜色数；文件不在就回 0
	python3 - "$1" <<'PY' 2>/dev/null || echo 0
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
}

printf '%s\n' "# 脚本	判定	颜色数	ms/帧	备注" > "$OUT/scan.tsv"
nok=0; nempty=0; nshader=0; ncrash=0; ntmo=0

for f in ken/*.pss tigrou/*.pss; do
	[ -f "$f" ] || continue
	nm=$(basename "$f" .pss | tr ' ' '_')
	log="$LOGD/$nm.log"
	png="$OUT/scanpng/$nm.png"
	rm -f polydraw_bench.txt "$png"
	timeout "$TMO" "$BIN" "$f" --frames "$N" --out "$png" >"$log" 2>&1
	rc=$?
	# polydraw_bench.txt 每行是 "\tfps\tms/帧" —— 开头那个 tab 让 $1 是空串，
	# 所以 ms/帧 是**第 3 格**。先前取的是 $2（fps），于是整张表的"ms/帧"
	# 印的其实是 fps（tree 那格 46.8 是 46.8 fps，不是 46.8 ms）。
	ms=$(awk -F'\t' 'END{if(NF>=3)printf "%.3f",$3}' polydraw_bench.txt 2>/dev/null)
	nc=$(colors "$png")
	note=""
	if   [ "$rc" = 124 ]; then verdict=超时; ntmo=$((ntmo+1))
	elif [ "$rc" != 0 ];  then verdict=崩;   ncrash=$((ncrash+1)); note="rc=$rc"
	elif grep -q "ERROR: [0-9]" "$log"; then
		verdict=着色器错; nshader=$((nshader+1))
		note=$(grep -m1 "ERROR: [0-9]" "$log" | cut -c1-70)
	elif [ "${nc:-0}" -ge 2 ]; then verdict=ok; nok=$((nok+1))
	else verdict=空画面; nempty=$((nempty+1))
	fi
	printf '%s\t%s\t%s\t%s\t%s\n' "$nm" "$verdict" "${nc:-0}" "${ms:-}" "$note" >> "$OUT/scan.tsv"
	printf '%-26s %-8s 色=%-5s %sms %s\n' "$nm" "$verdict" "${nc:-0}" "${ms:-?}" "$note"
done

echo
echo "ok $nok / 空画面 $nempty / 着色器错 $nshader / 崩 $ncrash / 超时 $ntmo（共 $((nok+nempty+nshader+ncrash+ntmo)) 份）"
echo "表在 $OUT/scan.tsv，每份的 polydraw 诊断在 $LOGD/"
