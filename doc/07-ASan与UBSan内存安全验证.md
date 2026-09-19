# 07 — ASan/UBSan 内存安全验证记录

本文档专记 2026-09-19 的内存安全复核这一条验证腿：为什么只有 clang64 能跑、
这条腿到底跑了什么、它暴露的两个真实缺陷（doc/06 P0-8、P0-9）以及**为此改动
的每一处代码**，最后给出负向对照的做法与"它没能覆盖什么"。
缺陷的现象/定位/根因叙事在 doc/04 的 E-14 与 N-6，本文按文件展开改动本身。

---

## 1. 为什么只有 clang64

| 工具链 | 目标三元组 | 有无 sanitizer 运行时 |
|---|---|---|
| MSYS2 gcc 15.2（`MSYSTEM=MSYS`） | `x86_64-pc-cygwin` | 无：MSYS2 不提供 `mingw-w64-x86_64-sanitizers` 这个包，`libasan`/`libubsan` 都拿不到 |
| MinGW64 gcc 16.1（`MSYSTEM=MINGW64`） | `x86_64-w64-mingw32` | 同上 |
| MSYS2 clang 22.1.8（`MSYSTEM=CLANG64`） | `x86_64-w64-windows-gnu` | **有**：`libclang_rt.asan_dynamic-x86_64.dll` 随工具链交付 |

也就是说这不是"换个编译选项"那么轻：为了让这条腿可跑，Makefile 必须先能
正确识别 clang 的目标三元组（见 §3 的 Makefile 改动），否则它会被当成 POSIX
目标去编 `env_posix.c`。

## 2. 这一条腿跑了什么

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

## 6. 这条腿覆盖不到的部分

- **泄漏**：Windows 版 ASan 不带 LeakSanitizer，`ASAN_OPTIONS=detect_leaks=1`
  会在 `main` 之前直接退出并打印 "detect_leaks is not supported on this
  platform"（脚本把这一行原样记进日志）。P0-9 这类"句柄/缓冲未归还"目前只能
  靠 API 行为反证。补齐办法是在 Linux/macOS 上跑同一脚本，那边默认带 LSan。
- **`-fno-sanitize-recover=all` 会掩盖后续结果**：首次遇到 UBSan 报告就终止，
  所以"枚举全部问题"需要临时换一份可恢复的构建单独跑一遍。本文两处缺陷都是
  这样找齐的，而不是指望一次 fatal 构建列全。
- **弱内存序架构**：kvdb 的 `ldb_skiplist_node::next[]` 是普通指针，官方是
  `std::atomic<Node*>`（relaxed load/store）。x86 上生成的机器码相同，故不属
  行为差异；换 ARM 需一并处理（doc/06 P2-2）。
- **snappy 压缩块的字节级等价**：参考库在无 libsnappy 的环境下把
  `kSnappyCompression` 静默降级为不压缩，黄金比对全在 `no_compression` 下做
  （doc/06 P2-8）。sanitizer 只证明了解码路径没有越界，没证明两边字节相同。
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
