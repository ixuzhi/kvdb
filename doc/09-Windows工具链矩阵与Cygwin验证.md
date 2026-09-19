# 09 — Windows 工具链矩阵与 Cygwin 首轮验证

日期：2026-09-20。上一轮（doc/08）在原生 Linux 上跑完四条通路，随后把
Windows 三条工具链回归了一遍。本轮的出发点是那句"Windows 已验证"——把它
当成未经检验的前提，在一台**不同的** Windows 机器、不同的 shell、不同的
MSYS2 版本上重新走一遍，并把跨引擎脚本真正跑起来。

结论先放：

- 引擎本体在四条 Windows 编译通路（MSYS2 msys、独立 Cygwin、MinGW-w64、
  MSYS2 clang64）上全部通过，127/127 一例不缺，一条代码都没改。
- **但 `doc/08` 记录的那个 MSYS2 三元组（`x86_64-pc-cygwin`）在本机没有复现
  现形**：本机是 `x86_64-pc-msys`。四条跨引擎脚本里有三条以三元组判 POSIX，
  于是它们在本机 MSYS2 上一律拒绝启动——"Windows 四条腿全绿"这个结论在
  另一台 Windows 机器上不可复现。这是本轮最主要的问题，不是引擎问题，是
  判据问题（§3）。
- 顺带挖出另外八处同类 harness 缺陷（§3 的 N-7…N-12、N-14、N-15 与 §5 的
  N-13），以及**一件从未被测过的事**：
  `env_win.c` 与 `env_posix.c` 两个后端的产物是否逐字节相同（§5）。
  其中最后两处（N-14、N-15）方向与前面相反：不是脚本拒绝了一次好运行，
  而是脚本**接受了一次坏运行**——msys 与 Cygwin 共享同一棵 `build/`，
  缓存判据却只认"文件在"不认"是谁产的"，于是参考库归档串档、`make` 对着
  上一个命名空间的对象树报"Nothing to be done"。这两处是收尾复跑逼出来的。
- Cygwin 首次实机：127/127 + 参考库 + 黄金比对 + 互操作 + 官方 `c_test`
  全绿（§4）。Cygwin **不**预定义 `_WIN32`，所以它正确地走了 POSIX 后端；
  这一点原本要靠猜（§4.1）。

---

## 1. 环境矩阵

这台机器上同时存在**三套 POSIX 命名空间**，它们互相看不见对方的 `/usr`，
这是本轮一半困惑的来源：

| shell | 挂载根 | `/usr/bin` 是谁的 | 有无 gcc | 运行时 |
|---|---|---|---|---|
| Git for Windows（本仓库日常的"Git Bash"） | `C:\Program Files\Git` | Git 自带的 367 个 coreutils | **无**（`/usr/bin/gcc` 不存在） | MSYS 3.6.9 |
| MSYS2 的 MSYS 层（`D:\msys64`） | `D:\msys64` | MSYS2 的 545 个包 | gcc 13.3.0，`x86_64-pc-msys` | msys-2.0 3.5.7 |
| 独立 Cygwin（`D:\cyg64`，本轮新装） | `D:\cyg64` | Cygwin 的包 | gcc 14.4.0，`x86_64-pc-cygwin` | cygwin1 3.6.10 |

外加一套纯原生 Windows：**w64devkit**（`_tools/w64devkit/`，未安装即用的
绿色工具链），gcc 16.2.0，三元组 `x86_64-w64-mingw32`。

两条对本轮影响最大的观察：

1. **在 Git Bash 里用绝对路径调 MSYS2 的 gcc 是坏的**。它会报
   `no include path in which to search for stdio.h`——因为该 gcc 按自己的
   挂载命名空间解析 `/usr/include`，而 Git Bash 的 `/usr` 是
   `C:\Program Files\Git\usr`。必须用 `/d/msys64/usr/bin/bash.exe -lc` 起一个
   真正的 MSYS2 登录 shell 才能用它的工具链。
2. **`MSYSTEM=MINGW64` 不能用来判断"我在哪个 shell 里"**。本机 Git Bash 与
   MSYS2 登录 shell 都报 `MINGW64`（前者是 Git for Windows 自带的默认值，
   后者是继承来的），而 `/mingw64/bin` 里一个编译器都没有。这正是 doc/06
   P2-12 ① 那一类"拿宿主特征当编译器特征"的判据又一次踩空。
3. **同一棵树，两套路径写法**。MSYS2 里仓库是 `/g/code/kvdb`（它也提供
   `/cygdrive/g`），独立 Cygwin 只认 `/cygdrive/g/code/kvdb`——照抄 §6 的
   清单到 Cygwin 上会先撞一句 `cd: /g/code/kvdb: No such file or directory`。
   更要紧的是这两侧看到的是**同一份磁盘内容**：`build/` 里的对象、归档与
   参考库缓存对四个命名空间都可见且同名，而运行时互不兼容。N-14、N-15
   就是这条不对称的账单。
4. **`/tmp` 也是命名空间私有的**（本轮另两次撞到）：Git Bash、MSYS2、Cygwin
   各自的 `/tmp` 互不相见，所以"上一个 shell 写进 `/tmp` 的探针文件"在下一个
   里必然找不到；clang 在 `TMP`/`TMPDIR` 都没设时更是直接
   `unable to make temporary file`（§4.3）。

---

## 2. 本轮装了什么，以及怎么卸干净

三件都是验证前置条件，本机原来都没有。卸载按逆依赖点名删，不 `R uninstall`
整个树。

