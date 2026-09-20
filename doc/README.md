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
| [04-调试与错误修复记录.md](04-调试与错误修复记录.md) | 已修复缺陷的完整档案：引擎级 E-1..E-25、平台/环境级 N-1..N-19、测试级 T-1..T-16，另有 5 条过程性事故，每条含现象 / 定位过程 / 根因 / 修复 / 验证 |
| [05-测试套件移植说明.md](05-测试套件移植说明.md) | harness 设计、92 个用例与 leveldb 原测试的逐条对照（另有 38 个新增用例，见该文件表格）、移植中发现的测试级缺陷、未移植项及原因 |
| [06-遗留问题清单.md](06-遗留问题清单.md) | 交付评审结论 + 七轮复核（跨引擎二进制兼容性、内存安全与工具链矩阵、原生 Linux 环境、Windows 工具链矩阵、第二台 Windows 机器复跑、原生 Linux 泄漏维度、原生 Linux 的 C 语言使用检查）：P0 已修复 10 项、P1 功能缺口 6 项、P2 风险与未验证 13 项、P3 质量优化 7 项 |
| [07-ASan与UBSan内存安全验证.md](07-ASan与UBSan内存安全验证.md) | 内存安全这轮验证的专档：为什么（Windows 上）只有 clang64 能跑、脚本跑什么、`ldb_arena_allocate_aligned` 与 logger `destroy` 钩子等每一处改动、负向对照的做法与坑、sanitizer 覆盖不到的部分（含 2026-09-20 在 Linux 上默认开启的 LeakSanitizer 那一格） |
| [08-Linux原生环境验证.md](08-Linux原生环境验证.md) | 首次原生 Linux（Debian 12 / glibc / gcc 12）全通路验证：环境矩阵、**为验证新装了什么软件与怎么卸干净**、四条通路逐条结果与留档路径、P0-10 的来龙去脉、四个 harness 陷阱（`make CFLAGS=` 吞掉 `+=`、静默链接陈旧归档、`find -quit` 在 BSD 上静默失效、缺 `sha256sum` 时逐字节判据 fail-open）、snappy 压缩等价性实测结论、**Windows 三条工具链的回归实测（已跑完）**、延后 TODO、§11 次日第二轮：打开 LeakSanitizer、五族泄漏的逐族字节表与负向对照、**§12 第三轮：全量复跑 + C 语言使用检查（16 选项告警矩阵、`-fanalyzer` 97 条的逐族分诊、LSan 的"可达即不报"会替缺陷打掩护）** |
| [09-Windows工具链矩阵与Cygwin验证.md](09-Windows工具链矩阵与Cygwin验证.md) | 2026-09-20 把"Windows 已验证"重新变成可复现结论这一轮：**mingw / msys2 / cygwin 三个互不可见的 POSIX 命名空间矩阵**、九处 harness 缺陷（N-7…N-15：POSIX 判据只认 `-cygwin`、钉死 PATH 把 git 也扔了、嵌套子模块被误判成改动、缺 `g++` 的提示指向脚本随后会拒绝的包、`env_posix.c` 那句过期且方向危险的注释、本地化 shell 让 `grep warning:` 恒为 0、新腿缺 `TMP` 守卫以致环境问题伪装成后端不一致、**参考库归档与 `make` 对象树跨命名空间串档**）及各自的负向对照、**独立 Cygwin 首轮全绿**、`_WIN32` 在 MSYS2/Cygwin 上的实测值、新脚本腿 `run_cross_backend.sh`（`env_win` 与 `env_posix` 落盘字节互比 + 8 进程锁竞争专项）、clang64 重装后的 ASan/UBSan 复跑、N-14/N-15 之后的第二遍收尾（§9.1）、**第二台 Windows 机器的复跑（§10：数字逐项对上，另逼出 N-16…N-19 四处 harness 缺陷）** |

## 阅读建议

- 想快速理解实现 → 读 01（架构）+ 02（格式）。
- 想了解"哪些坑、为什么这样写" → 读 04（每个 bug 都是一次
  leveldb 内部不变量的再发现）。
- 想接手继续开发 → 先读 06 的 P1/P2，特别是 POSIX 构建验证
  与故障注入 Env 两项。
- 想核对某个常量/格式 → 02 的表格均标注了对应的 leveldb 源文件。
- 想复跑或扩展内存安全验证 → 07（ASan/UBSan 这轮验证的用法、逐文件改动、
  负向对照的坑，以及它测不到的部分——含"改完代码 LSan 才变红时，先分辨是
  新坏的还是刚看得见"那一条）。
- 想在别的平台/环境上接手验证，或想知道"这轮装了什么、怎么卸" →
  08（原生 Linux 全通路记录：环境矩阵、依赖与卸载、四个 harness 陷阱、
  Windows 三条工具链的回归实测、延后 TODO、三轮复跑的逐条读数）。
