# 08 — Linux 原生环境（Debian 12 / glibc）验证记录

本文档记 2026-09-19 在**真正的原生 Linux（glibc）**上把 kvdb 的全部验证腿跑一遍的
过程：环境矩阵、为跑通这些腿新装的软件与卸载方法、四条腿的逐条结果、本轮
发现并修复的缺陷（doc/06 P0-10）、踩到的两个工具链坑、以及"哪些仍然没覆盖"。
**次日（2026-09-20）的第二轮另记 §11**：打开 LeakSanitizer、闭合 §9 的 T1（引擎侧与
测试侧共五族泄漏被修掉），并把四条腿在最新代码上全部复跑一遍。
**同日第三轮另记 §12**：再做一次全量复跑 + 一次 **C 语言使用检查**（严格警告矩阵、
`-fanalyzer`、逐文件人工复查），产出引擎级 6 条与测试级 3 条修复、3 个新用例
（套件 127 → **130**）、告警基线 6 → **0**。

> §1 那张表是**首轮（2026-09-19）**的读数，保留是为了能看出每一轮各自量到什么；
> 仓库当前状态的读数以 **§12.2** 为准。

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

> **本节记的是 2026-09-19 的首轮。** 次日在同一台机器上做了第二轮：打开
> LeakSanitizer（§9 的 T1 由此关闭）、把四条腿在最新代码上全部复跑一遍。
> 逐条结果、泄漏 breakdown 与负向对照见 **§11**。

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

### 2026-09-20 第二轮开工前的实际包状态

和上表**不一样**：`g++` / `g++-12` / `libstdc++-12-dev` 三个已在位（用户在这两轮之间
恢复了环境，`dpkg -l` 实测 `ii`），所以 L1/L2 不需要重装就能跑；`lcov` 及其两个 perl
依赖、`libsnappy-dev` 四个仍缺，于是 §9 的 T3（行/分支覆盖率）与 P2-8 的 snappy 正式
比对这一轮依旧做不了——`libsnappy1v5` 运行库系统里有，缺的是头文件，`run_interop.sh`
的独立探测因此打印 `SKIP Snappy interop: independent MSYS2 Snappy dependency probe
failed`（`build_linux/logs/R3-L2-interop.out:18`，同一条腿的 `Config:` 行是
`SNAPPY=0`）。**第二轮没有新装任何包**，故上面那份卸载清单仍然完整有效。

---

## 4. 四条腿的逐条结果

### L0 原生构建 + 全量单测

```bash
make -j4                                   # → build/libleveldb.a, build/kvdb_tests
TMPDIR=/root/work/kvdb/build_linux/tmp ./build/kvdb_tests   # 127 tests, 0 failed
```

gcc 12 在 `-Wall -Wextra` 下的 6 条告警（Linux 这轮的构建日志首次逐条记下；
均为既有代码质量问题，不是 Linux 行为差异，**当时**未在本轮修改）：
**这 6 条已于 2026-09-20 第三轮全部清零**，去向见下面第二张表的"清理方式"列
与 doc/04 E-24（`inputs0_size` 那条由 E-23 移植官方日志行时自然转成"被使用"）。

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
| `table.c:420` const 丢弃 | 官方把 filter 块**复制**进 `new char[]` | 改 `t->filter_data = block.alloc;`——`block.data.data` 本就是同一个 `buf` 的 const 视图，换过去既消警告又与下一行 `block.alloc = NULL` 的"转移所有权"意图对齐。**2026-09-20 第三轮没照这条做**：留在 `data.data` 上加了 `(char*)` 与一句所有权注释（doc/04 E-24）。两个写法都能消掉这一条警告，选后者的理由是"取的是刚读出来的那个 slice 的字节"这件事由 `data` 表达更直接，而 `-Wcast-qual` 在引擎里另有 43 处同族、消不完（§12.3） |
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
剩下 macOS（T6）与 §9 的 T2–T8（T1 已于 2026-09-20 关闭，见 §11）。

---

## 9. 仍**未**覆盖（TODO，按决定本轮延后）

已确认延后的四项 Linux 独有增量是 T1–T4；T5–T8 是随之记下的既有遗留。
**T1 已于次日（2026-09-20）关闭，见 §11；T7 已于同日的第三轮在 Linux 侧关闭，
见 §12.2–§12.3（Windows 四条腿未复跑）；其余六项状态未变。**
**注意 T1–T5 里凡是提到"包已就位"的，首轮结束时都已卸掉**（§3），
次日第二轮开工前的实际包状态另见 §3 末小节。重装要两个
开关一起给，否则 apt 会去拉 `lcov` 的 recommends（`libgd-perl` 等，缓存里没有）而报
`Unable to fetch some archives`：

