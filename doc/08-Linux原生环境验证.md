# 08 — Linux 原生环境（Debian 12 / glibc）验证记录

本文档记 2026-09-19 在**真正的原生 Linux（glibc）**上把 kvdb 的全部验证腿跑一遍的
过程：环境矩阵、为跑通这些腿新装的软件与卸载方法、四条腿的逐条结果、本轮
发现并修复的缺陷（doc/06 P0-10）、踩到的两个工具链坑、以及"哪些仍然没覆盖"。

背景：在此之前所有实测证据都来自 Windows 系的三条工具链（MSYS2 `env_posix`、
MinGW64 `env_win` 静态、clang64 `env_win` 动态），`doc/06` 的 P2-1 因此一直挂着
"真正的原生 Linux（glibc）/macOS 有待一次实机确认"。而 `scripts/` 下的四条腿脚本
是照 MSYS2 写的，Linux 上会自我拒绝——这轮先把脚本改成跨 POSIX 双通路，再跑。

---

## 1. 结论速览

| 腿 | 命令 | 结果 | 证据 |
|---|---|---|---|
| L0 原生构建 + 全量单测 | `make -j4` → `./build/kvdb_tests` | **127 tests, 0 failed**；构建 0 error、6 warning | `build_linux/logs/L0-build*.log`、`L0-unit-afterfix.log`、`L0-final-build-full.log`+`L0-final-unit.log`（文档定稿前的最后一次 `make clean` 重建） |
| L1 逐字节黄金比对 | `bash scripts/run_golden.sh` | **rc=0**：109 项 PASS + 33 文件逐字节相同，仅 `levels` 布局按 P2-7 允许不同 | `build/interop/golden-20260919-051538/` |
| L2 互操作矩阵 + 官方 c_test | `RUN_C_TEST=1 bash scripts/run_interop.sh` | **rc=0**：65 个阶段全 rc=0；未修改的 `leveldb/db/c_test.c` 直连 kvdb **PASS** | `build/interop/run-17GyoMyu/` |
| L3 ASan + UBSan | `bash scripts/run_sanitizers.sh`（gcc 版） | **rc=0**：127 例 + 官方 c_test 16 阶段 + `golden_driver` 10 负载 create/verify，**零报告** | `build/san-runs/interop-20260919-051449/`，脚本按 §6 重构后又跑一遍：`interop-20260919-054158/` |
| 本轮修复 | `src/version_set.c:1446` | P0-10：`get_range2` 把 NULL 传给 `memcpy`，UBSan 判 UB | 见 §5 |
| snappy 探路（P2-8） | /tmp 内一次性实验，未入库 | 压缩块**不逐字节相同**，但**双向可解**、摘要一致 | 见 §7 |

一句话：**格式兼容、互操作、内存安全三条主张在原生 glibc 上全部复现成立**，
且这轮新增的编译器（gcc 12 + glibc 头文件）确实比 clang64 多看见一处真实 UB。
本机（Windows + MSYS2 三条通路）的回归随后也全部实跑通过，见 §8。

---

## 2. 环境与工具链

| 项 | 值 |
|---|---|
| 发行版 | Debian GNU/Linux 12 (bookworm)，kernel 6.1.0-50-amd64 |
| C 编译器 | `gcc (Debian) 12.2.0-14+deb12u1`，`-dumpmachine` = `x86_64-linux-gnu` |
| C++ 编译器 | `g++ (Debian) 12.2.0`（本轮新装，见 §3） |
| libc | glibc 2.36（`pthread` 已并入 `libc`，`-lpthread` 仍由 Makefile 提供） |
| Env 后端 | `src/env_posix.c` + pthread；实证：`env_win.o` 的 `.text` 为 **0 字节**（空 TU），`env_posix.o` 有 4717 字节代码 |
| 官方参考库 | `leveldb` 子模块固定提交 `7ee830d02b623e8ffe0b95d59a74db1e58da04c5`（`1.23-91-g7ee830d`） |

参考库的 port 配置探针在两条腿上的实测值（右侧一列来自本机 MSYS2 腿，
`build/interop/official/<triple>/include/port/port_config.h` 与 `run_interop.sh` 打出的
`Config:` 行都可核对；2026-09-20 起参考库按三元组分目录，本轮当时是共享的
`official/include/…`，见 doc/04 N-14）：

| 宏 | Linux(glibc) | MSYS2(cygwin) | 含义 |
|---|---|---|---|
| `HAVE_FDATASYNC` | **1** | **1** | 两侧都走 `fdatasync`——Cygwin 也提供它，这条**不是**差异 |
| `HAVE_O_CLOEXEC` | 1 | 1 | 同 |
| `HAVE_FULLFSYNC` | 0 | 0 | 同（macOS 才为 1） |
| `HAVE_CRC32C` / `HAVE_SNAPPY` / `HAVE_ZSTD` | 0 | 0 | 刻意关，保持两引擎同处"纯软件 CRC + 不压缩"基线 |

上表没有一项在两腿之间取值不同，所以参考库这一侧不存在"环境差异需要解释"。
即便 `fdatasync`/`fsync` 真的不同，影响的也只是落盘时机、不写进任何磁盘字节
——L1 仍逐字节相同即为证。

