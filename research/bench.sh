#!/bin/sh
# Times PERRY --check: on every example, and on generated programs of growing size.
#   sh research/bench.sh <examples folder> [old PERRY to compare with]
PERRY=${PERRY:-$(cd "$(dirname "$0")/.." && pwd)/PERRY}
EX=$1; OLD=$2
cd "$EX" || exit 1
time_all() { start=$(date +%s%N); for f in *.perry; do "$1" --check "$f" >/dev/null 2>&1; done; end=$(date +%s%N); echo $(( (end - start) / 1000000 )); }
n=$(ls *.perry | wc -l)
echo "$n examples, total ms for --check: new $(time_all "$PERRY")${OLD:+, old parse-only $(time_all "$OLD")}"
tmp=$(mktemp -d)
for case in "1000 40" "10000 40" "100000 40" "1000 100000" "10000 100000"; do
  set -- $case; lines=$1; names=$2
  python3 - "$lines" "$names" > "$tmp/big.perry" <<'PY'
import sys
n = int(sys.argv[1]); names = int(sys.argv[2])
out = ["define_ with_fee(amount):", "    give_back_ amount + 2% of amount", "rate = 11.75% per year", "total = 0.00 ZAR"]
i = 0
while len(out) + 8 <= n:
    k = i % names
    out += [f"price{k} = {100 + i % 900}.50 ZAR",
            f"usd{k} = convert_ price{k} to_ USD at_rate_ 18.20 ZAR per USD",
            f"check_if_ price{k} is_greater_than_ 500.00 ZAR:",
            f"    price{k} = with_fee(price{k})",
            f"go_through_ [price{k}, 10.00 ZAR] as_ p:",
            f"    total = total + p",
            f"show_me_results(future_value_(price{k}, rate, 5 years))",
            f"show_me_results(usd{k})"]
    i += 1
print("\n".join(out))
PY
  start=$(date +%s%N); "$PERRY" --check "$tmp/big.perry" >/dev/null 2>&1; rc=$?; end=$(date +%s%N)
  echo "$lines lines, $( [ "$names" -lt 1000 ] && echo "$names names reused" || echo "every name new"): $(( (end - start) / 1000000 )) ms (exit $rc)"
done
rm -rf "$tmp"
