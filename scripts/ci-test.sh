#!/bin/sh
# Run a QtTest binary and, when it fails, turn its failure messages into
# GitHub Actions annotations so they are readable without downloading the log.
# Usage: scripts/ci-test.sh ./build/tests/tst_f3 [qtest options...]
set -u
out=$(mktemp)
"$@" >"$out" 2>&1
status=$?
cat "$out"
if [ "$status" -ne 0 ] && [ -n "${GITHUB_ACTIONS:-}" ]; then
    name=$(basename "$1")
    # FAIL!/QWARN lines plus the "Loc:" line that follows each failure.
    grep -A1 -E '^(FAIL!|QWARN|QFATAL)|Received a fatal error' "$out" | grep -v '^--$' | head -n 40 |
        while IFS= read -r line; do
            # Annotations are one line: escape what the workflow command syntax needs.
            msg=$(printf '%s' "$line" | sed -e 's/%/%25/g' -e 's/\r/%0D/g')
            printf '::error title=%s::%s\n' "$name" "$msg"
        done
    tail -n 5 "$out" | while IFS= read -r line; do
        printf '::error title=%s (tail)::%s\n' "$name" "$line"
    done
fi
rm -f "$out"
exit "$status"