```bash
apt-get install --no-install-recommends --no-download -y g++ libsnappy-dev lcov  # 全离线，7 个包
```

| # | 项 | 现状 | 怎么补 |
|---|---|---|---|
| T1 | ~~**LeakSanitizer 泄漏维度**~~（doc/06 P2-9 的唯一残留） | **已关闭（2026-09-20）**：脚本在 `*linux*` 上改为默认开启 LSan，首跑报 929,842 字节 / 201 个分配，逼出五族缺陷并全部修掉，复跑 `rc=0` 零报告——逐条见 §11 与 doc/04 E-16/E-17/E-18、T-12/T-13 | 保留原配方（也是本轮做的事）：`SAN_DETECT_LEAKS=1 bash scripts/run_sanitizers.sh`（现在默认就是开的，`SAN_DETECT_LEAKS=0` 才是关）；先给 `-O0/volatile` 的假泄漏做负向对照（`-O1` 下死代码消除会把探针本身优化掉，本轮踩过） |
| T2 | **TSan + 并发压测**（P2-2） | gcc 的 `libtsan` 就位（`/usr/lib/gcc/.../12/libtsan.so`），一条探针能建能跑 | 需另写 `SAN="-fsanitize=thread"` 的一次构建（TSan 与 ASan 不能混）+ 移植官方 `db_test` 的 `MultiThreadTest`（MemTableTrash 场景）并加时长 |
| T3 | **行/分支覆盖率报告**（"覆盖完全"的量化口径） | `lcov` 装过又卸了（可离线装回，见本节开头）；`gcov` 随 gcc 还在，但**本轮从未做过 `--coverage` 构建**，全仓库 0 个 `.gcno/.gcda`，没有现成数据 | `make clean && make OBJDIR=build_cov BINDIR=build_cov SANFLAGS="--coverage"`（**必须走 `SANFLAGS`**：命令行给 `CFLAGS=` 会吞掉 `+=` 的 `-D_GNU_SOURCE`/`-lpthread`，正是 §6 坑①），再 `lcov --capture --directory build_cov --output-file cov.info` + `genhtml`，未命中行逐条回填 doc/05。**配方前半段已实测**（在 `/tmp` 里做的一次性构建，探针产物随后删掉）：`SANFLAGS="--coverage"` 的编译行里 `-D_GNU_SOURCE` 与 `--coverage` 并存、25 个 `.gcno`、跑完 127/0 并落下 36 个 `.gcda`；后半段（`--capture`/`genhtml`）因 `lcov` 已卸未跑，重装后再验。 |
| T4 | **多进程锁语义专项**（P2-6） | Linux 的 `fcntl` 区域锁与 Windows 独占打开语义不同，只有原生 POSIX 能测真值 | 两进程同时 `leveldb_open` 同一目录，断言第二个拿到 `IO error`；与官方 `env_posix` 行为对照 |
| T5 | snappy 压缩模式并入正式腿（P2-8，见 §7） | 探路已完成 | 决定官方库是否默认开 `HAVE_SNAPPY`，并加 `sst-snappy` 模式 |
| T6 | macOS 实机确认（P2-1 的另一半） | Linux 那轮的机器与这台 Windows 机都没有 macOS | 同 L0/L1 两条腿；注意 `HAVE_FULLFSYNC=1` 会让 port 配置探针结果不同。**动身前先按源码读到的四处 GNU 依赖做准备**：① 三元组断言只接受 `*-cygwin`/`*-msys`/`*linux*`（`build_official.sh`、`run_interop.sh`；2026-09-20 补了 `*-msys`），`x86_64-apple-darwin…` 会被拒；② `/usr/bin/timeout` 在 macOS 上不存在（两条脚本用它包每次执行）；③ `sha256sum`/`stat -c` 这两处已在 §8.3 第 3 条改成 fail-closed + POSIX 写法；④ `cp -a` 是 GNU 拼写，BSD 侧待核。§8.3 第 1 条（`CC` 判据）与第 2 条（`find -quit`）也正是为这条通路铺的 |
| T7 | ~~§4 的告警集合（Linux gcc 12 与 MSYS gcc 15.2 各 6 条，MINGW64 gcc 16.1 与 clang64 各 8 条）~~ | **Linux 侧已关闭（2026-09-20 第三轮）**：`make clean && make -j4` 现在 **0 error / 0 warning**，`130 tests, 0 failed`。六条的去处是三处死码删除（`fs_dir_exists`/`unwrap_comparator`/`cache_key`）、一处可见性（`vs_setup_other_inputs` 补 `static`）与一处 `unused-variable`，逐条见 doc/04 E-24；**Windows 四条腿未复跑**，那 6/6/8/8 是改前值 | 剩下的活只有一件：在四条 Windows 通路上各重跑一次 `make clean && make -j4` 并把 `LC_ALL=C` 下的告警数记账（doc/06 文末待办）。`table.c:426` 的 const 丢弃**没有改**，是补了所有权注释——那 44 条 `-Wcast-qual` 是这套 C 写法的固有代价，见 §12.3 |
| T8 | 官方 `corruption_test`（P1-2 → P2-3） | 需先实现故障注入 Env，非环境缺口 | 见 doc/06 P1-2 |

