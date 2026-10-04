#!/usr/bin/env bash
# Scan all .pss scripts and report per-frame cost (llvm mode) via phaseprof.
# Each script is guarded by `timeout` so a slow/looping pss cannot stall the
# whole scan. Timeouts are reported separately and NOT mixed with real failures.
set -u
ROOT=/Users/wurui/Documents/polydraw
BIN=$ROOT/c_impl/build/phaseprof
RES=256
FRAMES=90
PER_TIMEOUT=30   # seconds per script

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SCAN_DIR="${SCAN_OUT_DIR:-$SCRIPT_DIR/_scan_tmp}"
mkdir -p "$SCAN_DIR"
ok="$SCAN_DIR/fps_ok.txt"
timeout_list="$SCAN_DIR/fps_timeout.txt"
fail_list="$SCAN_DIR/fps_fail.txt"
unsupported_list="$SCAN_DIR/fps_unsupported.txt"
: > "$ok"; : > "$timeout_list"; : > "$fail_list"; : > "$unsupported_list"

while IFS= read -r f; do
  [ -s "$f" ] || continue
  echo $f
  base=$(basename "$f")
  # ARB assembly shaders (!ARBvp1.0 / !ARBfp1.0) are a known renderer gap:
  # the GL backend only accepts GLSL. Flag them as UNSUPPORTED, not FAIL,
  # so a missing feature isn't mistaken for a real regression.
  if grep -qE '!!ARB(vp|fp)1\.0' "$f"; then
    printf '%s | ARB-assembly shader (unsupported by GLSL backend)\n' "$base" >> "$unsupported_list"
    continue
  fi
  # run under `timeout`; capture the grep pipeline result for TIMEOUT detection
  line=$(timeout "${PER_TIMEOUT}s" "$BIN" "$f" --frames "$FRAMES" --res "$RES" 2>/dev/null | grep -E 'llvm ')
  rc=${PIPESTATUS[0]}
  if [ -z "$line" ]; then
    if [ "$rc" -eq 124 ]; then
      printf '%s | TIMEOUT(>%ss)\n' "$base" "$PER_TIMEOUT" >> "$timeout_list"
    else
      printf '%s | FAIL\n' "$base" >> "$fail_list"
    fi
    continue
  fi
  fps=$(echo "$line" | grep -oE '\([ ]*[0-9.]+ fps\)' | grep -oE '[0-9.]+')
  run=$(echo "$line" | grep -oE 'run=[ ]*[0-9.]+' | grep -oE '[0-9.]+')
  rend=$(echo "$line" | grep -oE 'render=[ ]*[0-9.]+' | grep -oE '[0-9.]+')
  readm=$(echo "$line" | grep -oE 'read=[ ]*[0-9.]+' | grep -oE '[0-9.]+')
  draws=$(echo "$line" | grep -oE 'draws/frame=[ ]*[0-9]+' | grep -oE '[0-9.]+')
  printf '%s | %s | %s | %s | %s | %s\n' "$base" "$fps" "$run" "$rend" "$readm" "$draws" >> "$ok"
done < <(find "$ROOT/ken" "$ROOT/tigrou" -name '*.pss' 2>/dev/null)

echo "=== OK (parsed) ==="
{ echo "script | llvm_fps | run_ms | render_ms | read_ms | draws/frame"; sort -t'|' -k2 -n "$ok"; } | column -t -s'|'

echo
echo "=== TIMEOUT (skipped, analyze separately) ==="
column -t -s'|' "$timeout_list"

echo
echo "=== FAIL (real run/compile error) ==="
column -t -s'|' "$fail_list"

echo
echo "=== UNSUPPORTED (known feature gap, not a regression) ==="
column -t -s'|' "$unsupported_list"
