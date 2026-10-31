#!/bin/bash
# Compare PERRY --check against running each example.
P=${PERRY:-$(cd "$(dirname "$0")/.." && pwd)/PERRY}
cd "$1"
for f in *.perry; do
  run=$(timeout 10 $P "$f" </dev/null 2>&1); rc=$?
  chk=$(timeout 10 $P --check "$f" 2>&1); crc=$?
  rerr=$(echo "$run" | grep -m1 ': error:' | sed 's/^[^:]*:\([0-9]*\): error: /\1: /')
  cerr=$(echo "$chk" | grep ': error:' | sed 's/^[^:]*:\([0-9]*\): error: /\1: /' | tr '\n' '|')
  printf "%s\t%s\t%s\tRUN: %s\tCHECK: %s\n" "$rc" "$crc" "$f" "$rerr" "$cerr"
done