---

## 10. 一键复现

```bash
cd /root/work/kvdb
apt-get install --no-install-recommends -y g++   # L1/L2 要现编 C++ 参考库；首轮装后卸过，第二轮起本机已在位
git submodule update --init leveldb                 # 固定 pin，脚本会校验提交与未改动
export TMPDIR=$PWD/build_linux/tmp; mkdir -p "$TMPDIR"

make -j4 && ./build/kvdb_tests                     # L0
bash scripts/run_golden.sh                         # L1（需 g++）
RUN_C_TEST=1 bash scripts/run_interop.sh           # L2（需 g++）
bash scripts/run_sanitizers.sh                     # L3（Linux 上自动选 gcc，且默认开 LSan ⇒ 见 §11）
```

留档目录（`.gitignore` 已覆盖 `build/`，不会入库）：`build_linux/logs/`、
`build/interop/golden-*`、`build/interop/run-*`、`build/san-runs/*`。

---

## 11. 2026-09-20 第二轮：泄漏维度（T1）闭环 + 四条腿复跑

首轮在这台机器上把 LSan 的**能力**验到位、却把**跑一遍**记成了 T1。本轮补上：
先让脚本默认开这一维，再量首跑结果，修掉报出来的每一族，最后把四条腿在最新代码上重跑。

### 11.1 先改装置：`run_sanitizers.sh` 在 Linux 上默认开 LSan

判据按宿主三元组分岔（`*linux*` ⇒ 默认开，其余 ⇒ 不支持），并把结果打成一行写进 `run.log`：

```
leak check: ON (SAN_DETECT_LEAKS=0 to disable), probe says: 0 tests, 0 failed
```

`SAN_DETECT_LEAKS=0` 是唯一的关闭方式。**这行文本本身就是判据**：改动前它打印的是
"SUPPORTED here (SAN_DETECT_LEAKS=1 to enable)"，两版对照能立刻看出这次到底有没有在查泄漏——
一条默认关着的检查，绿色和没跑过是无法区分的。

### 11.2 首跑报出的全部泄漏（`SAN_DETECT_LEAKS=1`，未修任何代码）

`build_linux/logs/R2-L3-leaks.out` → `build/san-runs/interop-20260920-000803/`，`rc=1`：

| 消费者 | 报出的量 |
|---|---|
| 127 例套件 | `929842 byte(s) leaked in 201 allocation(s)` |
| 官方 `c_test` | `33328 byte(s) / 10 allocation(s)` |
| `golden_driver` 每个负载的 create | `126–135 B / 2 allocation(s)` |

套件那一百来个分配按栈聚出来恰好五族（下表 7 行，E-16 与 T-13 各占两行），
逐族对上的字节数之和 = 929,842、对象数之和 = 201（无剩余）：

