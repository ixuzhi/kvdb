#!/usr/bin/env bash
# Run from Git Bash with:
# /d/ProgramFiles/msys64/usr/bin/bash.exe -lc 'bash /d/code/kvdb/scripts/run_interop.sh'
# or on native Linux with:  bash scripts/run_interop.sh
# No cmake, make, clean, submodule edits, or main object/library writes.
# Reference sources/settings mirror the pinned CMakeLists.txt and port config.
set -euo pipefail
# /usr/bin and /bin lead so the absolute tool paths below cannot resolve to a
# foreign-namespace gcc; the inherited PATH stays on as a tail because git is
# not part of a default MSYS2 install, and dropping it made this script die at
# `git: command not found` (rc=127) before it could report anything useful.
export PATH=/usr/bin:/bin${PATH:+:$PATH}
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ROOT="$REPO/build/interop"
PIN=7ee830d02b623e8ffe0b95d59a74db1e58da04c5
CC=/usr/bin/gcc
CXX=/usr/bin/g++
AR=/usr/bin/ar
TIMEOUT=${INTEROP_TIMEOUT:-30}
# Optional API smoke test; coordinator can run it separately without duplication.
RUN_C_TEST=${RUN_C_TEST:-0}
mkdir -p "$ROOT"
RUN=$(mktemp -d "$ROOT/run-XXXXXXXX")
exec > >(tee "$RUN/run.log") 2>&1
printf 'Artifacts: %s\n' "$RUN"
command -v git >/dev/null 2>&1 || {
  printf 'git not found on PATH - the reference checkout must be verified.\n' >&2
  printf 'MSYS2: pacman -S git\n' >&2; exit 2; }
commit=$(git -C "$REPO/leveldb" rev-parse HEAD)
[[ "$commit" == "$PIN" ]] || { printf 'Wrong official commit: %s\n' "$commit"; exit 2; }
# Windows checkout has CRLF; normalize only for this read-only integrity check.
# --ignore-submodules=all: the guard means "nobody edited the C++ sources this
# script compiles". third_party/benchmark is a nested submodule pointer, is
# never compiled here, and drifts on any clone where it was not checked out -
# counting it refused every reference build on a fresh Windows checkout.
git -c core.autocrlf=true -C "$REPO/leveldb" diff --quiet HEAD --ignore-submodules=all -- || { printf 'Official tracked files modified; refusing reference build\n'; exit 2; }
printf 'Official commit: %s\n' "$commit"
# Fail here rather than at the first "$CXX" line below: this script builds and
# links the reference engine itself, so a missing C++ compiler (the normal state
# once the verification packages are uninstalled - see doc/08 §3) is a
# precondition failure, not an engine failure.
[[ -x "$CXX" ]] || {
  printf '%s not found - the official reference engine is C++ and needs a compiler.\n' "$CXX" >&2
  printf 'This script builds with /usr/bin/g++, so the package has to provide that path.\n' >&2
  printf 'Debian: apt-get install g++   MSYS2 (MSYS shell): pacman -S gcc   Cygwin: setup -P gcc-g++\n' >&2
  exit 2; }
"$CC" --version
"$CXX" --version
"$AR" --version
# The POSIX reference build needs a compiler whose target *is* POSIX. Three
# triples qualify: native Linux, standalone Cygwin, and MSYS2's msys layer -
# the last of these reports x86_64-pc-msys, not the -cygwin suffix this test
# used to require, so every current MSYS2 install was refused.
TRIPLE=$("$CC" -dumpmachine)
case "$TRIPLE" in
  *linux*|*-cygwin|*-msys) ;;
  *) printf 'Expected a POSIX compiler (native Linux gcc, Cygwin gcc, or MSYS2 /usr/bin gcc), got %s\n' "$TRIPLE"; exit 2 ;;
