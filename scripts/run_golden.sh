#!/usr/bin/env bash
# Byte-level (binary format) compatibility harness between the official
# LevelDB C++ build and kvdb.
#
# Run from Git Bash with:
#   /d/ProgramFiles/msys64/usr/bin/bash.exe -lc 'bash /d/code/kvdb/scripts/run_golden.sh'
# or on native Linux with:
#   bash scripts/run_golden.sh
#
# For every deterministic workload mode it:
#   1. creates a DB with the official engine and one with kvdb, from the SAME
#      compiled driver source linked against each library;
#   2. compares the two directories file by file, byte by byte (SSTable, WAL,
#      MANIFEST, CURRENT), which is the strictest writer-side format test;
#   3. has BOTH engines read BOTH directories (point gets plus forward and
#      reverse scans) and requires identical content digests, which is the
#      reader-side cross-format test.
#
# Env: MODES="..." override the mode list, KEEP=1 to keep the artifacts,
#      GOLDEN_TIMEOUT=seconds, KVDB_LIB=path to the kvdb archive.
set -uo pipefail
export PATH=/usr/bin:/bin
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
CC=/usr/bin/gcc
TIMEOUT=${GOLDEN_TIMEOUT:-120}
KVDB_LIB=${KVDB_LIB:-$REPO/build/libleveldb.a}
MODES=${MODES:-"sst sst-bloom sst-restart1 sst-bigblock wal wal-big wal-frag edge tomb levels"}
# Modes whose file numbering depends on background-compaction timing.
EXPLORATORY=${EXPLORATORY:-"levels"}
# The info log (LOG, LOG.old) carries engine text and timestamps; the lock file
# is engine-local. Everything else in the directory is format state.
EXCLUDE='^(LOG|LOG\.old|LOCK)$'

RUN=${RUN_DIR:-$REPO/build/interop/golden-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$RUN"
exec > >(tee "$RUN/run.log") 2>&1
printf 'Artifacts: %s\n' "$RUN"
mkdir -p "$RUN/bin" "$RUN/db" "$RUN/logs"

[[ -f "$KVDB_LIB" ]] || { printf 'Missing %s; run make first\n' "$KVDB_LIB"; exit 2; }
# A stale archive silently tests last week's engine instead of the working
# tree - the first Linux golden run looked green while linked against a
# two-week-old build/libleveldb.a that segfaulted on modes the current
# sources handle fine. `| head -1` rather than find's -quit: BSD/macOS find
# has no -quit, and an unsupported primary would turn this guard into a
# silent no-op on exactly the platform where it still has to be proven out.
if [[ -n "$(find "$REPO/src" "$REPO/include" -name '*.[ch]' -newer "$KVDB_LIB" -print 2>/dev/null | head -1)" ]]; then
  printf '%s is older than the sources in src/; run make first\n' "$KVDB_LIB" >&2
  exit 2
fi
# Every byte verdict below is a digest comparison (MSYS2 has sha256sum but
# neither cmp nor diff), so an absent digest tool would leave both sides empty
# and make "" == "" report SAME - a green run that compared nothing. Pick one
# now and prove it emits 64 hex chars: BSD/macOS has no sha256sum but does ship
# shasum, and failing here loudly is the difference between "unsupported host"
# and "format compatibility confirmed". A function rather than a command array:
# bash 3.2 (the macOS default) treats an empty array as an unbound variable.
if command -v sha256sum >/dev/null 2>&1; then
  digest() { sha256sum "$@"; }
elif command -v shasum >/dev/null 2>&1; then
  digest() { shasum -a 256 "$@"; }
else
  printf '%s needs sha256sum (or shasum -a 256) to compare bytes\n' "$0" >&2
  exit 2
fi
if ! [[ $(digest <<< probe | cut -c1-64) =~ ^[0-9a-f]{64}$ ]]; then
  printf '%s: the digest command produced no 64-hex digest; refusing to compare\n' "$0" >&2
  exit 2
fi
OFFICIAL_LIB=$(bash "$REPO/scripts/build_official.sh" | tail -1) || {
  printf 'official reference build failed\n'; exit 2; }
printf 'kvdb library:     %s (%s)\n' "$KVDB_LIB" "$(digest "$KVDB_LIB" | cut -c1-16)"
printf 'official library: %s (%s)\n' "$OFFICIAL_LIB" "$(digest "$OFFICIAL_LIB" | cut -c1-16)"

"$CC" -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror \
  -I"$REPO/leveldb/include" -c "$REPO/tests/interop/golden_driver.c" \
  -o "$RUN/bin/golden_driver.o" || { printf 'driver compile failed\n'; exit 2; }
/usr/bin/g++ "$RUN/bin/golden_driver.o" "$OFFICIAL_LIB" -pthread \
  -o "$RUN/bin/official.exe" || { printf 'official link failed\n'; exit 2; }
"$CC" "$RUN/bin/golden_driver.o" "$KVDB_LIB" -pthread \
  -o "$RUN/bin/kvdb.exe" || { printf 'kvdb link failed\n'; exit 2; }

result=0
printf 'mode\tcheck\tstatus\tdetail\n' > "$RUN/results.tsv"
note() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >> "$RUN/results.tsv"; }