| 族 | 字节 / 对象 | 栈顶（分配点 ← 调用者） | 归属 |
|---|---|---|---|
| log reader 的读缓冲 | 917,504 / 28（**Indirect**） | `ldb_buffer_reserve util.c:28 ← ldb_buffer_resize ← ldb_log_reader_new log.c:129 ← recover_log_file db.c:436` | **E-16** |
| log reader 结构体 | 2,688 / 28（Direct） | `ldb_log_reader_new log.c:118 ← recover_log_file db.c:436` | **E-16** |
| 开库时丢弃的 env 状态 | 5,760 / 69（Direct） | `ldb_status_new util.c:106 ← … ← ldb_db_impl_new db.c:63`（memenv 46 + posix 23） | **E-17** |
| `db_new_db` 的 version edit | 1,568 / 49 | `… ← ldb_buffer_append_str ← ldb_version_edit_set_comparator version_edit.c:72 ← db_new_db db.c:146` | **E-18** |
| 脚手架丢弃的状态 | 2,242 / 22 | `ldb_status_new ← ldb_env_get_children / ldb_env_delete_dir ← ldb_test_destroy_dir test_main.c:60、:69` | **T-12** |
| 表用例的文件句柄 + 一个 lookup key | 64 / 4 与 16 / 1 | `mem_new_random_access_file env_mem.c:243 ← tt_open test_table.c:67`；`ldb_lookup_key_init util.c:646` | **T-13** |

三件值得记下来的事：

- **98.7% 的字节是"间接泄漏"**——只能通过一个已经泄漏的对象到达，所以 LSan 把 28 个
  32 KB 读缓冲（`LDB_LOG_BLOCK_SIZE` = 32,768，`kvdb.h:668`）记在 `ldb_buffer_reserve` 头上，
  而那 96 字节的 reader 结构体才是真正该修的地方。只看总量会去查错的那一层。
- **E-17 那一族只在错误路径上有 `msg` 可泄**（成功状态不持有堆内存），所以本轮修掉的
  29 处里只有 `db.c:63` 那几处被实测报出，其余 20 多处是机械扫描找出的**预防性**修复。
  doc/04 E-17 把这两类分开记了。
- 五族全部落在同一句话里：**官方 C++ 靠析构函数自动归还，C 的值类型必须自己说什么时候还**。
  E-16/E-18 是漏了 `*_destroy`，E-17/T-12 是丢弃了一个值类型——后者还在代码里新加了
  `ldb_status_release()`，好让"故意忽略这个错误"在源码里看得见。

### 11.3 修完之后：`rc=0` 且默认开着跑

| 腿 | 命令 | 结果 | 留档 |
|---|---|---|---|
| L3（显式开 LSan） | `SAN_DETECT_LEAKS=1 bash scripts/run_sanitizers.sh` | `rc=0`，127 例 + 官方 `c_test` 16 阶段 + 10 负载 create/verify **零泄漏报告** | `build_linux/logs/R3-L3-leaks.out` → `build/san-runs/interop-20260920-002712/` |
| L3（脚本改为默认开之后） | `bash scripts/run_sanitizers.sh` | `rc=0`，同上 | `R3-L3-default.out` → `-003009/` |
| L3（读真实官方目录） | `OFFICIAL_DBS=build/interop/golden-20260920-003147 bash scripts/run_sanitizers.sh` | `rc=0`；`foreign databases: …/golden-20260920-003147` | `R3-L3-official.out` → `-003525/` |
| 负向对照 | 把 `recover_log_file` 里补的那行换成一次故意的 `ldb_log_reader_new(...)` | `rc=1`、`1840384 byte(s) / 112 allocation(s)`，`c_test` 与 `verify sst` 各 `65728 B / 4` | `R3-L3-negctl.out` → `-002934/` |
| L0 | `make -j4 && ./build/kvdb_tests` | `127 tests, 0 failed`，6 条告警与首轮一字不差 | `R3-L0-build.log`、`R3-L0-unit.log`、`R3-L0-defaultbuild.log` |
| L1 | `bash scripts/run_golden.sh` | `rc=0`：109 PASS / 33 SAME / 1 DIFF（`levels` 布局，P2-7 允许）/ 0 FAIL；**`results.tsv` 145 行与修复前 `cmp` 为空** | `R3-L1-golden.out` → `build/interop/golden-20260920-003147/` |
| L2 | `RUN_C_TEST=1 bash scripts/run_interop.sh` | `rc=0`：65 阶段全 rc=0；未修改的官方 `c_test` `rc=0` | `R3-L2-interop.out` → `build/interop/run-xEPUzNri/` |

**"字节中性"是这轮最要紧的一条**：五族修复动了 `db.c`/`table.c`/`repair.c`/`version_set.c`/
`dbformat.c`/`c_api.c` 的控制流（其中 E-17 六处是 `if (ldb_ok(<内联调用>))` 的等价改写），
而 L1 的逐条判定与逐文件摘要和修复前完全相同，L3 的 `sst-bigblock` 摘要也仍是
`714b697132214d29`——归还内存不该改变磁盘，这一条被独立量过而不是被假设。