- 想知道"用工具扫一遍 C 代码能得到什么" → 08 §12.3/§12.4：16 条额外诊断
  只有 1 条有输出、`-fanalyzer` 的 97 条全部判为模型局限。这两组数字量的是
  工具的边界，不是代码的质量；本轮六条真缺陷全出自人工逐文件复查。
- 想搞清楚 Windows 上那几套 POSIX 仿真层到底谁是谁、为什么脚本会在另一台
  机器上起不来 → 09（mingw/msys2/cygwin 命名空间矩阵、九处 harness 缺陷与
  负向对照、独立 Cygwin 首轮、两个 Env 后端的字节互比）。

## 关键数字

- 引擎：25 个 .c + 2 个 .h，13,151 行 C11，零第三方依赖
- 测试：**130** 个用例，0 失败；**原生 Linux** 的 `-Wall -Wextra` 构建
  **零告警**（2026-09-20 第三轮把基线从 6 条降到 0 条）。**原生 Linux(glibc)/POSIX**
  在干净对象树上验证过 130 例；另外四条工具链（MSYS2/POSIX、
  **独立 Cygwin/POSIX**、MinGW64/Windows 静态、clang64/Windows 动态）
  仍是从干净对象树验证过 **127** 例的状态——第 128–130 例与本轮改动
  还没在 Windows 上复跑过，那四条腿改前的 6/6/8/8 条告警因此也仍是待重测值
  （根 README 的工具链表下方有同一条限定）
- 脚本腿：5 条 —— `build_official.sh` + `run_golden.sh`（逐字节黄金比对）、
  `run_interop.sh`（双向互操作 + 未修改的官方 `c_test`）、
  `run_cross_backend.sh`（2026-09-20 新增，`env_win` vs `env_posix` 字节互比）、
  `run_sanitizers.sh`（同一批证据过 ASan/UBSan）。吃现成归档的两条腿（黄金、
  互操作）与 `make` 本身都会核对三元组：参考库缓存按
  `build/interop/official/<triple>/` 分目录且以摘要认档，kvdb 对象树由
  `build/.target` 记账，不符即拒绝（doc/04 N-14、N-15）
- 内存安全：`-fsanitize=address,undefined -fno-sanitize-recover=all` 下
  130 例 + 官方 `c_test` + 黄金驱动 **零报告**，Windows 侧只有 clang64 能跑
  （三支 Windows gcc 直接链接探针均为 `cannot find -lasan`，doc/09 §4.3，那一侧
  仍是 127 例规模）、原生 Linux 侧用 gcc 12（`scripts/run_sanitizers.sh` 按宿主自动选）；
  泄漏维度 **2026-09-20 在原生 Linux 上闭环**：`run_sanitizers.sh` 默认开启
  LeakSanitizer（`SAN_DETECT_LEAKS=0` 关闭），首跑报 929,842 B / 201 个分配、五族全部修掉后
  `rc=0` 且零泄漏，负向对照 `rc=1`；Windows 侧仍无 LeakSanitizer 故仍不可测
  （doc/06 P2-9、doc/08 §11）。第三轮同一套检查又逼出一条被**全局单例抑制**掉的
  驱动侧泄漏（doc/07 §7 第三条），并顺带做了 C 语言使用检查：16 条额外诊断
  只有 `-Wcast-qual` 有输出，`-fanalyzer` 的 97 条全部判为模型局限（doc/08 §12）
- 跨引擎兼容性：官方 `db/c_test.c` 未修改直接通过；9 种确定性负载下与真实
  leveldb 产出的全部文件逐字节相同，且两引擎交叉读取摘要一致（见根 README）；
  snappy 压缩块**不**逐字节相同（kvdb 大 3.4%）但双向可解、摘要一致（doc/08 §7）
- 跨后端兼容性（2026-09-20 新增维度）：同一台 Windows 上 `env_posix` 与
  `env_win` 各建一份引擎，9/9 模式、33 个文件逐字节相同、58 项 PASS、0 FAIL；
  跨进程锁在两种后端下都出现"有人开成 / 有人被拒"（`ERROR_SHARING_VIOLATION`
  与 `EAGAIN`），doc/09 §5
- 开发：≈5.5 小时，估算 ≈37 万 token（方法学与局限见 03）
- 已修复缺陷：引擎级 12（实现期）+ 2（sanitizer 轮）+ 1（原生 Linux 轮）
  + 3（原生 Linux 泄漏轮 E-16/E-17/E-18，三条同属"C++ 有析构、C 的值类型没有"那一类）
  + 6（C 语言使用检查轮 E-19/E-20/E-21/E-22/E-23/E-25；E-24 是零告警基线整治，
  不算运行时缺陷）、
  平台级 6 + 9（Windows 工具链矩阵轮，全部是验证装置缺陷、引擎零改动）、
  测试级 11 + 5（T-12…T-16）+ 本轮 T-14/T-15/T-16、评审新增 1（repair 序列号）
