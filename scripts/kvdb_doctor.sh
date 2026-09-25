#!/usr/bin/env bash
# kvdb_doctor.sh - enumerate the compilation/execution environments on this
# machine, decide which verification legs each one can run, and print exact
# install commands for whatever is missing.
#
# Why this exists: a Windows dev box typically carries several mutually
# invisible POSIX namespaces (MSYS2 msys / MINGW64 / CLANG64, standalone
# Cygwin, Git Bash + w64devkit) plus a native-Linux option. They share the
# disk but not the runtime, and every verification leg has hard requirements
# (golden/interop need a POSIX g++; sanitize needs a compiler that SHIPS a
# sanitizer runtime; cross-backend needs one Windows target AND one POSIX
# target). Guessing wrong produces the exact class of false conclusions
# catalogued as N-7..N-19 in doc/04. This script replaces guessing with a
# printed matrix; scripts/check_all.sh consumes the same inventory.
#
# Usage:
#   bash scripts/kvdb_doctor.sh                 # report (always rc 0)
#   bash scripts/kvdb_doctor.sh --emit FILE    # inventory: name|present|...
#   bash scripts/kvdb_doctor.sh --json          # inventory as JSON lines
set -u
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
EMIT=""; MODE=table
while [ $# -gt 0 ]; do
  case "$1" in
    --emit) EMIT=${2:?--emit needs a path}; shift 2 ;;
    --json) MODE=json; shift ;;
    -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

TMPD=${TMPDIR:-/tmp}; TMPD=$(cd "$TMPD" 2>/dev/null && pwd) || TMPD=$REPO/build
mkdir -p "$TMPD" 2>/dev/null || true
RECS="$TMPD/kvdb_doctor_recs.$$"
: > "$RECS"
cleanup() { rm -f "$RECS"; }
trap cleanup EXIT

WIN_REPO=$(cd "$REPO" && pwd -W 2>/dev/null || echo "$REPO")

# ------------------------------------------------------------------ probes
triple()   { "$1" -dumpmachine 2>/dev/null || echo "-"; }
gpp_ok()   { d=$(dirname "$1"); [ -x "$d/g++.exe" ] || [ -x "$d/g++" ]; }
san_ok() { # real link probe - the only honest test (N-* history)
  d=$(dirname "$1")
  ls "$d"/libclang_rt.asan_dynamic-*.dll "$d"/libasan* >/dev/null 2>&1 && { echo yes; return; }
  src="$TMPD/_san_probe_$$.c"; exe="$TMPD/_san_probe_$$.exe"
  printf 'int main(void){return 0;}' > "$src" 2>/dev/null || { echo no; return; }
  if "$1" -fsanitize=address "$src" -o "$exe" >/dev/null 2>&1; then echo yes; else echo no; fi
  rm -f "$src" "$exe" 2>/dev/null
}
rec() { printf '%s|%s|%s|%s|%s|%s|%s|%s|%s\n' "$@" >> "$RECS"; }
repo_in() { # msys|cygwin -> path as seen from that shell
  if [ "$1" = cygwin ] && command -v cygpath >/dev/null 2>&1; then
    cygpath -u "$WIN_REPO" 2>/dev/null || echo "$REPO"
  else echo "$REPO"; fi
}

MSYS_ROOT=""
for c in "${MSYS2_ROOT:-}" /d/msys64 /c/msys64 /opt/msys64 "$HOME/msys64"; do
  [ -n "$c" ] && [ -x "$c/usr/bin/gcc.exe" ] && { MSYS_ROOT=$c; break; }
done

# 1. w64devkit (bundled, Windows-native -> env_win)
W64="$REPO/_tools/w64devkit/w64devkit/bin/gcc.exe"
[ -x "$W64" ] || W64=""
if [ -n "$W64" ]; then
  rec w64devkit yes current "$REPO" "$W64" "$(triple "$W64")" win "$(san_ok "$W64")" build_mingw
else
  rec w64devkit no "" "" "" "-" win no build_mingw
fi

