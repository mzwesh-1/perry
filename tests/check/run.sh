#!/bin/sh
# Runs PERRY --check on each test and compares with the .expected file.
#   sh tests/check/run.sh            check
#   sh tests/check/run.sh --update   write the .expected files again
cd "$(dirname "$0")" || exit 1
perry=../../PERRY
failed=0
for f in *.perry mod/uses_tools.perry; do
    out=$($perry --check "$f" 2>&1)
    if [ "$1" = "--update" ]; then
        printf '%s\n' "$out" > "${f%.perry}.expected"
    elif [ "$out" != "$(cat "${f%.perry}.expected")" ]; then
        echo "FAIL $f"
        printf '%s\n' "$out" | diff "${f%.perry}.expected" - | head -20
        failed=1
    fi
done
[ "$failed" = 0 ] && [ "$1" != "--update" ] && echo "all --check tests pass"
exit $failed
