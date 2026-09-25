#!/usr/bin/env bash
# verify_all.sh - independent verification suite (tools/verify). Deliberately
# ORTHOGONAL to the ported leveldb suite and to scripts/check_all.sh:
#   A model_fuzz        - model-based randomized differential (public C API,
#                         binary keys, own oracle)
#   B format_probe     - third-language (JS) byte-level parser; verifies
#                         every block CRC, index routing, bloom membership;
#                         compares kvdb-written vs official-written dirs
#   F iterator_snapshot- snapshot isolation vs an independent model while
#                         the writer crosses flushes/compactions
#   D crash_torture     - acknowledged (sync) writes + hard process death;
#                         recovered state must be exactly a prefix
#   C corrupt_fuzz      - structure-aware byte damage; the property is
#                         crash-freedom, not answer-equality
# Multi-process readers are intentionally absent: leveldb's LOCK is
# exclusive, so a second process cannot open the same DB by design.
#
# Usage: bash tools/verify/verify_all.sh [quick|full]
#   quick: fewer rounds (default); full: 3x rounds
# Evidence: build/verify/<timestamp>/; exit code = number of failed legs.
set -u
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
MODE=${1:-quick}
MULT=1; [ "$MODE" = full ] && MULT=3
TS=$(date +%Y%m%d-%H%M%S)
OUT="$REPO/build/verify/$TS"
mkdir -p "$OUT"
CC=${CC:-gcc}
CFLAGS="-std=c11 -O2 -Wall -Wextra -I$REPO/include"
LIB="$REPO/build/libleveldb.a"
FAIL=0
row() { printf '%-20s %-6s %s\n' "$1" "$2" "$3" | tee -a "$OUT/summary.txt"; case "$2" in FAIL) FAIL=$((FAIL+1));; esac; }

echo "== building tools (current toolchain: $($CC -dumpmachine 2>/dev/null || echo '?'))"
CUR_TRIPLE=$($CC -dumpmachine 2>/dev/null || echo '?')
# The archive must belong to THIS toolchain's namespace (doc/04 N-14/N-15),
# and a from-scratch run may not have one at all. The Makefile's .target guard
# would refuse a mixed tree, so clean it here - and do not swallow that
# refusal the way a grep for "error" would.
PREV=$(cat "$REPO/build/.target" 2>/dev/null || echo '')
if [ "$PREV" != "$CUR_TRIPLE" ]; then
  echo "   build/ belongs to '${PREV:-none}', current is $CUR_TRIPLE - rebuilding"
  make -C "$REPO" clean >/dev/null 2>&1
fi
make -C "$REPO" -s || { echo "engine build failed" >&2; exit 2; }
[ -f "$LIB" ] || { echo "missing $LIB after build" >&2; exit 2; }
LINK=""
case "$CUR_TRIPLE" in *msys|*cygwin|*linux*|*darwin*) LINK="-lpthread" ;; esac
TIMEOUT=${TIMEOUT:-900}
TMO=""
command -v timeout >/dev/null 2>&1 && TMO="timeout $TIMEOUT"   # a hung leg must fail, not block the suite forever
for t in model_fuzz iterator_snapshot crash_torture reopen_probe mkfixture_kvdb; do
  $CC $CFLAGS "$REPO/tools/verify/$t.c" "$LIB" -o "$REPO/build/verify/$t.exe" $LINK 2>"$OUT/build-$t.log" || {
    echo "failed to build $t (see $OUT/build-$t.log)" >&2; tail -5 "$OUT/build-$t.log" >&2; exit 2
  }
done
# node resolution: inside an MSYS2/Cygwin login shell the inherited Windows
# PATH may not carry node, and "node missing" must never be mistaken for a
# detection (a missing tool is infrastructure, not evidence).
NODE=$(command -v node 2>/dev/null || true)
if [ -z "$NODE" ]; then
  for c in "/c/Program Files/nodejs/node" "/c/Program Files/nodejs/node.exe" "$HOME/AppData/Local/Programs/nodejs/node"; do
    [ -x "$c" ] && { NODE="$c"; break; }
  done
fi
[ -n "$NODE" ] || { echo "node required for format_probe/corrupt_fuzz (install nodejs)" >&2; exit 2; }

echo "== A: model fuzz ($MULT x 5 seeds x 10k steps)"
a_ok=1
for s in 1 2 3 4 5; do
  for m in $(seq 1 $MULT); do
    $TMO "$REPO/build/verify/model_fuzz.exe" $((s*100+m)) 10000 "$REPO/build/verify/mf-db" >>"$OUT/model_fuzz.log" 2>&1 || a_ok=0
  done
done
row model_fuzz "$([ $a_ok = 1 ] && echo PASS || echo FAIL)" "log: model_fuzz.log"

