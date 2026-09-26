#!/usr/bin/env bash
# audit_all.sh — doc/14 新验证体系的总入口
# N1 接口面机械审计 / N2 选项×行为全矩阵 / N3 同步锁步差分 /
# N4 格式角落矩阵 / N5 交替生命周期+跨引擎修复 / N6 随机崩溃+跨引擎恢复
# 全部驱动只对官方 c.h 编译、链两库（常设 ABI 替换证明）。
# 用法: bash tools/audit/audit_all.sh [quick|full]
#   quick: N3 3000 ops × 3 seeds；full: 20000 ops × 5 seeds + 更多崩溃轮
set -u
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
MODE=${1:-quick}
TS=$(date +%Y%m%d-%H%M%S)
EVID="$REPO/build/audit/$TS"
mkdir -p "$EVID"
export AUDIT_DIR="$EVID"
export PATH=/usr/bin:/bin${PATH:+:$PATH}

printf 'kvdb 新验证体系（doc/14） 证据目录: %s\n' "$EVID"

# 前置：kvdb 归档（POSIX 三元组）与官方归档
TRIPLE=$(gcc -dumpmachine 2>/dev/null || true)
case "$TRIPLE" in
  *linux*|*-cygwin|*-msys) ;;
  *) echo "audit_all: 需要 POSIX 三元组的编译器（msys/cygwin/linux），当前: ${TRIPLE:-无}"; exit 2 ;;
esac
KVDB_LIB=${KVDB_LIB:-$REPO/build/libleveldb.a}
if [[ ! -f "$KVDB_LIB" ]] || [[ -n "$(find "$REPO/src" "$REPO/include" -name '*.[ch]' -newer "$KVDB_LIB" -print 2>/dev/null | head -1)" ]]; then
  echo "audit_all: build/libleveldb.a 缺失或比源码旧 → 先 make"
  (cd "$REPO" && make) > "$EVID/make.log" 2>&1 || { tail -5 "$EVID/make.log"; exit 2; }
fi
OFFICIAL_LIB=$(ls "$REPO"/build/interop/official/$TRIPLE/lib/libleveldb_official.a 2>/dev/null | head -1)
if [[ ! -f "$OFFICIAL_LIB" ]]; then
  echo "audit_all: 官方参考库缺失 → 跑 scripts/build_official.sh"
  (cd "$REPO" && bash scripts/build_official.sh) > "$EVID/build_official.log" 2>&1 \
    || { tail -5 "$EVID/build_official.log"; exit 2; }
  OFFICIAL_LIB=$(ls "$REPO"/build/interop/official/$TRIPLE/lib/libleveldb_official.a 2>/dev/null | head -1)
fi
case "$MODE" in
  full) export LOCKSTEP_OPS=20000; export LOCKSTEP_SEEDS="1 2 3 4 5"; export CRASH_ROUNDS=14 ;;
  *)    export LOCKSTEP_OPS=3000;  export LOCKSTEP_SEEDS="1 2 3";        export CRASH_ROUNDS=8 ;;
esac

summary=(); total_fail=0
run_leg() {
  local id=$1 script=$2
  printf '\n===== %s =====\n' "$id"
  if bash "$script" > "$EVID/$id.log" 2>&1; then
    summary+=("$id PASS")
  else
    rc=$?
    summary+=("$id FAIL(rc=$rc)")
    total_fail=$((total_fail+1))
  fi
  sed 's/^/  /' "$EVID/$id.log" | tail -25
}

run_leg N1 "$REPO/tools/audit/api_surface_audit.sh"
run_leg N2 "$REPO/tools/audit/run_option_matrix.sh"
run_leg N3 "$REPO/tools/audit/run_lockstep.sh"
run_leg N4 "$REPO/tools/audit/run_corner_matrix.sh"
run_leg N5 "$REPO/tools/audit/run_lifecycle.sh"
run_leg N6 "$REPO/tools/audit/run_crash.sh"

printf '\n===== 汇总 =====\n'
for s in "${summary[@]}"; do printf '%s\n' "$s"; done
printf '证据: %s\n' "$EVID"
exit $total_fail
