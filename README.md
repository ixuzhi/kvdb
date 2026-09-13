# kvdb — LevelDB 兼容的纯 C 存储引擎

用 **纯 C（C11，零第三方依赖）** 完整实现的 LevelDB 存储引擎。接口与
LevelDB 官方 C 绑定（`leveldb/c.h`）**完全兼容**，磁盘格式（SSTable /
WAL / MANIFEST / CURRENT）与 LevelDB **逐字节兼容**，并把 LevelDB 的
测试用例集移植到 C 后全部跑通。

```text
92 tests, 0 failed
```

## 目录结构

```text
kvdb/
├── include/leveldb/c.h      # 公共 C API（与 leveldb/c.h 逐签名一致）
├── src/                     # 引擎实现（模块划分对应 leveldb 源码布局）
│   ├── kvdb.h               # 内部总头文件
│   ├── port.[ch]            # 线程/互斥/条件变量/原子操作（Win32 + POSIX）
│   ├── util.c               # slice/buffer/status/varint/crc32c/hash/
│   │                        #   bytewise 比较器/bloom 策略/logging
│   ├── env.c                # Env 通用封装
│   ├── env_win.c            # Windows Env 实现（文件/锁/后台调度/日志）
│   ├── env_mem.c            # 内存 Env（对应 helpers/memenv）
│   ├── cache.c              # 分片 LRU Cache（对应 util/cache.cc）
│   ├── dbformat.c           # 内部键/文件名/CURRENT（db/ + db/filename.cc）
│   ├── skiplist.c           # 跳表（db/skiplist.h，高度 12、分支 4）
│   ├── memtable.c           # 内存表（arena + 跳表，引用计数）
│   ├── log.c                # WAL 写/读（32KB 块、7B 头、FULL/FIRST/MIDDLE/LAST）
│   ├── write_batch.c        # WriteBatch 编码 + memtable 回放
│   ├── block.c              # 块句柄/页脚格式 + 块构建器 + 块迭代器
│   ├── filter_block.c       # bloom 过滤块构建/读取（2KB 基准）
│   ├── table.c              # SSTable 构建器 + 读取器（index/metaindex/footer）
│   ├── two_level.c          # 二级迭代器（index → data block）
│   ├── iterator.c           # 迭代器基类 + 空迭代器 + 合并迭代器
│   ├── snappy.c             # 自实现 Snappy 压缩/解压（规范兼容）
│   ├── version_edit.c       # VersionEdit 编解码（tag 1..9）
│   ├── version_set.c        # 版本管理/查找/压缩挑选/LogAndApply/恢复
│   ├── table_cache.c        # 打开的 SSTable 的 LRU 缓存 + BuildTable
│   ├── db.c                 # DBImpl：写路径/恢复/后台压缩/快照/属性
│   ├── db_iter.c            # 用户级迭代器（合并同 user key 多版本）
│   ├── repair.c             # 修复（log→table 转换 + 重建 MANIFEST）
│   └── c_api.c              # leveldb/c.h 的实现
├── tests/                   # 移植自 LevelDB 测试套件
│   ├── harness.h / test_main.c
│   ├── test_util.c          # crc32c / coding / bloom / dbformat / filename
│   │                        #   / hash / snappy 测试
│   ├── test_cache.c         # cache_test
│   ├── test_log.c           # log_test
│   ├── test_batch.c         # write_batch_test
│   ├── test_skiplist.c      # skiplist_test + memtable_test
│   ├── test_table.c         # table_test + block + filter_block
│   ├── test_db.c            # db_test（读写/快照/迭代器/压缩/恢复/修复/…）
│   └── test_c_api.c         # c_test
├── leveldb/                 # LevelDB 官方源码克隆（参考与测试用例来源）
├── Makefile
└── build/                   # 构建产物
```

## 与 LevelDB 的兼容性

### 1. C API 兼容（`include/leveldb/c.h`）
所有类型与函数签名与 LevelDB 1.23 的 `c.h` 完全一致：`leveldb_open /
put / get / delete / write / create_iterator / snapshot / approximate_sizes /
compact_range / destroy_db / repair_db / writebatch_* / options_* /
filterpolicy_create_bloom / cache_create_lru / create_default_env /
leveldb_free / major_version(1) / minor_version(23)` 等。为现有使用
LevelDB C API 的代码换一个头文件路径即可链接本实现。

### 2. 磁盘格式兼容
- **SSTable**：数据块（前缀压缩 + 重启点）、5 字节块尾（CRC32C 掩码 +
  压缩类型 + 长度）、metaindex（`filter.leveldb.BuiltinBloomFilter2`）、
  index（`FindShortestSeparator`/`FindShortSuccessor` 缩短键、
  restart_interval=1）、48 字节 footer + magic `0xdb4775248b80fb57`。