| 装了什么 | 命令 | 怎么卸 |
|---|---|---|
| MSYS2 的 git（跨引擎脚本要 `git rev-parse`/`diff`） | `pacman -S --noconfirm git` | `pacman -Rns git`（会带走 perl-* 依赖，需逐个点名） |
| 独立 Cygwin 到 `D:\cyg64`（`x86_64-pc-cygwin` 通路只有它能提供） | `setup-x86_64.exe -q -N --no-admin --site https://mirrors.ustc.edu.cn/cygwin/ --local-package-dir D:\cyg64-pkgs --root D:\cyg64 --packages gcc-core,gcc-g++,make,bash,coreutils,diffutils,findutils,sed,grep`，随后再 `--packages git` | 删 `D:\cyg64` 与 `D:\cyg64-pkgs` 两个目录即可；`--no-admin` 安装不写系统级注册表 ACL，但会在当前用户注册表留一条安装记录（`HKCU\Software\Cygwin\InstallLocation`），要彻底抹掉需一并删除 |
| MSYS2 clang64 工具链（恢复 ASan/UBSan 通路，上一轮之后被卸掉了） | `pacman -S --noconfirm --needed mingw-w64-clang-x86_64-toolchain` | `pacman -Rns` 同名包 |

镜像选择记一笔：Cygwin 官方站与 kernel.org 在本机只有 ~46 KB/s，
`mirrors.ustc.edu.cn` 实测 15 MB/s；kernel.org 那一次跑到一半被换掉，
`D:\cyg64` 是用 USTC 装完的。MSYS2 侧 `mirrorlist.mingw` 首行已是 TUNA，
但本轮 clang 包实际吞吐仍只有 ~46 KB/s——**同一家镜像对不同仓库的限速
不同，不要假定换镜像一定解决**。

---

## 3. 八处 harness 缺陷 N-7…N-12、N-14、N-15（全部已修，且都有负向对照）

这一节的这几条都不是引擎缺陷，而是"验证装置把自己的前提当成了事实"。
它们共同的后果是：**在一台环境不完全吻合的机器上，一次什么都没验证的运行
看起来像一次通过的运行。**（N-13 记在 §5，它属于新腿自己的坑。）

### N-7 POSIX 判据只认 `-cygwin`，而 MSYS2 报的是 `-msys`

- **现象**：`bash scripts/build_official.sh` 在 MSYS2 登录 shell 里
  `exit 2`，报 `Expected a POSIX compiler ... got x86_64-pc-msys`。
- **根因**：`build_official.sh` 与 `run_interop.sh` 用
  `[[ $("$CC" -dumpmachine) == *-cygwin || ... == *linux* ]]` 判"这是不是
  POSIX 编译器"。doc/08 记的三元组是 `x86_64-pc-cygwin`，本机 MSYS2
  （gcc 13.3.0 / msys 运行时 3.5.7）的 `/usr/bin/gcc -dumpmachine` 是
  `x86_64-pc-msys`。两种拼法都真实存在过，判据不能只写一种。
- **修复**：三处判据统一改为 `*linux*|*-cygwin|*-msys`；`run_sanitizers.sh`
  的"Windows 家族但编译器不是 clang"早退守卫同样补 `*msys*`（否则
  `CC=gcc` 会穿过守卫、几分钟后死在链接期，报一个没人会读的错）。
- **验证**：改后 `build_official.sh`/`run_golden.sh`/`run_interop.sh` 在
  MSYS2 上三条腿全 `rc=0`（§6）。`*-msys` 这个拼法在 Cygwin 上不会出现，
  所以旧通路不受影响（Cygwin 走的是 `*-cygwin` 分支，§4）。
- **为什么 Makefile 没中这个招**：它判的是"三元组里含不含 `mingw`/`windows`"，
  `x86_64-pc-msys` 两者都不含 → 落到 POSIX 分支，选对了后端。**同一件事
  用"排除法"写就不会漏，用"枚举法"写就会**——这条差异值得记住。

### N-8 `export PATH=/usr/bin:/bin` 把 git 也一起扔了

- **现象**：脚本连自己的第一条断言都没到就死：
  `scripts/build_official.sh: 行 26: git: 未找到命令`，`rc=127`。
- **定位**：第 16 行把 PATH 硬钉成 `/usr/bin:/bin`。初衷是对的（防止
  `/usr/bin/gcc` 被别的命名空间的 gcc 抢走），但 MSYS2 的默认安装**不含
  git**（`pacman -Q git` 未找到；Git for Windows 的 git 在这个命名空间外），
  于是钉死 PATH 等于钉死"没有 git"。
- **修复**：改为 `export PATH=/usr/bin:/bin${PATH:+:$PATH}`——仍然把 msys 的
  `/usr/bin` 排在最前（所以绝对工具路径与 `gcc` 的解析结果不变），只是把
  继承来的 PATH 留在尾部当兜底；并在脚本开头加一条 `command -v git` 前置
  检查，缺失时给出三家包管理器的安装命令而不是 rc=127。
  `run_golden.sh` 也改了：它自己钉 PATH 后才调 `build_official.sh`，
  覆盖式赋值会把尾部清掉，只改被调用方是没用的。
- **验证**：装 git 前 `rc=127`，装后 `rc=0`；`command -v git` 分支在
  `PATH=/nonexistent` 下确实吐出提示并 `exit 2`。

### N-9 "官方源码被改动"守卫把嵌套子模块的指针漂移算成改动