---

## 3. 为验证安装的软件（以及怎么卸干净）

装之前先留基线，卸之后可核对：

```bash
dpkg -l > /tmp/dpkg.before; apt-mark showmanual | sort > /tmp/manual.before
```

`dpkg -l` 前后差集实证，本轮**净增 7 个包**（`--no-install-recommends`，无 recommends 连带）：

| 包 | 版本 | 为什么必须装 |
|---|---|---|
| `g++` | 4:12.2.0-3 | 官方 leveldb 是 C++；L1/L2 的参考库与 `official.exe` 链接都要它 |
| `g++-12` | 12.2.0-14+deb12u1 | `g++` 的实际编译器 |
| `libstdc++-12-dev` | 12.2.0-14+deb12u1 | C++ 标准库头文件与静态库 |
| `lcov` | 1.16-1 | 覆盖率腿（L5）的工具，**本轮按决定未跑**，装而不用 |
| `libjson-perl` | 4.10000-1 | `lcov` 的依赖 |
| `libperlio-gzip-perl` | 0.20-1+b1 | `lcov --html` 生成 `genhtml` 输出的依赖 |
| `libsnappy-dev:amd64` | 1.1.9-3 | 打开参考库的 `HAVE_SNAPPY=1` 以探 P2-8 的压缩字节等价（§7）；运行库 `libsnappy1v5` 系统里本就有 |

**未装**（以及为什么）：`clang`/`llvm`/`libc++-dev`——Debian 的 gcc 自带
`libasan`/`libubsan`/`libtsan` 三套运行时（`/usr/lib/gcc/x86_64-linux-gnu/12/` 下可见），
Windows 腿"只能 clang"的理由在这里不成立；`cmake`——脚本手工编译官方源文件清单，不走
CMake；`valgrind`——与 ASan 高度重叠且慢；`pkg-config`、`strace`——用不上。

### 卸载

**已执行**（2026-09-19，本轮验证与文档定稿之后按用户指示当场卸掉）。先干跑核对，
结果正好是这 7 个、`0 newly installed, 7 to remove`，没有任何既有包被牵连：

```bash
apt-get -s purge g++ g++-12 libstdc++-12-dev lcov libjson-perl libperlio-gzip-perl libsnappy-dev
apt-get purge -y g++ g++-12 libstdc++-12-dev lcov libjson-perl libperlio-gzip-perl libsnappy-dev
dpkg -l > /tmp/dpkg.after; apt-mark showmanual | sort > /tmp/manual.after
diff /tmp/dpkg.before /tmp/dpkg.after      # 空
diff /tmp/manual.before /tmp/manual.after  # 空
```

两条 `diff` 都为空：包集合与"手工标记为直接安装"的集合都逐字节回到了装之前的基线。

> ⚠ 别在这台机器上顺手 `apt-get autoremove`：干跑显示它会带走
> `slirp4netns` / `libslirp0`——那两个是**本机既有的孤儿包，与本次验证无关**。

`apt-get autoclean` 的作用与预期不同，值得记一笔：它按"版本是否已过期"清缓存，
所以**这 7 个包的 `.deb` 全都留在 `/var/cache/apt/archives` 里**（版本仍是当前版），
反倒删掉了 `sudo`/`libcurl4`/`gpg-wks-server` 三个与本轮无关的过期缓存。
留下的缓存是有用的：重装可以完全离线，但两个开关要一起给——`lcov` 的 recommends
（`libgd-perl` 等）从来没装过也不在缓存里，单给 `--no-download` 会报
`Unable to fetch some archives`；`apt-get -s install --no-install-recommends --no-download
g++ libsnappy-dev lcov` 干跑实测 7 个包全部 `Conf` 命中缓存（命令见 §9 开头）。

### 卸完之后哪些腿还能跑

| 腿 | 卸后状态 | 实测 |
|---|---|---|
| L0 构建 + 单测 | **照跑**（只需 gcc + libc6-dev，都在系统里） | `make clean && make -j4` → 127 tests, 0 failed，6 条告警不变（`build_linux/logs/L0-postpurge-*.log`） |
| L3 ASan + UBSan | **照跑**（gcc 的 sanitizer 运行时随 gcc 走；`c_test.c` 是 C） | 卸后重跑仍 rc=0 零报告：`build/san-runs/interop-20260919-060143/` |
| L1 黄金比对 | **不可跑**：每次都要现编官方 C++ 参考库 | `exit 2`，`/usr/bin/g++ not found … apt-get install g++`（`build/interop/golden-20260919-060005/run.log`） |
| L2 互操作 + 官方 c_test | **不可跑**：同上 | `exit 2`，同一条提示（`build/interop/run-FlXU6kMd/run.log`） |

原来这两条腿在 `g++` 缺失时的表现是裸报错（`line 34: /usr/bin/g++: No such file or
directory`，L2 更是直接 `rc=127`），容易被误读成引擎问题。现在
`build_official.sh` 与 `run_interop.sh` 各自在动工前 `[[ -x "$CXX" ]]` 显式检查并给出
安装命令，两条都以 rc=2 干净退出。`build/interop/official/` 里的缓存归档保持原样
（卸了 `g++` 它也不可用，因为官方侧驱动仍要用 `g++` 链接）。