- **WAL / MANIFEST**：32KB 块、7 字节头、FULL/FIRST/MIDDLE/LAST 分片、
  记录 CRC32C（与 leveldb 完全相同的 Extend/Value/Mask 语义）。
- **文件命名**：`/CURRENT`、`/LOCK`、`/LOG(.old)`、`%06llu.log|.ldb|.sst|
  .dbtmp`、`MANIFEST-%06llu`；`CURRENT` 经 `.dbtmp` 原子改名写入。
- **内部键**：`user_key + (seq<<8|type)`，内部比较器
  `leveldb.InternalKeyComparator`（user 升序、seq 降序、type 降序）。
- **Bloom**：`leveldb.BuiltinBloomFilter2`，k = round(bits×0.69) 截断
  [1,30]，种子 `0xbc9f1d34`，双重哈希探测。

### 3. 行为兼容
- 写路径：writer 队列 + `BuildBatchGroup`（128KB/1MB 上限）+ WAL 先行 +
  memtable 提交；`sync` 选项；`MakeRoomForWrite` 的 L0 8/12 迟滞。
- 压缩：L0 触发 4/8/12 文件；每层 10MB×10^ 增长；grandparent 重叠
  10×max_file_size 停止；扩展上限 25×；`AddBoundaryInputs` 边界文件；
  平凡移动（trivial move）；seek 触发压缩（`allowed_seeks = file_size/
  16384`，下限 100）；memtable 输出最多下推至 L2。
- 恢复：CURRENT → MANIFEST 校验比较器 → 重放 log（按序）→ 未见登记的
  log 也重放 → 缺失表文件报 Corruption → 观测到的最大 sequence 恢复。
- 读取：memtable → imm → 版本逐层（L0 新→旧、L≥1 二分）；
  `UpdateStats`/`RecordReadSample` 驱动 seek 压缩；LRU block cache
  （cache_id + offset 键）与 table cache（max_open_files-10）。
- 迭代器：DBIter 方向切换语义、快照可见性、tombstone 隐藏；合并迭代器
  的方向切换（leveldb 精确算法）；两路清理回调保护 memtable/version。

## 构建与测试

```bash
make            # 生成 build/libleveldb.a 与 build/kvdb_tests.exe
make test       # 运行全部测试
./build/kvdb_tests.exe <关键字>   # 只运行名称匹配的测试
```

环境说明：本仓库在 Windows（Git Bash）下开发，工具链为
[w64devkit](https://github.com/skeeto/w64devkit)（GCC 16，位于
`_tools/w64devkit/`，首次构建时自动下载）。构建系统按编译器目标
（`gcc -dumpmachine`）自动选择 Env 后端：MinGW 用 `env_win.c`，
其余环境（Linux / macOS / MSYS2）用 `env_posix.c` + pthread。
POSIX 后端已在 MSYS2 环境（`pacman -S make gcc` 后直接 `make`）
构建并跑通全部 92 个用例；原生 Linux/macOS 有待一次实机确认。

## 测试套件（移植自 LevelDB）

| 测试文件 | 对应 LevelDB 测试 | 覆盖内容 |
|---|---|---|
| test_util.c | crc32c_test、coding_test、bloom_test、dbformat_test、filename_test、hash、snappy | RFC3720 CRC 向量、varint/fixed 编解码、bloom 误报率、内部键排序、文件名解析、snappy 往返 |
| test_cache.c | cache_test | 命中/未中、驱逐、剪枝、容量 0、NewId、TotalCharge |
| test_log.c | log_test | 读写、多块、跨块分片、块边界 |
| test_batch.c | write_batch_test | 计数/序列、回放到 memtable、append |
| test_skiplist.c | skiplist_test、memtable_test | 2000 键插入查找、重复键、Prev、多版本可见性、tombstone |
| test_table.c | table_test | 5000 键往返、Seek/Prev、InternalGet+bloom、ApproximateOffset、块构建/双向迭代、filter block |
| test_db.c | db_test | 读写、PutDeleteGet、快照（多快照+隐藏）、迭代器（空/单/多/删除/Prev/多版本）、压缩触发、跨压缩删除、CompactRange、恢复、序列号恢复、属性、ApproximateSizes、DestroyDB、Repair |
| test_c_api.c | c_test | 不存在库打开失败、PutGetDelete、WriteBatch、迭代器、快照、属性、CompactRange、bloom 选项、ApproximateSizes、版本号 |

## 已知限制

- `Options::reuse_logs` 未实现（恒按 false 处理，同 leveldb 默认值）。
- 压缩仅支持 Snappy（自实现，规范兼容）；不支持 zstd（新版 leveldb 扩展）。
- `db_test` 中依赖故障注入 Env（SpecialEnv / CorruptedKey 等）的少数
  用例（如读损坏数据、写中断恢复）未移植；其余核心场景均已覆盖。
- 未实现 `leveldbutil`（dumpfile）与基准工具 `db_bench`。
