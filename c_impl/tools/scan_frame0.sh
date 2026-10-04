#!/usr/bin/env bash
# Scan ALL .kc demos at --frame 0 with a 3s per-script timeout.
# Flags solid/uniform output (a likely correctness bug) and timeouts.
set -u
DEMOS=${1:-/Users/wurui/Downloads/evaldraw/demos}
BIN=/Users/wurui/Documents/polydraw/c_impl/build/evaldraw
PER_TIMEOUT=3
W=320
H=240
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT="${SCAN_OUT_DIR:-$SCRIPT_DIR/_scan_tmp}"
echo "$OUT"
mkdir -p "$OUT"

run_one() {
  f=$1
  base=$(basename "$f" .kc)
  png="$OUT/$base.png"
  t0=$(python3 -c 'import time;print(time.time())')
  echo $f
  msg=$(timeout "${PER_TIMEOUT}s" "$BIN" "$f" --frame 0 --w "$W" --h "$H" --no-jit -o "$png" 2>&1)
  echo $msg
  rc=$?
  t1=$(python3 -c 'import time;print(time.time())')
  ms=$(python3 -c "print(f'{($t1-$t0)*1000:.0f}')")

  if [ $rc -eq 124 ]; then
    printf '%s|TIMEOUT|>%s000|-\n' "$base" "$PER_TIMEOUT"
    return
  fi
  if [ $rc -ne 0 ] || [ ! -s "$png" ]; then
    why=$(echo "$msg" | tr '\n' ' ' | cut -c1-70)
    printf '%s|FAIL|%s|%s\n' "$base" "$ms" "${why}"
    return
  fi
  stat=$(python3 - "$png" <<'PY'
import sys,zlib,struct
p=sys.argv[1]
d=open(p,'rb').read();pos=8;w=h=0;bd=ct=0;idat=b''
while pos<len(d):
    ln=struct.unpack('>I',d[pos:pos+4])[0];typ=d[pos+4:pos+8];body=d[pos+8:pos+8+ln]
    if typ==b'IHDR':w,h,bd,ct=struct.unpack('>IIBB',body[:10])
    elif typ==b'IDAT':idat+=body
    elif typ==b'IEND':break
    pos+=12+ln
raw=zlib.decompress(idat);ch={0:1,2:3,4:2,6:4}[ct];stride=w*ch
prev=bytearray(stride);out=[];i=0
for y in range(h):
    ft=raw[i];i+=1;line=bytearray(raw[i:i+stride]);i+=stride
    if ft==1:
        for x in range(ch,stride):line[x]=(line[x]+line[x-ch])&255
    elif ft==2:
        for x in range(stride):line[x]=(line[x]+prev[x])&255
    elif ft==3:
        for x in range(stride):
            a=line[x-ch]if x>=ch else 0;line[x]=(line[x]+((a+prev[x])>>1))&255
    elif ft==4:
        for x in range(stride):
            a=line[x-ch]if x>=ch else 0;b=prev[x];c=prev[x-ch]if x>=ch else 0
            pp=a+b-c;pa,pb,pc=abs(pp-a),abs(pp-b),abs(pp-c)
            pr=a if(pa<=pb and pa<=pc)else(b if pb<=pc else c)
            line[x]=(line[x]+pr)&255
    out.append(bytes(line));prev=line
px=b''.join(out);n=w*h
nonblack=0;cols=set();distinct=set()
for k in range(0,len(px),ch):
    r,g,b=px[k],px[k+1]if ch>1 else px[k],px[k+2]if ch>2 else px[k]
    if r or g or b:nonblack+=1
    if len(cols)<5000:cols.add((r,g,b))
# solid = only 1 color => almost certainly a bug
solid=1 if len(cols)<=1 else 0
print(f"{nonblack*100//max(n,1)},{len(cols)},{solid}")
PY
)
  nb=${stat%%,*}; rest=${stat#*,}; cols=${rest%%,*}; solid=${stat##*,}
  flag=""
  if [ "$solid" = "1" ]; then flag=" SOLID!"; fi
  printf '%s|OK|%s|nonblack=%s%% colors=%s%s\n' "$base" "$ms" "$nb" "$cols" "$flag"
}
export -f run_one
export BIN OUT PER_TIMEOUT W H

find "$DEMOS" -name "*.kc" -print0 | sort -z | xargs -0 -P 6 -I{} bash -c "run_one {}" > "$OUT/all.txt" 2>/dev/null

echo "=== TIMEOUT / FAIL ==="
{ echo "script|status|ms|detail"; grep -E '\|(TIMEOUT|FAIL)\|' "$OUT/all.txt"; } | column -t -s'|'
echo
echo "=== SOLID (likely bug) ==="
grep -i 'SOLID' "$OUT/all.txt" || echo "(none)"
echo
echo "=== OK slowest first ==="
{ echo "script|status|ms|detail"; grep '|OK|' "$OUT/all.txt" | sort -t'|' -k3 -rn | head -40; } | column -t -s'|'
echo
total=$(find "$DEMOS" -name '*.kc' | wc -l | tr -d ' ')
ok_n=$(grep -c '|OK|' "$OUT/all.txt" || true)
to_n=$(grep -c '|TIMEOUT|' "$OUT/all.txt" || true)
fa_n=$(grep -c '|FAIL|' "$OUT/all.txt" || true)
so_n=$(grep -ci 'SOLID' "$OUT/all.txt" || true)
echo "total=$total ok=$ok_n timeout=$to_n fail=$fa_n solid=$so_n"