# compare <label> <dirA> <dirB>: identical file set and identical bytes.
# MSYS2 here ships sha256sum/od/xxd but neither cmp nor diff, so equality goes
# through the digest resolved above and the first-difference search through a
# small python pass. Sizes use `wc -c` rather than `stat -c %s` because the
# latter is GNU-only (BSD spells it `stat -f %z`) and only ever feeds a message.
compare_bytes() {
  local label=$1 a=$2 b=$3 status=ok
  local list_a list_b
  list_a=$(cd "$a" && find . -maxdepth 1 -type f ! -name 'LOG*' ! -name 'LOCK' | sort)
  list_b=$(cd "$b" && find . -maxdepth 1 -type f ! -name 'LOG*' ! -name 'LOCK' | sort)
  if [[ "$list_a" != "$list_b" ]]; then
    note "$label" 'bytes' 'DIFF' 'file set differs'
    printf '  file set differs\n    official:%s\n    kvdb    :%s\n' \
      "$(tr '\n' ' ' <<< "$list_a")" "$(tr '\n' ' ' <<< "$list_b")"
    return 1
  fi
  local f fa fb ha hb
  while IFS= read -r f; do
    [[ -z "$f" ]] && continue
    fa="$a/$f"; fb="$b/$f"
    if [[ ! -f "$fa" || ! -f "$fb" ]]; then
      status=DIFF
      note "$label" 'bytes' 'ERROR' "$f missing"
      printf '  %s: missing on one side\n' "$f"
      continue
    fi
    ha=$(digest < "$fa"); hb=$(digest < "$fb")
    if [[ "$ha" != "$hb" ]]; then
      status=DIFF
      note "$label" 'bytes' 'DIFF' "$f ($(wc -c < "$fa") vs $(wc -c < "$fb") bytes)"
      printf '  %s differs: official=%s bytes, kvdb=%s bytes\n' "$f" \
        "$(wc -c < "$fa")" "$(wc -c < "$fb")"
      /usr/bin/python3 - "$fa" "$fb" <<'PY' 2>/dev/null | sed 's/^/    /'
import sys
a = open(sys.argv[1], 'rb').read()
b = open(sys.argv[2], 'rb').read()
n = min(len(a), len(b))
i = next((k for k in range(n) if a[k] != b[k]), n)
print("first differing byte at offset %d: official=%s kvdb=%s" %
      (i, a[i:i+8].hex() if i < len(a) else "EOF", b[i:i+8].hex() if i < len(b) else "EOF"))
lo = max(0, i - 16); hi = min(max(len(a), len(b)), i + 48)
for name, data in (("official", a), ("kvdb", b)):
    window = data[lo:hi]
    print("%-8s %06d  %s%s" % (name, lo, window.hex(' '), "  <" + "".join(
        chr(c) if 32 <= c < 127 else "." for c in window) + ">" if window else "  <empty>"))
PY
    else
      note "$label" 'bytes' 'SAME' "$f ($(wc -c < "$fa") bytes)"
    fi
  done <<< "$list_a"
  [[ "$status" == ok ]]
}

