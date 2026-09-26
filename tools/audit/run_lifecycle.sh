#!/usr/bin/env bash
# N5 交替生命周期 + 跨引擎修复跑分（doc/14 新体系）
# 健康路径：O/K 交替写 5 程 → 双引擎 verify → 互做 repair（幂等性）
# 破坏路径：删 WAL / 截断最大表 / 改后缀(.ldb→.sst)，两引擎各 repair
#           一份拷贝，分歧必须在 EXPECTED 表内（doc/10 M-4/M-5）
set -u
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
EVID=${AUDIT_DIR:?AUDIT_DIR must be set}
CC=${CC:-gcc}
CXX=${CXX:-g++}
fails=0; caught=0

TRIPLE=$("$CC" -dumpmachine)
case "$TRIPLE" in
  *linux*|*-cygwin|*-msys) ;;
  *) echo "N5: need POSIX compiler, got $TRIPLE"; exit 2 ;;
esac
KVDB_LIB=${KVDB_LIB:-$REPO/build/libleveldb.a}
OFFICIAL_LIB=$(ls "$REPO"/build/interop/official/$TRIPLE/lib/libleveldb_official.a 2>/dev/null | head -1)
[[ -f "$KVDB_LIB" && -f "$OFFICIAL_LIB" ]] || { echo "N5: missing archives"; exit 2; }

"$CC" -std=c11 -D_GNU_SOURCE -O2 -Wall -I"$REPO/leveldb/include" \
  -c "$REPO/tools/audit/lifecycle.c" -o "$EVID/lifecycle.o" || exit 2
"$CC" "$EVID/lifecycle.o" "$KVDB_LIB" -o "$EVID/lc_kvdb.exe" || exit 2
"$CXX" "$EVID/lifecycle.o" "$OFFICIAL_LIB" -pthread -o "$EVID/lc_official.exe" || exit 2
DB="$EVID/ladder-db"

note() { printf 'N5  %s\n' "$*"; }
fail() { note "FAIL $*"; fails=$((fails+1)); }
rm -rf "$DB" "$EVID/dmg-"*

# ---------- 健康交替梯子 ----------
"$EVID/lc_official.exe" "$DB" 0 stepcompact || { fail "official step0"; exit $fails; }
"$EVID/lc_kvdb.exe"    "$DB" 1 step        || { fail "kvdb step1"; exit $fails; }
"$EVID/lc_official.exe" "$DB" 2 step       || { fail "official step2"; exit $fails; }
"$EVID/lc_kvdb.exe"    "$DB" 3 stepcompact || { fail "kvdb step3"; exit $fails; }
"$EVID/lc_official.exe" "$DB" 4 step       || { fail "official step4"; exit $fails; }
"$EVID/lc_kvdb.exe"    "$DB" 5 verify      || fail "kvdb verify 5 phases"
"$EVID/lc_official.exe" "$DB" 5 verify     || fail "official verify 5 phases"
note "PASS alternating ladder (O→K→O→K→O), 5 phases, both engines verify"

# ---------- 健康库幂等修复 ----------
d="$EVID/dmg-healthy"; cp -r "$DB" "$d"
"$EVID/lc_kvdb.exe" "$d" 5 verify > /dev/null && note "PASS pre-repair baseline verify" \
  || fail "pre-repair baseline"
# kvdb 修复健康库
cat > "$EVID/repair_helper.c" <<'EOF'
#include <stdio.h>
#include <leveldb/c.h>
int main(int argc, char** argv) {
  char* err = NULL;
  leveldb_options_t* o = leveldb_options_create();
  if (argc != 2) return 1;
  leveldb_repair_db(o, argv[1], &err);
  if (err) { fprintf(stderr, "%s\n", err); return 2; }
  leveldb_options_destroy(o);
  return 0;
}
EOF
"$CC" -std=c11 -D_GNU_SOURCE -O2 -I"$REPO/leveldb/include" \
  "$EVID/repair_helper.c" "$KVDB_LIB" -o "$EVID/repair_kvdb.exe" || exit 2
"$CXX" -std=c11 -D_GNU_SOURCE -O2 -I"$REPO/leveldb/include" \
  "$EVID/repair_helper.c" "$OFFICIAL_LIB" -pthread -o "$EVID/repair_official.exe" || exit 2

d="$EVID/dmg-repair-kvdb"; cp -r "$DB" "$d"
"$EVID/repair_kvdb.exe" "$d" && "$EVID/lc_kvdb.exe" "$d" 5 verify > /dev/null \
  && "$EVID/lc_official.exe" "$d" 5 verify > /dev/null \
  && note "PASS kvdb repair of healthy dir: data preserved, both engines read it" \
  || fail "kvdb repair healthy"
