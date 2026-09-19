#!/usr/bin/env bash
# Build the OFFICIAL LevelDB static library from the pinned submodule source
# with a POSIX toolchain (MSYS2 /usr/bin, standalone Cygwin, or a native Linux
# gcc), and cache it for reuse by the golden byte-comparison runner.  The cache
# is per target triple under build/interop/official/<triple>/, because an
# archive built in one POSIX namespace cannot be linked in another (see the
# OUT= note below).
#
# Run from Git Bash with:
#   /d/ProgramFiles/msys64/usr/bin/bash.exe -lc 'bash <this script>'
# or on native Linux with:
#   bash <this script>
# Last line of stdout is the absolute path of the produced archive.
#
# The source list and the port_config probes mirror the pinned CMakeLists.txt
# (see also scripts/run_interop.sh, which builds the same set into a throwaway
# run directory).
set -euo pipefail
# /usr/bin and /bin lead so the absolute tool paths below cannot resolve to a
# foreign-namespace gcc; the inherited PATH stays on as a tail because git is
# not part of a default MSYS2 install, and dropping it made this script die at
# `git: command not found` (rc=127) before it could report anything useful.
export PATH=/usr/bin:/bin${PATH:+:$PATH}
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
PIN=7ee830d02b623e8ffe0b95d59a74db1e58da04c5
CC=/usr/bin/gcc
CXX=/usr/bin/g++
AR=/usr/bin/ar
QUIET=${QUIET:-1}
say() { [[ "$QUIET" == 1 ]] || printf '%s\n' "$@" >&2; }

BASE=$REPO/build/interop/official
command -v git >/dev/null 2>&1 || {
  printf 'git not found on PATH - the official checkout must be verified.\n' >&2
  printf 'MSYS2: pacman -S git   Cygwin: setup -P git   Debian: apt-get install git\n' >&2
  exit 2; }
commit=$(git -C "$REPO/leveldb" rev-parse HEAD)
[[ "$commit" == "$PIN" ]] || { printf 'Wrong official commit: %s\n' "$commit" >&2; exit 2; }
# Windows checkouts carry CRLF; normalize only for this read-only check.
# --ignore-submodules=all: the guard means "nobody edited the C++ sources this
# script compiles". third_party/benchmark is a nested submodule pointer, is
# never compiled here, and drifts on any clone where it was not checked out -
# counting it refused every reference build on a fresh Windows checkout.
git -c core.autocrlf=true -C "$REPO/leveldb" diff --quiet HEAD --ignore-submodules=all -- || {
  printf 'Official tracked files modified; refusing reference build\n' >&2; exit 2; }
# The reference build needs a compiler whose target *is* POSIX. Three triples
# qualify: native Linux, standalone Cygwin, and MSYS2's msys layer - the last
# reports x86_64-pc-msys, not the -cygwin suffix this test used to require, so
# every current MSYS2 install was refused.
TRIPLE=$("$CC" -dumpmachine)
case "$TRIPLE" in
  *linux*|*-cygwin|*-msys) ;;
  *) printf 'Expected a POSIX compiler (native Linux gcc, Cygwin gcc, or MSYS2 /usr/bin gcc), got %s\n' "$TRIPLE" >&2; exit 2 ;;
esac
# One output directory per target triple, because a reference archive belongs to
# the runtime it was compiled against: an msys object wants msys-only symbols
# such as __pthread_normal_mutex_initializer_np, and a Cygwin object wants
# Cygwin's. These namespaces are mutually invisible at runtime yet share one
# /g/code/kvdb, so a single shared path let the MSYS2 leg link Cygwin's archive
# and die at ld with an "undefined reference" that read like an engine bug
# (doc/04 N-14).
OUT=$BASE/$TRIPLE
# Checked by path, not by `command -v`: the whole script pins absolute /usr/bin
# tools, and a missing C++ compiler is the normal state after the verification
# packages have been uninstalled (see doc/08 §3).
[[ -x "$CXX" ]] || {
  printf '%s not found - the official reference library is C++ and needs a compiler.\n' "$CXX" >&2
  printf 'This script builds with /usr/bin/g++, so the package has to provide that path.\n' >&2
  printf 'Debian: apt-get install g++   MSYS2 (MSYS shell): pacman -S gcc   Cygwin: setup -P gcc-g++\n' >&2
  exit 2; }

ARCHIVE=$OUT/lib/libleveldb_official.a
STAMP=$OUT/.built-$commit-$TRIPLE-$("$CC" -dumpversion)-$("$CXX" -dumpversion)
# The stamp's *existence* says nothing about the bytes it names: a build
# interrupted after `ar` but before `touch`, or an archive swapped in by hand,
# leaves a stamp vouching for a file it never saw. So the stamp holds the
# archive digest and is only honoured when the digest still matches - and "no
# digest tool available" means "no cache", never "cache valid", the same
# fail-closed rule run_golden.sh applies to its byte comparisons.
digest_of() {
  local line
  if command -v sha256sum >/dev/null 2>&1; then line=$(sha256sum -- "$1")
  elif command -v shasum >/dev/null 2>&1; then line=$(shasum -a 256 -- "$1")
  else return 1; fi
  printf '%s\n' "${line%% *}"
}
if [[ -f "$STAMP" && -f "$ARCHIVE" ]]; then
  want=$(head -n 1 "$STAMP" 2>/dev/null || :)
  have=$(digest_of "$ARCHIVE" || :)
  if [[ -n "$want" && -n "$have" && "$want" == "sha256 $have" ]]; then
    say 'reusing cached official build'
    printf '%s\n' "$ARCHIVE"
    exit 0
  fi
  say 'cache stamp does not match the archive; rebuilding'
  rm -f "$STAMP"
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
rm -f "$ARCHIVE"
"$AR" rcs "$ARCHIVE" "${objects[@]}"
# The stamp records a digest, not a timestamp: `touch` made "a build happened
# here" the evidence, which is precisely the claim being checked.
if sum=$(digest_of "$ARCHIVE"); then
  printf 'sha256 %s\n' "$sum" > "$STAMP"
else
  printf 'unverified\n' > "$STAMP"  # cannot match a real digest, so this never caches
  say 'no sha256sum/shasum - archive built but not cached; the next run rebuilds it'
fi
printf '%s\n' "$ARCHIVE"