# cross_verify <label> <mode> <src db dir> <creator> <reader>
cross_verify() {
  local label=$1 mode=$2 src=$3 creator=$4 reader=$5
  local tag="$label-$creator-$reader"
  local dst="$RUN/db/$label-read-by-$reader-created-by-$creator"
  rm -rf "$dst"; cp -a "$src" "$dst" || { note "$label" "verify-$reader" 'ERROR' 'copy'; return 1; }
  local out rc=0
  out=$(/usr/bin/timeout -k 5s "${TIMEOUT}s" "$RUN/bin/$reader.exe" verify "$dst" "$mode" \
        2>&1) || rc=$?
  printf '%s\n' "$out" > "$RUN/logs/$tag.log"
  if ((rc)); then
    note "$label" "verify-$reader" 'FAIL' "rc=$rc ${out##*$'\n'}"
    printf '  %s could not read the %s-written database: %s\n' "$reader" "$creator" "$out"
    return 1
  fi
  local digest
  digest=$(printf '%s\n' "$out" | tail -1)
  if [[ "$digest" != VERIFY\ * ]]; then
    # Exit status alone is not enough: a driver that dies after its last printf
    # (lost stdio buffer) would look green here otherwise.
    note "$label" "verify-$reader" 'FAIL' 'no VERIFY line'
    printf '  %s read the %s-written database but printed no digest\n' "$reader" "$creator"
    return 1
  fi
  printf '%s\n' "$digest" > "$RUN/logs/$tag.digest"
  note "$label" "verify-$reader" 'PASS' "$digest"
}

digest_of() { cat "$RUN/logs/$1.digest" 2>/dev/null; }

for mode in $MODES; do
  printf '\n===== mode %s =====\n' "$mode"
  for engine in official kvdb; do
    db="$RUN/db/$mode-$engine"
    rm -rf "$db"
    rc=0
    /usr/bin/timeout -k 5s "${TIMEOUT}s" "$RUN/bin/$engine.exe" create "$db" "$mode" \
      > "$RUN/logs/$mode-$engine-create.log" 2>&1 || rc=$?
    last=$(tail -1 "$RUN/logs/$mode-$engine-create.log" 2>/dev/null)
    if ((rc)) || [[ "$last" != CREATE\ ok* ]]; then
      printf '  CREATE FAILED (%s, rc=%s): %s\n' "$engine" "$rc" "$(tail -3 "$RUN/logs/$mode-$engine-create.log")"
      note "$mode" "create-$engine" 'FAIL' "rc=$rc ${last:-no output}"
      result=1
    else
      note "$mode" "create-$engine" 'PASS' "$last"
    fi
  done
  if [[ -d "$RUN/db/$mode-official" && -d "$RUN/db/$mode-kvdb" ]]; then
    # Exploratory modes let the engine schedule background compactions, so the
    # file numbering legitimately differs; their reads must still agree.
    case " $EXPLORATORY " in *" $mode "*) bytes_optional=1 ;; *) bytes_optional=0 ;; esac
    if compare_bytes "$mode" "$RUN/db/$mode-official" "$RUN/db/$mode-kvdb"; then
      printf '  BYTES IDENTICAL\n'
      note "$mode" 'bytes' 'PASS' 'all files identical'
    elif ((bytes_optional)); then
      printf '  BYTE DIFFERENCE tolerated (exploratory mode)\n'
      note "$mode" 'bytes' 'INFO' 'layout may differ, reads must not'
    else
      printf '  BYTE DIFFERENCE (see above)\n'
      result=1
    fi
    for creator in official kvdb; do
      for reader in official kvdb; do
        cross_verify "$mode" "$mode" "$RUN/db/$mode-$creator" "$creator" "$reader" || result=1
      done
    done
    # All four reads of the same logical database must agree exactly: that is
    # the reader-side check (each engine parsing the other's bytes).
    ref=$(digest_of "$mode-official-official")
    for creator in official kvdb; do
      for reader in official kvdb; do
        got=$(digest_of "$mode-$creator-$reader")
        if [[ -z "$ref" || "$got" != "$ref" ]]; then
          printf '  DIGEST MISMATCH %s-%s-%s vs %s-official-official\n' \
            "$mode" "$creator" "$reader" "$mode"
          note "$mode" "digest-$creator-$reader" 'FAIL' "$got != $ref"
          result=1
        else
          note "$mode" "digest-$creator-$reader" 'PASS' 'agrees'
        fi
      done
    done
    printf '  files: '; (cd "$RUN/db/$mode-official" && ls -s . | tr '\n' ' '); printf '\n'
  fi
done

printf '\n===== summary =====\n'
if command -v column >/dev/null 2>&1; then
  column -t -s $'\t' "$RUN/results.tsv"
else
  awk -F'\t' '{printf "%-14s %-16s %-6s %s\n", $1, $2, $3, $4}' "$RUN/results.tsv"
fi
printf 'RESULT rc=%s; evidence: %s\n' "$result" "$RUN"
[[ ${KEEP:-1} == 1 ]] || rm -rf "$RUN"
exit "$result"
