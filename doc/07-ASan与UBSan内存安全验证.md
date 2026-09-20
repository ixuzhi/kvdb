# 07 — ASan/UBSan 内存安全验证记录

本文档专记 2026-09-19 的内存安全复核这一轮验证：为什么只有 clang64 能跑、
它到底跑了什么、暴露的两个真实缺陷（doc/06 P0-8、P0-9）以及**为此改动
的每一处代码**，最后给出负向对照的做法与"它没能覆盖什么"。
缺陷的现象/定位/根因叙事在 doc/04 的 E-14 与 N-6，本文按文件展开改动本身。

---

## 1. 为什么只有 clang64

| 工具链 | 目标三元组 | 有无 sanitizer 运行时 |
|---|---|---|
| MSYS2 gcc 15.2（`MSYSTEM=MSYS`） | ~~`x86_64-pc-cygwin`~~ 本轮实测该层报 `x86_64-pc-msys`，见下方更正 | 无：MSYS2 不提供 `mingw-w64-x86_64-sanitizers` 这个包，`libasan`/`libubsan` 都拿不到 |
| MinGW64 gcc 16.1（`MSYSTEM=MINGW64`） | `x86_64-w64-mingw32` | 同上 |
| 独立 Cygwin gcc 14.4（2026-09-20 补） | `x86_64-pc-cygwin` | 无，且是**仓库层面不可能**：`setup.ini` 的 16,087 个包里按名筛 `asan` 命中 0，`/usr/lib` 下无任何 `libasan` |
| MSYS2 clang 22.1.8（`MSYSTEM=CLANG64`） | `x86_64-w64-windows-gnu` | **有**：`libclang_rt.asan_dynamic-x86_64.dll` 随工具链交付 |
| 原生 Linux gcc 12.2（Debian 12，2026-09-19 补） | `x86_64-linux-gnu` | **有，且不必换编译器**：`/usr/lib/gcc/x86_64-linux-gnu/12/` 下 `libasan`/`libubsan`/`libtsan` 齐备，`libasan.so.8` 还带 LeakSanitizer |

> **2026-09-20 更正**：这张表原来把 MSYS2 的 msys 层写成 `x86_64-pc-cygwin`。
> 本机重装后的 MSYS2（gcc 13.3.0 / msys 运行时 3.5.7）里 `/usr/bin/gcc
> -dumpmachine` 实报 `x86_64-pc-msys`，两种拼法是否同一世代的同一环境已无从
> 对照（doc/09 W2）。这件事不只是记账错误：`build_official.sh` 与
> `run_interop.sh` 正是拿 `*-cygwin` 当 POSIX 判据的，于是在这台机器上官方
> 参考库直接拒绝构建（doc/04 N-7）。判据现已统一成 `*linux*|*-cygwin|*-msys`。
>
> 同一天把"Windows 的 gcc 没有 sanitizer 运行时"从推断升级成直接探针：同一份
> 空程序交给 msys gcc 13.3、w64devkit gcc 16.2、Cygwin gcc 14.4 各试一次
> `-fsanitize=address,undefined`，三支全部 `cannot find -lasan` / `-lubsan`，
> 只有 clang 链得上（doc/09 §4.3）。

也就是说这不是"换个编译选项"那么轻：为了让这轮验证可跑，Makefile 必须先能
正确识别 clang 的目标三元组（见 §3 的 Makefile 改动），否则它会被当成 POSIX
目标去编 `env_posix.c`。

> 本节标题里的"只有 clang64"仅对 **Windows 主机**成立。次日在原生 Linux 上复跑时，
> `scripts/run_sanitizers.sh` 已改为按宿主自动选编译器（Windows 目标→clang，其余→gcc），
> 并在那里发现了 Windows 侧根本看不见的一处 UB（P0-10，见 §3.6 与 doc/08）。

## 2. 这一轮验证跑了什么

一条命令（需在 MSYS2 的 CLANG64 shell 内）：

```bash
OFFICIAL_DBS=build/interop/golden-20260919-153942 bash scripts/run_sanitizers.sh
```

`scripts/run_sanitizers.sh` 做四件事：

