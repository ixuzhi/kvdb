#!/usr/bin/env bash
# Memory-safety leg for kvdb: build the whole engine with AddressSanitizer and
# UndefinedBehaviorSanitizer, then run three consumers over it:
#
#   1. the ported unit suite (tests/*);
#   2. the UNMODIFIED official leveldb/db/c_test.c conformance test;
#   3. tests/interop/golden_driver.c create/verify, optionally pointed at
#      databases written by the real C++ engine (see OFFICIAL_DBS below), which
#      exercises the reader over foreign bytes under the sanitizer.
#
# Which compiler: MSYS2 ships no GCC sanitizer runtime for any Windows target
# (pacman -S mingw-w64-x86_64-sanitizers does not exist), so there clang64 is
# the only toolchain that can build these checks. On a native Linux host the
# GCC runtime is present, so gcc is the default there - and unlike the Windows
# build, glibc ASan carries LeakSanitizer (see SAN_DETECT_LEAKS below).
#
# Run from Git Bash with:
#   MSYSTEM=CLANG64 /d/ProgramFiles/msys64/usr/bin/bash -c \
#     'export PATH=/clang64/bin:/usr/bin:/bin; bash /d/code/kvdb/scripts/run_sanitizers.sh'
# or on native Linux with:
#   bash scripts/run_sanitizers.sh
#
# Env: OFFICIAL_DBS=<golden evidence dir> also verifies the official engine's
#      files (produced by scripts/run_golden.sh); MODES, SAN, KEEP=1,
#      SAN_DETECT_LEAKS=1 turns LeakSanitizer on (unsupported on Windows).
set -uo pipefail
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# Default compiler follows the host: Windows cross targets need clang, a POSIX
# host toolchain already carries its own sanitizer runtimes.
if [[ -z "${CC:-}" ]]; then
  case "$(/usr/bin/gcc -dumpmachine 2>/dev/null)" in
    *cygwin*|*mingw*|*windows*) CC=clang ;;
    *) CC=gcc ;;
  esac
fi
SAN=${SAN:--fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all}
CFLAGS_SAN="-std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter $SAN"
TRIPLE=$("$CC" -dumpmachine 2>/dev/null)
# glibc hides POSIX declarations under -std=c11 unless the feature-test macro is
# set; the Makefile does this for the library, the script must do it for the
# standalone consumers it compiles itself. Cygwin is exempt (its headers want
# the macro left alone) and Windows targets must not see it at all.
POSIX_DEFS=
case "$TRIPLE" in
  *linux*) POSIX_DEFS=-D_GNU_SOURCE ;;
esac
# Only the Windows backend needs TMP/TEMP pointed at a Win32 path; a POSIX host
# resolves its scratch directory through env_posix.c.
ON_WINDOWS=0
case "$TRIPLE" in
  *windows*|*mingw*|*cygwin*) ON_WINDOWS=1 ;;
esac
OBJDIR=${OBJDIR:-build/san}
MODES=${MODES:-"sst sst-bloom sst-restart1 sst-bigblock wal wal-big wal-frag edge tomb levels"}
cd "$REPO" || exit 2
# Setting MSYSTEM only selects the shell's PATH when the process is started by
# MSYS2 itself; prefixing the variable in an ordinary Git Bash does not make
# clang appear. Check that before anything on disk is removed.
if ! command -v "$CC" >/dev/null 2>&1; then
  printf '%s: %s is not on PATH.\n' "$0" "$CC" >&2
  if [[ $CC == clang ]]; then
    printf 'Open an "MSYS2 CLANG64" shell, or invoke it directly:\n' >&2
    printf '  MSYSTEM=CLANG64 /path/to/msys64/usr/bin/bash -c %s\n' \
           "'export PATH=/clang64/bin:/usr/bin:/bin; cd $REPO && bash scripts/run_sanitizers.sh'" >&2
  else
    printf 'This leg needs a compiler that ships a sanitizer runtime (Debian: apt-get install gcc).\n' >&2
    printf 'On MSYS2 only clang has one: CC=clang with an "MSYS2 CLANG64" shell.\n' >&2
  fi
  exit 2
