#!/usr/bin/env bash
# N6 随机崩溃点 + 跨引擎恢复跑分（doc/14 新体系）
# 每轮：随机时刻 kill -9 写入进程 → 两引擎各自重开（一份拷贝）→
#   前缀性质（已确认写全部在场且值正确）+ 两引擎恢复出的键集摘要必须全等。
# 另有 3 轮确定性 WAL 尾截断：两引擎必须同开或同拒，开则键集同。
set -u
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
EVID=${AUDIT_DIR:?AUDIT_DIR must be set}
CC=${CC:-gcc}
CXX=${CXX:-g++}
ROUNDS=${CRASH_ROUNDS:-10}
fails=0

TRIPLE=$("$CC" -dumpmachine)
case "$TRIPLE" in
  *linux*|*-cygwin|*-msys) ;;
  *) echo "N6: need POSIX compiler, got $TRIPLE"; exit 2 ;;
esac
KVDB_LIB=${KVDB_LIB:-$REPO/build/libleveldb.a}
OFFICIAL_LIB=$(ls "$REPO"/build/interop/official/$TRIPLE/lib/libleveldb_official.a 2>/dev/null | head -1)
[[ -f "$KVDB_LIB" && -f "$OFFICIAL_LIB" ]] || { echo "N6: missing archives"; exit 2; }

"$CC" -std=c11 -D_GNU_SOURCE -O2 -Wall -I"$REPO/leveldb/include" \
  -c "$REPO/tools/audit/crash.c" -o "$EVID/crash.o" || exit 2
"$CC" "$EVID/crash.o" "$KVDB_LIB" -o "$EVID/crash_kvdb.exe" || exit 2
"$CXX" "$EVID/crash.o" "$OFFICIAL_LIB" -pthread -o "$EVID/crash_official.exe" || exit 2

note() { printf 'N6  %s\n' "$*"; }
fail() { note "FAIL $*"; fails=$((fails+1)); }
DB="$EVID/crash-db"; COPY="$EVID/crash-db-copy"; ACK="$EVID/crash-ack.txt"

for round in $(seq 1 "$ROUNDS"); do
  rm -rf "$DB" "$COPY" "$ACK"
  writer="kvdb"; [[ $((round % 2)) -eq 0 ]] && writer="official"
  "$EVID/crash_${writer}.exe" write "$DB" "$ACK" 200000 > /dev/null 2>&1 &
  wpid=$!
  t=$(awk -v s=$RANDOM 'BEGIN{srand(s);printf "%.2f", 0.3 + rand()*1.7}')
  sleep "$t"
  kill -9 $wpid 2>/dev/null
  wait $wpid 2>/dev/null
  ack=$(tail -1 "$ACK" 2>/dev/null | grep -E '^[0-9]+$' || echo 0)
  cp -r "$DB" "$COPY" 2>/dev/null || { fail "round $round: copy failed"; continue; }
  r1=OK; r2=OK
  out1=$("$EVID/crash_kvdb.exe" verify "$DB" "$ack" 2>&1); rc1=$?
  out2=$("$EVID/crash_official.exe" verify "$COPY" "$ack" 2>&1); rc2=$?
  # 恢复键集必须两引擎全等（同一 WAL 字节 → 同一重放结果）
  s1=$("$EVID/crash_kvdb.exe" scan "$DB" 2>&1); rcs1=$?
  s2=$("$EVID/crash_official.exe" scan "$COPY" 2>&1); rcs2=$?
  if [[ $rc1 -ne 0 || $rcs1 -ne 0 ]]; then r1="kvdb rc=$rc1/$rcs1 $out1 $s1"; fi
  if [[ $rc2 -ne 0 || $rcs2 -ne 0 ]]; then r2="official rc=$rc2/$rcs2 $out2 $s2"; fi
  if [[ "$s1" != "$s2" ]]; then
    fail "round $round (writer=$writer kill@${t}s ack=$ack): recovered sets differ: kvdb[$s1] official[$s2]"
  elif [[ $rc1 -ne 0 || $rc2 -ne 0 || $rcs1 -ne 0 || $rcs2 -ne 0 ]]; then
    fail "round $round (writer=$writer kill@${t}s ack=$ack): $r1 | $r2"
  else
    note "PASS round $round: writer=$writer kill@${t}s ack=$ack, prefix ok, recovered sets identical ($s1)"
  fi
done

# ---------- 确定性 WAL 尾截断轮 ----------
for round in 1 2 3; do
  rm -rf "$DB" "$COPY" "$ACK"
  "$EVID/crash_kvdb.exe" write "$DB" "$ACK" 300 > /dev/null 2>&1
  logf=$(ls "$DB"/*.log 2>/dev/null | head -1)
  [[ -n "$logf" ]] || { fail "truncate round $round: no WAL"; continue; }
  sz=$(wc -c < "$logf")
  pct=$((60 - round * 10))
  head -c $((sz * pct / 100)) "$logf" > "$logf.tmp" && mv "$logf.tmp" "$logf"
  cp -r "$DB" "$COPY"
  s1=$("$EVID/crash_kvdb.exe" scan "$DB" 2>&1); rc1=$?
  s2=$("$EVID/crash_official.exe" scan "$COPY" 2>&1); rc2=$?
  if [[ $rc1 -eq 0 && $rc2 -eq 0 ]]; then
    if [[ "$s1" == "$s2" ]]; then
      note "PASS truncate-${pct}%: both engines opened, recovered sets identical ($s1)"
    else
      fail "truncate-${pct}%: both opened but recovered sets differ: [$s1] vs [$s2]"
    fi
  elif [[ $rc1 -ne 0 && $rc2 -ne 0 ]]; then
    note "PASS truncate-${pct}%: both engines rejected the damaged dir (consistent)"
  else
    fail "truncate-${pct}%: open verdict differs kvdb rc=$rc1 [$s1] official rc=$rc2 [$s2]"
  fi
done

exit $fails