1. 用 `-std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter` 加上
   `-fsanitize=address,undefined -fno-omit-frame-pointer
   -fno-sanitize-recover=all` 重建**整个**引擎（`OBJDIR=build/san`，先清对象树；
   `SAN=` 可整串覆盖，例如只留 `-fsanitize=address` 单独看某一条报告）；
2. 跑移植的 127 个用例；
3. 用**未经修改的官方** `leveldb/db/c_test.c` 直接链接这份带插桩的库；
4. 编译 `tests/interop/golden_driver.c` 对同一份带插桩的库做 create/verify，
   覆盖 10 种确定性负载；给了 `OFFICIAL_DBS` 时，verify 读的是**真实 leveldb
   写出的目录**，于是读侧的解析/解压/迭代路径也在 sanitizer 下走了一遍 foreign bytes。

两点值得记住的机制：

- **退出码 0 就等于"零报告"**。`-fno-sanitize-recover=all` 是编译期、逐 TU 的
  属性，写进库的目标文件后运行时改 `UBSAN_OPTIONS=halt_on_error=0` 也放宽不了；
  所以脚本不需要再 grep 一遍日志才敢判 PASS（脚本仍 grep，作为双重证据）。
- **留档目录在 `build/san-runs/` 而不是 `build/san/`**（被清的那棵对象树里），
  且脚本把 clang 是否在 PATH 的检查排在 `rm -rf` 之前。这条规则是被咬出来的：
  初版顺序颠倒，一次误运行就清掉了当轮唯一的证据。

## 3. 为 sanitizer 暴露的缺陷所做的改动

### 3.1 P0-8：arena 补齐 `AllocateAligned`（UBSan 报 misaligned address）

| 文件 | 改动 |
|---|---|
| `src/util.c:854` | 新增 `ldb_arena_allocate_aligned`：对齐/slop 部分与官方 `Arena::AllocateAligned` 同构（`align = max(8, sizeof(void*))`，快路径前进 `bytes + slop`），剩余空间不足时走 `arena_allocate_fallback`（新分配的块天然对齐，不再加 slop），出口再 `assert` 一次对齐——这条 assert 正是官方用来兜住该不变量的那句。`memory_usage` 的记账沿用 kvdb arena 自己的口径（只记请求字节数，官方在快路径根本不动计数器、只在开新块时累加），不影响对齐语义 |
| `src/kvdb.h:526` | 声明，并注明"要把返回值强转成指针类型（跳表节点）的调用方不能拿 bump 指针的任意对齐地址" |
| `src/skiplist.c:31` | `node_new` 从 `ldb_arena_allocate` 改到对齐版 |

对齐缺失能活过 127 个用例，是因为 memtable 条目按变长布局交替占用 arena：前
一个条目占奇数字节，后一个节点的地址就只剩 2/4 字节对齐。x86-64 与 Windows
对此宽容，UBSan 不宽容。

### 3.2 P0-9：logger 归还 OS 句柄（官方 c_test 在 Windows 上首报）

| 文件 | 改动 |
|---|---|
| `src/kvdb.h:146` | `ldb_logger` 增 `void (*destroy)(struct ldb_logger*)`；`kvdb.h:442` 声明 `ldb_logger_destroy` |
| `src/env.c:88` | `ldb_logger_destroy`：非 NULL 且挂了钩子才调，钩子为空表示该 logger 不归自己释放 |
| `src/env_win.c:521,542` | `file_logger_destroy` 先 `fclose` 再 `free`；`win_new_logger` 挂上钩子 |
| `src/env_posix.c:361,382` | 同上，对齐官方 `~POSIXFileLogger` 的所有权约定：`FILE*` 属于 logger |
| `src/env_mem.c:430,442` | 内存 logger 释放自己的 `ldb_buffer`（原本同样滞留） |
| `src/db.c:128`、`src/repair.c:295` | 两处 `free(impl->options.info_log)` 改为 `ldb_logger_destroy(...)` |

Windows 上 `DestroyDB` 之后同路径重开报 win32 error 32（共享冲突），根因是上
一个进程内的 `fopen` 从没 `fclose`。POSIX 允许 unlink 打开中的文件，所以这条
在此前的所有 POSIX 运行里完全隐形——这也是它原先被低估为 P3-1 的原因。

### 3.3 clang 独有的严格性逼出的改动

