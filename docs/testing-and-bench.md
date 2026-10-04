# 判据体系与性能口径

> 每把尺子的来历与踩过的坑在 [port/README.md](../port/README.md) 与
> [bench/README.md](../bench/README.md)；本文是"怎么跑、判什么、哪些数不能比"。

## 尺子总表（arm64 原生腿）

| 脚本 | 判什么 | 现行结果 | 大约耗时 |
| --- | --- | --- | --- |
| `bench/test-a64.sh` | 语言能不能跑：16 行**带期望值**（递归/多函数/static/内建），错了就红 | 16/16 | 秒级 |
| `bench/test-ops.sh` | JIT 每族指令算得对不对：83 行按指令族（含 12 行 NaN、9 行数组、越界一夹）。**不手算期望值**，判"三条路逐行相同" | 83 行 0 差 | 秒级 |
| `bench/render-a64.sh` | 出图：4 份代表 `.pss` 出 PNG，抽样不同颜色数 ≥ 2（全黑=没画上去） | 4 过 0 红 | ~1 分钟 |
| `bench/gui-a64.sh` | GUI：真窗口、非黑像素 > 0、标题栏 fps > 0（每份 `SECS` 秒，默认 5） | 3/3 | ~1 分钟 |
| `bench/scan-a64.sh` | 摸家底：整份语料 53 份分类账（ok/空画面/着色器错/崩/超时）+ ms/帧，落 `$OUT/scan.tsv` 与 `scanlog/` | ok 51 / 空 1 / 着色器错 1 / 崩 0 / 超时 0 | 十几分钟 |

`BIN=` 与 `OUT=` 两个 env 决定量哪条腿、落哪儿 —— x64 两条腿靠它们各落一份，
**别让两条腿的产物互相覆盖**（一边 ELF 一边 Mach-O，踩过）。

### 差分的具体跑法（`test-ops.sh`）

```sh
bash bench/test-ops.sh               > /tmp/a.txt   # arm64 JIT（默认开）
PD_JIT=0 bash bench/test-ops.sh     > /tmp/b.txt   # 同一二进制，关 JIT
diff /tmp/a.txt /tmp/b.txt                          # 必须为空
```

更细的差分在程序内：`PD_JIT=2` 每次调用两条路都跑、位级对不上就印 ——
查"算错了"最快的一格（见 [jit.md](jit.md)）。

## `/bench:N`：给原版装的出数口

原版只在标题栏显示 fps，没有任何能批量取数的出口；`polydraw.c` 上五处插桩
（每处带 `//Omni benchmark` 记号，`git diff` 一眼看得见加了什么）：

1. 三个全局量（帧数 / 已计帧 / 起点计数器）；
2. 命令行认 `/bench:N`；
3. `if (!ActiveApp) Sleep(100)` 那道门在基准档不挡（ssh 的进程拿不到焦点，
   会一直睡着）；
4. 交帧后计帧：第 1 帧关 vsync、**前 30 帧暖机**（编译/上传都在头几帧）、
   满 N 帧写 `polydraw_bench.txt` 再退（两列：fps、ms/帧）；
5. 跳过主循环那句礼让 `Sleep(1)`（默认定时器粒度下实际睡 15.6ms，
   把无着色器的脚本全钉死在 ~63fps —— 跳过后 `28_peaks` 从 15.8ms 掉到 0.107ms）。

注意口径：`/bench:N` 有 30 帧暖机门槛，**慢脚本连一个数都拿不到**；要量它们用
GUI 档的标题 fps（没有暖机门槛）。

## 原版参考：谁有资格当"参考"

**量原版性能只许用 Windows x86 + MSVC 那条腿**（`bench/build.cmd` +
`powershell run-bench.ps1`），两条硬规矩：

* **必须 x86**：`kplib.c` 有 9 处 32 位内联汇编，`eval.c` 那台 JIT 发的是 x87 机器码
  —— x64 版本物理上不存在；
* **`/FORCE:MULTIPLE`**：`mysrand` 在两份里各定义一次（原文就有的脏），
  不改源码去迁就链接器 —— 改了就不是"原版"。

