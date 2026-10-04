#!/usr/bin/env bash
# Scan EVAL-only cost (bench) for all .pss: interp vs llvm jit.
set -u
ROOT=/Users/wurui/Documents/polydraw
BIN=$ROOT/c_impl/build/bench
FRAMES=200

while IFS= read -r f; do
  [ -s "$f" ] || continue
  echo $f
  out=$("$BIN" "$f" --frames "$FRAMES")
  echo $out
  interp=$(echo "$out" | grep -oE 'interp:[ ]*[0-9.]+ fps' | grep -oE '[0-9.]+')
  llvm=$(echo "$out"   | grep -oE 'llvm:[ ]*[0-9.]+ fps'  | grep -oE '[0-9.]+')
  sljit=$(echo "$out"  | grep -oE 'sljit:[ ]*[0-9.]+ fps' | grep -oE '[0-9.]+')
  if [ -z "$interp" ]; then
    printf '%s | EVAL_FAIL\n' "$(basename "$f")"
    continue
  fi
  printf '%s | interp=%s | llvm=%s | sljit=%s\n' "$(basename "$f")" "$interp" "$llvm" "$sljit"
done < <(find "$ROOT/ken" "$ROOT/tigrou" -name '*.pss' 2>/dev/null) \
  | sort -t'|' -k2 -n
