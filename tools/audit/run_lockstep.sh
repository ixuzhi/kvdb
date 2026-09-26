#!/usr/bin/env bash
# N3 同步锁步差分跑分（doc/14 新体系）
# 同一种子驱动两条相同操作序列分别跑在两库上，trace 逐行比较。
# 任何一步逻辑结果（点查/扫描/seek 窗口/快照读）不同即 FAIL。
# 进程内另有排序数组模型自检（exit 2 = 引擎对模型错，不是引擎间分歧）。
# 依赖纪律：不使用 diff/cmp（msys 无 diffutils）。
set -u
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
EVID=${AUDIT_DIR:?AUDIT_DIR must be set}
CC=${CC:-gcc}
CXX=${CXX:-g++}
NOPS=${LOCKSTEP_OPS:-3000}
SEEDS=${LOCKSTEP_SEEDS:-"1 2 3 4 5"}
fails=0

TRIPLE=$("$CC" -dumpmachine)
case "$TRIPLE" in
  *linux*|*-cygwin|*-msys) ;;
  *) echo "N3: need POSIX compiler, got $TRIPLE"; exit 2 ;;
esac
KVDB_LIB=${KVDB_LIB:-$REPO/build/libleveldb.a}
OFFICIAL_LIB=$(ls "$REPO"/build/interop/official/$TRIPLE/lib/libleveldb_official.a 2>/dev/null | head -1)
[[ -f "$KVDB_LIB" && -f "$OFFICIAL_LIB" ]] || { echo "N3: missing archives"; exit 2; }

"$CC" -std=c11 -D_GNU_SOURCE -O2 -Wall -I"$REPO/leveldb/include" \
  -c "$REPO/tools/audit/lockstep.c" -o "$EVID/lockstep.o" || exit 2
"$CC" "$EVID/lockstep.o" "$KVDB_LIB" -o "$EVID/ls_kvdb.exe" || exit 2
"$CXX" "$EVID/lockstep.o" "$OFFICIAL_LIB" -pthread -o "$EVID/ls_official.exe" || exit 2

for seed in $SEEDS; do
  dbk="$EVID/db-kvdb-$seed"; dbo="$EVID/db-official-$seed"
  rm -rf "$dbk" "$dbo"
  "$EVID/ls_kvdb.exe" "$seed" "$NOPS" "$dbk" "$EVID/trace-kvdb-$seed.txt"; rck=$?
  "$EVID/ls_official.exe" "$seed" "$NOPS" "$dbo" "$EVID/trace-official-$seed.txt"; rco=$?
  if [[ $rck -ne 0 || $rco -ne 0 ]]; then
    printf 'N3  FAIL seed=%s: self/model failure kvdb=%d official=%d\n' "$seed" "$rck" "$rco"
    fails=$((fails+1)); continue
  fi
  n1=$(wc -l < "$EVID/trace-kvdb-$seed.txt"); n2=$(wc -l < "$EVID/trace-official-$seed.txt")
  if [[ "$n1" == "$n2" ]] && cmp -s "$EVID/trace-kvdb-$seed.txt" "$EVID/trace-official-$seed.txt" 2>/dev/null; then
    printf 'N3  PASS seed=%s ops=%s: %s trace lines identical\n' "$seed" "$NOPS" "$n1"
  else
    # cmp 可能不存在（msys 无 diffutils 一族）：用 sha256 兜底，再逐行定位
    h1=$(sha256sum "$EVID/trace-kvdb-$seed.txt" | awk '{print $1}')
    h2=$(sha256sum "$EVID/trace-official-$seed.txt" | awk '{print $1}')
    if [[ "$n1" == "$n2" && "$h1" == "$h2" ]]; then
      printf 'N3  PASS seed=%s ops=%s: %s trace lines identical\n' "$seed" "$NOPS" "$n1"
    else
      printf 'N3  FAIL seed=%s: traces diverge (kvdb=%s lines, official=%s lines)\n' "$seed" "$n1" "$n2"
      awk 'NR==FNR{a[FNR]=$0;next} a[FNR]!=$0{printf "    line %d: kvdb[%s] official[%s]\n", FNR, a[FNR], $0; if(++n>=5) exit 0}' \
        "$EVID/trace-kvdb-$seed.txt" "$EVID/trace-official-$seed.txt"
      fails=$((fails+1))
    fi
  fi
done
exit $fails