- **现象**：守卫 `git -c core.autocrlf=true -C leveldb diff --quiet HEAD --`
  在一个正常仓库上判定"tracked files modified"并拒绝构建参考库。
- **定位**：脏的是 `third_party/benchmark`——**嵌套**子模块的 gitlink，因为
  它从未被 checkout 到官方 pin 的提交。这个目录里的任何 .cc 都不在
  `build_official.sh` 的源文件清单里。
- **修复**：`--ignore-submodules=all`。守卫的语义收窄成它本来该有的意思：
  "有没有人改过这个脚本要编译的 C++ 源码"。
- **负向对照**：往 `leveldb/db/dbformat.cc`（真的会编译的文件）追加一行，
  守卫仍然 `rc=2` 拒绝；`git checkout --` 还原后又 `rc=0`。
  放宽的是噪声，不是判据。

### N-10 缺 `g++` 时的提示会把你指向一个脚本随后拒绝的包

- **现象**：`[[ -x "$CXX" ]]` 失败时打印
  `MSYS2: pacman -S mingw-w64-ucrt-x86_64-gcc`。
- **根因**：这个包装到 `/ucrt64/bin`，**不产生** `/usr/bin/g++`；就算产生了，
  它的三元组是 `x86_64-w64-windows-gnu`，会被本脚本第 31 行的 POSIX 断言
  拒绝。照提示做，两步之后仍跑不起来。
- **修复**：提示改为 `MSYS2 (MSYS shell): pacman -S gcc`、
  `Cygwin: setup -P gcc-g++`、`Debian: apt-get install g++`，并加一行
  说明"本脚本用 `/usr/bin/g++`，包必须提供这个路径"。`run_interop.sh`
  同一处同改（顺把它原来漏到 stdout 的那行错误改到 stderr）。

另有一处措辞级修正：`env_posix.c` 头部注释写着"本后端从未在本项目
（Windows）的开发环境里执行过，按 best-effort 看待"——这在 doc/06 P2-1
记录 MSYS2 与原生 Linux 已实机验证之后就过期了，且过期方向危险（它会让人
不敢用这条通路）。已改为陈述事实并点出"`_WIN32` 判定必须排除掉那些同时
提供 POSIX 语义的平台"这一真实约束。

---

### N-12 本地化的 shell 让 `grep warning:` 静默返回 0

收尾复跑三条 Windows 通路时顺手数了一遍各家的警告，得到一张看起来很舒服的
表：msys **0** 条、Cygwin 6 条、w64devkit 8 条。"MSYS2 那条工具链零警告"像是
一个值得记下来的平台差异。

它是假的，而且假得和 §3 前四条同一个形状——**判据把自己的前提当成了事实**，
这里的前提是"编译器用英文说话"：

```
$ bash -lc 'gcc -dumpmachine'           # 登录 shell source 了 /etc/profile
x86_64-pc-msys                          #   里面把 LANG 设成了 zh_CN.UTF-8
$ bash -lc 'make …' | grep -c 'warning:'
0                                       # gcc 打的是"警告："，不是 "warning:"
$ bash -c 'export LC_ALL=C; make …' | grep -c 'warning:'
6                                       # 与 Cygwin 完全相同的 6 条
```

`-lc`（登录 shell）与 `-c`（非登录）的差别就是 `/etc/profile` 的差别，而本轮
所有 MSYS2 通路都是按 §6 的清单用 `-lc` 跑的——所以这个 0 会在任何照抄清单的
人身上复现。同一份源码、同一个编译器，只是 stderr 换了一种语言。

等 `LC_ALL=C` 之后三家的真实基线是：

| 通路 | 编译器 | `-Wall -Wextra` 警告 |
|---|---|---|
| MSYS2 msys | gcc 13.3.0 | 6 |
| 独立 Cygwin | gcc 14.4.0 | 6（与上一行逐条同形） |
| w64devkit | gcc 16.2.0 | 8 = 上述 6 条 + 2 条 `-Wunused-but-set-variable`（更新的 gcc 才查） |

也就是说 **Windows 的 POSIX 两层与 Linux 在这一项上没有差异**：doc/08 §4 记的
那 6 条跨 gcc 12/13/14 完全一致，第 7、8 条是 gcc 15+ 才有的新检查，与平台无关。

处置：不改代码（P3-3 已经把这 6 条记成既有质量问题），只在文档里把"怎么数
警告"写对。**凡是靠 grep 编译器输出做判据的地方，都要先 `export LC_ALL=C`**——
`run_golden.sh` 那几条比摘要的脚本不受影响（它们比的是自己程序的输出，里面
没有本地化的诊断），但任何人写"构建必须零警告"这类 CI 守卫时都会先踩这一脚。

---

### N-14 参考库归档跨命名空间串档

前三条都是"脚本拒绝了一次好运行"。这一条相反：**脚本接受了一次坏运行**，
而且是在收尾复跑时才炸。

```
MSYS2, bash scripts/run_golden.sh
  MAKE rc=0 / OFFICIAL rc=0
  ld: build/interop/official/lib/libleveldb_official.a(db_db_impl.cc.o):
      undefined reference to `__pthread_normal_mutex_initializer_np'
  official link failed
