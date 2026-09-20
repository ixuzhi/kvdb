#!/usr/bin/env bash
# Cross-backend compatibility leg: does the Windows Env (env_win.c, selected
# when _WIN32 is defined) produce and read *exactly* the bytes the POSIX Env
# (env_posix.c) does?
#
# This is the one pairing the other legs never cover. run_golden.sh and
# run_interop.sh both compare kvdb against the official C++ engine, and both
# refuse a native-Windows compiler, so on Windows every byte of their evidence
# comes from env_posix.c alone - env_win.c is only ever exercised by the unit
# suite and by c_test, which never compares files to another backend's output.
# A divergence in CreateFileA semantics, flush ordering, path separators, or
# directory listing would therefore be invisible to the format harnesses.
#
# Two engines are built from the same sources into isolated object trees (one
# per backend), each with its own copy of tests/interop/golden_driver.c. For
# every workload mode the script then
#   1. writes the database twice, once per backend;
#   2. compares the two directories file by file, byte by byte;
#   3. has each backend read the other's database and requires identical
#      digests.
#
# Run from an MSYS2 shell (or Git Bash with a POSIX gcc reachable) with:
#   bash scripts/run_cross_backend.sh
# Env: CC_POSIX=/usr/bin/gcc  CC_WIN=path/to/native-windows/gcc
#      MODES="..."  KEEP=1
#
# Database arguments are deliberately RELATIVE: a POSIX-namespace binary and a
# native Win32 binary resolve /g/... differently, so absolute evidence paths
# would make one of them write somewhere else. Everything runs with the shell
# positioned inside the evidence directory.
set -uo pipefail
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
TIMEOUT=${GOLDEN_TIMEOUT:-120}
MODES=${MODES:-"sst sst-bloom sst-restart1 sst-bigblock wal wal-big wal-frag edge tomb"}
# `levels` is left out on purpose: its file numbering depends on
# background-compaction timing, so it can differ without meaning anything.
# run_golden.sh treats it as exploratory for the same reason.

# A byte comparison is only as good as the digest tool underneath it: with
# neither sha256sum nor shasum available both sides would hash to an empty
# string and every file would read SAME. Fail closed instead. Same discipline
# as scripts/run_golden.sh.
if command -v sha256sum >/dev/null 2>&1; then
  digest() { sha256sum | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
  digest() { shasum -a 256 | cut -d' ' -f1; }
else
  printf 'Neither sha256sum nor shasum found; refusing (a byte comparison would fail open)\n' >&2
  exit 2
fi

RUN=${RUN_DIR:-$REPO/build/interop/cross-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$RUN/bin" "$RUN/logs" "$RUN/obj"
cd "$RUN" || exit 2
exec > >(tee "$RUN/run.log") 2>&1
printf 'Artifacts: %s\n' "$RUN"
result=0
note() { printf '%-14s %-14s %-7s %s\n' "$1" "$2" "$3" "$4"; }

# ---- pick the two toolchains ------------------------------------------------
CC_POSIX=${CC_POSIX:-/usr/bin/gcc}
if [[ -z "${CC_WIN:-}" ]]; then
  if [[ -x "$REPO/_tools/w64devkit/w64devkit/bin/gcc.exe" ]]; then
    CC_WIN="$REPO/_tools/w64devkit/w64devkit/bin/gcc.exe"
  else
    CC_WIN=gcc
  fi
fi
for pair in "posix:$CC_POSIX" "windows:$CC_WIN"; do
  c=${pair#*:}
  command -v "$c" >/dev/null 2>&1 || {
    printf '%s compiler not found: %s\n' "${pair%%:*}" "$c" >&2
    printf 'Pass CC_WIN=... (e.g. _tools/w64devkit/w64devkit/bin/gcc.exe)\n' >&2
    exit 2; }
done
T_POSIX=$("$CC_POSIX" -dumpmachine)
T_WIN=$("$CC_WIN" -dumpmachine)
# Same triple on both sides would compare a backend against itself and report a
# meaningless green, so require that each side really is what it claims.
case "$T_POSIX" in *linux*|*-cygwin|*-msys) ;; *)
  printf 'CC_POSIX=%s targets %s, which is not a POSIX triple.\n' "$CC_POSIX" "$T_POSIX" >&2; exit 2 ;; esac
case "$T_WIN" in *mingw*|*windows*) ;; *)
  printf 'CC_WIN=%s targets %s, which is not a native-Windows triple.\n' "$CC_WIN" "$T_WIN" >&2
  printf 'The Windows half of this leg needs a compiler that defines _WIN32.\n' >&2; exit 2 ;; esac