# 2-4. MSYS2 layers
if [ -n "$MSYS_ROOT" ]; then
  SH2="$MSYS_ROOT/usr/bin/bash.exe"; RP2=$(repo_in msys)
  C="$MSYS_ROOT/usr/bin/gcc.exe"
  rec msys2-msys yes "$SH2" "$RP2" "$C" "$(triple "$C")" posix "$(san_ok "$C")" build_msys
  C="$MSYS_ROOT/mingw64/bin/gcc.exe"
  if [ -x "$C" ]; then rec msys2-mingw64 yes "$SH2" "$RP2" "$C" "$(triple "$C")" win "$(san_ok "$C")" build_mingw64
  else rec msys2-mingw64 no "" "" "" "-" win no build_mingw64; fi
  C="$MSYS_ROOT/clang64/bin/clang.exe"
  if [ -x "$C" ]; then rec msys2-clang64 yes "$SH2" "$RP2" "$C" "$(triple "$C")" win "$(san_ok "$C")" build_clang64
  else rec msys2-clang64 no "" "" "" "-" win no build_clang64; fi
else
  rec msys2-msys    no "" "" "" "-" posix no build_msys
  rec msys2-mingw64 no "" "" "" "-" win   no build_mingw64
  rec msys2-clang64 no "" "" "" "-" win   no build_clang64
fi

# 5. standalone Cygwin (POSIX -> env_posix)
CYG=""
for c in /c/cygwin64/bin/gcc.exe /c/cygwin/bin/gcc.exe /usr/bin/gcc; do
  [ -x "$c" ] && case "$(triple "$c")" in *cygwin*) CYG=$c; break ;; esac
done
if [ -n "$CYG" ]; then
  case "$CYG" in
    /c/cygwin64/bin/gcc.exe) CSH=/c/cygwin64/bin/bash.exe ;;
    /c/cygwin/bin/gcc.exe)   CSH=/c/cygwin/bin/bash.exe ;;
    *)                      CSH=/bin/bash ;;
  esac
  rec cygwin yes "$CSH" "$(repo_in cygwin)" "$CYG" "$(triple "$CYG")" posix "$(san_ok "$CYG")" build_cygwin
else
  rec cygwin no "" "" "" "-" posix no build_cygwin
fi

# 6. native Linux
case "$(uname -s 2>/dev/null)" in
  Linux*)
    if [ -x /usr/bin/gcc ]; then
      rec linux yes /bin/bash "$REPO" /usr/bin/gcc "$(triple /usr/bin/gcc)" posix "$(san_ok /usr/bin/gcc)" build_linux
    else rec linux no "" "" "" "-" posix no build_linux; fi ;;
  *) rec linux no "" "" "" "-" posix no build_linux ;;
esac

# ------------------------------------------------------------------ output
if [ -n "$EMIT" ]; then cp "$RECS" "$EMIT"; exit 0; fi

if [ "$MODE" = json ]; then
  while IFS='|' read -r n p sh rp cc tr be san tree; do
    printf '{"name":"%s","present":%s,"shell":"%s","repo":"%s","cc":"%s","triple":"%s","backend":"%s","sanitizer":"%s","tree":"%s"}\n' \
      "$n" "$([ "$p" = yes ] && echo true || echo false)" "$sh" "$rp" "$cc" "$tr" "$be" "$([ "$san" = yes ] && echo true || echo false)" "$tree"
  done < "$RECS"
  exit 0
fi

legs_for() { # backend san -> leg list
  if [ "$1" = posix ]; then
    printf 'unit, official, golden, interop, cross-backend(posix)'
    [ "$2" = yes ] && printf ', sanitize(+LSan on Linux)'
  else
    printf 'unit, c_test, cross-backend(win)'
    [ "$2" = yes ] && printf ', sanitize'
  fi
}
hint_for() { case "$1" in
  w64devkit)      echo "仓库自带（_tools/，首次构建自动下载）；或从 GitHub Release 手动放置" ;;
  msys2-msys)     echo "pacman -S --needed gcc make git" ;;
  msys2-mingw64)  echo "pacman -S --needed mingw-w64-x86_64-gcc" ;;
  msys2-clang64)  echo "pacman -S --needed mingw-w64-x86_64-clang  ← Windows 上 sanitizer 的唯一来源" ;;
  cygwin)         echo "setup-x86_64.exe -q -P gcc-core,gcc-g++,make,git" ;;
  linux)          echo "apt-get install -y build-essential git cmake" ;;