| 文件 | 改动 | 为什么 GCC 侧看不出来 |
|---|---|---|
| `src/write_batch.c:34` | 定义处去掉按值返回上的 `const`（`const ldb_slice` → `ldb_slice`） | 限定符修饰按值返回结果无意义：GCC 只给 `-Wignored-qualifiers` 警告，clang 直接判 "conflicting types"（与 `kvdb.h` 的声明不一致） |
| `tests/test_util.c:478` | snappy 样本数组两条相邻字符串之间补逗号 | 漏逗号时 C 会把相邻字面量静默拼接成一条，样本数少一个；GCC 无警告，clang 的 `-Wstring-concatenation` 报出 |

### 3.4 工具链与构建侧

| 文件 | 改动 |
|---|---|
| `Makefile:34` | Windows 目标识别扩为"三元组含 `windows` **或** `mingw`"，否则 clang64 的 `x86_64-w64-windows-gnu` 会被判成 POSIX 目标 |
| `Makefile:35` | `-static` 只对 mingw 加：clang 的 ASan 运行时是 DLL，静态链接会让它无法解析 |
| `Makefile:3,81` | `$(AR) := ar` 笔误改为 `AR := ar`（此前靠 make 内置变量兜底）；`clean` 不再 `rm -rf $(OBJDIR)`——默认 `OBJDIR=build` 下那会连 `build/interop/`、`build/san-runs/` 里不可重生的跨引擎留档一起删 |
| `.gitignore` | 增 `build_*/`，覆盖 README 里 `OBJDIR=build_mingw` 这类按工具链分开的对象树 |

### 3.5 测试与脚本

| 文件 | 改动 |
|---|---|
| `tests/test_util.c:442` | `arena.AlignedAllocationsSurviveOddSizedNeighbours`：奇数大小分配与对齐分配交替 17 轮，断言 `%8==0`、按 height=3 的真实请求长度写入 `key`/`next[0..2]`，并留一条 2048 字节请求走 fallback |
| `tests/test_api_extra.c:343` | `api_extra.DestroyDbThenReopenInSameProcess`：同一进程内 3 轮 close→DestroyDB→重开→确认已清空→再写→压缩 |
| `scripts/run_sanitizers.sh` | 本文 §2 的那条命令，可复现、带留档 |

### 3.6 原生 Linux 复跑这一轮的配套改动（含随后 Windows 回归修掉的两处）

