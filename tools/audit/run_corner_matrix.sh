#!/usr/bin/env bash
# N4 格式角落矩阵跑分（doc/14 新体系）
# 每个场景两引擎各写一份目录：
#   strict  — 除 LOG 系与 LOCK 外全部文件逐字节比对（确定性编码场景）
#   digest  — 4 个方向交叉读取（两引擎 × 两目录）摘要必须全等
# 依赖纪律：不使用 diff/cmp（msys 无 diffutils）。
set -u
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
EVID=${AUDIT_DIR:?AUDIT_DIR must be set}
CC=${CC:-gcc}
CXX=${CXX:-g++}
fails=0

TRIPLE=$("$CC" -dumpmachine)
case "$TRIPLE" in
  *linux*|*-cygwin|*-msys) ;;
  *) echo "N4: need POSIX compiler, got $TRIPLE"; exit 2 ;;
esac
KVDB_LIB=${KVDB_LIB:-$REPO/build/libleveldb.a}
OFFICIAL_LIB=$(ls "$REPO"/build/interop/official/$TRIPLE/lib/libleveldb_official.a 2>/dev/null | head -1)
[[ -f "$KVDB_LIB" && -f "$OFFICIAL_LIB" ]] || { echo "N4: missing archives"; exit 2; }

"$CC" -std=c11 -D_GNU_SOURCE -O2 -Wall -I"$REPO/leveldb/include" \
  -c "$REPO/tools/audit/corner_matrix.c" -o "$EVID/corner.o" || exit 2
"$CC" "$EVID/corner.o" "$KVDB_LIB" -o "$EVID/cm_kvdb.exe" || exit 2
"$CXX" "$EVID/corner.o" "$OFFICIAL_LIB" -pthread -o "$EVID/cm_official.exe" || exit 2

mapfile -t SCENARIOS < <("$EVID/cm_kvdb.exe" list)
# digest 判据的场景（后台 flush 时序使文件编号与布局不保证一致）
DIGEST_ONLY="multibuf"
is_digest() { [[ " $DIGEST_ONLY " == *" $1 "* ]]; }
sha_of() { sha256sum "$1" 2>/dev/null | awk '{print $1}'; }

for scn in "${SCENARIOS[@]}"; do
  dbk="$EVID/db-kvdb-$scn"; dbo="$EVID/db-official-$scn"
  rm -rf "$dbk" "$dbo"
  "$EVID/cm_kvdb.exe" write "$dbk" "$scn" > "$EVID/$scn.kvdb.write" 2>&1; rck=$?
  "$EVID/cm_official.exe" write "$dbo" "$scn" > "$EVID/$scn.official.write" 2>&1; rco=$?
  if [[ $rck -ne 0 || $rco -ne 0 ]]; then
    printf 'N4  FAIL %s: write rc kvdb=%d official=%d\n' "$scn" "$rck" "$rco"
    head -3 "$EVID/$scn.kvdb.write" "$EVID/$scn.official.write" | sed 's/^/    /'
    fails=$((fails+1)); continue
  fi
  # 4 方向交叉读取摘要（两个引擎读两个目录）
  s_kk=$("$EVID/cm_kvdb.exe" scan "$dbk" 2>&1);     r_kk=$?
  s_ok=$("$EVID/cm_official.exe" scan "$dbk" 2>&1); r_ok=$?
  s_ko=$("$EVID/cm_kvdb.exe" scan "$dbo" 2>&1);     r_ko=$?
  s_oo=$("$EVID/cm_official.exe" scan "$dbo" 2>&1); r_oo=$?
  cross_ok=1
  [[ $r_kk -ne 0 || $r_ok -ne 0 || $r_ko -ne 0 || $r_oo -ne 0 ]] && cross_ok=0
  [[ "$s_kk" != "$s_ok" || "$s_kk" != "$s_ko" || "$s_kk" != "$s_oo" ]] && cross_ok=0

  if is_digest "$scn"; then
    if [[ $cross_ok -eq 1 ]]; then
      printf 'N4  PASS %s (digest, 4-way cross-read equal: %s)\n' "$scn" "$s_kk"
    else
      printf 'N4  FAIL %s (cross-read: k/k=[%s] o/k=[%s] k/o=[%s] o/o=[%s])\n' "$scn" "$s_kk" "$s_ok" "$s_ko" "$s_oo"
      fails=$((fails+1))
    fi
  else
    # strict：文件集合 + 逐文件字节（排除 LOG 系与 LOCK）
    (cd "$dbk" && find . -maxdepth 1 -type f ! -name 'LOG*' ! -name 'LOCK' -printf '%f\n' | sort) > "$EVID/$scn.kfiles"
    (cd "$dbo" && find . -maxdepth 1 -type f ! -name 'LOG*' ! -name 'LOCK' -printf '%f\n' | sort) > "$EVID/$scn.ofiles"
    fsd=$(awk 'NR==FNR{a[$0]=1;next} !($0 in a){print "kvdb-only: "$0; if(++n>=5)exit}' \
        "$EVID/$scn.ofiles" "$EVID/$scn.kfiles"
      awk 'NR==FNR{a[$0]=1;next} !($0 in a){print "official-only: "$0; if(++n>=5)exit}' \
        "$EVID/$scn.kfiles" "$EVID/$scn.ofiles")
    if [[ -n "$fsd" ]]; then
      printf 'N4  FAIL %s: file-set differs\n' "$scn"
      printf '%s\n' "$fsd" | sed 's/^/    /'
      fails=$((fails+1)); continue
    fi
    bad=0; first=""
    while IFS= read -r f; do
      a=$(sha_of "$dbk/$f"); b=$(sha_of "$dbo/$f")
      if [[ "$a" != "$b" ]]; then bad=$((bad+1)); [[ -z "$first" ]] && first="$f"; fi
    done < "$EVID/$scn.kfiles"
    if [[ $bad -eq 0 && $cross_ok -eq 1 ]]; then
      printf 'N4  PASS %s (strict: %s files byte-identical, cross-read equal)\n' "$scn" "$(wc -l < "$EVID/$scn.kfiles")"
    else
      printf 'N4  FAIL %s: %s/%s files differ (first: %s), cross_ok=%s\n' "$scn" "$bad" "$(wc -l < "$EVID/$scn.kfiles")" "$first" "$cross_ok"
      fails=$((fails+1))
    fi
  fi
done
exit $fails