---

## 4. 四条腿的逐条结果

### L0 原生构建 + 全量单测

```bash
make -j4                                   # → build/libleveldb.a, build/kvdb_tests
TMPDIR=/root/work/kvdb/build_linux/tmp ./build/kvdb_tests   # 127 tests, 0 failed
```

gcc 12 在 `-Wall -Wextra` 下的 6 条告警（Linux 这轮的构建日志首次逐条记下；
均为既有代码质量问题，不是 Linux 行为差异，未在本轮修改）：

| 位置 | 告警 |
|---|---|
| `src/table.c:420` | `assignment discards ‘const’ qualifier`（丢弃 const 限定符，值得改） |
| `src/version_set.c:1562` | `unused variable ‘inputs0_size’` |
| `src/env_mem.c:66` | `‘fs_dir_exists’ defined but not used` |
| `src/c_api.c:62` | `‘unwrap_comparator’ defined but not used` |
| `tests/test_cache.c:13` | `‘cache_key’ defined but not used` |
| `tests/test_db.c:125` | `‘dbt_contains’ defined but not used` |

这 6 处**不是 Linux 独有**：Windows 侧随后复跑，MSYS gcc 15.2 报出完全相同的
6 个位置，MINGW64 gcc 16.1 与 clang64 clang 22.1 在这 6 条之外多两条 `set but not used`
（`src/db.c:447` 的 `compactions`、`src/version_set.c:1190` 的 `read_records`）。
八条逐条对过官方源，**没有一条指向行为差异**，所以 T7 是纯清理，且有一条要留神：

| 告警 | 官方对应 | 清理方式 |
|---|---|---|
| `version_set.c:1562 inputs0_size` | `db/version_set.cc:1406` 同名变量只喂给 1422 行的 `fprintf` 调试行 | 删；分支条件实际用的是 `inputs1_size + expanded0_size`，两边一致 |
| `db.c:447 compactions` | 官方 `db_impl.cc:472` **读它**：`reuse_logs && last_log && compactions == 0` | 别顺手删——它在 kvdb 无人读只因为 `reuse_logs` 未实现（doc/06 P1-1，`src/db.c:499` 已注明恒按 false），实现 P1-1 时正好要用 |
| `version_set.c:1190 read_records` | 官方 `version_set.cc:988` 只把它打进调试行 | 删 |
| `table.c:420` const 丢弃 | 官方把 filter 块**复制**进 `new char[]` | 改 `t->filter_data = block.alloc;`——`block.data.data` 本就是同一个 `buf` 的 const 视图，换过去既消警告又与下一行 `block.alloc = NULL` 的"转移所有权"意图对齐 |
| `env_mem.c:66`、`c_api.c:62`、`test_cache.c:13`、`test_db.c:125` | 官方无对应物，是移植/测试辅助留下的死 static | 删（或标 `__attribute__((unused))`，但那是掩盖） |

另：跑完留下 6 个 `$TMPDIR/leveldbtest-<pid>` 目录。这是**每进程一个**的 scratch 命名
（`posix_get_test_directory` 用 `TMPDIR`+pid，`env_win.c` 同构），不是逐用例泄漏；
`ldb_destroy_db` 只删它认识的 DB 文件，故目录壳会残留。

### L1 逐字节黄金比对（10 负载）

rc=0，`results.tsv` 计 109 PASS + 33 SAME（`sst`/`sst-bloom`/`sst-restart1`/
`sst-bigblock`/`wal`/`wal-big`/`wal-frag`/`edge`/`tomb` 九种严格负载的全部文件与官方
逐字节相同），唯一非绿行是 `levels	bytes	DIFF`——P2-7 记录在案的压缩调度差异，
脚本按 exploratory 容忍，且它的 4 路交叉读摘要仍要求一致（实测一致）。

参与比对的两侧归档指纹（复现时应能对上）：
`kvdb library: build/libleveldb.a (b84843367d1d4c3b)`、
`official library: …/libleveldb_official.a (db9be72d24574caa)`。

### L2 互操作矩阵 + 官方 c_test

`basic`/`edge` × `wal`/`sst` × 4 个方向（official→official、kvdb→kvdb、official→kvdb、
kvdb→official）× create/read/update/final = 64 个阶段，加 `official-c-test` 一行，
**65 行全 rc=0**。物理前提校验符合预期：`wal` 侧 create 后 `tables=0 nonempty_WALs=1`，
update 后 `tables=1`；`sst` 侧 create 即 `tables=1`。
未经修改的官方 `leveldb/db/c_test.c` 直接链 kvdb 跑通（rc=0）。

### L3 ASan + UBSan（gcc）

```bash
bash scripts/run_sanitizers.sh      # 现在自动选 gcc：Linux 上 gcc 自带运行时
```

`sanitized build PASS → unit suite PASS(127/0) → unit diagnostics PASS →
official c_test PASS(16 phases) → golden driver 10/10 verify PASS →
golden diagnostics PASS`，全链零 `ERROR: AddressSanitizer` / `runtime error`。
`-fno-sanitize-recover=all` 下任何一条报告都会当场终止进程，所以"退出码 0"
就等于"零报告"，不是"报告被忽略"。