echo "== B: independent format probe (JS) + engine compare"
b_ok=1
# fixtures: fresh copies every run (a stale fixture would be overwritten with
# NEW sequence numbers -> different internal keys -> a bogus digest diff)
rm -rf "$REPO/build/verify/fx-kvdb"
"$REPO/build/verify/mkfixture_kvdb.exe" "$REPO/build/verify/fx-kvdb" >/dev/null 2>&1
"$NODE" "$REPO/tools/verify/format_probe.js" "$REPO/build/verify/fx-kvdb" "$OUT/bloom-oracle.bin" >"$OUT/format_probe_kvdb.json" 2>&1 || b_ok=0
OFF="$REPO/build/interop/official"
OFFDIR=$(find "$OFF" -name 'libleveldb_official.a' 2>/dev/null | head -1)
CUR_TRIPLE=$($CC -dumpmachine 2>/dev/null || echo '?')
if [ -n "$OFFDIR" ]; then
  OFF_TRIPLE=$(basename "$(dirname "$(dirname "$OFFDIR")")")
  if [ "$OFF_TRIPLE" != "$CUR_TRIPLE" ]; then
    # An archive built in one POSIX namespace cannot be linked in another
    # (doc/04 N-14/N-15). Record a SKIP, not a FAIL - and say how to get it.
    echo "SKIP official-compare: archive is for $OFF_TRIPLE, current toolchain is $CUR_TRIPLE; run verify_all.sh from the matching shell (e.g. MSYS2: /d/msys64/usr/bin/bash.exe -lc 'bash tools/verify/verify_all.sh')" | tee -a "$OUT/summary.txt"
    b_ok=1
  elif command -v g++ >/dev/null; then
    g++ -std=c++11 -O1 -I"$REPO/leveldb/include" -I"$REPO/leveldb" \
      "$REPO/tools/verify/mkfixture_official.cc" "$OFFDIR" -o "$REPO/build/verify/mkfixture_official.exe" -lpthread 2>"$OUT/build-official.log" \
      && rm -rf "$REPO/build/verify/fx-official" \
      && "$REPO/build/verify/mkfixture_official.exe" "$REPO/build/verify/fx-official" >/dev/null 2>&1 \
      && "$NODE" "$REPO/tools/verify/format_probe.js" "$REPO/build/verify/fx-official" >"$OUT/format_probe_official.json" 2>&1 \
      && "$NODE" "$REPO/tools/verify/format_probe.js" --compare "$REPO/build/verify/fx-kvdb" "$REPO/build/verify/fx-official" >"$OUT/format_compare.txt" 2>&1 || b_ok=0
  else
    echo "SKIP official-compare: g++ not available for the fixture" | tee -a "$OUT/summary.txt"
    b_ok=1
  fi
else
  echo "SKIP official-compare: official archive absent (scripts/build_official.sh; see doc/12)" | tee -a "$OUT/summary.txt"
fi
row format_probe "$([ $b_ok = 1 ] && echo PASS || echo FAIL)" "log: format_probe_*.json, format_compare.txt"

# B2: bloom membership oracle - the OFFICIAL FilterPolicy asks whether every
# user key of the kvdb table is a maybe-match in the filter bytes kvdb wrote.
# (The JS probe checks filter STRUCTURE only: its own membership
# re-implementation cried wolf four times on the healthy engine, doc/13 §4.)
if [ -n "${OFFDIR:-}" ] && [ "${OFF_TRIPLE:-}" = "$CUR_TRIPLE" ] && command -v g++ >/dev/null; then
  g++ -std=c++11 -O1 -I"$REPO/leveldb/include" -I"$REPO/leveldb" \
    "$REPO/tools/verify/official_bloom_check.cc" "$OFFDIR" \
    -o "$OUT/official_bloom_check.exe" -lpthread 2>"$OUT/build-bloom-oracle.log" \
    && "$OUT/official_bloom_check.exe" "$OUT/bloom-oracle.bin" >"$OUT/bloom_oracle.txt" 2>&1
  o_rc=$?
  row bloom_oracle "$([ $o_rc = 0 ] && echo PASS || echo FAIL)" "official FilterPolicy on kvdb filter bytes; log: bloom_oracle.txt"
else
  echo "SKIP bloom_oracle: official archive absent or from another namespace" | tee -a "$OUT/summary.txt"
fi

echo "== F: iterator snapshot isolation"
f_ok=1
for s in 1 2 3; do "$REPO/build/verify/iterator_snapshot.exe" $s 2000 "$REPO/build/verify/is-db" >>"$OUT/iterator_snapshot.log" 2>&1 || f_ok=0; done
row iterator_snapshot "$([ $f_ok = 1 ] && echo PASS || echo FAIL)" "log: iterator_snapshot.log"

