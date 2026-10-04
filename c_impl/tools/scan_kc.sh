#!/usr/bin/env bash
# Batch-scan evaldraw .kc demos for render speed + correctness.
# HARD CONSTRAINT: the whole scan must finish within TOTAL_BUDGET seconds.
# Strategy: small resolution, 1 frame, short per-script timeout, run in parallel.
set -u

DEMOS=${1:-/Users/wurui/Downloads/evaldraw/demos}
BIN=/Users/wurui/Documents/polydraw/c_impl/build/evaldraw
TOTAL_BUDGET=55      # seconds for the entire scan (leave headroom under 60s)
PER_TIMEOUT=3        # seconds per script
JOBS=8               # parallel workers
W=96
H=72
FRAME=2

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT="${SCAN_OUT_DIR:-$SCRIPT_DIR/_scan_tmp}"
mkdir -p "$OUT"

run_one() {
  f=$1
  base=$(basename "$f")
  png="$OUT/$base.png"
  t0=$(python3 -c 'import time;print(time.time())')
  msg=$(timeout "${PER_TIMEOUT}s" "$BIN" "$f" --frame "$FRAME" --w "$W" --h "$H" --no-jit -o "$png" 2>&1)
  rc=$?
  t1=$(python3 -c 'import time;print(time.time())')
  ms=$(python3 -c "print(f'{($t1-$t0)*1000:.0f}')")

  if [ $rc -eq 124 ]; then
    printf '%s|TIMEOUT|>%s000|-\n' "$base" "$PER_TIMEOUT"
    return
  fi
  if [ $rc -ne 0 ] || [ ! -s "$png" ]; then
    why=$(echo "$msg" | tr '\n' ' ' | cut -c1-70)
    printf '%s|FAIL|%s|%s\n' "$base" "$ms" "${why:--}"
    return
  fi
  # correctness proxy: fraction of non-black pixels + distinct colors
  stat=$(python3 - "$png" <<'PY'
import sys,zlib,struct
p=sys.argv[1]
d=open(p,'rb').read()
# minimal PNG decode (8-bit RGB/RGBA, no interlace)
pos=8; w=h=0; bd=ct=0; idat=b''
while pos<len(d):
    ln=struct.unpack('>I',d[pos:pos+4])[0]; typ=d[pos+4:pos+8]; body=d[pos+8:pos+8+ln]
    if typ==b'IHDR': w,h,bd,ct=struct.unpack('>IIBB',body[:10])
    elif typ==b'IDAT': idat+=body
    elif typ==b'IEND': break
    pos+=12+ln
raw=zlib.decompress(idat)
ch={0:1,2:3,4:2,6:4}[ct]
stride=w*ch
prev=bytearray(stride); out=[]
i=0
for y in range(h):
    ft=raw[i]; i+=1
    line=bytearray(raw[i:i+stride]); i+=stride
    if ft==1:
        for x in range(ch,stride): line[x]=(line[x]+line[x-ch])&255
    elif ft==2:
        for x in range(stride): line[x]=(line[x]+prev[x])&255
    elif ft==3:
        for x in range(stride):
            a=line[x-ch] if x>=ch else 0
            line[x]=(line[x]+((a+prev[x])>>1))&255
    elif ft==4:
        for x in range(stride):
            a=line[x-ch] if x>=ch else 0
            b=prev[x]; c=prev[x-ch] if x>=ch else 0
            pp=a+b-c
            pa,pb,pc=abs(pp-a),abs(pp-b),abs(pp-c)
            pr=a if (pa<=pb and pa<=pc) else (b if pb<=pc else c)
            line[x]=(line[x]+pr)&255
    out.append(bytes(line)); prev=line
px=b''.join(out)
n=w*h; nonblack=0; cols=set()
for k in range(0,len(px),ch):
    r,g,b=px[k],px[k+1] if ch>1 else px[k],px[k+2] if ch>2 else px[k]
    if r or g or b: nonblack+=1
    if len(cols)<600: cols.add((r,g,b))
print(f"{nonblack*100//max(n,1)},{len(cols)}")
PY
)
  printf '%s|OK|%s|nonblack=%s%% colors=%s\n' "$base" "$ms" "${stat%%,*}" "${stat##*,}"
}
export -f run_one
export BIN OUT PER_TIMEOUT W H FRAME

# Run the whole scan under one global timeout so we can never exceed the budget.
timeout "${TOTAL_BUDGET}s" bash -c '
  find "$1" -name "*.kc" -print0 | xargs -0 -P '"$JOBS"' -I{} bash -c "run_one {}"
' _ "$DEMOS" > "$OUT/all.txt" 2>/dev/null
scan_rc=$?

echo "=== FAIL / TIMEOUT ==="
{ echo "script|status|ms|detail"; grep -E '\|(FAIL|TIMEOUT)\|' "$OUT/all.txt" | sort; } | column -t -s'|'

echo
echo "=== OK, slowest first ==="
{ echo "script|status|ms|detail"; grep '|OK|' "$OUT/all.txt" | sort -t'|' -k3 -rn; } | column -t -s'|'

echo
total=$(find "$DEMOS" -name '*.kc' | wc -l | tr -d ' ')
done_n=$(wc -l < "$OUT/all.txt" | tr -d ' ')
ok_n=$(grep -c '|OK|' "$OUT/all.txt" || true)
to_n=$(grep -c '|TIMEOUT|' "$OUT/all.txt" || true)
fa_n=$(grep -c '|FAIL|' "$OUT/all.txt" || true)
echo "total=$total scanned=$done_n ok=$ok_n timeout=$to_n fail=$fa_n"
[ $scan_rc -eq 124 ] && echo "NOTE: global budget ${TOTAL_BUDGET}s hit; $((total-done_n)) scripts unscanned."
exit 0