```

同一份脚本、同一台机器一小时前刚 rc=0，所以第一反应是"msys 的 pthread 支持
坏了"。三条命令就翻掉：

| 检查 | 结果 | 含义 |
|---|---|---|
| `nm -u libleveldb_official.a \| grep normal_mutex` | 命中 | 归档**需要**这个符号 |
| `nm -u /usr/lib/libpthread.a \| grep -c normal_mutex` | `0` | msys 的 libpthread **不提供**它 |
| `ls -la official/` | 归档 mtime=01:09；`.built-…-13.3.0-13.3.0`(00:55) 与 `.built-…-14-14`(01:09) **两个空标记并存** | 01:09 是 Cygwin 那一轮，它把归档覆盖了 |

`__pthread_normal_mutex_initializer_np` 是 Cygwin 的 pthread 桩，msys 里对应
物是另一个名字。也就是说 MSYS2 链接的是 **Cygwin 编出来的参考库**。根因有三层，
缺一层都不会发生：

1. 标记文件名里没有目标三元组——脚本上一行刚算出 `TRIPLE` 却没用；
2. 输出路径只有一个共享的 `build/interop/official/`，msys 与 Cygwin 在它上面
   完全可见（运行时却互不可见，这是 §1 那张环境矩阵表的核心不对称）；
3. 命中判据是"标记文件存在"，而不是"这个归档是那次构建的产物"。

修复对应三层：按三元组分目录（`official/<triple>/{obj,include,lib,probe}`）、
标记文件名带三元组、标记内容改成**归档摘要**并在命中前重算；没有 sha 工具时
写一个永不匹配的 `unverified`（⇒ 每次重建），绝不判为"缓存有效"——这与
doc/08 §5 第 4 条"`sha256sum` 缺失时逐字节判据 fail-open"是同一条规则。

验证与负向对照：

- Cygwin 侧把旧产物迁进新目录后 → `reusing cached official build`（缓存命中
  路径本身被验到，不是只验了重建）。
- 给归档追加一个字节再跑 → `cache stamp does not match the archive;
  rebuilding`，重建后摘要变化，第三次跑回到 `reusing`。
- MSYS2 建出自己的 `x86_64-pc-msys/`（39 秒）后黄金腿 rc=0：
  **109 PASS / 33 SAME / 1 DIFF / 1 INFO**，唯一 DIFF 是 `levels` 的布局差异
  （该模式本就按"布局可变、读取不可变"判），四条交叉摘要全部 `agrees`。
  留档 `build/interop/golden-20260920-021110`。
- 同一棵 msys 树上再跑 `RUN_C_TEST=1 bash scripts/run_interop.sh` → rc=0，
  未改动的官方 `c_test` 直连 kvdb `PASS`（留档 `run-D7SUdnww`）。这是本轮
  MSYS2 互操作腿第一次跑到终点。

### N-15 `make` 在共享对象树上报"Nothing to be done"

N-14 修完顺手查同一类问题的另一半：kvdb **自己**的对象树是不是也跨命名空间共享。

```
$ /d/msys64/usr/bin/bash.exe -lc 'cd /g/code/kvdb && make -j4'   # msys 建完整棵树
$ /d/cyg64/bin/bash.exe -lc 'cd /cygdrive/g/code/kvdb && make -n'
make: Nothing to be done for 'all'.                                # ← 危险的那一行
```

`OBJDIR` 默认 `build`，四种三元组写出的 `.o` **文件名完全相同**，而 make 只按
mtime 判新旧，看不见"是谁编的"。于是 Cygwin 的 shell 里 `make` 得到"无需构建"，
`make test` 跑的是需要 `msys-2.0.dll` 的 msys 二进制——Cygwin 的 PATH 上没有它，
Windows 弹的是"找不到 DLL"。更糟的是部分重建：刚改过的那几个文件由 Cygwin
编，其余仍是 msys 的，一桶混合对象在 ld 报出的未定义符号读起来像引擎缺陷。

修复把"是谁编的"变成树上的一条记录：`$(OBJDIR)/.target` 由 `$(LIB)` 的 recipe
写入当前 `$(CC) -dumpmachine`，Makefile **解析期**比对，不一致就 `$(error …)`
并指名 `make clean`。三点设计说明：① 记在 `$(OBJDIR)` 而不是仓库根，因此
`run_sanitizers.sh`（`build/san`，动手前 `rm -rf`）与 `run_cross_backend.sh`
（`$RUN/obj/<name>`，每次全新）天然不受影响；② `clean` 目标本身豁免——它就是
补救手段，不豁免就成了打不开的死锁；③ 脚本侧同源，`run_golden.sh`/
`run_interop.sh` 吃现成归档时除 mtime 外再比 `.target` 与本 shell 的三元组，
没有 `.target` 只表示"早于该记录"，不作判据（不因此拒绝旧的合法构建）。

验证：

| 场景 | 结果 |
|---|---|
| Cygwin `make` / `make -n`，树是 msys 建的 | 都 `rc=2`：`Makefile:72: *** build/ holds objects built for x86_64-pc-msys but gcc targets x86_64-pc-cygwin; run 'make clean' before switching toolchains.` |
| w64devkit gcc（第三种三元组） | 同样拦下；`make clean` 正常通过 |
| Cygwin clean 重建 | `x86_64-pc-cygwin`，127/127，黄金腿 rc=0 |
| w64devkit clean 重建 | `x86_64-w64-mingw32`，127/127，静态链接 |
| MSYS2 脚本对着 Cygwin 建的树 | `run_golden.sh`/`run_interop.sh` 各自 `rc=2`，同一句 remedy（留档 `golden-20260920-021054`） |
| MSYS2 clean 重建后 `run_cross_backend.sh` | rc=0（`Makefile` 改动没有打断这条腿），留档 `cross-20260920-021255` |

---

## 4. Cygwin 首轮

Cygwin 是"mingw / msys2 / **cygwin**"这个提法里唯一从未被实机跑过的一条：
之前所有 Windows 证据都来自 MSYS2 的两个层加 w64devkit。

### 4.1 关键前置问题：Cygwin 到底定不定 `_WIN32`

这决定后端选择对不对，而 `env_posix.c`/`env_win.c`/`port.h`/`port.c`
**全部**以 `#if defined(_WIN32)` 为唯一开关。风险很具体：如果 Cygwin 定义了
`_WIN32`，那么