日志里的 leak 行变了：Windows 腿打印的是 `detect_leaks is not supported on this
platform`，Linux 腿打印 `SUPPORTED here (SAN_DETECT_LEAKS=1 to enable)`——补齐泄漏维度
的开关已经就位（本轮按决定未拉响，见 §9）。

---

## 5. 本轮发现的缺陷：P0-10 `get_range2` 把 NULL 传进 `memcpy`

**现象**：L3 首跑，`unit suite` 与 `official c_test` 双双 rc=1，报告唯一且可复现：

```
src/version_set.c:1448:3: runtime error: null pointer passed as argument 2,
which is declared to never be null
    #0 get_range2           src/version_set.c:1448
    #1 vs_setup_other_inputs src/version_set.c:1542
    #2 ldb_version_set_pick_compaction … #3 background_compaction src/db.c:675
```

（`unit.log` 里 8 次命中全在这一行；`c_test` 停在 `Test repair` 阶段。）

**根因**：`get_range2` 用两次 `memcpy` 把两个输入数组拼成一个。`SetupOtherInputs` 在
"该层没有其它文件"时传的是 `NULL + 0`。传 NULL 给 `memcpy` 即使 `n==0` 也是 C 标准
意义的 UB，而 glibc 把 `memcpy` 的形参声明成 `nonnull`，UBSan 据此判罚；被内联展开的
`memcpy` 同样可以假定实参非空并据此优化。官方 C++ 版这里是
`std::vector<FileMetaData*> all = inputs1; all.insert(...)`，从不交出空基址——
**这条 UB 是 C 重写时引入的，不是从 leveldb 继承的。**

**修复**（`src/version_set.c:1442`）：两次拷贝各加 `n1 > 0` / `n2 > 0` 守卫，
`n==0` 时连目标侧 `all`（`malloc(0)` 可能返回 NULL）也不再被写。行为字节中性：
修复后 L1 重跑仍 109 PASS + 33 SAME，L0 仍 127/0。

**为什么 clang64 腿没报**：Windows 上同一份代码走的是 `env_win` + MSYS2 clang 的
libc++/msvcrt 声明，`memcpy` 没带 `nonnull` 属性，UBSan 的
`nonull-argument` 检查因此无从触发。这是"换编译器才看得见"的一类，也正是
doc/06 P2-1 想要原生 Linux 的核心理由。

---

## 6. 两个工具链坑（已进脚本，别再踩）

1. **`make` 命令行覆盖 `CFLAGS=` 会连 `+=` 一起吞掉。**
   `run_sanitizers.sh` 原来传 `make CFLAGS="$CFLAGS_SAN" LDFLAGS=…`，在 MSYS2/clang
   路径上无害（Windows 分支本就不追加东西）；在 Linux 上 POSIX 分支的
   `CFLAGS += -D_GNU_SOURCE`、`LDFLAGS += -lpthread` 会被命令行变量**覆盖掉**，
   `env_posix.c` 直接编不过。`make -n` 展开实证：编译行里既没有 `-D_GNU_SOURCE`
   也没有 `-lpthread`。
   改法：Makefile 新增 `SANFLAGS`，在平台分支**之后** `CFLAGS += $(SANFLAGS)` /
   `LDFLAGS += $(SANFLAGS)`（这样调用方的 `-O1` 覆盖默认 `-O2`，又保留分支自己的
   追加），脚本改为只传 `SANFLAGS`。

2. **跨引擎腿会静默链接陈旧归档。**
   本轮第一次跑 L1 没传 `KVDB_LIB`，用的是 `build/` 里 09‑13 的旧 `libleveldb.a`，
   结果在 `sst-bloom`/`sst-bigblock` 上 SIGSEGV、`edge` 上 `block.c:184` 断言、
   `tomb` 读不到点查——**看起来像 HEAD 的四个 Linux 专属缺陷，实际全是旧库的问题**。
   用当前源码重建后同一条腿 rc=0。
   改法：`run_golden.sh` 与 `run_interop.sh` 现在都要 `src/`、`include/` 里没有比
   归档更新的 `.c/.h`，否则 `exit 2` 并提示先 `make`。负向对照已做：把一个与源码
   同龄的归档 `touch` 到过去时间后两条腿都拒绝运行（rc=2），不是摆设。

   > **这条守卫在 2026-09-20 被证明只有一维，而那一维不够。** mtime 说的是
   > "归档比源码新"，没说"归档是谁建的"。Windows 一台机器上并存的 msys 与
   > Cygwin 共享同一棵 `build/`，运行时却互不可见，于是参考库缓存串了档
   > （N-14）、`make` 对着上一个命名空间的对象树回了一句 `Nothing to be
   > done`（N-15）。两处现在都按三元组记账：参考库分目录 + 摘要认档，
   > 对象树 `$(OBJDIR)/.target`。编号与全过程见 doc/04 N-14/N-15、doc/09 §3。

### 脚本侧跨平台改造清单（Linux 与 MSYS2 双通路）