一处操作细节值得记：L1 第一次以 `rc=2` 退出，报 `Missing …/build/libleveldb.a`。
根因是本轮 L0 一直在 `OBJDIR=build_linux` 里构建，而跨引擎腿默认吃 `build/`。
不是缺陷、也不是守卫失灵——守卫正是靠"归档不存在"才没让它链上别的东西。
`make clean && make -j4` 回到默认 `build/` 后 L1/L2 立即通过。

---

## 12. 2026-09-20 第三轮：全量复跑 + C 语言使用检查

前面十一节问的都是"**这个平台上跑得通吗**"。这一轮换一个问题：
"**这套 C 代码本身写得对不对，以及工具能替我们看到什么程度**"。
两件事一起做：① 四条腿（L0/L1/L2/L3）在最终代码上完整重跑；② 一次 C 语言
使用检查 = 严格警告矩阵（基线之外再叠 16 条诊断）+ `-fanalyzer` 静态分析 +
逐文件人工复查（所有权、生命周期、`const` 正确性、别名、可移植性写法）。

产物：**引擎级 6 条**（E-19…E-23、E-25）、**测试级 3 条**（T-14/T-15/T-16）、
**新增 3 个用例**（127 → 130）、**1 条只登记不修**（doc/06 P1-6，编号 D-4）。
全文在 doc/04；本轮小节在 doc/06 文末。

### 12.1 环境增量：**0 个包**

本轮没有装任何东西，也没有卸。开工前后各取一次包名全集：
`dpkg-query -W -f='${Package}\n' | sort -u` 两侧都是 **627** 个，
`comm` 双向为空（新增 0、消失 0）。手工标记（`apt-mark showmanual`）同样不变。
§3 那张卸载清单因此仍然有效，不需要更新。

### 12.2 四条腿的最终读数

| 腿 | 命令 | 结果 | 留档 |
|---|---|---|---|
| L0 | `make clean && make -j4` → `./build/kvdb_tests` | 构建 **0 error / 0 warning**（改前基线是 6 条）；**`130 tests, 0 failed`** | `/tmp/R5-build-final2.log`、本轮复跑输出 |
| L1 | `bash scripts/run_golden.sh` | `rc=0`：109 PASS / 33 SAME / 1 DIFF（`levels` 布局，P2-7 允许）+ 1 INFO / **0 FAIL**；`results.tsv` **145 行** | `build/interop/golden-20260920-050206/` |
| L2 | `RUN_C_TEST=1 bash scripts/run_interop.sh` | `rc=0`：65 阶段全 `rc=0`；未修改的官方 `c_test` `rc=0`；`SKIP Snappy interop`（本机无 `libsnappy-dev`，P2-8 原样保留） | `build/interop/run-m68B3TDI/` |
| L3 | `bash scripts/run_sanitizers.sh` | `rc=0`，`leak check: ON`；130 例 + 官方 `c_test`(16 phases) + 10 负载 create/verify **零 ASan/UBSan/LSan 报告** | `build/san-runs/interop-20260920-050327/` |

**字节中性这条又被独立量了一次**：本轮动了 `db.c`/`repair.c`/`version_set.c`/
`util.c`/`kvdb.h`/`env_mem.c`/`table.c`/`memtable.c`/`cache.c`/`c_api.c` 十处控制流
或生命周期，而 L1 的 `results.tsv` 与泄漏修复轮（`golden-20260920-003147`）
**逐字节相同**：两边摘要都是 `c11044f19bd73d8f…`，`cmp` 为空。
L3 的 `sst-bigblock` 摘要仍是 `714b697132214d29`，与 Windows 侧那两遍一致。

新用例的"先红后绿"是在 **pristine HEAD** 的独立 worktree 里量的（随后清掉）：

| 新用例 | HEAD 症状 | 现在 |
|---|---|---|
| `db.RepairWalWithBloomFilter`（D-3） | `FAIL tests/test_db.c:718: 1 == dbt_get(...) (1 vs 0)` | PASS |
| `db.MemenvRenameSemantics`（E-25） | 跑到该例即 `rc=139`（SIGSEGV，自改名释放后继续用 `f->name`） | PASS |
| `db.EmptySlicesWithFilesInHigherLevels`（E-22） | `rc=0`——**只在 sanitizer 腿才红**，这类"sanitizer-only 用例"值得单记 | PASS（且 L3 零报告） |

