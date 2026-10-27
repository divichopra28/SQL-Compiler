#!/bin/sh
# ---------------------------------------------------------------------
#  Regression tests for SQL-C
#
#  Every tests/<category>/*.sql file is run in compact mode against a
#  fresh database that has been loaded with tests/sample_data.sql.
#  The output is compared with the matching .expected file.
#
#    sh tests/run_tests.sh            run all tests
#    UPDATE=1 sh tests/run_tests.sh   re-record the expected outputs
# ---------------------------------------------------------------------
cd "$(dirname "$0")/.." || exit 1
BIN=./sqlc
[ -x "$BIN" ] || { echo "build first: make"; exit 1; }

pass=0; fail=0; new=0
for f in tests/ddl/*.sql tests/dml/*.sql tests/dql/*.sql tests/dcl/*.sql tests/tcl/*.sql tests/errors/*.sql; do
    tmp=$(mktemp -d)
    $BIN -c --no-color -d "$tmp" tests/sample_data.sql > /dev/null 2>&1
    $BIN -c --no-color -d "$tmp" "$f" 2>&1 | sed -E 's/ \([0-9.]+ sec\)//' > "$tmp/out.txt"
    exp="${f%.sql}.expected"
    if [ ! -f "$exp" ] || [ "$UPDATE" = "1" ]; then
        cp "$tmp/out.txt" "$exp"
        echo "  [SAVED] $f"
        new=$((new + 1))
    elif diff -u "$exp" "$tmp/out.txt" > "$tmp/diff.txt"; then
        echo "  [PASS]  $f"
        pass=$((pass + 1))
    else
        echo "  [FAIL]  $f"
        head -40 "$tmp/diff.txt"
        fail=$((fail + 1))
    fi
    rm -rf "$tmp"
done
echo
echo "passed: $pass   failed: $fail   recorded: $new"
[ "$fail" -eq 0 ]