| 文件 | 改动 |
|---|---|
| `scripts/build_official.sh` | 断言从"三元组必须是 `x86_64-pc-cygwin`"放宽为"`*-cygwin` 或 `*linux*`"；用法注释加 Linux 一行 |
| `scripts/run_interop.sh` | 同上放宽；`KVDB` 改为可用 `KVDB_LIB=` 覆盖；新增归档时效守卫 |
| `scripts/run_golden.sh` | 新增归档时效守卫；用法注释加 Linux 一行 |
| `scripts/run_sanitizers.sh` | `CC` 缺省按宿主选（Windows 目标→clang，其余→gcc）；`-D_GNU_SOURCE` 只在 `*linux*` 下加到脚本自己编的 `c_test.o`/`golden_driver.o`；`TMP`/`cygpath` 兜底块只对 Windows/cygwin/mingw 目标生效（POSIX 上原来会退化成 `TMP=.`，把留档写进仓库）；leak 行按平台分别陈述，Linux 侧支持 `SAN_DETECT_LEAKS=1`；make 走 `SANFLAGS`。定稿前又收了一次口：三元组只求值一次（`TRIPLE`/`ON_WINDOWS` 两个变量），"编译器不在 PATH"的提示按选中的 `CC` 分支给（原来在 Linux 上 gcc 缺失时会反建议"用 gcc"）。这次改动之后 L3 重跑过一遍，仍 rc=0 零报告（§1 表第二个证据目录） |
| `Makefile` | 新增 `SANFLAGS` 通道（见上） |
| （2026-09-20）上述四条 | POSIX 判据再补 `*-msys`；`export PATH=/usr/bin:/bin` 改为保留继承尾部并加 `command -v git` 前置检查；`git diff` 守卫加 `--ignore-submodules=all`；缺 `g++` 的提示改指会产出 `/usr/bin/g++` 的包。**2026-09-20 的第二次放宽**，动机是换了一台 Windows 机器后三条腿起不来，逐条负向对照见 doc/09 §3 |
| （2026-09-20）`scripts/run_cross_backend.sh` | 新脚本：同一台机器上用 POSIX 三元组与 Windows 三元组各建一份引擎，比落盘字节与四方向摘要，并用 8 并发进程探跨进程锁。不依赖官方参考库 |

---

## 7. snappy 压缩等价性（P2-8 探路结论，未入库）

`libsnappy-dev` 装上后，参考库第一次真的能开 `HAVE_SNAPPY=1`。在 `/tmp` 里做一次性
实验（不入库）：同一份 `golden_driver.c`，把 `compression` 从环境变量切换，官方侧
`-lsnappy`、kvdb 侧用自带 `src/snappy.c`，`sst` 负载各写一遍再对比。

| 文件 | official | kvdb | 判定 |
|---|---|---|---|
| `000005.ldb` | 21,454 B | 22,179 B | **DIFF**（kvdb 大 725 B ≈ +3.4%） |
| `MANIFEST-000002` | 124 B | 124 B | **DIFF**（同长度不同字节：VersionEdit 记的 `file_size` 变了，是上游差异的连带结果，非独立差异） |
| `000004.log` / `CURRENT` | — | — | SAME |
| 交叉读 4 路摘要 | 一致 | 一致 | **PASS**：官方能解 kvdb 的压缩块，kvdb 也能解官方的 |

结论：P2-8 从"从未比对"精确化为——**压缩器不位相同，但块级互操作成立**。这与
`src/snappy.c` 头部自述（"minimal, spec-compliant… 官方解码器接受、也能解官方产物"）
一致，不存在虚假声明；kvdb 的编码器是简化版，产出合法但更长的流。
影响面是压缩率与由此产生的字节级差异，不是"对方读不了"。

**尚未闭环**：这个模式没进 `run_golden.sh` 的正式模式集（要同时决定参考库是否默认开
`HAVE_SNAPPY`、严格模式是否允许 `.ldb` 字节不同而降级为"摘要一致"），故 P2-8 仍开着。

---

## 8. 与 Windows 通路的差异，以及 Windows 侧回归结果

> 本节先前写的是"Windows 通路只做了静态推理，未实机复跑（那台机器上没有 MSYS2）"。
> 回归已在本机跑完：Windows 10 19044 + MSYS2 三条工具链，下表五个风险点逐条换成实测
> 结论。跑的过程中另外暴露出三处脚本缺陷，已在 §8.3 修掉；两个新的 harness 坑记在 §8.4。

### 8.1 Windows 侧环境

| 工具链 | 编译器 | 三元组 | Env 后端 | 本轮用到的命令 |
|---|---|---|---|---|
| MSYS2 MSYS | gcc 15.2.0 | `x86_64-pc-cygwin` | `env_posix` + pthread | `make OBJDIR=build_l0w BINDIR=build_l0w`、`run_golden.sh`、`RUN_C_TEST=1 run_interop.sh` |
| MSYS2 MINGW64 | gcc 16.1.0 | `x86_64-w64-mingw32` | `env_win`（`-static`） | `make OBJDIR=build_mingw BINDIR=build_mingw` |
| MSYS2 CLANG64 | clang 22.1.8 | `x86_64-w64-windows-gnu` | `env_win` + ASan/UBSan | `MSYSTEM=CLANG64 bash scripts/run_sanitizers.sh` |