### 12.3 严格警告矩阵：16 条额外诊断，只有 `-Wcast-qual` 有输出

做法（**必须走 `SANFLAGS`**，给 `CFLAGS=` 会吞掉 Makefile 的 `+=` 追加，正是 §6 坑①）：

```bash
make clean && make -j4 OBJDIR=/tmp/strictA BINDIR=/tmp/strictA \
  SANFLAGS="-Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
            -Wold-style-definition -Wredundant-decls -Wpointer-arith -Wcast-align \
            -Wwrite-strings -Wformat=2 -Wnull-dereference -Wshift-overflow \
            -Wduplicated-cond -Wimplicit-fallthrough -Wvla -Wcast-qual"
```

结果：**`-Wcast-qual` 49 条 / 48 个位置**（`tests/test_db.c:722` 一行两处），
其余 **15 条全部为空**。分布是 `src/` 44 条（13 个文件，`version_set.c` 7 条最多）、
`tests/` 5 条。

这 44 条**没有一条可以直接行动**——它们全是同一句话的两种写法：
① vtable 下转（`ldb_iterator*` → `ldb_memtable_iterator*` 一类，`const` 在
官方 C++ 里由 `const` 成员函数承担，C 里没有对应的落点）；② "缓存/表持有键的字节"
却要把键交给一个不收 `const` 的底层 API。也就是说 `-Wcast-qual` 在这套代码上
量的是"C++ 的 const 系统比 C 的表达力强"这件事，不是缺陷密度。
测试侧那 5 条同形（`test_db.c:722` 是本轮换掉的策略 `->destroy()`，两个 cast 在同一行）。

### 12.4 `-fanalyzer`：97 条 / 7 个 checker，五族形状**零真阳性**

```bash
make clean && make OBJDIR=/tmp/fa SANFLAGS="-fanalyzer" -j4   # 留档 /tmp/R5-fanalyzer.log
```

| checker | 条数 |
|---|---|
| `-Wanalyzer-possible-null-dereference` | 58 |
| `-Wanalyzer-possible-null-argument` | 19 |
| `-Wanalyzer-malloc-leak` | 9 |
| `-Wanalyzer-null-argument` | 5 |
| `-Wanalyzer-use-of-uninitialized-value` | 4 |
| `-Wanalyzer-null-dereference` | 1 |
| `-Wanalyzer-double-free` | 1 |

逐族判定（每族都到具体行看过）：

| 形状 | 覆盖条数 | 为什么不是缺陷 |
|---|---|---|
| **`assert` 当 OOM 策略**：`malloc`/`realloc`/`strdup` 的返回值没判空就用 | 82（58+19+5） | 仓库的分配失败策略就是 `assert(p)`——这与官方 C++ 的 `new` 抛 `bad_alloc` 是同位替换，不是"忘了检查"。分析器不知道这条约定，于是把"NULL 被解引用"的整条路径都吐出来。**要消掉它要么全局判空（与官方分歧更大），要么给它一份约定文件（没有这种东西）** |
| **按值搬运**：`ldb_status` 按值穿过 vtable 槽、`ldb_buffer_swap` 的三条整体赋值（`src/util.c:71`） | use-of-uninitialized-value 里的 3 条 `offsetof(ldb_status, code)` + `leak of 'tmp.data'` 2 条 | 值语义结构体换了名字/换了位置，路径敏感状态机认不出 store 跟过去了。C++ 里 `std::string` 移动赋值同理，只是分析器对 libstdc++ 有专门建模、对本仓库的 C 结构没有 |
| **首成员强转与 `free(&b->base)`**（`src/util.c:753`；`src/cache.c:40/96`） | `leak of 'b'` 1 条、`leak of '<unknown>'` 2 条 | `base` 是结构体首成员，`&b->base == (void*)b`——C 保证成立，分析器不推这件事。`free(t->list)` 之后紧接着 `t->list = new_list` 也是同一个"换名"问题 |
| **grow-by-realloc 的出参**（`src/version_set.c:247`、`src/repair.c:183/190`） | `leak of 'inputs'`/`'expanded0'`/`'tables'`、`double-‘free’ of ‘logs’` | 出参指向的列表由调用方持有、被 `realloc` 换过地址；`repair.c:183` 那句更直接：`logs` 与 `tables` 是两条独立列表，分析器把它们合并成了同一个符号才判成 double-free。LSan 在同一条腿上报 0，这条判罚就是假的 |
| **测试里故意交的 NULL**（`tests/test_recovery_extra.c:16/191`、`tests/test_api_extra.c:111/118`、`tests/test_util.c:178`） | 6（5 `null-argument` + 1 `null-dereference`） | 这些是**错误路径注入**本身：故意传空串/空缓冲去逼引擎的失败分支。分析器把它们读成"程序会崩" |