- `env_posix.c` 整体编译成空目标文件，`env_win.c` 反而被编进来；
- 而 Makefile 是按三元组选后端的，`x86_64-pc-cygwin` 不含 `mingw`/`windows`
  → 它以为自己在建 POSIX 通路，于是**构建系统的意图与代码的意图相反**；
- `env_win.c` 用 `CreateFileA`，它看不懂 Cygwin 的 `/cygdrive/...` 路径。

实测（`gcc -dM -E - </dev/null`，默认 std 与 `-std=c11` 都试）：

```
#define __CYGWIN__ 1
#define __unix__ 1
```

**`_WIN32` 不预定义**（`fdatasync`、`O_CLOEXEC` 齐备）。所以 Cygwin 正确落到
`env_posix.c` + pthread，与 Makefile 的判断一致；原本准备的那个
"把 `_WIN32` 改成 `LDB_PLATFORM_WIN32` 并显式排除 Cygwin/MSYS"的重构
**不需要做**，也没有顺手加进去。MSYS2 的 msys 层同理测过：它
`__CYGWIN__` 与 `__MSYS__` 都定义，但 `_WIN32` 不定义。

### 4.2 四条通路

`D:\cyg64` 装好后（含 git），全部以 Cygwin 自己的 shell 与自己的归档跑：

| 通路 | 结果 | 留档 |
|---|---|---|
| `make OBJDIR=build_cygwin` + 127 用例 | **rc=0，127/127**；6 条 `-Wall -Wextra` 警告（1 `discarded-qualifiers`、1 `unused-variable`、4 `unused-function`），与 doc/08 §4 记的 Linux gcc 12 那一组逐条同形，**没有一条是 Cygwin 独有的** | `build_cygwin/` |
| `scripts/build_official.sh` | **rc=0**，`Config: FDATASYNC=1 FULLFSYNC=0 O_CLOEXEC=1 CRC32C=0 SNAPPY=0 ZSTD=0` | `build/interop/official/` |
| `scripts/run_golden.sh` | **rc=0**：33 个文件逐字节 SAME，109 条 PASS；唯一的 `DIFF` 是既有的探索性 `levels`（文件集不同、4 路摘要一致） | `build/interop/golden-20260920-010924/` |
| `RUN_C_TEST=1 scripts/run_interop.sh` | **rc=0**，未修改的官方 `db/c_test.c` 直连 kvdb `PASS` | `build/interop/run-Ivnad9xj/` |

跑法上有一处必须记：跨引擎腿吃的是 `KVDB_LIB=` 指定的归档，而**它必须与
参考库同一命名空间**。拿 MSYS2 建的 `build/libleveldb.a` 去和 Cygwin 建的
参考库链接，会把 msys-2.0.dll 与 cygwin1.dll 混进同一个进程。本轮用
`KVDB_LIB=$PWD/build_cygwin/libleveldb.a` 隔离。

这句告诫当时只是"跑法上记一下"，没有任何东西挡着人忘——而收尾那一轮就是
忘了之后爆的（参考库那一侧，N-14）。同一判据现在已经落成代码：归档自己声明
是谁产的（`build/.target`，N-15），两条吃现成归档的腿在动工前比三元组，
`build_official.sh` 按三元组分目录、并以摘要认档。负向对照在 §9.1 的表里。

### 4.3 sanitizer：Cygwin 也拿不到，这回是穷尽式确认

`-fsanitize=address` 与 `-fsanitize=undefined` 在 Cygwin gcc 14.4.0 上都
`cannot find -lasan` / `-lubsan`。不止"没装"：抓 Cygwin 的 `setup.ini`
（22 MB，16,087 个包）按包名筛 `asan`，**命中 0**。所以 doc/07 那句
"Windows 上只有 clang64 能跑 sanitizer"现在覆盖到 MSYS2-msys、w64devkit、
独立 Cygwin 三类 gcc 目标，且 Cygwin 是仓库层面不可能，不是本机没装。
（MSYS2 的 mingw64/ucrt64 层本机根本没装 gcc——`/d/msys64/mingw64/bin/gcc.exe`
不存在，MinGW-w64 那一支由 w64devkit 代表，见 §1。）

挖包数据库终究是间接证据，所以补一组直接探针：同一个只含 `int main(){return 0;}`
的文件，拿去让 Windows 家族里每一支编译器都试一次 `-fsanitize=address,undefined`。

| 编译器 | 三元组 | 链接结果 |
|---|---|---|
| MSYS2 msys gcc 13.3.0 | `x86_64-pc-msys` | `cannot find -lasan` / `-lubsan` |
| w64devkit gcc 16.2.0 | `x86_64-w64-mingw32` | `cannot find -lasan` / `-lubsan` |
| Cygwin gcc 14.4.0 | `x86_64-pc-cygwin` | `cannot find -lasan` / `-lubsan`，且 `/usr/lib` 下无任何 `libasan` |
| MSYS2 clang 22.1.8 | `x86_64-w64-windows-gnu` | **链接成功** |

