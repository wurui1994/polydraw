# PolyDraw —— Ken Silverman 的 OpenGL/GLSL 脚本工具，与它的 arm64 / x86-64 移植

PolyDraw 原本是 **Tigrou**（概念与初版，2010-01-02）与 **Ken Silverman**（EVAL 编译器、
GUI、修复与增强）合写的 Windows 小程序：`.pss` 脚本描述几何、GLSL 着色器与动画，
脚本语言内嵌 Ken 的 x87 表达式编译器（EVAL / `kasm87`），渲染走 OpenGL 固定管线。
原文只支持 32 位 Windows + x86 + MSVC。

本仓库 `master` 是在**原文一个字节不动**的前提下完成的移植与跑通：

* **arm64 macOS**（Apple Silicon 原生）—— 语言 / 出图 / GUI 三条轴全绿，
  另有一台自研的 arm64 JIT 与 x86-64 JIT；
* **x86-64** —— 两条腿：macOS 走 Rosetta 2（GL 走 CGL，真 GPU），linux/amd64 走
  Docker（无头 EGL surfaceless + llvmpipe）。与 arm64 共用同一套源码，只四处分岔。

> 完整的探案过程与逐条证据在 [port/README.md](port/README.md)（技术日记，权威）；
> 本目录下 `docs/` 是按主题整理的四份文档。

## 现状

| 判据 | 结果 | 跑法 |
| --- | --- | --- |
| 语言（带期望值） | **16/16** | `bash bench/test-a64.sh` |
| 指令族差分（JIT ↔ 解释器） | **83 行逐行相同** | `bash bench/test-ops.sh`（差分用法见 [docs/testing-and-bench.md](docs/testing-and-bench.md)） |
| 出图（PNG 非空画面） | **4 过 / 0 红** | `bash bench/render-a64.sh` |
| GUI（真窗口 + 实时帧） | **3/3** | `bash bench/gui-a64.sh` |
| 整份语料 53 份分类账 | **ok 51 / 空画面 1 / 着色器错 1 / 崩 0 / 超时 0** | `bash bench/scan-a64.sh` |

剩下两份**不在移植这一侧**：`geo_duptris` 是 Apple GL 驱动不给 QUADS 进几何着色器的
语义差；`gspiral` 是参考脚本本身依赖非标准的 int→float 隐式提升。arm64 与 x86-64
两条腿的语料分类**逐份一致（53/53）**。

性能（2026-10 本机实测与日记记录）：

* arm64 JIT：`sqrt` 循环微基准 56.8 → 5.2 µs（10.9x，值逐位相同）；`fib(20)` 1280 → 511 µs；
  GUI 实测 `texture` 60.8 → 120.5 fps、`clock` 59.8 → 120.0 fps；
* 立即模式攒批：`balls`（16384 球/帧）窗口里 **30 → 115 fps**（离屏 28.8 → 4.2 ms/帧）；
* 原版参考（Windows x86，量法见 [bench/README.md](bench/README.md)）：`balls2k` 0.636 ms/帧、
  `curvybuild` 32.4 ms/帧 —— 原版自己低于 60fps 的整份语料里只有两份。

## 快速开始（arm64 macOS）

依赖：Xcode 的 clang、homebrew 的 GLFW（`brew install glfw`）、python3（判据脚本用）。

```sh
bash bench/build-a64.sh        # 全部产物落 bench/out/（eval_a64 / eval_bench / polydraw_a64）
bash bench/test-a64.sh         # 16/16
bash bench/test-ops.sh         # 83 行（差分判法见 docs）
bash bench/render-a64.sh       # 4/4
```

跑一份脚本：

```sh
bench/out/polydraw_a64 ken/ceilflor2.pss                       # 默认 --gui：开窗口实时跑，关窗即退
bench/out/polydraw_a64 ken/ceilflor2.pss --render --frames 3 --size 640x480 --out out.png
bench/out/polydraw_a64 ken/balls.pss --gui --size 640x480
```

`--frames N` / `--out` / `--render` 任给其一即离屏出图；`--size` 给的是**帧缓冲**尺寸
（Retina 上是窗口的两倍）。命令行也认原文那套 `/bench:N`（30 帧暖机 + N 帧计时，
写 `polydraw_bench.txt` 后退出）。诊断开关（env）：`PD_JIT=0/1/2`、`PD_IMM=0/1`、
`PD_RUNDBG` / `PD_TEXDBG` / `PD_GLDBG` / `PD_GUIDBG`、`PD_MOUSE=x,y`，逐个说明见
[port/README.md](port/README.md)。

x86-64 那两条腿（Rostta / Docker）的构建与运行见 [docs/architecture.md](docs/architecture.md)
与 [docs/testing-and-bench.md](docs/testing-and-bench.md)。

## 目录

| 路径 | 内容 |
| --- | --- |
| `polydraw_src/` | Ken 的原文（**一个字节不动**；按声明拆成 `eval/` `pd/` `kp/` 共 36 个模块，逐字节可复原 —— 见 [docs/c-split.md](docs/c-split.md)；缝合文件 `*.stitch.c` / `*.a64.stitch.c`） |
| `port/` | 移植层：`pd_port.h`、`a64/`（垫片、GL、GUI、JIT）、`x64/`（JIT 发射器、EGL）、[README.md](port/README.md) 技术日记 |
| `bench/` | 构建 / 判据 / 扫描脚本与 [README.md](bench/README.md)（原版性能尺子的口径） |
| `tools/` | `mk*.mjs` —— 生成"与原文可 diff 的替换文件"（`pd_a64_run.c`、`kasm_main_a64.c` 等） |
| `ken/`、`tigrou/` | 语料：53 份 `.pss` 脚本 |
| `polydraw.txt`、`polydraw_src/eval.txt` | 原版说明与 EVAL 语言说明 |

## 分支

* `master` —— 本轴：C 原版 + arm64/x86-64 移植（自 `add origin source code` 以来的全部实现）；
* `archive` —— JS/Python 重写实现的另一轴（`js_impl/`、`c_impl/`、`pyref/`），历史与工作区产物一并保留在该分支。

## 关联项目

* **Omni**（`github.com/wurui1994/omni`，镜像 `gitee.com/clover1994/omni`）—— 工具链仓库。
  本仓库起手用的拆分系统就是它的通用 C 前端能力 `omni c split`（ADR-0046：
  语法层定边界、原文字节做内容、逐字节复原），见 [docs/c-split.md](docs/c-split.md)。

## 文档

* [docs/architecture.md](docs/architecture.md) —— 架构与移植方法：三种挂接手段、
  GL 上下文三档、GUI 腿、立即模式攒批、x86-64 的四处分岔；
* [docs/jit.md](docs/jit.md) —— 两台 JIT（A64 / X64 发射器）：thunk、调用约定、
  差分判据、按指令二分的调试开关；
* [docs/porting-holes.md](docs/porting-holes.md) —— 原文在非 x86 上的 25 个洞：
  清单、找洞方法论、与"洞外"（驱动语义 / 脚本自身的毛病）的分界；
* [docs/testing-and-bench.md](docs/testing-and-bench.md) —— 判据体系与性能口径：
  每根尺子怎么跑、`/bench:N` 插桩原理、哪些信号不可靠；
* [docs/c-split.md](docs/c-split.md) —— 拆分系统与关联项目 Omni：`omni c split` 的
  五条原则、实现要点、两道闸与 polydraw 这边的产物。

许可：见 [LICENSE](LICENSE) 与 `polydraw_src/eval.txt` 内 Ken 的许可条款。
