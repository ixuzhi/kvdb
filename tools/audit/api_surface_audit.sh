#!/usr/bin/env bash
# N1 接口面机械审计（doc/14 新体系）
# 1) 公共头逐签名清单：归一化后做集合比较（官方头用 LEVELDB_EXPORT 宏、
#    kvdb 用 extern，所以锚定声明本身而不是存储类说明符）；
# 2) 归档符号表双向 diff：官方导出的每个 leveldb_* 符号 kvdb 必须有；
#    kvdb 多出的 leveldb_* 符号=命名空间污染。
# 3) 枚举常量一致；版本函数=1.23。
# 依赖纪律：不使用 diff/cmp（MSYS2 msys 层没有 diffutils）。
# 退出码 = 失败项数。在 POSIX 三元组的 shell（msys/Cygwin/Linux）里跑。
set -u
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
EVID=${AUDIT_DIR:?AUDIT_DIR must be set by audit_all.sh}
CC=${CC:-gcc}
mkdir -p "$EVID"
fails=0
note() { printf 'N1  %s\n' "$*"; }

TRIPLE=$("$CC" -dumpmachine)
case "$TRIPLE" in
  *linux*|*-cygwin|*-msys) ;;
  *) echo "N1: need POSIX compiler, got $TRIPLE"; exit 2 ;;
esac
KVDB_LIB=${KVDB_LIB:-$REPO/build/libleveldb.a}
OFFICIAL_LIB=$(ls "$REPO"/build/interop/official/$TRIPLE/lib/libleveldb_official.a 2>/dev/null | head -1)
[[ -f "$KVDB_LIB" ]] || { echo "N1: missing $KVDB_LIB (make first)"; exit 2; }
[[ -f "$OFFICIAL_LIB" ]] || { echo "N1: missing official archive; run scripts/build_official.sh"; exit 2; }

# ---------- 1) 头文件逐签名清单（集合比较） ----------
extract() {
  sed -e 's/LEVELDB_EXPORT //g' "$1" \
    | awk 'BEGIN{inc=0}
      {
        line = $0; out = "";
        while (1) {
          if (inc) {
            p = index(line, "*/");
            if (p == 0) { line = ""; break; }
            line = substr(line, p + 2); inc = 0;
          } else {
            p = index(line, "/*");
            if (p == 0) break;
            out = out substr(line, 1, p - 1) " ";
            line = substr(line, p + 2);
            q = index(line, "*/");
            if (q == 0) { inc = 1; line = ""; break; }
            line = substr(line, q + 2);
          }
        }
        print out line;
      }' \
    | tr '\n' ' ' \
    | sed 's/;/;\n/g' \
    | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' \
    | tr -s ' ' \
    | grep -E 'leveldb_[a-z_]+\(|^enum \{' \
    | grep -v 'extern "C"' \
    | sed 's/^extern //'
}
extract "$REPO/include/leveldb/c.h" > "$EVID/kvdb_sigs.txt"
extract "$REPO/leveldb/include/leveldb/c.h" > "$EVID/official_sigs.txt"
n_kvdb=$(wc -l < "$EVID/kvdb_sigs.txt")
n_off=$(wc -l < "$EVID/official_sigs.txt")
sigdiff=$(awk 'NR==FNR{a[$0]=1;next} !($0 in a){print "kvdb-only: "$0}' \
    "$EVID/official_sigs.txt" "$EVID/kvdb_sigs.txt"
  awk 'NR==FNR{a[$0]=1;next} !($0 in a){print "official-only: "$0}' \
    "$EVID/kvdb_sigs.txt" "$EVID/official_sigs.txt")
if [[ -z "$sigdiff" ]]; then
  note "PASS signatures: $n_off declarations identical (normalized set)"
else
  printf '%s\n' "$sigdiff" > "$EVID/sig_diff.txt"
  note "FAIL signatures: $n_off official vs $n_kvdb kvdb"
  printf '%s\n' "$sigdiff" | head -10 | sed 's/^/    /'
  fails=$((fails+1))
fi

# ---------- 2) 符号表双向 diff ----------
nm -g --defined-only "$OFFICIAL_LIB" 2>/dev/null | awk '{print $NF}' \
  | grep '^leveldb_' | sed 's/@@.*//' | sort -u > "$EVID/official_syms.txt"
nm -g --defined-only "$KVDB_LIB" 2>/dev/null | awk '{print $NF}' \
  | grep '^leveldb_' | sed 's/@@.*//' | sort -u > "$EVID/kvdb_syms.txt"
n_off=$(wc -l < "$EVID/official_syms.txt")
n_kvdb=$(wc -l < "$EVID/kvdb_syms.txt")
missing=$(comm -23 "$EVID/official_syms.txt" "$EVID/kvdb_syms.txt")
extra=$(comm -13 "$EVID/official_syms.txt" "$EVID/kvdb_syms.txt")
if [[ -z "$missing" && -z "$extra" ]]; then
  note "PASS symbols: $n_off leveldb_* symbols, exact two-way match"
else
  note "FAIL symbols: official=$n_off kvdb=$n_kvdb"
  [[ -n "$missing" ]] && { note "  missing in kvdb:"; printf '%s\n' "$missing" | sed 's/^/    /'; }
  [[ -n "$extra" ]] && { note "  extra in kvdb:"; printf '%s\n' "$extra" | sed 's/^/    /'; }
  fails=$((fails+1))
fi

# ---------- 3) 枚举与版本 ----------
e1=$(grep -m1 'enum {' "$REPO/include/leveldb/c.h" | tr -d ' \r\n')
e2=$(grep -m1 'enum {' "$REPO/leveldb/include/leveldb/c.h" | tr -d ' \r\n')
if [[ "$e1" == "$e2" ]]; then
  note "PASS enum constants identical"
else
  note "FAIL enum constants differ: [$e1] vs [$e2]"
  fails=$((fails+1))
fi
cat > "$EVID/version_probe.c" <<'EOF'
#include <stdio.h>
#include <leveldb/c.h>
int main(void) { printf("%d.%d\n", leveldb_major_version(), leveldb_minor_version()); return 0; }
EOF
if "$CC" -std=c11 -I"$REPO/leveldb/include" "$EVID/version_probe.c" "$KVDB_LIB" \
     -o "$EVID/version_probe.exe" 2>/dev/null; then
  v=$("$EVID/version_probe.exe")
  if [[ "$v" == "1.23" ]]; then note "PASS version = $v"; else note "FAIL version = $v (expected 1.23)"; fails=$((fails+1)); fi
else
  note "FAIL version probe link"; fails=$((fails+1))
fi

exit $fails