三条通路都是 Git Bash 起 MSYS2 shell、手工导出 `PATH` 的取法（`MSYSTEM=MSYS` 单独
前缀不会让 `clang` 出现在普通 Git Bash 里，脚本里那条提示就是为此写的）。

> **2026-09-20 更正（保留上表原文不改，因为它是那台机器的真实读数）**：
> 本轮换了一台 Windows 机器，同一层的 `/usr/bin/gcc -dumpmachine` 报的是
> `x86_64-pc-msys`（gcc 13.3.0 / msys 运行时 3.5.7），不是上表的
> `x86_64-pc-cygwin`（gcc 15.2.0）。也就是说 msys 层这个三元组**在 MSYS2 的
> 两个世代之间确实变过**（更早的 msys gcc 沿用 Cygwin 的 `--host`，后来改成
> 自报 `-msys`），所以判据两种拼法都必须收——只写一种的后果已经在另一台机器
> 上兑现过一次：`build_official.sh` 直接拒绝构建参考库（doc/04 N-7、doc/09 §3）。
> 同一轮里独立 Cygwin 首次装起来，它一直报 `x86_64-pc-cygwin`（gcc 14.4.0）。
> 另：本机 MSYS2 **没有装** mingw64/ucrt64 的 gcc（`/d/msys64/mingw64/bin/gcc.exe`
> 不存在），MinGW-w64 那一支由 w64devkit gcc 16.2.0 代表。

### 8.2 五个风险点的实测结论

| 改动 | 对 Windows 通路的影响 | 实测 |
|---|---|---|
| `Makefile` `SANFLAGS` 通道 | clang64 路径不再有"命令行覆盖 CFLAGS"这回事，改为基线标志 + 追加：`build.log` 里确实出现重复的 `-std=c11`/`-O1`/`-Wall`（后者生效），`LDFLAGS` 也带上编译期标志。`-static` 仍只给 mingw 分支 | **rc=0**：`MSYSTEM=CLANG64 bash scripts/run_sanitizers.sh` → 127 例 0 失败、官方 c_test `PASS (16 phases)`、`golden_driver` 10 负载 create/verify 全绿、零 ASan/UBSan 报告。留档 `build/san-runs/interop-20260919-183717/`，其后的 `-184815/`、`-185101/` 两次复跑同结果。ASan 的 DLL 解析正常（能跑起来并打印报告即为证） |
| `build_official.sh`/`run_interop.sh` 断言放宽 | `x86_64-pc-cygwin` 仍被接受 | **rc=0**：`RUN_C_TEST=1 bash scripts/run_interop.sh`（MSYS）65 个阶段全部 `rc=0`，与 Linux 侧同数；参考库照建（`Config: FDATASYNC=1 FULLFSYNC=0 O_CLOEXEC=1 CRC32C=0 SNAPPY=0 ZSTD=0; Bloom disabled`），未改动的 `c_test.c` 直连 kvdb `PASS`。留档 `build/interop/run-BP15JkRf/` |
| 归档时效守卫 | MSYS2 的 `find -newer` 与 mtime 精度、`core.autocrlf` checkout 时间可能误报"比源码旧" | 四条通路连跑**没有一次误报**。反向对照做实了：复制 `build/libleveldb.a` 并 `touch` 到 12:00，用 `KVDB_LIB=` 指过去，`run_golden.sh` 与 `run_interop.sh` 都打印 `... is older than the sources in src/; run make first` 并 `rc=2`。守卫会拦，不是摆设 |
| `run_sanitizers.sh` 的 `TMP` 守卫 | 条件从"TMP 未设"变为"Windows 目标 **且** TMP 未设"；三元组一次求值存进 `TRIPLE`/`ON_WINDOWS` | 行为与改前等价，且**不需要**在脚本外预设 `TMP`：clang64 那轮打印的是 `TMP was unset; pointing the native backend at D:\ProgramFiles\msys64\tmp`，随后 127 例全过 |
| `src/version_set.c` P0-10 修复 | 纯 C 逻辑，平台中性 | **字节中性在 Windows 侧同样成立**：P0-10 之前那轮 golden（`golden-20260919-161058`）与之后这轮（`golden-20260919-184220`，`rc=0`）的 `results.tsv` **逐行相同**——109 PASS / 33 SAME / 1 DIFF(`levels`) / 1 INFO，摘要与逐项判定无一变动。两跑之间唯一变化的磁盘产物是**官方引擎自己写的** `levels-official/MANIFEST-000002`（同为 346 字节，差在第 139 字节 `kLastSequence` 483→485，以及该记录的 CRC）；名字以 `-kvdb` 结尾的 30 棵目录（kvdb 是最后的写入方，`levels-kvdb` 也在其列）逐字节未变。这正是 `golden_driver.c` 开头声明的"`levels` 是故意留出的探索性模式（走自动后台 compaction），字节布局允许不同"，不是回归 |

