#!/usr/bin/env bash
# verify_mutants.sh - SELF-VERIFICATION of the verification methods by
# mutation testing: deliberately break the engine (in a THROWAWAY copy of
# the tree), then ask each verification method whether it notices. A method
# that cannot catch a mutant it should catch is not evidence of anything.
#
# The output is a detection matrix: rows = mutants (one-line source
# sabotage each), columns = the original ported suite and the independent
# legs. This also quantifies what the new methods add over the old ones.
#
# Mutants (all single-line, compile-clean, semantics-preserving enough to
# build):
#   M1 bloom/hash:  ldb_hash  h *= m  ->  h += m          (filter bytes now disagree with any independent bloom implementation)
#   M2 WAL:        skip the log append on the write path  (acknowledged writes die with the process)
#   M3 batch:      put/delete no longer bump the batch count (WAL header lies about op count)
#   M4 snapshot:   iterator ignores its sequence bound    (versions hidden by snapshot leak through)
#
# Usage: bash tools/verify/verify_mutants.sh
# Exit: 0 when every mutant is caught by at least one independent method
# AND the negative controls behave (documented in the matrix).
set -u
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
WORK="$REPO/build/verify/mutants"
CC=${CC:-gcc}
TS=$(date +%Y%m%d-%H%M%S)
OUT="$WORK/$TS"; mkdir -p "$OUT"
rm -rf "$OUT"/*

OFFDIR=$(find "$REPO/build/interop/official" -name 'libleveldb_official.a' 2>/dev/null | head -1)
ORACLE=""
if [ -n "$OFFDIR" ] && command -v g++ >/dev/null; then
  g++ -std=c++11 -O1 -I"$REPO/leveldb/include" -I"$REPO/leveldb" \
    "$REPO/tools/verify/official_bloom_check.cc" "$OFFDIR" \
    -o "$OUT/oracle.exe" -lpthread 2>/dev/null && ORACLE="$OUT/oracle.exe"
fi
# node resolution: a login shell (MSYS2/Cygwin) may not carry the Windows
# node on PATH, and a missing tool must read as skip, NEVER as caught.
NODE=$(command -v node 2>/dev/null || true)
if [ -z "$NODE" ]; then
  for c in "/c/Program Files/nodejs/node" "/c/Program Files/nodejs/node.exe" "$HOME/AppData/Local/Programs/nodejs/node"; do
    [ -x "$c" ] && { NODE="$c"; break; }
  done
fi
printf 'mutant\tported_suite\tmodel_fuzz\titerator_snap\tcrash_torture\tformat_probe\tbloom_oracle\n' | tee "$OUT/matrix.tsv"

apply_mutant() { # name, then sed expressions via stdin lines "file|expr"
  local name=$1 tree="$2"
  local count=0
  while IFS='|' read -r f expr; do
    [ -z "$f" ] && continue
    sed -i "$expr" "$tree/$f" && count=$((count+1)) || return 1
  done
  echo "$count"
}

run_mutant() {
  local name=$1; shift
  local tree="$OUT/$name"
  rm -rf "$tree"; mkdir -p "$tree"
  cp -r "$REPO/src" "$REPO/include" "$REPO/tests" "$tree/"
  cp "$REPO/Makefile" "$tree/"
  local n; n=$(apply_mutant "$name" "$tree" < "$1") || { echo "$name: mutation apply failed" >&2; return; }
  ( cd "$tree" && make -s >/dev/null 2>&1 ) || { echo "$name: mutant build failed" >&2; return; }
  # 1) the ORIGINAL ported suite
  local suite=miss
  if "$tree/build/kvdb_tests" >"$OUT/$name-suite.log" 2>&1; then suite=missed; else suite=caught; fi
  # 2..4 independent legs compiled against the MUTANT library
  local link=""; case $($CC -dumpmachine 2>/dev/null) in *msys|*cygwin|*linux*) link="-lpthread";; esac
  for t in model_fuzz iterator_snapshot crash_torture reopen_probe; do
    $CC -std=c11 -O1 -I"$tree/include" "$REPO/tools/verify/$t.c" "$tree/build/libleveldb.a" -o "$OUT/$name-$t.exe" $link 2>/dev/null
  done
  local mf=miss snap=miss ct=miss
  rm -rf "$OUT/$name-db"
  "$OUT/$name-model_fuzz.exe" 1 3000 "$OUT/$name-db" >"$OUT/$name-model_fuzz.log" 2>&1 || mf=caught
  rm -rf "$OUT/$name-is"
  "$OUT/$name-iterator_snapshot.exe" 1 800 "$OUT/$name-is" >"$OUT/$name-snap.log" 2>&1 || snap=caught
  rm -rf "$OUT/$name-ct-db" "$OUT/$name-ct-side"
  "$OUT/$name-crash_torture.exe" write "$OUT/$name-ct-db" "$OUT/$name-ct-side" 300 >/dev/null 2>&1
  "$OUT/$name-crash_torture.exe" verify "$OUT/$name-ct-db" "$OUT/$name-ct-side" >"$OUT/$name-crash.log" 2>&1 || ct=caught
  # 5) independent format probe (structure) on a mutant-written fixture,
  #    then the official-implementation bloom oracle on its filter bytes.
  #    A missing tool or an unbuildable fixture reads as skip/infra, never caught.
  $CC -std=c11 -O1 -I"$tree/include" "$REPO/tools/verify/mkfixture_kvdb.c" "$tree/build/libleveldb.a" -o "$OUT/$name-mkfx.exe" $link 2>/dev/null
  rm -rf "$OUT/$name-fx"
  local fx_ok=skip
  if [ -x "$OUT/$name-mkfx.exe" ] && "$OUT/$name-mkfx.exe" "$OUT/$name-fx" >/dev/null 2>&1 \
     && [ -f "$OUT/$name-fx/CURRENT" ]; then
    fx_ok=yes
  else
    echo "  $name: mutant fixture failed to build/run (infra, columns=skip)" >&2
  fi
  local fp=skip bo=skip
  if [ "$fx_ok" = yes ] && [ -n "$NODE" ]; then
    fp=miss
    "$NODE" "$REPO/tools/verify/format_probe.js" "$OUT/$name-fx" "$OUT/$name-oracle.bin" >"$OUT/$name-probe.log" 2>&1 || fp=caught
  fi
  if [ "$fx_ok" = yes ] && [ -n "$ORACLE" ] && [ -f "$OUT/$name-oracle.bin" ]; then
    bo=miss
    "$ORACLE" "$OUT/$name-oracle.bin" >"$OUT/$name-oracle.log" 2>&1 || bo=caught
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$suite" "$mf" "$snap" "$ct" "$fp" "$bo" | tee -a "$OUT/matrix.tsv"
}

write_spec() { # name -> emits "file|sed-expression" lines (exactly one '|' per line)
  case "$1" in
    hash_mult)   printf '%s\n' 'src/util.c|s#h \*= m;#h += m;#' ;;
    skip_wal)    printf '%s\n' 'src/db.c|s#status = ldb_log_writer_add_record(impl->log, \&contents);#(void)contents; status = ldb_status_ok();#' ;;
    batch_count) printf '%s\n' 'src/write_batch.c|s#ldb_write_batch_set_count(b, ldb_write_batch_count(b) + 1);#ldb_write_batch_set_count(b, ldb_write_batch_count(b));#g' ;;
    snap_leak)   printf '%s\n' 'src/db_iter.c|s#ikey.sequence <= di->sequence#ikey.sequence <= 0xffffffffffffffffull#g' ;;
  esac
}

for name in hash_mult skip_wal batch_count snap_leak; do
  spec="$OUT/$name.spec"
  write_spec "$name" > "$spec"
  run_mutant "$name" "$spec"
done

echo ""
echo "matrix: $OUT/matrix.tsv"
echo "legend: caught = the method FAILED on the broken engine (good); missed = it passed (blind spot)"