fi
# Evidence lands outside OBJDIR on purpose: the archive must be wiped before a
# sanitized build (a stale env_posix.o from another toolchain would poison it)
# and that wipe must not take earlier runs with it.
RUN=${RUN_DIR:-$REPO/build/san-runs/interop-$(date +%Y%m%d-%H%M%S)}
rm -rf "$OBJDIR"
mkdir -p "$RUN"
exec > >(tee "$RUN/run.log") 2>&1
printf 'Artifacts: %s\n' "$RUN"

# The native Windows backend asks GetTempPathA for a scratch directory, and that
# API falls through to C:\Windows when TMP/TEMP/TMPDIR are all unset - which an
# MSYS2 env shell does leave unset. Every real-filesystem test then fails on
# directory permissions rather than on anything in the engine.
if [[ $ON_WINDOWS == 1 && -z "${TMP:-}" && -z "${TEMP:-}" && -z "${TMPDIR:-}" ]]; then
  TW=$(/usr/bin/cygpath -w /tmp 2>/dev/null) || TW=.
  export TMP="$TW" TEMP="$TW"
  printf 'TMP was unset; pointing the native backend at %s\n' "$TW"
fi

printf 'toolchain: %s\n' "$("$CC" --version | head -1) [$TRIPLE]"
result=0
note() { printf '%-22s %-8s %s\n' "$1" "$2" "$3"; }

# SANFLAGS, not CFLAGS: a command-line `make CFLAGS=...` replaces the Makefile's
# flags AND swallows its `+=` appends, which would drop -D_GNU_SOURCE and
# -lpthread from the POSIX branch.
make -j4 CC="$CC" OBJDIR="$OBJDIR" BINDIR="$OBJDIR" SANFLAGS="$CFLAGS_SAN" \
  > "$RUN/build.log" 2>&1
if [[ $? -ne 0 ]]; then
  note 'sanitized build' 'FAIL' "see $RUN/build.log"
  tail -20 "$RUN/build.log"
  exit 2
fi
note 'sanitized build' 'PASS' "$OBJDIR/libleveldb.a"

# Any sanitizer report is fatal with -fno-sanitize-recover=all, so a clean exit
# code here means no report at all, not merely an ignored one.
export ASAN_OPTIONS=detect_leaks=0
export UBSAN_OPTIONS=print_stacktrace=1
# detect_leaks=1 is not merely ignored on Windows, it stops the process before
# main(), so leaks cannot be checked with any local Windows toolchain. On Linux
# glibc the same option is live, and SAN_DETECT_LEAKS=1 turns it on for every
# consumer below - the leak leg itself is still tracked separately.
case "$TRIPLE" in
  *linux*) printf 'leak check: SUPPORTED here (SAN_DETECT_LEAKS=1 to enable), probe says: %s\n' \
    "$(ASAN_OPTIONS=detect_leaks=1 "$OBJDIR/kvdb_tests" nosuchtest 2>&1 | head -1)"
    [[ "${SAN_DETECT_LEAKS:-0}" == 1 ]] && export ASAN_OPTIONS=detect_leaks=1 ;;
  *) printf 'leak check: %s\n' "$(ASAN_OPTIONS=detect_leaks=1 "$OBJDIR/kvdb_tests" nosuchtest 2>&1 | head -1)" ;;
esac

out=$("$OBJDIR/kvdb_tests" 2>&1); rc=$?
printf '%s\n' "$out" > "$RUN/unit.log"
summary=$(printf '%s\n' "$out" | tail -1)
if [[ $rc -eq 0 && "$summary" == *", 0 failed" ]]; then
  note 'unit suite' 'PASS' "$summary"
else
  note 'unit suite' 'FAIL' "rc=$rc $summary"
  grep -E '\[   FAILED \]|ERROR: AddressSanitizer|runtime error' "$RUN/unit.log" | head -20
  result=1
fi
if grep -qE 'ERROR: AddressSanitizer|runtime error|SUMMARY' "$RUN/unit.log"; then
  note 'unit diagnostics' 'FAIL' 'sanitizer reports found'
  result=1