| 文件 | 改动 | 为什么到这一轮才浮出来 |
|---|---|---|
| `src/version_set.c:1442` | **P0-10**：`get_range2` 的两次 `memcpy` 各加 `n1 > 0`/`n2 > 0` 守卫。`SetupOtherInputs` 在该层无其它文件时传 `NULL + 0`，而向 `memcpy` 交 NULL 即使长度为 0 也是 UB | glibc 把 `memcpy` 形参声明为 `nonnull`，UBSan 的 `nonull-argument` 检查据此判罚；Windows 侧 libc++/msvcrt 没有该属性声明，同一份代码静默通过。官方 C++ 版是 `std::vector` 合并，从不交出空基址——这条 UB 属 C 重写引入 |
| `Makefile:6` | 新增 `SANFLAGS` 通道：命令行 `make CFLAGS=…` 会**同时吞掉** Makefile 里的 `+=` 追加（POSIX 分支的 `-D_GNU_SOURCE`、`-lpthread` 会静默消失，`make -n` 可实证），改为在平台分支之后 `+= $(SANFLAGS)` | Windows 分支本来什么都不追加，所以这个覆盖语义在 MSYS2 上从未暴露 |
| `scripts/run_sanitizers.sh` | `CC` 按宿主自动选（Windows 回归时改为**只有 `*linux*` 才默认 gcc**，其余一律 clang；Windows 三元组配非 clang 的 `CC` 直接 `exit 2`，且这道守卫排在 `rm -rf "$OBJDIR"` 之前，误跑不会毁掉上一次归档）；`-D_GNU_SOURCE` 只加给 `*linux*` 下脚本自己编的 `c_test.o`/`golden_driver.o`；`TMP`/`cygpath` 兜底块收窄到 Windows/cygwin/mingw 目标（POSIX 上原来会退化成 `TMP=.`，把留档写进仓库）；leak 行按平台分别陈述并支持 `SAN_DETECT_LEAKS=1` | 原判据用 `/usr/bin/gcc -dumpmachine` 求值：Git Bash 里该路径没有 gcc（拿到空串→选 gcc），MINGW64 里它又是 cygwin 目标（三元组不含 `mingw`），两条都会挑中一个没有 sanitizer runtime 的编译器。详见 doc/08 §8.3 |
| `scripts/build_official.sh`、`scripts/run_interop.sh` | 编译器断言从"必须 `x86_64-pc-cygwin`"放宽为 `*-cygwin` 或 `*linux*`；`KVDB` 路径可 `KVDB_LIB=` 覆盖 | 旧断言让 Linux 上连参考库都拒绝构建 |
| `scripts/build_official.sh`、`scripts/run_interop.sh`、`scripts/run_golden.sh`、`scripts/run_sanitizers.sh` | **2026-09-20 再放宽一次**：POSIX 判据补 `*-msys`；PATH 从钉死 `/usr/bin:/bin` 改成保留继承尾部；`git diff` 守卫加 `--ignore-submodules=all`；缺 `g++` 的提示改指真正会产出 `/usr/bin/g++` 的包 | 上一行的那次放宽只到 `*-cygwin`，而本机 MSYS2 报 `x86_64-pc-msys`——四条腿里三条在这台 Windows 机器上根本起不来。逐条负向对照见 doc/09 §3（N-7…N-10） |
| `scripts/run_golden.sh`、`scripts/run_interop.sh` | 新增归档时效守卫：`src/`、`include/` 里有比归档更新的 `.c/.h` 即 `exit 2`。写法是 `find … -print \| head -1`，不用 GNU 独有的 `-quit`（BSD/macOS 上会报错退出，配合 `2>/dev/null` 与空串判断正好让守卫静默失效） | 首轮 L1 拿 09‑13 的旧 `build/libleveldb.a` 跑，报出 4 个"伪 Linux 缺陷"（`sst-bloom`/`sst-bigblock` SIGSEGV、`edge` 断言、`tomb` 点查 NotFound）；换当前库立即全绿。守卫的负向对照两侧都做过：Linux 与 Windows 上各把归档 `touch` 到过去时间、用 `KVDB_LIB=` 指过去，两条脚本都拒绝运行（rc=2） |

## 4. 负向对照：怎么确认新用例真能抓到缺陷

两条修复各自做了"摘掉修复、看新用例是否恰好失败"的对照。对照必须在
**另开一份不带 sanitizer 的原生 Windows 构建**上做，理由见 §6。

| 摘掉什么 | 观察到什么 |
|---|---|
| `src/skiplist.c:31` 换回 `ldb_arena_allocate` | UBSan 立刻复报 `skiplist.c:31` 的 misaligned address；`arena.*` 用例的 `%8` 断言按 arena 当前状态概率性失败（这正是它必须造奇数邻居的原因） |
| `src/env_win.c:542` 的钩子挂接 | 全量套件崩在 `c_api.ApproximateSizes`；单独跑新用例则恰好在 `tests/test_api_extra.c:355`（`leveldb_destroy_db` 后 `err == NULL` 那条）失败 |

第一次 logger 对照是**无效**的：当时 `ar r` 因为 gcc 更早失败（缺 `TMP`，
"Cannot create temporary file in C:\Windows"）而没有真的替换成员，我拿到的
"泄漏版"二进制其实是没改过的归档，于是新用例假绿通过。教训是：做负向对照前
必须先确认被对照的那份二进制确实变了。

## 5. 结果

| 检查 | 结果 |
|---|---|
| 127 用例 / clang64 + ASan + UBSan | `127 tests, 0 failed`，无 `ERROR: AddressSanitizer`、无 `runtime error` |
| 官方 `db/c_test.c`（未修改） | 16 个阶段全 PASS |
| `golden_driver` 10 种负载 create + verify | 全部 rc=0；verify 读官方引擎目录，摘要与跨引擎黄金比对留档逐字符相同（`wal` 与 `sst` 内容一致，故 10 负载对应 9 个不同摘要） |
| 另外两条工具链（不带 sanitizer） | MSYS `env_posix` 127-127；MINGW64 `env_win` 静态 127-127 且官方 `c_test` PASS |
| 改动后重跑跨引擎黄金比对 | rc=0，严格模式仍 9/9 负载、33 个文件逐字节相同、109 项 PASS |