echo "== D: crash torture ($((12*MULT)) rounds)"
d_ok=1; d_warn=0
for i in $(seq 1 $((12*MULT))); do
  rm -rf "$REPO/build/verify/ct-db" "$REPO/build/verify/ct-side"
  $TMO "$REPO/build/verify/crash_torture.exe" write "$REPO/build/verify/ct-db" "$REPO/build/verify/ct-side" 600 >/dev/null 2>&1
  $TMO "$REPO/build/verify/crash_torture.exe" verify "$REPO/build/verify/ct-db" "$REPO/build/verify/ct-side" >>"$OUT/crash_torture.log" 2>&1
  rc=$?
  [ $rc -eq 0 ] || { [ $rc -eq 4 ] && d_warn=$((d_warn+1)) || d_ok=0; }
done
row crash_torture "$([ $d_ok = 1 ] && echo PASS || echo FAIL)" "prefix ok; open-refused warnings=$d_warn; log: crash_torture.log"

echo "== C: corruption fuzz ($((150*MULT)) iterations)"
"$NODE" "$REPO/tools/verify/corrupt_fuzz.js" $((150*MULT)) >"$OUT/corrupt_fuzz.txt" 2>&1
c_rc=$?
row corrupt_fuzz "$([ $c_rc = 0 ] && echo PASS || echo FAIL)" "log: corrupt_fuzz.txt"

echo "== SELF-TEST: negative control (drop one acknowledged key -> must FAIL)"
rm -rf "$REPO/build/verify/ct-db" "$REPO/build/verify/ct-side"
"$REPO/build/verify/crash_torture.exe" write "$REPO/build/verify/ct-db" "$REPO/build/verify/ct-side" 200 >/dev/null 2>&1
"$REPO/build/verify/crash_torture.exe" verify-drop "$REPO/build/verify/ct-db" "$REPO/build/verify/ct-side" 3 >"$OUT/self_test_drop.txt" 2>&1
drop_rc=$?
if [ $drop_rc -eq 3 ]; then row self_test:neg-control PASS "detector proved non-vacuous (rc=3 as required)"; else row self_test:neg-control FAIL "detector did NOT notice a dropped acknowledged key (rc=$drop_rc)"; fi

echo "== COMPARATIVE: same harnesses linked against the OFFICIAL engine"
cmp_ok=1
if [ -n "${OFFDIR:-}" ] && [ "${OFF_TRIPLE:-}" = "$CUR_TRIPLE" ]; then
  for t in model_fuzz iterator_snapshot crash_torture; do
    gcc -std=c11 -O2 -I"$REPO/include" -c "$REPO/tools/verify/$t.c" -o "$OUT/$t.o" 2>/dev/null \
      && g++ "$OUT/$t.o" "$OFFDIR" -o "$OUT/$t-official.exe" -lpthread 2>>"$OUT/build-official-tools.log" \
      || cmp_ok=0
  done
  for s in 1 2 3; do
    $TMO "$OUT/model_fuzz-official.exe" $s 10000 "$OUT/cmp-db-official" >>"$OUT/comparative.txt" 2>&1 || cmp_ok=0
    $TMO "$REPO/build/verify/model_fuzz.exe"          $s 10000 "$OUT/cmp-db-kvdb"     >>"$OUT/comparative.txt" 2>&1 || cmp_ok=0
  done
  for s in 1 2; do
    $TMO "$OUT/iterator_snapshot-official.exe" $s 1500 "$OUT/cmp-is-official" >>"$OUT/comparative.txt" 2>&1 || cmp_ok=0
    $TMO "$REPO/build/verify/iterator_snapshot.exe"          $s 1500 "$OUT/cmp-is-kvdb"     >>"$OUT/comparative.txt" 2>&1 || cmp_ok=0
  done
  rm -rf "$OUT/cmp-ct-official" "$OUT/cmp-ct-kvdb"
  $TMO "$OUT/crash_torture-official.exe" write "$OUT/cmp-ct-official" "$OUT/cmp-ct-official.side" 300 >/dev/null 2>&1; [ -s "$OUT/cmp-ct-official.side" ] || cmp_ok=0   # the writer self-terminates by design; require a non-empty side log
  $TMO "$OUT/crash_torture-official.exe" verify "$OUT/cmp-ct-official" "$OUT/cmp-ct-official.side" >>"$OUT/comparative.txt" 2>&1 || cmp_ok=0
  $TMO "$REPO/build/verify/crash_torture.exe"          write "$OUT/cmp-ct-kvdb" "$OUT/cmp-ct-kvdb.side" 300 >/dev/null 2>&1
  $TMO "$REPO/build/verify/crash_torture.exe"          verify "$OUT/cmp-ct-kvdb" "$OUT/cmp-ct-kvdb.side" >>"$OUT/comparative.txt" 2>&1 || cmp_ok=0
  row comparative "$([ $cmp_ok = 1 ] && echo PASS || echo FAIL)" "same seeds through both engines; log: comparative.txt"
else
  echo "SKIP comparative: official archive absent or from another namespace (run from the matching shell)" | tee -a "$OUT/summary.txt"
fi

echo ""
echo "=============================================================="
echo " independent verification: FAILED legs = $FAIL    evidence: $OUT"
echo "=============================================================="
exit $FAIL
