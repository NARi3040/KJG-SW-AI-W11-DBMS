#!/bin/sh
# 사용법: tests/run_sql.sh <minidb 실행 파일>
# tests/sql/*.sql을 테스트마다 빈 임시 폴더에서 실행하고, 출력(stdout+stderr)을 같은 이름의 .out과 비교해요.
set -u

if [ $# -ne 1 ] || [ ! -x "$1" ]; then
    echo "usage: $0 <minidb 실행 파일>" >&2
    exit 2
fi
bin=$(realpath "$1")

pass=0
fail=0
for sql in tests/sql/*.sql; do
    [ -e "$sql" ] || continue
    name=${sql%.sql}
    abs_sql=$(realpath "$sql")
    if [ ! -f "$name.out" ]; then
        echo "FAIL $sql: $name.out 없음"
        fail=$((fail + 1))
        continue
    fi
    dir=$(mktemp -d)
    (cd "$dir" && "$bin" "$abs_sql" >actual 2>&1)
    if diff -u "$name.out" "$dir/actual"; then
        echo "PASS $sql"
        pass=$((pass + 1))
    else
        echo "FAIL $sql"
        fail=$((fail + 1))
    fi
    rm -rf "$dir"
done

echo "SQL 테스트: 통과 $pass, 실패 $fail"
[ "$fail" -eq 0 ]