**最强的一条反证是跨检查器互斥**：`-fanalyzer` 报了 9 条 `malloc-leak`，
而 LSan 在同一天、同一棵对象树的上报 **0 泄漏**——一个是模型、一个是运行时实测，
冲突时运行时赢。所以本轮对 `-fanalyzer` 的结论不是"它没用"，而是
"**在这套 C 写法上它产不出可直接行动的结论**"：97 条要人读完 97 条才知道是 0 条，
成本高于收益。人工复查仍是主判据，而 E-19/E-20/E-21 三条全出自人工那一侧。

一处必须记的坑：**`-fanalyzer` 不能配 `-fsyntax-only`**。本轮实测同一份 `src/util.c`：
`-fsyntax-only -fanalyzer` 吐 **0** 条，去掉 `-fsyntax-only` 真做代码生成则吐 **6** 条。
分析器挂在 RTL 上，不生成代码就不开工——拿 `-fsyntax-only` 快速扫一遍会得到一次
"什么都没发现"的假绿。

### 12.5 方法论：LSan 的"可达即不报"会替缺陷打掩护

本轮最值钱的一条不是修掉的哪一处，而是这个：

```
C++ 析构 → C 显式 destroy 的翻译漏了一处   （真缺陷）
        ↑
被一个文件级 static 单例永久钉成"全局可达"  （LSan 因此一条都不报）
        ↓
把单例改成按实例存储 → 抑制消失 → 立刻报 144 B / 3 allocations
```

`ldb_get_internal_filter_policy` 返回 `static` 对象地址（E-19），任何经它可达的
分配在 LSan 眼里都是根上的活对象。所以**上一轮"零泄漏"有一部分是这块全局撑起来的**。
推论两条：① 泄漏检查的绿色不是绝对量，它依赖"谁还持有指针"；② 修完一处之后
LSan 变红，第一反应应该是分辨"**是新坏的，还是刚看得见**"——本轮答案是后者，
`golden_driver.c` 那个漏销毁从第一天起就在（T-14）。

同一枚硬币的反面也记一句：`-fanalyzer` 因为不知道约定而**多报**，
LSan 因为看见全局而**少报**。两个方向的偏差都来自"工具在用模型代替代码"，
所以本轮的每条判定都尽量找了第二个独立工具来对照（LSan↔`-fanalyzer`、
L1 摘要↔L3 摘要、HEAD worktree 的"先红"↔修复后的"后绿"）。

### 12.6 三条只在插桩构建里出现的报告（登记，不修）

`-Wformat-truncation= null format string` ×3：`src/util.c:79`、
`src/env_posix.c:341`、`src/env_mem.c:209`（`env_win.c:498` 同形，Linux 腿不参与编译）。
三处都是 `vsnprintf(NULL, 0, fmt, ap)` 这一句"先探长度"的官方写法。
**普通 `-O1`/`-O2` 构建一条都不报**，加上 `-fsanitize=address,undefined` 就报
（本轮实测：插桩构建 3 条，同一行同一文件去掉插桩 0 条）——
是 ASan 拦截版 `vsnprintf` 让格式串可空性判定走了另一条路径。
仓内所有调用方给的 `fmt` 都是字面量，判为误报，只登记。

### 12.7 一键复现本轮

```bash
cd /root/work/kvdb
export LC_ALL=C                                  # 否则中文 locale 下 grep 'warning:' 恒为 0
make clean && make -j4 && ./build/kvdb_tests     # L0：0 警告 + 130/0
bash scripts/run_golden.sh                        # L1
RUN_C_TEST=1 bash scripts/run_interop.sh          # L2
bash scripts/run_sanitizers.sh                    # L3（Linux 上默认开 LSan）
# 12.3/12.4 的两条矩阵命令见各自小节代码块
```
