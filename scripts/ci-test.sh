#!/bin/sh
# Run a QtTest binary and, when it fails, turn each failure and the output that led to
# it into a GitHub Actions annotation, readable without downloading the log.
# Usage: scripts/ci-test.sh [sudo] ./build/tests/tst_f3 [qtest options...]
set -u
out=$(mktemp)
"$@" >"$out" 2>&1
status=$?
cat "$out"
if [ "$status" -ne 0 ] && [ -n "${GITHUB_ACTIONS:-}" ]; then
    name=$1
    [ "$name" = sudo ] && name=$2
    name=$(basename "$name")
    # One annotation per failure (at most 3): the 30 lines before it and its location.
    # Multi-line messages use %0A; % itself must be escaped first.
    lines=$(grep -n -E '^(FAIL!|QFATAL)|Received a fatal error' "$out" | cut -d: -f1 | head -n 3)
    [ -n "$lines" ] || lines=$(wc -l <"$out")
    for n in $lines; do
        from=$((n > 30 ? n - 30 : 1))
        msg=$(sed -n "${from},$((n + 2))p" "$out" | sed -e 's/%/%25/g' -e 's/\r/%0D/g' | awk '{printf "%s%%0A", $0}')
        printf '::error title=%s::%s\n' "$name" "$msg"
    done
fi
rm -f "$out"
exit "$status"