四支里三支在同一处失败，说明这是 Windows 的 gcc 移植一贯不带
`libsanitizer`，而不是某个环境少装了包。顺带记一个会让 clang 也"假失败"的
坑：clang 在 `TMP`/`TMPDIR` 都没设的 shell 里会报
`unable to make temporary file`，看上去像 sanitizer 坏了，其实只是找不到临时目录
（§8 的调用形式里 `bash -lc` 会 source profile，直接 `bash -c` 就不会）。
`run_sanitizers.sh` 对这件事有显式处理，检测到 `TMP` 未设时自己指到
`D:\msys64\tmp` 并打印一行说明。

---

## 5. 新增通路：`scripts/run_cross_backend.sh`（两个 Env 后端互比）

本轮补的最大空白。此前的跨引擎证据（黄金比对、互操作）**全部**由
`env_posix.c` 产出——因为那两个脚本按设计拒绝原生 Windows 编译器。于是
`env_win.c` 写出的字节从来没有和 `env_posix.c` 写出的字节比对过；
`CreateFileA` 的刷新顺序、`%s\\*` 的目录遍历、路径分隔符这类差异，
格式比对工具是看不见的。

这条腿把同一份源码建两遍（一遍 POSIX 三元组、一遍 `x86_64-w64-mingw32`），
各配一份 `tests/interop/golden_driver.c`，然后对每个负载：两边各写一个库 →
逐文件逐字节比对 → 四种"读方 × 被读方"组合交叉打开，要求摘要全等。

顺带把 doc/06 P2-6（"Windows 锁实现是进程内计数 + 独占打开，跨进程冲突
依赖共享模式语义"——从未测过）也测了：对同一个库并发起 8 个进程。

首轮结果（`build/interop/cross-20260920-012453/`，`rc=0`）：

| 项 | 结果 |
|---|---|
| 9 个负载的逐字节比对 | **33 个文件全部 SAME，0 DIFF**（`000005.ldb`/`000003.log`/`CURRENT`/`MANIFEST-*`） |
| 交叉读取 | 4 路摘要全等（每模式） |
| 跨进程锁（原生 Windows 后端） | **PASS**：5 个打开成功、3 个被锁拒绝（win32 error 32 共享冲突），成功者摘要一致 |
| 跨进程锁（POSIX 后端，同一台机器） | **PASS**：4 开 / 4 拒（`flock`），成功者摘要一致 |

两个后端的锁语义不同（一个 `flock(2)`、一个共享模式），但**都真的拒绝**了
第二个持有者，且没有任何一次失败是锁以外的原因——这是 P2-6 要的判据。
脚本对"零冲突"也判 FAIL：8 个并发进程里通常约一半碰撞，一次都不碰撞说明
探针失效了，而不是平台变好了。

写这条腿以及复跑它时踩到三个自己的坑，都值得记：

1. **裸 `wait` 会死锁**。脚本用 `exec > >(tee run.log)` 留档，于是 shell 的
   后台任务里有个 `tee` coprocess；`wait` 不带参数会连它一起等，而它要等
   脚本关闭 stdout 才退出。首轮运行在模式循环全部通过之后卡死在这里。
   必须收集 `$!` 再 `wait "${pids[@]}"`。
2. 数据库路径参数**全部用相对路径**，并在证据目录里 `cd` 进去再跑：POSIX
   命名空间的进程看到 `/g/code/...`，原生 Win32 进程会把同一个字符串当成
   "当前盘根 + g/code/..."。相对路径是唯一两边语义一致的写法。
3. 收尾复跑时又添了一条（N-13）：**`TMP` 必须是 Win32 形状**。这条腿故意把
   原生 Windows 编译器拉进来，而它向 Win32 要 scratch 目录；给它 MSYS 写法的
   `/msys64/tmp`，会被按错误的根改写成 `D:\msys64\msys64\tmp`，于是只有 Windows
   半边构建失败——看上去正是这条腿想查的"两个后端不一致"。`run_sanitizers.sh`
   从 P2-11② 起就带 `TMP` 兜底，这条腿当时忘了带。现在动工前先用 `CC_WIN`
   真编一个空文件：设了但路径错的 `TMP` 和没设一样致命，只有真编一次才看得见。

---

## 6. 复跑清单

**先 `export LC_ALL=C LANG=C` 再跑**——MSYS2 的登录 shell 会把 LANG 设成
`zh_CN.UTF-8`，此后任何 `grep 'warning:'` 之类的判据都会静默拿到 0（N-12）。

```bash
# 全部在真正的 MSYS2 登录 shell 里（Git Bash 里调 MSYS2 的 gcc 是坏的，§1）
/d/msys64/usr/bin/bash.exe -lc 'cd /g/code/kvdb && make clean && make -j4'
/d/msys64/usr/bin/bash.exe -lc 'cd /g/code/kvdb && bash scripts/build_official.sh'
/d/msys64/usr/bin/bash.exe -lc 'cd /g/code/kvdb && bash scripts/run_golden.sh'
/d/msys64/usr/bin/bash.exe -lc 'cd /g/code/kvdb && RUN_C_TEST=1 bash scripts/run_interop.sh'
/d/msys64/usr/bin/bash.exe -lc 'cd /g/code/kvdb && bash scripts/run_cross_backend.sh'   # §5

# MinGW-w64（绿色工具链，免安装；在 Git Bash 里即可）
PATH=/g/code/kvdb/_tools/w64devkit/w64devkit/bin:$PATH \
  make -j4 CC=gcc AR=ar OBJDIR=build_mingw BINDIR=build_mingw && ./build_mingw/kvdb_tests.exe

# Cygwin：用自己的 shell、自己的归档（§4.2 的命名空间告诫）
/d/cyg64/bin/bash.exe -lc 'cd /cygdrive/g/code/kvdb \
  && make -j4 OBJDIR=build_cygwin BINDIR=build_cygwin && ./build_cygwin/kvdb_tests \
  && KVDB_LIB=$PWD/build_cygwin/libleveldb.a bash scripts/run_golden.sh'

# clang64 的 ASan/UBSan（§8）：必须是 MSYS2 的登录 shell，且手工把 /clang64/bin 顶前
MSYSTEM=CLANG64 /d/msys64/usr/bin/bash.exe -c \
  'export PATH=/clang64/bin:/usr/bin:/bin; cd /g/code/kvdb && bash scripts/run_sanitizers.sh'
```

