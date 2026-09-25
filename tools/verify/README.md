# tools/verify — 正交独立验证工具箱

入口：`verify_all.sh`（一键六段 + 负向对照 + 对比双跑）与 `verify_mutants.sh`
（变异检测矩阵）。方法论与全部踩坑档案见 [doc/13](../../doc/13-独立验证报告.md)。
本文件只做**工具地图**：谁在什么时候用、退出码契约是什么。

## 五个腿（C harness，只经 `include/leveldb/c.h`）

| 工具 | 角色 | 退出码契约 |
|---|---|---|
| `model_fuzz.c` | A：模型化随机差分。预言机=自建排序数组模型；二进制键、批写/批删、全压缩、周期重开 | 0 过；3 模型不符；1 IO |
| `format_probe.js` | B：第三语言（JS）字节级解析器——CRC32C/varint/日志分片/VersionEdit/SST 块/前缀压缩/snappy 解码；校验每块 CRC、index 路由、层级不重叠、无孤儿文件；`--compare A B` 跨引擎摘要比对；可选第二参输出 bloom oracle 输入 | 0 零问题；1 有 problems；2 探针自身错误 |
| `iterator_snapshot.c` | F：快照隔离。barrier 取模型副本建迭代器，写线程跨 flush 推进；**正向+反向**全量迭代必须=副本 | 0 过；3 快照不符；1 错误 |
| `crash_torture.c` | D：崩溃一致性。`write`=逐条 sync 写+fsync 记账后**进程自毁**（TerminateProcess/SIGKILL）；`verify`=重开状态必须是 [0,ack] 的**完整前缀**（`max==ack` 完整性断言）；`verify-drop K`=负向对照（故意删一个已确认键，必须报 3） | 0 通过；3 违例；4 打开被拒（合法，仅记录） |
| `reopen_probe.c` + `corrupt_fuzz.js` | C：结构化字节损坏的无崩溃属性。每轮翻 1~8 字节（MANIFEST 25%/.ldb 45%/.log 30%），子进程 15s 超时重开 | probe：0 正常开/4 合法拒；fuzz：0 无崩溃无挂死，否则 1 |
| `mkfixture_kvdb.c` / `mkfixture_official.cc` | B 腿夹具：同工作负载（20 键+压缩，**带 bloom**）各写一份，供探针与 compare 用 | — |

## 官方实现的“仲裁器”与 oracle（`official_*.cc`，链接官方归档）

这些不是被测对象，而是**第三方事实来源**——探针与 harness 与官方实现
三次分歧全部由它们裁定（doc/13 §4）：

| 工具 | 仲裁什么 |
|---|---|
| `official_bloom_check.cc` | **B2 段**：官方 `FilterPolicy::KeyMayMatch` 对 kvdb 写出的 filter 字节逐键提问——假阴性即失败（0 假阴性过；3 有假阴性）。替代了自研 JS 成员性检查（四次误报后退役） |
| `official_table_check.cc` | 官方 table reader + `verify_checksums=true` 读 kvdb 写的表 |
| `official_footer.cc` | 官方 `Footer::DecodeFrom` 打印句柄（仲裁 metaindex/index 语义） |
| `official_crc.cc` | 官方 `crc32c.cc` 对任意块/记录区间算 CRC（仲裁掩码公式与块覆盖范围） |
| `official_logdump.cc` | 官方 `log::Reader` 解出日志首几帧的字节基准 |
| `official_open.cc` | 官方 `DB::Open` 快速判定某目录是否可开（注意：open+close 会改写目录，探针前先复制） |

## 编排

| 脚本 | 作用 |
|---|---|
| `verify_all.sh [quick\|full]` | 六段（A/B/B2/F/D/C）+ 负向对照 + 对比双跑（同 harness 链官方归档跑同种子）；每腿带超时护栏（`TIMEOUT=`，默认 900s）；三元组守卫；缺工具/缺官方归档一律 SKIP 并说明。证据 `build/verify/<时间戳>/` |
| `verify_mutants.sh` | 变异检测：4 个一次性破坏（hash_mult / skip_wal / batch_count / snap_leak）在丢弃式副本树里重建，问五种方法各自报不报；矩阵 `build/verify/mutants/<时间戳>/matrix.tsv`。**须从与官方归档同一命名空间的壳运行**（oracle 列需要链接它）；夹具未生成或工具缺失一律记 `skip`——基础设施故障永不冒充 `caught` |

## 运行前提

- 引擎先 `make`（脚本会校验 `build/.target` 与当前工具链一致并按需重建）；
- B2/compare/对比段需要官方归档：`bash scripts/build_official.sh`（doc/12），
  且必须在**同一命名空间**的壳里跑（msys 归档不能在 mingw 壳里链）；
- node（B/C 段）；脚本内含 Windows 常见安装路径的解析器。
