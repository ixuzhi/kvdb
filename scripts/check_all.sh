#!/usr/bin/env bash
# check_all.sh - one-click verification for kvdb. Runs the legs that this
# machine's environments can support, in the right namespace each, logs every
# leg under build/checks/<timestamp>/, and prints one summary table.
#
# It replaces "which of my five toolchains do I run, and in which shell" with
# a printed decision: scripts/kvdb_doctor.sh produces the inventory, this
# script consumes it. Missing environments become SKIP rows with the exact
# install command, never silent omissions.
#
# Usage:
#   bash scripts/check_all.sh                 # full: unit matrix + all legs
#   bash scripts/check_all.sh --quick         # unit(current posix) + golden + interop
#   bash scripts/check_all.sh --legs=unit,golden,sanitize
#   bash scripts/check_all.sh --no-c-test     # skip the official c_test leg
#   bash scripts/check_all.sh --dry-run       # print the plan, run nothing
#   OUT=/tmp/kvdbcheck bash scripts/check_all.sh   # custom evidence dir
#
# Exit code: number of FAILED legs (SKIP does not fail). 0 = everything that
# could run, passed.
set -u
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
TS=$(date +%Y%m%d-%H%M%S)
OUT=${OUT:-$REPO/build/checks/$TS}
LEGS="unit,official,golden,interop,cross-backend,sanitize"
QUICK=0; DRY=0; C_TEST=1
while [ $# -gt 0 ]; do
  case "$1" in
    --legs=*)    LEGS=${1#--legs=} ;;
    --quick)     QUICK=1; LEGS="unit,golden,interop" ;;
    --no-c-test) C_TEST=0 ;;
    --dry-run)   DRY=1 ;;
    -h|--help)   sed -n '2,18p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

mkdir -p "$OUT" || { echo "cannot create $OUT" >&2; exit 2; }
INV="$OUT/doctor.env"
bash "$REPO/scripts/kvdb_doctor.sh" --emit "$INV" || { echo "doctor failed" >&2; exit 2; }

# TMP guard (N-13: a bad TMP used to masquerade as a backend mismatch)
if [ -z "${TMP:-}" ] || ! mkdir -p "$TMP" 2>/dev/null; then
  TMP="$OUT/tmp"; mkdir -p "$TMP" || exit 2
  export TMP
fi

has_leg() { case ",$LEGS," in *",$1,"*) return 0 ;; *) return 1 ;; esac; }
PASS=0; FAILN=0; SKIPN=0
SUMMARY="$OUT/summary.txt"; : > "$SUMMARY"

say() { printf '%s\n' "$*"; }
row() { # leg status detail
  printf '%-14s %-6s %s\n' "$1" "$2" "$3" | tee -a "$SUMMARY"
  case "$2" in
    PASS) PASS=$((PASS+1)) ;;
    FAIL) FAILN=$((FAILN+1)) ;;
    SKIP) SKIPN=$((SKIPN+1)) ;;
  esac
}

# run a leg: name, log-basename, shell-to-use(current|abs-path), command, [msys_layer]
# The MSYSTEM layer is a PER-CALL argument, never a global: a leftover
# MSYSTEM=CLANG64 from an earlier unit leg used to launch the cross-backend
# leg inside the CLANG64 environment, where /clang64/bin/as is clang's
# integrated assembler - the w64devkit gcc then assembled its own .s with
# LLVM's `as`, which rejects gcc's `.ident` directive. Intermittent-looking,
# order-dependent, and entirely self-inflicted.
run_leg() { # name logname mode cmd [msys_layer]
  local name=$1 log=$2 mode=$3 cmd=$4 layer=${5:-} rc
  if [ "$DRY" = 1 ]; then row "$name" PLAN "[$mode${layer:+ MSYSTEM=$layer}] $cmd"; return; fi
  say ""
  say "=== [$name] $cmd"
  if [ "$mode" = current ]; then
    ( eval "$cmd" ) > "$OUT/$log.log" 2>&1
  else
    ( MSYSTEM="$layer" "$mode" -lc "$cmd" ) > "$OUT/$log.log" 2>&1
  fi
  rc=$?
  if [ $rc -eq 0 ]; then
    row "$name" PASS "log: ${log#$OUT/}.log"
  else
    row "$name" FAIL "rc=$rc  log: ${log#$OUT/}.log  (tail -30 ${log#$OUT/}.log)"
    tail -30 "$OUT/$log.log" | sed 's/^/    | /'
  fi
}