两点关于清单本身的说明：

- 上面按工具链分开了对象目录（`build_mingw`、`build_cygwin`），这是 N-15
  的**人工绕法**。现在 Makefile 自己会记账（`$(OBJDIR)/.target`），所以复用
  默认的 `build/` 也是安全的：换了三元组就会 `rc=2` 并让你 `make clean`。
  分开仍然更快（两条通路可以并存着各自增量构建），但不再是"不分开就会拿到
  错的东西"。
- 参考库缓存同理，只是它由 `build_official.sh` 自己管：现在落在
  `build/interop/official/<triple>/`，msys 与 Cygwin 各存一份、各自命中，
  不再互相覆盖（N-14）。清单里那条 `build_official.sh` 因此可以从任意一条
  腿开始跑，不必先手工删缓存。

---

## 7. 延后项，以及本轮之内就闭环的两条

W1（clang64 通路）在本轮之内闭环，结论移到 §8；W2 是写着写着被 doc/08 的
旧记录回答掉的，留在表里划掉。W3–W5 仍是开着的。

| # | 项 | 为什么没做 |
|---|---|---|
| W1 | ~~clang64 的 ASan/UBSan 通路~~ | 本机原本没有 clang64（上一轮之后被卸载），重装 400 MB+ 吞吐仅 ~46 KB/s。**已在本轮闭环，见 §8** |
| W2 | ~~`x86_64-pc-cygwin` 与 `x86_64-pc-msys` 两种拼法是否同一 MSYS2 世代会变~~ **本轮之后已答**：会变。doc/08 §8.1 记的是 2026-09-19 那台机器的 MSYS2 msys 层 = gcc 15.2.0 / `x86_64-pc-cygwin`，本机同一层 = gcc 13.3.0 / `x86_64-pc-msys`。判据两种都收，所以这个答案只用来解释历史，不影响正确性 |
| W3 | `run_sanitizers.sh` 在 Cygwin 上跑 | Cygwin 无 sanitizer 运行时（§4.3），跑不了；脚本的 `*cygwin*` 分支会正确早退 |
| W4 | 把 `sst-snappy` 并进黄金比对（doc/06 P2-8） | 需要先定参考库是否默认 `HAVE_SNAPPY=1`，与本轮平台主题无关 |
| W5 | 多进程互斥的"同进程多实例"分支（doc/06 P2-6 的另一半） | §5 测的是跨进程。同进程内两个 `leveldb_open` 走的是进程内计数锁，仍未单独覆盖 |

---

## 8. 补记：clang64 的 ASan/UBSan 通路（W1 已闭环）

W1 写下来时这台机器上还没有 clang64——上一轮之后被卸载了，重装要在 ~46 KB/s
的镜像上吞 400 MB+。换 USTC 镜像后装完，本节记录复跑结果，所以 Windows 的
内存安全通路从"上一轮的结论"变回"本轮可复现的结论"。

装的是 `mingw-w64-clang-x86_64-toolchain`（clang 22.1.8，target
`x86_64-w64-windows-gnu`）。关键的前置检查不是 `clang --version`，而是运行时
库在不在场——脚本早退时说的就是这件事：

```
$ ls /clang64/lib/clang/*/lib/windows/ | grep -iE 'asan|ubsan'
libclang_rt.asan_dynamic-x86_64.dll.a
libclang_rt.asan_dynamic_runtime_thunk-x86_64.a
libclang_rt.ubsan_standalone-x86_64.a
libclang_rt.ubsan_standalone_cxx-x86_64.a
```

调用形式必须是"MSYS2 的 bash + 手工把 `/clang64/bin` 顶到 PATH 前面"：

```bash
MSYSTEM=CLANG64 /d/msys64/usr/bin/bash.exe -c \
  'export PATH=/clang64/bin:/usr/bin:/bin; cd /g/code/kvdb && bash scripts/run_sanitizers.sh'
```

`MSYSTEM=CLANG64` 只是标注（§1 的教训在这里同样成立：在 Git Bash 里加这个
前缀不会改 PATH，脚本会直接报 "clang is not on PATH"）。

结果 `rc=0`，证据留在 `build/san-runs/interop-20260920-013321/`：


| 项 | 结果 |
|---|---|
| 插桩构建 | PASS，`build/san/libleveldb.a` |
| 单测 | PASS，127 tests / 0 failed，且无任何 ASan/UBSan 报告 |
| 官方未改动的 `c_test` | PASS（16 phases） |
| 黄金驱动 create + 10 种模式 verify | 全 PASS，无任何报告 |
| 泄漏维度 | 平台不支持，脚本如实打印 `detect_leaks is not supported on this platform` 后继续（doc/06 P2-9 仍是开着的） |