单测与告警也各跑了一遍：MSYS `build_l0w/kvdb_tests.exe` 与 MINGW64
`build_mingw/kvdb_tests.exe` 都是 **127 tests, 0 failed**（`TMP` 由 Git Bash 侧
`cygpath -w /tmp` 给定）；两份构建日志留在 `build/msys-l0-build.log`、
`build/mingw64-l0-build.log`（`build/` 已被忽略，不入库）。告警位置见 §4 的新表——MSYS
gcc 15.2 与 Linux gcc 12 报出**完全相同的 6 处**，MINGW64 gcc 16.1 与 clang64 各多两处
`set but not used`。T7 因此在四条通路上是同一件事，与平台无关。

### 8.3 本轮实测暴露的三处脚本缺陷（已修）

1. **`run_sanitizers.sh` 的 `CC` 默认值选错方向**。原逻辑是"三元组含
   `cygwin|mingw|windows` 才用 clang，否则 gcc"，求值用的是 `/usr/bin/gcc -dumpmachine`。
   在 Git Bash 里 `/usr/bin` 没有 gcc，命令替换拿到空串 → 落到 `*)` 分支选了 gcc；
   而 MINGW64 的 `/usr/bin/gcc` 是 **cygwin** gcc，三元组 `x86_64-pc-cygwin` 同样不含
   `mingw`。也就是说默认路径会挑一个根本没有 sanitizer runtime 的编译器。现改为
   **只有 `*linux*` 才默认 gcc**，其余一律 clang；并加一道早退守卫：Windows 系三元组
   + 非 clang 的 `CC` 直接 `exit 2` 给出处方（放在 `rm -rf "$OBJDIR"` **之前**，
   所以误跑不会毁掉上一次的归档——已验证 `build/san/libleveldb.a` 时间戳未动）。
2. **`find -print -quit`**（`run_golden.sh`、`run_interop.sh` 的归档时效守卫）。GNU find
   有 `-quit`，BSD/macOS find 没有；那里它会报错退出，配合 `2>/dev/null` 与 `-n ""`
   判断，守卫在"仍需首次证明"的 macOS 上恰好**静默失效**。改成 `-print | head -1`，
   两派都支持，代价只是多扫几个文件。
3. **`run_golden.sh` 的字节比较在"没有 `sha256sum` 的宿主"上是 fail-open 的**——与上面
   第 2 条同一类（错得很安静，而且正好错在唯一还没测的平台上）。MSYS2 这一带有
   `sha256sum` 却没有 `cmp`/`diff`，所以逐字节判据其实写在摘要上：
   `ha=$(sha256sum < A); hb=$(sha256sum < B); [[ $ha != $hb ]]`——
   命令不存在时两边都取到空串，`"" == ""` → 每个文件都记 `SAME`、`RESULT rc=0`，
   一次什么都没比的运行看起来像"格式完全兼容"。现在脚本开头就选定摘要命令
   （`sha256sum`，没有则 BSD 的 `shasum -a 256`）并**要求它真吐出 64 位十六进制**，
   否则 `exit 2`；顺带把只出现在提示文本里的 `stat -c %s`（GNU 写法）换成
   POSIX 的 `wc -c <`。两个负向对照都做实了：把 `sha256sum` 换成吐乱码的替身 →
   `the digest command produced no 64-hex digest; refusing to compare`、`rc=2`；
   把两个 `command -v` 都指向不存在的工具 → `needs sha256sum (or shasum -a 256)`、`rc=2`。
   改完复跑整条 L1：`rc=0`、109 PASS / 33 SAME / 1 DIFF / 1 INFO，`results.tsv` 与改前
   **逐行相同**（`golden-20260919-191526/`）。用函数而非命令数组来选工具，是因为
   macOS 自带的 bash 3.2 在 `set -u` 下把空数组当成未绑定变量。

### 8.4 两个新的 harness 坑（补 §6）

- **不要在脚本运行时编辑它**。bash 按字节偏移增量读取脚本，边跑边改会让它从错位的
  中间处继续解析，报出根本不存在的语法错误。本轮 `run_interop.sh` 就在跑到一半时被编辑，
  留下 `line 107: syntax error near unexpected token 'then'`、`rc=2`（`build/interop-rerun.log`）；
  同一份工作副本 `bash -n` 干净、原样复跑 `rc=0`。**看日志判缺陷前，先确认那次跑没被自己的编辑打断。**
- **外层重定向会被脚本自身的 `exec > >(tee …)` 抢走**。`bash scripts/x.sh > out.log`
  可能只拿到前半段，尾部（含 `RESULT rc=`）进了 process substitution 那份，读成"失败"。
  权威副本永远是 `$RUN/run.log`。

小结：doc/06 P2-1 的"原生 Linux"这一半已闭合，Windows 三条通路在 P0-10 之后仍全绿。
剩下 macOS（T6）与 §9 的 T1–T8。

---

## 9. 仍**未**覆盖（TODO，按决定本轮延后）

已确认延后的四项 Linux 独有增量是 T1–T4；T5–T8 是随之记下的既有遗留。
**注意 T1–T5 里凡是提到"包已就位"的，本轮结束时都已卸掉**（§3）。重装要两个
开关一起给，否则 apt 会去拉 `lcov` 的 recommends（`libgd-perl` 等，缓存里没有）而报
`Unable to fetch some archives`：