esac; }

cat <<'HEADER'
================================================================
 kvdb 验证环境矩阵（scripts/kvdb_doctor.sh）
================================================================
HEADER
printf ' %-14s %-4s %-22s %-6s %-4s %-14s %s\n' 环境 存在 三元组 后端 SAN 对象树 "可跑的腿"
printf ' %s\n' "----------------------------------------------------------------------------------------"
while IFS='|' read -r n p sh rp cc tr be san tree; do
  if [ "$p" = yes ]; then
    printf ' %-14s %-4s %-22s %-6s %-4s %-14s %s\n' "$n" yes "$tr" "$be" "$san" "$tree" "$(legs_for "$be" "$san")"
  else
    printf ' %-14s %-4s %-22s %-6s %-4s %-14s %s\n' "$n" NO - "$be" - "$tree" "缺 → $(hint_for "$n")"
  fi
done < "$RECS"
cat <<'RULES'

----------------------------------------------------------------------------------------
“该跑哪一套”的判定规则（腿 → 环境的硬性要求）
----------------------------------------------------------------------------------------
  unit（移植套件）    任意 gcc/clang；每套环境一棵专用对象树
                      （build_mingw / build_msys / build_cygwin / build_linux …）
  official+golden     必须 POSIX g++（官方库不能在 mingw 下构建，N-7）：
  +interop            msys / cygwin / linux 任一即可；run_interop.sh 还需 git；
                      RUN_C_TEST=1 才跑官方 c_test 直连
  cross-backend       需要“一 Windows 目标 + 一 POSIX 目标”各一；脚本在两个
                      命名空间里各建一棵临时树
  sanitize            必须**自带 sanitizer 运行时**的编译器：Windows 上只有
                      CLANG64（gcc 支系实测全部 cannot find -lasan）；Linux/
                      macOS 的 gcc/clang 都行，Linux 还额外获得 LeakSanitizer

----------------------------------------------------------------------------------------
缺环境的安装/配置
----------------------------------------------------------------------------------------
  MSYS2（在“MSYS2 MSYS”终端里）
    pacman -S --needed make gcc git                    # unit/official/golden/interop
    pacman -S --needed mingw-w64-x86_64-gcc             # Windows 目标 unit/c_test
    pacman -S --needed mingw-w64-x86_64-clang           # Windows 唯一 sanitizer 源
  独立 Cygwin
    setup-x86_64.exe -q -P gcc-core,gcc-g++,make,git    # 或官网图形安装
  原生 Linux
    sudo apt-get install -y build-essential git cmake
  w64devkit：仓库自带（_tools/），首构自动下载

----------------------------------------------------------------------------------------
多环境共存时的纪律（doc/09 的血泪教训）
----------------------------------------------------------------------------------------
  · 每套命名空间的 make 必须在**它自己的 shell** 里跑：msys-2.0.dll 与
    cygwin-1.dll 互不可见，跨树混用会得到“像链接错误”的假象；
  · 对象树按环境分开（build_msys/build_cygwin/build_mingw…），Makefile 的
    .target 守卫遇混用直接报错，而不是产出坏东西；
  · 官方库归档按三元组缓存于 build/interop/official/<triple>/，换命名空间
    即换缓存目录（脚本已如此处理）；
  · 一键入口：bash scripts/check_all.sh（自动挑腿、缺环境打印 SKIP 原因
    与安装提示），make doctor 看矩阵，make matrix 跑全部环境的 unit 腿。

  最短 PASS 路径：
    Windows：w64devkit 或 mingw64（unit+c_test） + msys 或 cygwin（golden+interop）
    Linux  ：一套 gcc 即可跑全部（含 sanitizer/LSan）
RULES
exit 0