else
  note 'unit diagnostics' 'PASS' 'no ASan/UBSan report'
fi

# ---- official conformance test, same source the C++ project ships ----------
"$CC" -std=c11 $POSIX_DEFS -O1 -g -Ileveldb/include -c leveldb/db/c_test.c \
  -o "$RUN/c_test.o" &&
  "$CC" $SAN "$RUN/c_test.o" "$OBJDIR/libleveldb.a" -o "$RUN/c_test.exe"
if [[ ! -f "$RUN/c_test.exe" ]]; then
  note 'official c_test' 'FAIL' 'could not build'
  result=1
else
  out=$("$RUN/c_test.exe" 2>&1); rc=$?
  printf '%s\n' "$out" > "$RUN/c_test.log"
  last=$(tail -1 "$RUN/c_test.log")
  if [[ $rc -eq 0 && "$last" == PASS ]]; then
    note 'official c_test' 'PASS' "$last ($(grep -c '=== Test' "$RUN/c_test.log") phases)"
  else
    note 'official c_test' 'FAIL' "rc=$rc $last"
    result=1
  fi
  grep -E 'ERROR: AddressSanitizer|runtime error' "$RUN/c_test.log" | head -5
fi

# ---- golden driver: writer and reader paths over real format bytes ---------
"$CC" -std=c11 $POSIX_DEFS $CFLAGS_SAN -Wall -Wextra -Wno-unused-parameter \
  -Ileveldb/include -c tests/interop/golden_driver.c -o "$RUN/golden_driver.o" &&
  "$CC" $SAN "$RUN/golden_driver.o" "$OBJDIR/libleveldb.a" -o "$RUN/golden_driver.exe"
if [[ ! -f "$RUN/golden_driver.exe" ]]; then
  note 'golden driver' 'FAIL' 'could not build'
  result=1
else
  src_dbs=${OFFICIAL_DBS:-}
  [[ -n "$src_dbs" && -d "$src_dbs/db" ]] || src_dbs=
  note 'golden driver' 'INFO' "foreign databases: ${src_dbs:-none, using the kvdb-written ones}"
  for mode in $MODES; do
    db="$RUN/db/$mode"
    mkdir -p "$RUN/db"  # CreateDir is single-level on Windows, as in leveldb
    rm -rf "$db"
    out=$("$RUN/golden_driver.exe" create "$db" "$mode" 2>&1); rc=$?
    if [[ $rc -ne 0 || "$(printf '%s\n' "$out" | tail -1)" != CREATE\ ok* ]]; then
      note "create $mode" 'FAIL' "rc=$rc $(printf '%s\n' "$out" | tail -1)"
      result=1
      continue
    fi
    verify_src=$db
    if [[ -n "$src_dbs" && -d "$src_dbs/db/$mode-official" ]]; then
      verify_src="$RUN/db/$mode-official"
      cp -a "$src_dbs/db/$mode-official" "$verify_src"
    fi
    out2=$("$RUN/golden_driver.exe" verify "$verify_src" "$mode" 2>&1); rc2=$?
    line=$(printf '%s\n' "$out2" | tail -1)
    if [[ $rc2 -ne 0 || "$line" != VERIFY\ * ]]; then
      note "verify $mode" 'FAIL' "rc=$rc2 $line"
      result=1
    else
      note "verify $mode" 'PASS' "$line"
    fi
    printf '%s\n' "$out" "$out2" > "$RUN/$mode.log"
  done
  if grep -qrE 'ERROR: AddressSanitizer|runtime error' "$RUN"/*.log; then
    note 'golden diagnostics' 'FAIL' 'sanitizer reports found'
    grep -rE 'ERROR: AddressSanitizer|runtime error' "$RUN"/*.log | head -5
    result=1
  else
    note 'golden diagnostics' 'PASS' 'no ASan/UBSan report'
  fi
fi

printf '\nRESULT rc=%s; evidence: %s\n' "$result" "$RUN"
[[ ${KEEP:-1} == 1 ]] || rm -rf "$RUN"
exit "$result"