# ------------------------------------------------------------------ doctor view
say "=============================================================="
say " kvdb check_all   $TS"
say " legs: $LEGS        out: $OUT"
say "=============================================================="
say ""
awk -F'|' '{printf "  %-14s %-5s %-22s %-6s san=%-4s %s\n",$1,$2,$6,$7,$8,$9}' "$INV"

# find-env helpers (records: name|present|shell|repo|cc|triple|backend|san|tree)
fld() { awk -F'|' -v n="$1" -v k="$2" '$1==n {print $k; found=1} END{if(!found) exit 1}' "$INV"; }
have_env() { [ "$(fld "$1" 2 2>/dev/null)" = yes ]; }
first_backend() { awk -F'|' '$2=="yes" && $7==b {print $1; exit}' b="$1" "$INV"; }
first_san() { awk -F'|' '$2=="yes" && $8=="yes" {print $1; exit}' "$INV"; }

# ------------------------------------------------------------------ leg: unit (matrix)
if has_leg unit; then
  # every available environment gets its own object tree; make clean first so
  # the .target guard can never reject a tree another env left behind.
  while IFS='|' read -r n p sh rp cc tr be san tree; do
    [ "$p" = yes ] || continue
    ccb=$cc
    command -v cygpath >/dev/null 2>&1 && ccb=$(cygpath -u "$cc" 2>/dev/null || echo "$cc")
    if [ "$sh" = current ]; then
      bindir=$(dirname "$ccb")
      run_leg "unit:$n" "unit-$n" current \
        "PATH='$bindir':\$PATH make -C '$REPO' OBJDIR=$tree BINDIR=$tree clean test"
    else
      layer=""
      case "$n" in msys2-mingw64) layer=MINGW64 ;; msys2-clang64) layer=CLANG64 ;; esac
      run_leg "unit:$n" "unit-$n" "$sh" \
        "cd '$rp' && make CC='$ccb' OBJDIR=$tree BINDIR=$tree clean test" "$layer"
    fi
  done < "$INV"
  if ! awk -F'|' '$2=="yes"' "$INV" | grep -q .; then
    row unit SKIP "没有可用编译器 → pacman -S gcc make / apt install build-essential"
  fi
fi

# ---------------------------------------------- leg: official + golden + interop
POSIX_ENV=$(first_backend posix)
POSIX_SHELL=$(fld "$POSIX_ENV" 3 2>/dev/null || echo "")
POSIX_REPO=$(fld "$POSIX_ENV" 4 2>/dev/null || echo "$REPO")
if has_leg official || has_leg golden || has_leg interop; then
  if [ -z "$POSIX_ENV" ]; then
    has_leg official && row official SKIP "需要 POSIX g++（msys/cygwin/linux 之一）"
    has_leg golden  && row golden  SKIP "同上：pacman -S gcc make git"
    has_leg interop && row interop SKIP "同上：pacman -S gcc make git"
  else
    # The golden/interop scripts link build/libleveldb.a from INSIDE the POSIX
    # namespace, and the Makefile's .target guard (correctly) refuses an
    # archive minted by another namespace. So the default tree is (re)built
    # here in the POSIX shell first. Side effect: after a full `make check`
    # the default build/ belongs to the POSIX env; the Windows unit legs use
    # their own dedicated trees and are unaffected.
    if [ "$DRY" = 1 ]; then
      row kvdb-lib PLAN "[$POSIX_SHELL] cd '$POSIX_REPO' && make clean && make -s"
    else
      run_leg kvdb-lib kvdb-lib "$POSIX_SHELL" "cd '$POSIX_REPO' && make clean >/dev/null && make -s"
    fi
    if has_leg official; then
      run_leg official official "$POSIX_SHELL" "cd '$POSIX_REPO' && bash scripts/build_official.sh"
    fi
    if has_leg golden; then
      run_leg golden golden "$POSIX_SHELL" "cd '$POSIX_REPO' && bash scripts/run_golden.sh"
    fi
    if has_leg interop; then
      [ "$C_TEST" = 1 ] && CT=1 || CT=0
      run_leg interop interop "$POSIX_SHELL" \
        "cd '$POSIX_REPO' && RUN_C_TEST=$CT bash scripts/run_interop.sh"
    fi
  fi
