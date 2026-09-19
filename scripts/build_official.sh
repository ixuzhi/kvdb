#!/usr/bin/env bash
# Build the OFFICIAL LevelDB static library from the pinned submodule source
# with the MSYS2 (POSIX) toolchain, and cache it for reuse by the golden
# byte-comparison runner.
#
# Run from Git Bash with:
#   /d/ProgramFiles/msys64/usr/bin/bash.exe -lc 'bash <this script>'
# Last line of stdout is the absolute path of the produced archive.
#
# The source list and the port_config probes mirror the pinned CMakeLists.txt
# (see also scripts/run_interop.sh, which builds the same set into a throwaway
# run directory).
set -euo pipefail
export PATH=/usr/bin:/bin
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
PIN=7ee830d02b623e8ffe0b95d59a74db1e58da04c5
CC=/usr/bin/gcc
CXX=/usr/bin/g++
AR=/usr/bin/ar
QUIET=${QUIET:-1}
say() { [[ "$QUIET" == 1 ]] || printf '%s\n' "$@" >&2; }

OUT=$REPO/build/interop/official
commit=$(git -C "$REPO/leveldb" rev-parse HEAD)
[[ "$commit" == "$PIN" ]] || { printf 'Wrong official commit: %s\n' "$commit" >&2; exit 2; }
# Windows checkouts carry CRLF; normalize only for this read-only check.
git -c core.autocrlf=true -C "$REPO/leveldb" diff --quiet HEAD -- || {
  printf 'Official tracked files modified; refusing reference build\n' >&2; exit 2; }
[[ $("$CC" -dumpmachine) == x86_64-pc-cygwin ]] || {
  printf 'Expected MSYS2 /usr/bin POSIX compiler, got %s\n' "$("$CC" -dumpmachine)" >&2; exit 2; }

STAMP="$OUT/.built-$commit-$("$CC" -dumpversion)-$("$CXX" -dumpversion)"
if [[ -f "$STAMP" && -f "$OUT/lib/libleveldb_official.a" ]]; then
  say 'reusing cached official build'
  printf '%s\n' "$OUT/lib/libleveldb_official.a"
  exit 0
fi

mkdir -p "$OUT/obj" "$OUT/include/port" "$OUT/lib" "$OUT/probe"
CXXFLAGS=(-std=c++17 -D_GNU_SOURCE -O2 -g -fno-exceptions -fno-rtti -pthread)
probe() {
  local name=$1 source=$2
  printf '%s\n' "$source" > "$OUT/probe/$name.cc"
  if "$CXX" "${CXXFLAGS[@]}" "$OUT/probe/$name.cc" -o "$OUT/probe/$name.exe" \
       > "$OUT/probe/$name.log" 2>&1; then printf '1'; else printf '0'; fi
}
fdatasync=$(probe fdatasync $'#include <unistd.h>\nint main(){return fdatasync(-1);}')
fullfsync=$(probe fullfsync $'#include <fcntl.h>\nint main(){return F_FULLFSYNC;}')
cloexec=$(probe cloexec $'#include <fcntl.h>\nint main(){return O_CLOEXEC;}')
[[ "$cloexec" == 1 ]] || { printf 'O_CLOEXEC required by this pinned POSIX source\n' >&2; exit 2; }
# CRC32C hardware acceleration and third-party codecs are disabled so the
# reference build shares kvdb's pure-software, uncompressed baseline.
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
done < "$REPO/leveldb/port/port_config.h.in" > "$OUT/include/port/port_config.h"
say "Config: FDATASYNC=$fdatasync FULLFSYNC=$fullfsync O_CLOEXEC=$cloexec CRC32C=0 SNAPPY=0 ZSTD=0"

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
: > "$OUT/build.log"
objects=()
for source in "${sources[@]}"; do
  obj="$OUT/obj/${source//\//_}.o"
  cmd=("$CXX" "${CXXFLAGS[@]}" -DLEVELDB_PLATFORM_POSIX=1 -DLEVELDB_COMPILE_LIBRARY
    -DLEVELDB_HAS_PORT_CONFIG_H=1 -I"$OUT/include" -I"$REPO/leveldb"
    -I"$REPO/leveldb/include" -c "$REPO/leveldb/$source" -o "$obj")
  printf '%q ' "${cmd[@]}" >> "$OUT/build.log"; printf '\n' >> "$OUT/build.log"
  if ! "${cmd[@]}" >> "$OUT/build.log" 2>&1; then
    printf 'Build failed: %s; see %s/build.log\n' "$source" "$OUT" >&2; exit 2; fi
  objects+=("$obj")
done
rm -f "$OUT/lib/libleveldb_official.a"
"$AR" rcs "$OUT/lib/libleveldb_official.a" "${objects[@]}"
touch "$STAMP"
printf '%s\n' "$OUT/lib/libleveldb_official.a"