```bash
apt-get install --no-install-recommends --no-download -y g++ libsnappy-dev lcov  # 全离线，7 个包
```

| # | 项 | 现状 | 怎么补 |
|---|---|---|---|
| T1 | **LeakSanitizer 泄漏维度**（doc/06 P2-9 的唯一残留） | LSan 在本机实测可用（`libasan.so.8`，负向探针能报 8 字节泄漏），开关已进脚本 | `SAN_DETECT_LEAKS=1 bash scripts/run_sanitizers.sh`；先给 `-O0/volatile` 的假泄漏做负向对照（`-O1` 下死代码消除会把探针本身优化掉，本轮踩过） |
| T2 | **TSan + 并发压测**（P2-2） | gcc 的 `libtsan` 就位（`/usr/lib/gcc/.../12/libtsan.so`），一条探针能建能跑 | 需另写 `SAN="-fsanitize=thread"` 的一次构建（TSan 与 ASan 不能混）+ 移植官方 `db_test` 的 `MultiThreadTest`（MemTableTrash 场景）并加时长 |
| T3 | **行/分支覆盖率报告**（"覆盖完全"的量化口径） | `lcov` 装过又卸了（可离线装回，见本节开头）；`gcov` 随 gcc 还在，但**本轮从未做过 `--coverage` 构建**，全仓库 0 个 `.gcno/.gcda`，没有现成数据 | `make clean && make OBJDIR=build_cov BINDIR=build_cov SANFLAGS="--coverage"`（**必须走 `SANFLAGS`**：命令行给 `CFLAGS=` 会吞掉 `+=` 的 `-D_GNU_SOURCE`/`-lpthread`，正是 §6 坑①），再 `lcov --capture --directory build_cov --output-file cov.info` + `genhtml`，未命中行逐条回填 doc/05。**配方前半段已实测**（在 `/tmp` 里做的一次性构建，探针产物随后删掉）：`SANFLAGS="--coverage"` 的编译行里 `-D_GNU_SOURCE` 与 `--coverage` 并存、25 个 `.gcno`、跑完 127/0 并落下 36 个 `.gcda`；后半段（`--capture`/`genhtml`）因 `lcov` 已卸未跑，重装后再验。 |
| T4 | **多进程锁语义专项**（P2-6） | Linux 的 `fcntl` 区域锁与 Windows 独占打开语义不同，只有原生 POSIX 能测真值 | 两进程同时 `leveldb_open` 同一目录，断言第二个拿到 `IO error`；与官方 `env_posix` 行为对照 |
| T5 | snappy 压缩模式并入正式腿（P2-8，见 §7） | 探路已完成 | 决定官方库是否默认开 `HAVE_SNAPPY`，并加 `sst-snappy` 模式 |
| T6 | macOS 实机确认（P2-1 的另一半） | Linux 那轮的机器与这台 Windows 机都没有 macOS | 同 L0/L1 两条腿；注意 `HAVE_FULLFSYNC=1` 会让 port 配置探针结果不同。**动身前先按源码读到的四处 GNU 依赖做准备**：① 三元组断言只接受 `*-cygwin`/`*-msys`/`*linux*`（`build_official.sh`、`run_interop.sh`；2026-09-20 补了 `*-msys`），`x86_64-apple-darwin…` 会被拒；② `/usr/bin/timeout` 在 macOS 上不存在（两条脚本用它包每次执行）；③ `sha256sum`/`stat -c` 这两处已在 §8.3 第 3 条改成 fail-closed + POSIX 写法；④ `cp -a` 是 GNU 拼写，BSD 侧待核。§8.3 第 1 条（`CC` 判据）与第 2 条（`find -quit`）也正是为这条通路铺的 |
| T7 | §4 的告警集合（Linux gcc 12 与 MSYS gcc 15.2 各 6 条，MINGW64 gcc 16.1 与 clang64 各 8 条） | 未改，属既有代码质量；位置已逐条对过官方源（§4 第二张表） | 逐条清（先 `table.c:420` 的 const 丢弃），改完四条通路各重跑一次 |
| T8 | 官方 `corruption_test`（P1-2 → P2-3） | 需先实现故障注入 Env，非环境缺口 | 见 doc/06 P1-2 |

---

## 10. 一键复现

```bash
cd /root/work/kvdb
apt-get install --no-install-recommends -y g++   # L1/L2 要现编 C++ 参考库；本轮结束后已卸
git submodule update --init leveldb                 # 固定 pin，脚本会校验提交与未改动
export TMPDIR=$PWD/build_linux/tmp; mkdir -p "$TMPDIR"

make -j4 && ./build/kvdb_tests                     # L0
bash scripts/run_golden.sh                         # L1（需 g++）
RUN_C_TEST=1 bash scripts/run_interop.sh           # L2（需 g++）
bash scripts/run_sanitizers.sh                     # L3（Linux 上自动选 gcc）
```

留档目录（`.gitignore` 已覆盖 `build/`，不会入库）：`build_linux/logs/`、
`build/interop/golden-*`、`build/interop/run-*`、`build/san-runs/*`。