fi

# ------------------------------------------------------------------ leg: cross-backend
if has_leg cross-backend; then
  # This leg compares the Windows Env against the POSIX Env, so it must run in
  # a POSIX shell (the script probes /usr/bin/gcc for the posix half) with the
  # Windows compiler passed explicitly as CC_WIN.
  WIN_CC=""
  for c in "$REPO/_tools/w64devkit/w64devkit/bin/gcc.exe" \
           "$(fld msys2-mingw64 5 2>/dev/null || true)"; do
    [ -n "$c" ] && [ -f "$c" ] && { WIN_CC=$c; break; }
  done
  if [ -z "$POSIX_ENV" ] || [ -z "$WIN_CC" ]; then
    row cross-backend SKIP "需要 POSIX 壳（msys/cygwin/linux）+ 一个 Windows 目标编译器（w64devkit 或 mingw64）"
  else
    winc=$(cygpath -u "$WIN_CC" 2>/dev/null || echo "$WIN_CC")
    # The native-Windows half of this leg must not see an MSYS-shaped TMP
    # (N-13 family: the script itself refuses "Do not hand an MSYS-style path
    # to a native-Windows compiler"). Normalize to a Win32 path here; a POSIX
    # login shell would otherwise re-export TMP=/tmp under msys.
    tmpw=$(cygpath -w /tmp 2>/dev/null || echo "${TEMP:-/tmp}")
    run_leg cross-backend cross-backend "$POSIX_SHELL" \
      "cd '$POSIX_REPO' && TMP='$tmpw' TEMP='$tmpw' CC_WIN='$winc' bash scripts/run_cross_backend.sh"
  fi
fi

# ------------------------------------------------------------------ leg: sanitize
if has_leg sanitize; then
  SAN_ENV=$(first_san)
  if [ -z "$SAN_ENV" ]; then
    row sanitize SKIP "没有自带 sanitizer 运行时的编译器 → Windows: pacman -S mingw-w64-x86_64-clang；Linux: gcc/clang 自带"
  else
    SAN_SHELL=$(fld "$SAN_ENV" 3 2>/dev/null || echo "")
    SAN_REPO=$(fld "$SAN_ENV" 4 2>/dev/null || echo "$REPO")
    if [ "$SAN_SHELL" = current ] || [ -z "$SAN_SHELL" ]; then
      run_leg sanitize sanitize current "cd '$REPO' && bash scripts/run_sanitizers.sh"
    else
      layer=""
      case "$SAN_ENV" in msys2-clang64) layer=CLANG64 ;; esac
      run_leg sanitize sanitize "$SAN_SHELL" "cd '$SAN_REPO' && bash scripts/run_sanitizers.sh" "$layer"
    fi
  fi
fi

# ------------------------------------------------------------------ final
say ""
say "=============================================================="
say " 汇总: PASS=$PASS  FAIL=$FAILN  SKIP=$SKIPN    证据: $OUT"
say " 明细: $SUMMARY"
say "=============================================================="
[ "$DRY" = 1 ] && exit 0
exit $FAILN