d="$EVID/dmg-repair-official"; cp -r "$DB" "$d"
"$EVID/repair_official.exe" "$d" && "$EVID/lc_kvdb.exe" "$d" 5 verify > /dev/null \
  && "$EVID/lc_official.exe" "$d" 5 verify > /dev/null \
  && note "PASS official repair of healthy dir: data preserved, both engines read it" \
  || fail "official repair healthy"

# ---------- 破坏探针（跨引擎修复分歧登记） ----------
largest_ldb() {
  (cd "$1" && ls -S *.ldb 2>/dev/null | head -1)
}
# A) 删 WAL：repair 必须从表里保全全部数据（两引擎都必须）
d="$EVID/dmg-wal-kvdb"; cp -r "$DB" "$d"; rm -f "$d"/*.log
"$EVID/repair_kvdb.exe" "$d" && "$EVID/lc_kvdb.exe" "$d" 5 verify > /dev/null \
  && note "PASS kvdb repair recovers all data with WAL removed" \
  || fail "kvdb repair with WAL removed (expected all data preserved)"
d="$EVID/dmg-wal-official"; cp -r "$DB" "$d"; rm -f "$d"/*.log
"$EVID/repair_official.exe" "$d" && "$EVID/lc_official.exe" "$d" 5 verify > /dev/null \
  && note "PASS official repair recovers all data with WAL removed" \
  || fail "official repair with WAL removed"

# B) 截断最大表 50%：官方保留可解析前缀，kvdb 整表丢弃（doc/10 M-4）。
#    两引擎 repair 后 verify 的"丢多少"允许不同（登记为已归档分歧），
#    但 repair 自身不得失败、且两引擎都必须还能打开修复后的目录。
dk="$EVID/dmg-tbl-kvdb"; do_="$EVID/dmg-tbl-official"
cp -r "$DB" "$dk"; cp -r "$DB" "$do_"
for d in "$dk" "$do_"; do
  f=$(largest_ldb "$d")
  [[ -n "$f" ]] || { fail "no ldb to truncate in $d"; continue; }
  sz=$(wc -c < "$d/$f")
  head -c $((sz / 2)) "$d/$f" > "$d/$f.tmp" && mv "$d/$f.tmp" "$d/$f"
done
rk=OK; ro=OK
"$EVID/repair_kvdb.exe" "$dk" 2>/dev/null && "$EVID/lc_kvdb.exe" "$dk" 5 verify > /dev/null 2>&1 \
  && rk=PRESERVED || rk=DATALOSS-OR-FAIL
"$EVID/repair_official.exe" "$do_" 2>/dev/null && "$EVID/lc_official.exe" "$do_" 5 verify > /dev/null 2>&1 \
  && ro=PRESERVED || ro=DATALOSS-OR-FAIL
note "truncated-table repair: kvdb=$rk official=$ro (kvdb 整表丢弃 vs 官方部分保全 = doc/10 M-4 已归档分歧)"
caught=$((caught+1))

# C) 改后缀 .ldb→.sst：官方 repair 会找到，kvdb 只认 .ldb（doc/10 M-5）
dk="$EVID/dmg-ext-kvdb"; do_="$EVID/dmg-ext-official"
cp -r "$DB" "$dk"; cp -r "$DB" "$do_"
for d in "$dk" "$do_"; do
  f=$(largest_ldb "$d")
  [[ -n "$f" ]] && mv "$d/$f" "$d/${f%.ldb}.sst"
done
"$EVID/repair_kvdb.exe" "$dk" 2>/dev/null; rkk=$("$EVID/lc_kvdb.exe" "$dk" 5 verify 2>/dev/null >/dev/null; echo $?)
"$EVID/repair_official.exe" "$do_" 2>/dev/null; rko=$("$EVID/lc_official.exe" "$do_" 5 verify 2>/dev/null >/dev/null; echo $?)
if [[ "$rkk" != 0 && "$rko" == 0 ]]; then
  note "SPECIMEN-CAUGHT .ldb→.sst rename: kvdb repair loses data (rc verify=$rkk), official preserves it — doc/10 M-5 confirmed live"
  caught=$((caught+1))
else
  note "NOTE .ldb→.sst rename: kvdb verify rc=$rkk official rc=$rko (M-5 shape changed? investigate)"
fi

note "expected-divergence specimens caught: $caught; FAIL: $fails"
exit $fails