esac
KVDB=${KVDB_LIB:-$REPO/build/libleveldb.a}
[[ -f "$KVDB" ]] || { printf 'Missing prebuilt %s; build it separately first\n' "$KVDB"; exit 2; }
# Same staleness trap as scripts/run_golden.sh: never interop-test an archive
# that predates the sources it claims to contain. `| head -1` instead of
# find's -quit, which BSD/macOS find does not have - there the guard would
# otherwise fail open as a silent no-op.
if [[ -n "$(find "$REPO/src" "$REPO/include" -name '*.[ch]' -newer "$KVDB" -print 2>/dev/null | head -1)" ]]; then
  printf '%s is older than the sources in src/; run make first\n' "$KVDB" >&2
  exit 2
fi
# ... and the same staleness trap across namespaces: build/ holds objects for
# whichever toolchain last ran make, and mtime cannot see that. Linking a
# foreign-namespace archive here surfaced as an "undefined reference" from ld
# that read like an engine defect (doc/04 N-14; the Makefile carries the
# matching guard). No .target file simply means an older build - not a claim.
ARCH_TARGET=$(head -n 1 "$(dirname "$KVDB")/.target" 2>/dev/null || :)
if [[ -n "$ARCH_TARGET" && "$ARCH_TARGET" != "$TRIPLE" ]]; then
  printf '%s was built for %s but this shell is %s; run make clean && make here first\n' \
    "$KVDB" "$ARCH_TARGET" "$TRIPLE" >&2
  exit 2
fi
# Snapshot the exact prebuilt library, avoiding concurrent rebuild races.
cp "$KVDB" "$RUN/libleveldb_kvdb.a"
sha256sum "$RUN/libleveldb_kvdb.a" "$REPO/tests/interop/interop_driver.c" "$REPO/scripts/run_interop.sh" > "$RUN/input.sha256"
mkdir -p "$RUN/obj" "$RUN/include/port" "$RUN/probe" "$RUN/db" "$RUN/logs"
# MSYS2 hides POSIX declarations in strict C++17 unless feature macros are set.
CXXFLAGS=(-std=c++17 -D_GNU_SOURCE -O2 -g -fno-exceptions -fno-rtti -pthread)
probe() {
  local name=$1 source=$2
  printf '%s\n' "$source" > "$RUN/probe/$name.cc"
  if "$CXX" "${CXXFLAGS[@]}" "$RUN/probe/$name.cc" -o "$RUN/probe/$name.exe" > "$RUN/probe/$name.log" 2>&1; then
    printf '1'
  else printf '0'; fi
}
fdatasync=$(probe fdatasync $'#include <unistd.h>\nint main(){return fdatasync(-1);}')
fullfsync=$(probe fullfsync $'#include <fcntl.h>\nint main(){return F_FULLFSYNC;}')
cloexec=$(probe cloexec $'#include <fcntl.h>\nint main(){return O_CLOEXEC;}')
# This official env_posix.cc checks defined(HAVE_O_CLOEXEC), so require support.
[[ "$cloexec" == 1 ]] || { printf 'O_CLOEXEC required by this pinned POSIX source\n'; exit 2; }
printf '#include <snappy.h>\n#include <string>\nint main(){std::string o; snappy::Compress("aaaabbbb",8,&o); return o.empty();}\n' > "$RUN/probe/snappy.cc"
if "$CXX" "${CXXFLAGS[@]}" "$RUN/probe/snappy.cc" -lsnappy -o "$RUN/probe/snappy.exe" > "$RUN/probe/snappy.log" 2>&1 && "$RUN/probe/snappy.exe"; then
  printf 'Snappy dependency available; compressed-mode tests NOT IMPLEMENTED in this baseline (no independent compressed validation claimed).\n'
else
  printf 'SKIP Snappy interop: independent MSYS2 Snappy dependency probe failed; see probe/snappy.log.\n'