这一轮顺带白捡到一个跨腿一致性检查：ASan 构建在 `sst-bigblock` 上算出的
摘要

```
VERIFY live=500 get-digest=714b697132214d29 fwd=fb19636d3ee54fe1 rev=8605be4e1773c06d
```

与 §5 里两个未插桩后端（`env_posix`、`env_win`）在同一种负载上算出的摘要
**逐字符相同**。也就是说 sanitizer 只改变了内存布局与检查，没改变任何一字
节的落盘内容——这正是这条腿应该有的性质，此前没人把它当判据看过。

---

## 9. 收尾：在最终代码上把三条 Windows 通路重跑一遍

前面所有改动落定之后，用干净的对象树把 Windows 三条通路重建重跑（这一遍的
意义是"文档里那句 127/127 描述的就是仓库现在这个样子"）：

| 通路 | 编译器 / 三元组 | 单测 | `-Wall -Wextra` 警告 |
|---|---|---|---|
| w64devkit | gcc 16.2.0 / `x86_64-w64-mingw32` | **127/127** | 8 |
| MSYS2 msys（`LC_ALL=C`） | gcc 13.3.0 / `x86_64-pc-msys` | **127/127** | 6 |
| 独立 Cygwin | gcc 14.4.0 / `x86_64-pc-cygwin` | **127/127** | 6 |

跨后端腿也重跑了一遍（`build/interop/cross-20260920-015224/`，`rc=0`，仍是
33 文件 SAME / 58 PASS / 0 FAIL）。这一遍顺带产出两件小事：

1. **锁探针的拆分是会变的**：posix 侧从首轮的 4 开成 / 4 被拒变成 3 / 5。
   这正好说明为什么 `probe_lock` 的判据是"两边都不为零"而不是某个具体数字——
   八个短命进程的重叠窗口本来就随调度变，写死数字的断言会有一天在无意义的原因
   下变红。**开成者之间的摘要必须一致**这一条才是不变量，它三轮都成立：posix
   侧 4/4 → 3/5 → 5/3，windows 侧 5/3 → 5/3 → 6/2（第三轮见 §9.1）。
2. **N-12 与 N-13 都是这一遍抓出来的**，而且都出自同一个更一般的坑：
   `bash -lc`（登录 shell，source `/etc/profile`）与 `bash -c` 不是同一个环境。
   前者给 `LANG=zh_CN.UTF-8`，于是 `grep 'warning:'` 恒为 0；后者不给 `TMP`，
   于是原生 Windows 编译器找不到 scratch 目录，只有 Windows 半边构建失败，
   看上去像"两个后端不一致"。§6 的清单因此补上了 `export LC_ALL=C`。
   N-13 的处置是给 `run_cross_backend.sh` 补上 `run_sanitizers.sh` 早就带着的
   `TMP` 兜底，并在动工前用 `CC_WIN` 真编一个空文件试探——设了但路径错的 `TMP`
   和没设一样致命，只有真编一次才看得见。负向对照：`TMP=/msys64/tmp` 下现在
   4 行内给出诊断并 `exit 2`（改前是建到一半的 `Error 3`）。

---

## 9.1 第二遍收尾：N-14 / N-15 落地之后再跑一次

上面那一遍之后又改了两处缓存判据（§3 的 N-14、N-15），所以"文档里的数字
描述仓库现在的样子"这句话作废了，重跑一遍。四棵树各自 `make clean` 起步：

| 通路 | 单测 | 参考库 | 黄金比对 | 互操作 + 官方 `c_test` | 跨后端 |
|---|---|---|---|---|---|
| MSYS2 msys | **127/127** | 新建 39 s → `official/x86_64-pc-msys/` | **rc=0**（109 PASS / 33 SAME / 1 DIFF / 1 INFO）→ `golden-20260920-021110` | **rc=0** → `run-D7SUdnww` | **rc=0** → `cross-20260920-021255` |
| 独立 Cygwin | **127/127** | 迁移后 `reusing cached` | **rc=0** → `golden-20260920-021013` | — | — |
| w64devkit | **127/127**（静态） | 不适用（脚本拒绝非 POSIX 编译器） | — | — | 作为上一行的 windows 半边 |

四处负向对照（守卫不咬人的话就不算守卫）：

| 注入 | 期望 | 实测 |
|---|---|---|
| 给 Cygwin 的参考归档追加一个字节 | 不认缓存、重建 | `cache stamp does not match the archive; rebuilding`，重建后摘要变化，第三跑回到 `reusing` |
| Cygwin 的 `make` / `make -n` 落在 msys 树上 | 拒绝 | `Makefile:72: *** build/ holds objects built for x86_64-pc-msys but gcc targets x86_64-pc-cygwin; run 'make clean' before switching toolchains.` `rc=2` |
| w64devkit 的 `make` 落在同一棵 msys 树上 | 拒绝（第三种三元组） | 同上；`make clean` 不受守卫影响，正常通过 |
| MSYS2 的两条脚本吃 Cygwin 建的归档 | 拒绝 | 各自 `rc=2`，同一句 remedy；留档 `golden-20260920-021054`（空壳，正是拒绝的证据） |

结论没变，只是这一次是"最终装置"给出的：**引擎侧本轮仍是一行未改**，Windows
三条工具链 + 跨后端 + clang64 内存安全五条腿都在仓库现在这个样子上跑通。

