# kvdb 项目文档索引

本目录收录 kvdb（纯 C 实现的 LevelDB 兼容存储引擎）的完整设计与
过程文档。项目主 README 在仓库根目录（`../README.md`），包含构建与
使用说明。

## 文档清单

| 文档 | 内容 |
|---|---|
| [01-架构设计.md](01-架构设计.md) | 模块结构图、C 语言下的核心设计决策（值语义 Status、vtable、引用计数、线程模型、arena）、写/读/压缩/恢复数据流、与 C++ 原版的逐文件映射表 |
| [02-磁盘格式与编码规范.md](02-磁盘格式与编码规范.md) | 全部磁盘格式的精确规范：varint/fixed/CRC32C（leveldb 特有语义）、内部键、文件命名、WAL 分片、WriteBatch、memtable 条目、SSTable（块/footer/bloom）、VersionEdit tag、层级常量表 |
| [03-实现过程与工时记录.md](03-实现过程与工时记录.md) | 11 个阶段的时间线重建、每阶段的耗时 / 估算 token / 难度评级、token 消耗构成分析、明确的估算方法论声明 |
| [04-调试与错误修复记录.md](04-调试与错误修复记录.md) | 已修复缺陷的完整档案：引擎级 E-1..E-15、平台/环境级 N-1..N-6、测试级 T-1..T-11，另有 5 条过程性事故，每条含现象 / 定位过程 / 根因 / 修复 / 验证 |
| [05-测试套件移植说明.md](05-测试套件移植说明.md) | harness 设计、92 个用例与 leveldb 原测试的逐条对照（另有 35 个新增用例，见该文件表格）、移植中发现的测试级缺陷、未移植项及原因 |
| [06-遗留问题清单.md](06-遗留问题清单.md) | 交付评审结论 + 2026-09-19 三轮复核（跨引擎二进制兼容性、内存安全与工具链矩阵、原生 Linux 环境）：P0 已修复 10 项、P1 功能缺口 5 项、P2 风险与未验证 12 项、P3 质量优化 7 项 |
| [07-ASan与UBSan内存安全验证.md](07-ASan与UBSan内存安全验证.md) | 内存安全这轮验证的专档：为什么（Windows 上）只有 clang64 能跑、脚本跑什么、`ldb_arena_allocate_aligned` 与 logger `destroy` 钩子等每一处改动、负向对照的做法与坑、sanitizer 覆盖不到的部分 |
| [08-Linux原生环境验证.md](08-Linux原生环境验证.md) | 首次原生 Linux（Debian 12 / glibc / gcc 12）全通路验证：环境矩阵、**为验证新装了什么软件与怎么卸干净**、四条通路逐条结果与留档路径、P0-10 的来龙去脉、四个 harness 陷阱（`make CFLAGS=` 吞掉 `+=`、静默链接陈旧归档、`find -quit` 在 BSD 上静默失效、缺 `sha256sum` 时逐字节判据 fail-open）、snappy 压缩等价性实测结论、**Windows 三条工具链的回归实测（已跑完）**、延后 TODO |

## 阅读建议

- 想快速理解实现 → 读 01（架构）+ 02（格式）。
- 想了解"哪些坑、为什么这样写" → 读 04（每个 bug 都是一次
  leveldb 内部不变量的再发现）。
- 想接手继续开发 → 先读 06 的 P1/P2，特别是 POSIX 构建验证
  与故障注入 Env 两项。
- 想核对某个常量/格式 → 02 的表格均标注了对应的 leveldb 源文件。
- 想复跑或扩展内存安全验证 → 07（ASan/UBSan 这轮验证的用法、逐文件改动、
  负向对照的坑，以及它测不到的部分）。
- 想在别的平台/环境上接手验证，或想知道"这轮装了什么、怎么卸" →
  08（原生 Linux 全通路记录：环境矩阵、依赖与卸载、四个 harness 陷阱、
  Windows 三条工具链的回归实测、延后 TODO）。

## 关键数字

- 引擎：25 个 .c + 2 个 .h，13,068 行 C11，零第三方依赖
- 测试：127 个用例，0 失败（MSYS2/POSIX、MinGW64/Windows 静态、
  clang64/Windows 动态、**原生 Linux(glibc)/POSIX** 四条工具链各自从干净对象树验证）
- 内存安全：`-fsanitize=address,undefined -fno-sanitize-recover=all` 下
  127 例 + 官方 `c_test` + 黄金驱动 **零报告**，Windows 侧用 clang64、
  原生 Linux 侧用 gcc 12（`scripts/run_sanitizers.sh` 按宿主自动选）；
  泄漏维度 Windows 上无 LeakSanitizer，Linux 上能力已就位但本轮未跑
  （doc/06 P2-9、doc/08 §9 T1）
- 跨引擎兼容性：官方 `db/c_test.c` 未修改直接通过；9 种确定性负载下与真实
  leveldb 产出的全部文件逐字节相同，且两引擎交叉读取摘要一致（见根 README）；
  snappy 压缩块**不**逐字节相同（kvdb 大 3.4%）但双向可解、摘要一致（doc/08 §7）
- 开发：≈5.5 小时，估算 ≈37 万 token（方法学与局限见 03）
- 已修复缺陷：引擎级 12（实现期）+ 2（sanitizer 轮）+ 1（原生 Linux 轮）、
  平台级 6、测试级 11、评审新增 1（repair 序列号）