fi
# All baseline tests explicitly set no_compression; do not reuse kvdb codec.
# Configure only generated output, not the official template or sources.
while IFS= read -r line; do
  line=${line%$'\r'}
  case "$line" in
    '#cmakedefine01 HAVE_FDATASYNC') line="#define HAVE_FDATASYNC $fdatasync" ;;
    '#cmakedefine01 HAVE_FULLFSYNC') line="#define HAVE_FULLFSYNC $fullfsync" ;;
    '#cmakedefine01 HAVE_O_CLOEXEC') line="#define HAVE_O_CLOEXEC $cloexec" ;;
    '#cmakedefine01 HAVE_CRC32C') line='#define HAVE_CRC32C 0' ;;
    '#cmakedefine01 HAVE_SNAPPY') line='#define HAVE_SNAPPY 0' ;;
    '#cmakedefine01 HAVE_ZSTD') line='#define HAVE_ZSTD 0' ;;
  esac
  printf '%s\n' "$line"
done < "$REPO/leveldb/port/port_config.h.in" > "$RUN/include/port/port_config.h"
printf 'Config: FDATASYNC=%s FULLFSYNC=%s O_CLOEXEC=%s CRC32C=0 SNAPPY=0 ZSTD=0; Bloom disabled\n' "$fdatasync" "$fullfsync" "$cloexec"
# Exactly the library .cc sources from pinned CMakeLists (POSIX + MemEnv).
sources=(
 db/builder.cc db/c.cc db/db_impl.cc db/db_iter.cc db/dbformat.cc
 db/dumpfile.cc db/filename.cc db/log_reader.cc db/log_writer.cc db/memtable.cc
 db/repair.cc db/table_cache.cc db/version_edit.cc db/version_set.cc db/write_batch.cc
 table/block_builder.cc table/block.cc table/filter_block.cc table/format.cc
 table/iterator.cc table/merger.cc table/table_builder.cc table/table.cc table/two_level_iterator.cc
 util/arena.cc util/bloom.cc util/cache.cc util/coding.cc util/comparator.cc util/crc32c.cc
 util/env.cc util/filter_policy.cc util/hash.cc util/logging.cc util/options.cc util/status.cc
 util/env_posix.cc helpers/memenv/memenv.cc
)
: > "$RUN/build.log"
objects=()
for source in "${sources[@]}"; do
  obj="$RUN/obj/${source//\//_}.o"
  cmd=("$CXX" "${CXXFLAGS[@]}" -DLEVELDB_PLATFORM_POSIX=1 -DLEVELDB_COMPILE_LIBRARY -DLEVELDB_HAS_PORT_CONFIG_H=1
    -I"$RUN/include" -I"$REPO/leveldb" -I"$REPO/leveldb/include" -c "$REPO/leveldb/$source" -o "$obj")
  printf '%q ' "${cmd[@]}" >> "$RUN/build.log"; printf '\n' >> "$RUN/build.log"
  if ! "${cmd[@]}" >> "$RUN/build.log" 2>&1; then printf 'Build failed: %s; see %s/build.log\n' "$source" "$RUN"; exit 2; fi
  objects+=("$obj")
done
"$AR" rcs "$RUN/libleveldb_official.a" "${objects[@]}"
# ONE C object against the official public header, linked twice. C++ linker
# supplies libstdc++ for the reference library, never links both engines together.
"$CC" -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -I"$REPO/leveldb/include" \
  -c "$REPO/tests/interop/interop_driver.c" -o "$RUN/driver.o"