留档：`build/san-runs/interop-<时间戳>/{run.log,build.log,unit.log,c_test.log,<mode>.log}`。

## 6. 这轮验证覆盖不到的部分

- **泄漏**：Windows 版 ASan 不带 LeakSanitizer，`ASAN_OPTIONS=detect_leaks=1`
  会在 `main` 之前直接退出并打印 "detect_leaks is not supported on this
  platform"（脚本把这一行原样记进日志）。P0-9 这类"句柄/缓冲未归还"目前只能
  靠 API 行为反证。**2026-09-19 原生 Linux 复跑已把这条的"做不到"改成"能做但暂未做"**：
  gcc 的 `libasan.so.8` 带 LSan，负向探针能报出 8 字节泄漏，脚本另加
  `SAN_DETECT_LEAKS=1` 开关；真正跑一遍并把结论入账记为 doc/08 §9 的 T1。
  （做该负向对照时注意 `-O1` 会把死掉的 `malloc`/写整条消除，探针要 `-O0` +
  `volatile`，否则得到的是"没报"的假象——本轮先踩了这个坑。）
- **`-fno-sanitize-recover=all` 会掩盖后续结果**：首次遇到 UBSan 报告就终止，
  所以"枚举全部问题"需要临时换一份可恢复的构建单独跑一遍。本文两处缺陷都是
  这样找齐的，而不是指望一次 fatal 构建列全。
- **弱内存序架构**：kvdb 的 `ldb_skiplist_node::next[]` 是普通指针，官方是
  `std::atomic<Node*>`（relaxed load/store）。x86 上生成的机器码相同，故不属
  行为差异；换 ARM 需一并处理（doc/06 P2-2）。
- **snappy 压缩块的字节级等价**：参考库在无 libsnappy 的环境下把
  `kSnappyCompression` 静默降级为不压缩，黄金比对全在 `no_compression` 下做
  （doc/06 P2-8）。sanitizer 只证明了解码路径没有越界，没证明两边字节相同。
- **`run_cross_backend.sh` 整条通路不在 sanitizer 下**。它两支引擎都是无插桩构建
  （脚本调 `make` 时不传 `SANFLAGS`，也就是本文这条通道与那条通路之间没有任何
  交集；Windows 半边即使想插也无从下手——doc/09 §4.3 的探针显示三支 Windows gcc
  都 `cannot find -lasan`），所以 8 进程并发争用 `LOCK` 那段、以及"同一份源码
  两个后端各自读写对方目录"这条新路径，从没被 ASan/UBSan 看过一眼。
  `env_win.c` 之所以算被内存安全覆盖过，是因为 clang64 三元组选的正是它，
  `c_test` 与黄金驱动都跑在它上面——不是因为这条跨后端通路（doc/06 P2-9）。
- 未做长时间压测与故障注入 Env（doc/06 P2-2、P1-2）。

## 7. 两处"看起来是缺陷其实不是"

- clang64 首跑时 api_extra/c_api 共 18 例全红：该 shell 没设 `TMP`，
  `GetTempPathA` 逐级回落到 `C:\Windows`，测试无法建目录。环境而非引擎，
  脚本现已在 `TMP` 缺失时指到 `cygpath -w /tmp`。
- 跨引擎摘要一度在 `edge`/`wal-big` 上"不一致"：我写的驱动里
  `fnv(h, leveldb_iter_key(it, &kn), kn)` 三个实参求值顺序未定，`kn` 可能是
  上一轮的长度。引擎无差异，驱动已改。

另记一处我自己埋的测试 bug，正好说明 sanitizer 的边界：`arena.*` 用例初版只
申请 `sizeof(ldb_skiplist_node)`（含 1 个 `next` 槽），却写了 `next[1]`——越界
8 字节。它落在 arena 的 4096 字节块**内部**，ASan 看不见；现在按 height=3 的
真实请求长度申请，并断言下一次分配不会落进本次请求的范围。