printf 'posix   backend: %s [%s] via %s\n' "$("$CC_POSIX" --version | head -1)" "$T_POSIX" "$CC_POSIX"
printf 'windows backend: %s [%s] via %s\n' "$("$CC_WIN" --version | head -1)" "$T_WIN" "$CC_WIN"

# The native-Windows half asks Win32 for its scratch directory, so TMP/TEMP must
# hold a *Win32* path - an MSYS-style /tmp gets rewritten against the wrong root
# ("Cannot create temporary file in D:\msys64\msys64\tmp") and only the Windows
# half fails, which reads like a backend disagreement. run_sanitizers.sh has
# carried this guard since P2-11; this leg needs it for the same reason.
if [[ -z "${TMP:-}" && -z "${TEMP:-}" && -z "${TMPDIR:-}" ]]; then
  TW=$(/usr/bin/cygpath -w /tmp 2>/dev/null) || TW=.
  export TMP="$TW" TEMP="$TW"
  printf 'TMP was unset; pointing the windows half at %s\n' "$TW"
fi
# cc1.exe sits in the compiler's own lib/ tree but resolves its DLL dependencies
# (libgcc_s_seh-1, libwinpthread, ...) through PATH, so an absolute CC_WIN invoked
# from a foreign namespace exits non-zero *without printing a single word* - and
# the probe below would report that as a bad TMP. Append, never prepend: /usr/bin
# has to keep providing make, sed and find.
CC_WIN_DIR=$(dirname "$CC_WIN")
if [[ "$CC_WIN" == */* && -d "$CC_WIN_DIR" ]]; then
  export PATH="$PATH:$CC_WIN_DIR"
fi
# Probing the compiler rather than the variable is the point: a set-but-wrong
# TMP is exactly as fatal as an unset one, and only a real compile sees it.
PROBE=$RUN/obj/tmpprobe.c
printf 'int main(void){return 0;}\n' > "$PROBE"
"$CC_WIN" -c "$PROBE" -o "$RUN/obj/tmpprobe.o" > "$RUN/logs/tmpprobe.log" 2>&1
probe_rc=$?
if ((probe_rc)); then
  if [[ -s "$RUN/logs/tmpprobe.log" ]]; then
    printf 'The windows compiler cannot build a trivial file:\n' >&2
    sed 's/^/  /' "$RUN/logs/tmpprobe.log" >&2
    printf 'TMP=%s TEMP=%s TMPDIR=%s - this leg needs a Win32-shaped, writable one.\n' \
      "${TMP:-unset}" "${TEMP:-unset}" "${TMPDIR:-unset}" >&2
    printf 'Run it from a login shell (bash -lc, which sets TMP via /etc/profile),\n' >&2
    printf 'or export TMP="$(cygpath -w /tmp)" first. Do not hand an MSYS-style\n' >&2
    printf 'path such as /msys64/tmp to a native-Windows compiler - it gets\n' >&2
    printf 'rewritten against the wrong root.\n' >&2
  else
    # No diagnostic at all means the compiler never got as far as its own driver.
    printf '%s exited %s without a word: it could not start cc1.exe.\n' \
      "$CC_WIN" "$probe_rc" >&2
    printf 'Its bin directory (%s) holds the DLLs cc1.exe needs, so this is not\n' "$CC_WIN_DIR" >&2
    printf 'a TMP problem: check that the directory is readable and that the\n' >&2
    printf 'compiler installation is complete.\n' >&2
  fi
  exit 2
fi
rm -f "$PROBE" "$RUN/obj/tmpprobe.o"

# ---- build one engine + driver per backend ----------------------------------
# Third argument onward is the link tail: -lpthread belongs on the POSIX side
# only, -static on the MinGW side (matching what the Makefile does for that
# triple). Handing -lpthread to the Windows link would be a silent no-op at best.
build_flavor() {
  local name=$1 cc=$2; shift 2
  local libs="$*"
  make -C "$REPO" -j4 CC="$cc" OBJDIR="$RUN/obj/$name" BINDIR="$RUN/obj/$name" \
    all > "$RUN/logs/build-$name.log" 2>&1 || {
      note "build $name" 'engine' 'FAIL' "see logs/build-$name.log"; return 1; }
  "$cc" -std=c11 -O1 -g -I"$REPO/leveldb/include" \
    -c "$REPO/tests/interop/golden_driver.c" -o "$RUN/obj/$name/gd.o" \
    >> "$RUN/logs/build-$name.log" 2>&1 || {
      note "build $name" 'driver' 'FAIL' "see logs/build-$name.log"; return 1; }
  "$cc" "$RUN/obj/$name/gd.o" "$RUN/obj/$name/libleveldb.a" \
    -o "$RUN/bin/$name.exe" $libs >> "$RUN/logs/build-$name.log" 2>&1 || {
      note "build $name" 'driver link' 'FAIL' "see logs/build-$name.log"; return 1; }
  note "build $name" 'ok' 'PASS' "obj/$name/libleveldb.a"
}
build_flavor posix "$CC_POSIX" -lpthread || exit 2
build_flavor windows "$CC_WIN" -static || exit 2

# ---- per-mode comparison -----------------------------------------------------
compare_bytes() {
  local label=$1 a=$2 b=$3 status=ok
  local list_a list_b f ha hb
  list_a=$(cd "$a" && find . -maxdepth 1 -type f ! -name 'LOG*' ! -name 'LOCK' | sort)
  list_b=$(cd "$b" && find . -maxdepth 1 -type f ! -name 'LOG*' ! -name 'LOCK' | sort)
  if [[ "$list_a" != "$list_b" ]]; then
    note "$label" 'bytes' 'DIFF' 'file set differs'
    printf '  posix  :%s\n  windows:%s\n' "$(tr '\n' ' ' <<< "$list_a")" \
                                           "$(tr '\n' ' ' <<< "$list_b")"
    return 1
  fi
  while IFS= read -r f; do
    [[ -z "$f" ]] && continue
    ha=$(digest < "$a/$f"); hb=$(digest < "$b/$f")
    if [[ "$ha" != "$hb" ]]; then
      status=DIFF
      note "$label" 'bytes' 'DIFF' "$f ($(wc -c < "$a/$f") vs $(wc -c < "$b/$f") bytes)"
    else
      note "$label" 'bytes' 'SAME' "$f ($(wc -c < "$a/$f") bytes)"
    fi
  done <<< "$list_a"
  [[ "$status" == ok ]]
}

for mode in $MODES; do
  for who in posix windows; do
    rm -rf "db-$who"
    mkdir -p "db-$who"
    out=$(/usr/bin/timeout -k 5s "${TIMEOUT}s" "bin/$who.exe" create "db-$who/$mode" "$mode" 2>&1); rc=$?
    line=${out##*$'\n'}
    if ((rc)) || [[ "$line" != CREATE\ ok* ]]; then
      note "$mode" "create-$who" 'FAIL' "rc=$rc $line"
      printf '%s\n' "$out" > "logs/$mode-$who-create.log"
      result=1; continue 2
    fi
    printf '%s\n' "$out" > "logs/$mode-$who-create.log"
    note "$mode" "create-$who" 'PASS' "$line"
  done
  compare_bytes "$mode" "db-posix/$mode" "db-windows/$mode" || result=1
  # Reader half of this leg: all four reads of the same logical database (each
  # backend over each backend's bytes) must print the same digest. Compared
  # against the first read rather than against a stored expected value - the
  # point is that the backends agree with each other, not that either matches
  # an outside oracle (that is what run_golden.sh does against official leveldb).
  ref=
  for creator in posix windows; do
    for reader in posix windows; do
      out=$(/usr/bin/timeout -k 5s "${TIMEOUT}s" "bin/$reader.exe" verify \
              "db-$creator/$mode" "$mode" 2>&1); rc=$?
      line=${out##*$'\n'}
      printf '%s\n' "$out" > "logs/$mode-$creator-$reader.log"
      tag="read-$reader-of-$creator"
      if ((rc)) || [[ "$line" != VERIFY\ * ]]; then
        note "$mode" "$tag" 'FAIL' "rc=$rc $line"
        result=1; continue
      fi
      if [[ -z "$ref" ]]; then
        ref=${line#VERIFY }
        note "$mode" "$tag" 'PASS' "$line (reference)"
      elif [[ "${line#VERIFY }" != "$ref" ]]; then
        note "$mode" "$tag" 'FAIL' "digest differs from the first read"
        printf '  expected: %s\n  got     : %s\n' "$ref" "${line#VERIFY }"
        result=1
      else
        note "$mode" "$tag" 'PASS' "$line"
      fi
    done
  done
done

# ---- cross-process LOCK exclusion (doc/06 P2-6) -----------------------------
# The Windows backend claims cross-process exclusion through CreateFileA's
# share mode rather than flock(2), and that difference was never tested. Open
# one database from several processes at once and require: every process either
# reads it or is refused by the lock - never anything else - and that all the
# readers agree byte for byte on what is in there.
#
# At least one refusal is required, otherwise a backend that silently allowed
# concurrent writers would pass. Empirically about half of 8 simultaneous
# opens collide, so a zero-collision run means the probe stopped working, not
# that the platform got lucky.
probe_lock() {
  # Separate `local` statements, not one: bash expands every word of a single
  # declaration before assigning any of it, so `local who=$1 db="db-$who/x"` builds
  # db from the *inherited* who - here the leftover `windows` from the mode loop
  # above. Both probes then shared one directory, and the posix half reported
  # refusals on a tree named db-windows (doc/04 N-16).
  local who=$1
  local mode=sst-bigblock n=8 i
  local db="db-$who/lockprobe"
  local -a pids=()
  rm -rf "$db" "lock-$who"; mkdir -p "$db" "lock-$who"
  "bin/$who.exe" create "$db" "$mode" > "logs/$who-lockprobe-seed.log" 2>&1 || {
    note "$who" 'lock probe' 'FAIL' 'could not seed the database'; return 1; }
  for ((i = 0; i < n; i++)); do
    { /usr/bin/timeout -k 5s 60s "bin/$who.exe" verify "$db" "$mode" \
        > "lock-$who/$i.log" 2>&1
      echo $? > "lock-$who/$i.rc"; } &
    pids+=($!)
  done
  # Wait on the probe's own children only. A bare `wait` also blocks on the
  # `tee` started by `exec > >(tee run.log)` above, and that coprocess only
  # exits once this script closes its stdout - so a bare wait deadlocks here.
  wait "${pids[@]}"
  local ok=0 refused=0 rc line first=
  for ((i = 0; i < n; i++)); do
    rc=$(cat "lock-$who/$i.rc" 2>/dev/null)
    line=$(tail -1 "lock-$who/$i.log" 2>/dev/null)
    if [[ "$rc" == 0 && "$line" == VERIFY\ * ]]; then
      ok=$((ok + 1))
      if [[ -z "$first" ]]; then first=${line#VERIFY }
      elif [[ "${line#VERIFY }" != "$first" ]]; then
        note "$who" 'lock probe' 'FAIL' "concurrent readers disagree"
        printf '  %s\n  %s\n' "$first" "${line#VERIFY }"
        return 1
      fi
    # A refusal has to be *the lock*, not any text containing four letters in a
    # row: the seed database is named lockprobe, the workload is sst-bigblock,
    # and LevelDB's own corruption messages say "block" - so a bare /LOCK/i
    # counted crashes and corruption as legitimate contention, which is exactly
    # the judgement "at least one refusal" exists to make. Match the file the
    # engine refuses on: "<db>/LOCK" followed by its colon or its errno.
    elif grep -qiE '/LOCK[: (]|win32 error 32|Resource deadlock|Resource temporarily|temporarily unavailable|being used by another process' \
         "lock-$who/$i.log" 2>/dev/null; then
      refused=$((refused + 1))
    else
      note "$who" 'lock probe' 'FAIL' "unexplained rc=${rc:-?} ${line:-no output}"
      return 1
    fi
  done
  if ((ok == 0)); then
    note "$who" 'lock probe' 'FAIL' "all $n opens refused ($refused by lock)"
    return 1
  fi
  if ((refused == 0)); then
    note "$who" 'lock probe' 'FAIL' "no contention: $ok/$n opened, lock not exercised"
    return 1
  fi
  note "$who" 'lock probe' 'PASS' "$ok opened, $refused lock-refused, digests agree"
}
probe_lock windows || result=1
probe_lock posix || result=1

printf '\nRESULT rc=%s; evidence: %s\n' "$result" "$RUN"
[[ ${KEEP:-1} == 1 ]] || rm -rf "$RUN"
exit "$result"