本分支的 `polydraw_src/` 与原始提交**逐字节相同**（外加那五处插桩），所以它就是
尺子本身。历史教训：另一条实现线曾把 x87 内联汇编从 `polydraw_src/` 里删掉以便
现代工具链编过（`5769932 remove asm`），拿那种改过的源量"参考"会把参考量慢
16 倍（`balls2k` 10.7ms → 实为 0.636ms）。

参考账（320×240，`pdref-fps.tsv`）：原版自己低于 60fps 的整份语料只有两份
（`tree` 10.288、`curvybuild` 32.353 ms/帧）—— "这门语言本来就是实时的"有了具体的数。

**跨架构的 ms/帧不可直接对比**：arm64 那档是离屏 320x240 + 自家 JIT，
原版那档是真窗口 + x87 JIT + vsync 关闭；x64 两条腿更不算数（Rosetta / qemu 转译 +
llvmpipe 软光栅，我们吐的机器码还要被再翻一次）。各自的数只当自己那条腿的前后对比。

## 不可靠信号清单（都真踩过）

| 信号 | 为什么不可靠 | 怎么办 |
| --- | --- | --- |
| scan 的颜色数 | `klock()` 驱动的脚本（球会掉、几何会长）同一命令连跑三趟能是 43 色或 1 色 | 同一二进制 A/B 交错多趟看分布 |
| 单趟扫描 | `geo_duptris` 有一次"从空画面变 ok"，复跑就回去了 | 结论一律复跑 |
| GUI 标题栏 fps | 原文每秒才写一次，慢腿（qemu）可能一次都没写上，`texture` 假红过 | 加长 `SECS`（docker-x64.sh 默认 12） |
| PNG md5 | 时间驱动的脚本两趟本来就不一样（`balls` 别拿 md5 判对错） | 用确定性探针脚本判 md5 |
| 探针全黑 | 没有片元着色器段原文就压根不画（`if (shadn[2])`） | 探针必须带 `@f`，先确认非黑 |
| `polydraw_bench.txt` 的列 | 每行开头是 tab，`$2` 其实是 fps 不是 ms | 取 `$3`（已修，留档） |

## 三条腿怎么跑

```sh
# 1) arm64 原生（Apple Silicon）
bash bench/build-a64.sh            # 产物 bench/out/
bash bench/test-a64.sh && bash bench/test-ops.sh && bash bench/render-a64.sh
bash bench/gui-a64.sh              # 开真窗口
bash bench/scan-a64.sh             # 整份语料分类账

# 2) osx x86-64（Rosetta 2；GL 走 CGL 真 GPU；GUI 桩掉 —— 没有 x86_64 的 GLFW 不值得引）
bash bench/build-x64.sh            # 产物 bench/out-x64-osx/
BIN=bench/out-x64-osx/eval_x64 bash bench/test-a64.sh
BIN=bench/out-x64-osx/polydraw_x64 OUT=bench/out-x64-osx bash bench/render-a64.sh
BIN=bench/out-x64-osx/polydraw_x64 OUT=bench/out-x64-osx bash bench/scan-a64.sh

# 3) linux/amd64（Docker：mesa/glfw/Xvfb/x11vnc；出图默认无头 EGL surfaceless）
bash bench/docker-x64.sh build|test|ops|render|gui|scan   # 产物 bench/out-x64/
bash bench/docker-x64.sh vnc ken/balls.pss                # 要用眼睛看动画：宿主 open vnc://127.0.0.1:5900
bash bench/docker-x64.sh x11                             # XQuartz 那一档：只判"开窗+帧在推进"，回读像素不可信
```

两条 x64 腿各自内部的判据（eval 16/16、ops 83 行、出图 4/4、GUI 4/4）与 arm64
一致；linux 与 arm64 的 ops 差只有 4 行 `tan/asin/acos/atan` 的末几个 ULP
（glibc 与 Apple libm 的差，两条腿自己内部都一致）。
