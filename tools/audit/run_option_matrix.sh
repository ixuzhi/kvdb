#!/usr/bin/env bash
# N2 选项×行为全矩阵跑分（doc/14 新体系）
# 单一驱动（只对官方头编译）链两库，同一场景各跑一遍，
# 对 must-equal 字段做跨引擎等值比对；已归档分歧（doc/10 M-1/F-2）
# 登记在 EXPECTED 表里：出现即记 EXPECTED（N7 检出力证据），
# 表外分歧才是 FAIL。实现定义字段（近似大小）只记录不比对。
# 依赖纪律：不使用 diff/cmp。
set -u
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
EVID=${AUDIT_DIR:?AUDIT_DIR must be set}
CC=${CC:-gcc}
CXX=${CXX:-g++}
DRIVER_DIR=$REPO/tools/audit
fails=0
caught=0

TRIPLE=$("$CC" -dumpmachine)
case "$TRIPLE" in
  *linux*|*-cygwin|*-msys) ;;
  *) echo "N2: need POSIX compiler, got $TRIPLE"; exit 2 ;;
esac
KVDB_LIB=${KVDB_LIB:-$REPO/build/libleveldb.a}
OFFICIAL_LIB=$(ls "$REPO"/build/interop/official/$TRIPLE/lib/libleveldb_official.a 2>/dev/null | head -1)
[[ -f "$KVDB_LIB" && -f "$OFFICIAL_LIB" ]] || { echo "N2: missing archives"; exit 2; }

# 常设 ABI 替换证明：一个 .o，两库各链一次
"$CC" -std=c11 -D_GNU_SOURCE -O2 -Wall -I"$REPO/leveldb/include" \
  -c "$DRIVER_DIR/option_matrix.c" -o "$EVID/option_matrix.o" || exit 2
"$CC" "$EVID/option_matrix.o" "$KVDB_LIB" -o "$EVID/om_kvdb.exe" || exit 2
"$CXX" "$EVID/option_matrix.o" "$OFFICIAL_LIB" -pthread -o "$EVID/om_official.exe" || exit 2

mapfile -t SCENARIOS < <("$EVID/om_kvdb.exe" list)
[[ ${#SCENARIOS[@]} -gt 0 ]] || { echo "N2: driver list failed"; exit 2; }

# 已归档分歧（格: 场景:字段:含义）——它们出现=新方法检出力成立
EXPECTED_DIV=(
  "paranoid-small-record:errclass:doc/10 M-1"
  "paranoid-count0-wal:errclass:doc/10 F-2"
  "lock-same-process:double_open_errclass:doc/14 D-1 (env_posix 缺进程内持锁集合)"
  "lock-same-process:destroy_open_errclass:doc/14 D-1"
)
# 实现定义、只记录不比较的字段
RECORD_ONLY="size"

printf '%s\t%s\t%s\t%s\n' scenario field kvdb official verdict > "$EVID/matrix.tsv"
for scn in "${SCENARIOS[@]}"; do
  dbk="$EVID/db-kvdb-$scn"; dbo="$EVID/db-official-$scn"
  "$EVID/om_kvdb.exe" kvdb "$dbk" "$scn" > "$EVID/$scn.kvdb.tsv" 2>"$EVID/$scn.kvdb.err"; rck=$?
  "$EVID/om_official.exe" official "$dbo" "$scn" > "$EVID/$scn.official.tsv" 2>"$EVID/$scn.official.err"; rco=$?
  if [[ $rck -ne 0 || $rco -ne 0 ]]; then
    printf '%s\tINTERNAL\texit=%d\texit=%d\tFAIL\n' "$scn" "$rck" "$rco" >> "$EVID/matrix.tsv"
    fails=$((fails+1)); continue
  fi
  awk -F'\t' '{print $2"\t"$3}' "$EVID/$scn.kvdb.tsv"      | sort > "$EVID/$scn.k"
  awk -F'\t' '{print $2"\t"$3}' "$EVID/$scn.official.tsv" | sort > "$EVID/$scn.o"
  joined=$(join -t$'\t' -j1 "$EVID/$scn.k" "$EVID/$scn.o" 2>/dev/null)
  if [[ -z "$joined" && -s "$EVID/$scn.k" ]]; then
    printf '%s\tFIELDS\tnone\tnone\tFAIL\n' "$scn" >> "$EVID/matrix.tsv"; fails=$((fails+1)); continue
  fi
  while IFS=$'\t' read -r field vk vo; do
    if [[ " $RECORD_ONLY " == *" $field "* ]]; then
      printf '%s\t%s\t%s\t%s\tRECORD\n' "$scn" "$field" "$vk" "$vo" >> "$EVID/matrix.tsv"
      continue
    fi
    if [[ "$vk" == "$vo" ]]; then
      printf '%s\t%s\t%s\t%s\tSAME\n' "$scn" "$field" "$vk" "$vo" >> "$EVID/matrix.tsv"
    else
      verdict=FAIL
      for spec in "${EXPECTED_DIV[@]}"; do
        e_scn=${spec%%:*}; rest=${spec#*:}; e_field=${rest%%:*}
        if [[ "$scn" == "$e_scn" && "$field" == "$e_field" ]]; then
          verdict="EXPECTED(${spec#*:})"; caught=$((caught+1))
        fi
      done
      printf '%s\t%s\t%s\t%s\t%s\n' "$scn" "$field" "$vk" "$vo" "$verdict" >> "$EVID/matrix.tsv"
      [[ "$verdict" == FAIL ]] && fails=$((fails+1))
    fi
  done <<< "$joined"
done

printf 'N2  %s fields SAME, %s RECORD, %s expected-divergence(s) caught, %s FAIL\n' \
  "$(grep -c SAME "$EVID/matrix.tsv")" "$(grep -c RECORD "$EVID/matrix.tsv")" "$caught" "$fails"
grep -v 'SAME' "$EVID/matrix.tsv" | tail -n +2 | sed 's/^/    /'
exit $fails