"$CXX" "$RUN/driver.o" "$RUN/libleveldb_official.a" -pthread -o "$RUN/official.exe"
"$CC" "$RUN/driver.o" "$RUN/libleveldb_kvdb.a" -pthread -o "$RUN/kvdb.exe"
printf 'BUILD PASS: official library and both standalone drivers\n'
sha256sum "$RUN/libleveldb_official.a" "$RUN/official.exe" "$RUN/kvdb.exe" > "$RUN/output.sha256"
result=0
printf 'case\tstage\tengine\trc\n' > "$RUN/results.tsv"
run_stage() {
  local label=$1 engine=$2 db=$3 stage=$4 storage=$5 data=$6 rc=0
  local log="$RUN/logs/$label-$stage.log"
  /usr/bin/timeout -k 5s "${TIMEOUT}s" "$RUN/$engine.exe" "$db" "$stage" "$storage" "$data" > "$log" 2>&1 || rc=$?
  printf '%s\t%s\t%s\t%s\n' "$label" "$stage" "$engine" "$rc" >> "$RUN/results.tsv"
  printf '%s %s (%s): rc=%s\n' "$label" "$stage" "$engine" "$rc"
  if ((rc)); then
    result=1
    while IFS= read -r line; do printf '  %s\n' "$line"; done < "$log"
    return 1
  fi
}
inspect_files() {
  local label=$1 db=$2 stage=$3 storage=$4
  local file tables=0 nonempty_logs=0
  shopt -s nullglob
  for file in "$db"/*.ldb "$db"/*.sst; do ((tables+=1)); done
  for file in "$db"/*.log; do [[ ! -s "$file" ]] || ((nonempty_logs+=1)); done
  find "$db" -maxdepth 1 -type f -printf '%f %s bytes\n' | sort > "$RUN/logs/$label-$stage-files.log"
  printf '%s physical %s: tables=%s nonempty_WALs=%s\n' "$label" "$stage" "$tables" "$nonempty_logs"
  # Only creator WAL-only is asserted: ordinary reopen may flush recovered WAL.
  if [[ "$storage" == wal && "$stage" == create ]]; then
    ((tables == 0 && nonempty_logs > 0)) || return 1
  elif [[ "$storage" == sst ]]; then
    ((tables > 0)) || return 1
  fi
}
for data in basic edge; do
  for storage in wal sst; do
    # Same-engine controls distinguish harness/backend issues from interop issues.
    for direction in official-official kvdb-kvdb official-kvdb kvdb-official; do
      writer=${direction%-*}; reader=${direction#*-}
      label="$direction-$storage-$data"
      db="$RUN/db/$label"
      if ! run_stage "$label" "$writer" "$db" create "$storage" "$data"; then
        printf 'BLOCKED %s remaining stages: create failed\n' "$label"; continue
      fi
      if ! inspect_files "$label" "$db" create "$storage"; then
        printf 'FAIL %s physical storage precondition\n' "$label"; result=1; continue
      fi
      # Both cross-read and cross-update use independent processes.
      if ! run_stage "$label" "$reader" "$db" read "$storage" "$data"; then
        printf 'BLOCKED %s update/final: read failed\n' "$label"; continue
      fi
      if ! run_stage "$label" "$reader" "$db" update "$storage" "$data"; then
        printf 'BLOCKED %s final: update failed\n' "$label"; continue
      fi
      if ! inspect_files "$label" "$db" update "$storage"; then
        printf 'FAIL %s updated physical storage precondition\n' "$label"; result=1; continue
      fi
      run_stage "$label" "$writer" "$db" final "$storage" "$data" || true
    done
  done
done
if [[ "$RUN_C_TEST" == 1 ]]; then
  "$CC" -std=c11 -D_GNU_SOURCE -O2 -g -I"$REPO/leveldb/include" "$REPO/leveldb/db/c_test.c" \
    "$RUN/libleveldb_kvdb.a" -pthread -o "$RUN/c_test_kvdb.exe"
  mkdir -p "$RUN/c-test-tmp"
  rc=0
  TMPDIR="$RUN/c-test-tmp" TEST_TMPDIR="$RUN/c-test-tmp/db" \
    /usr/bin/timeout -k 5s "${TIMEOUT}s" "$RUN/c_test_kvdb.exe" > "$RUN/c_test.log" 2>&1 || rc=$?
  printf 'Official unmodified c_test against kvdb: rc=%s\n' "$rc"
  printf 'official-c-test\tall\tkvdb\t%s\n' "$rc" >> "$RUN/results.tsv"
  ((rc == 0)) || result=1
else
  printf 'SKIP official c_test: RUN_C_TEST=0 (set 1 to run isolated with timeout)\n'
fi
printf 'RESULT rc=%s; evidence preserved: %s\n' "$result" "$RUN"
exit "$result"
