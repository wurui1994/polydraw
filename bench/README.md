# `origin-bench` —— 原版 PolyDraw + 性能基准（第三根尺子）

这个分支从 **`559ed7a` "add origin source code"** 长出来，只加两样东西：

* `polydraw_src/polydraw.c` 上 **五处 `/bench:N` 插桩**（+30 −2 行，见下）；
* 这个 `bench/` 目录（编译脚本、批量运行脚本、这份说明）。

**语言与渲染那两半一个字都没改** —— `eval.c`（含 `kasm87` 那台 x87 JIT）与 `kplib.c`
与原始提交**逐字节相同**。这一点是这根尺子的全部价值：它量的是 Ken 自己那份实现，
不是任何人的移植版。

## 为什么要单独一个分支

`master` 上那份 `polydraw_src/` 是**被改过的**：为了在现代工具链上编过，
后面的提交（`5769932 remove asm`）把 x87 JIT 那一摊内联汇编去掉了。
拿它量性能会把"参考"量成一个慢好几倍的东西 —— 先前我们就这么记过一轮账，
`balls2k` 的参考从 10.7ms 改成了 **0.636ms**（差 16 倍）。

所以这条规矩写在这儿：**量原版性能只许用这个分支**。

## 建这棵工作树

```sh
cd ~/Documents/polydraw
git worktree add -b origin-bench ../polydraw-bench 559ed7a
```

## 编（Windows + MSVC，必须 x86）

```
x86 Native Tools Command Prompt> cd polydraw-bench\bench
> build.cmd
```

`build.cmd` 里两条不能动的地方：

* **必须 x86**。`kplib.c` 有 9 处 32 位内联汇编，而 `eval.c` 那台 JIT 发的是 x87 机器码
  —— x64 版本在物理上不存在；
* **`/FORCE:MULTIPLE`**。`mysrand` 在 `polydraw.c` 与 `eval.c` 里各定义一次（两份相同）。
  这是原版就有的事，**不改源码**去迁就链接器 —— 改了它就不是"原版"了。

## 跑

```
> powershell -ExecutionPolicy Bypass -File run-bench.ps1 -Frames 300
```

出来的 `pdref-fps.tsv` 每行是 `脚本<TAB>fps<TAB>ms/帧`。

## `/bench:N` 那五处插桩，各自为什么

1. **三个全局量** `gbenchn / gbenchi / gbenchq0` —— 帧数、已计帧、起点计数器；
2. **命令行认 `/bench:N`** —— 原版只在标题栏显示 fps，没有任何能批量取数的出口；
3. **`if (!ActiveApp) Sleep(100)` 那道门在基准档不挡** —— ssh 起的进程拿不到焦点，
   不跳过这一句它会一直睡着，一帧都不画；
4. **交帧后计帧**：第 1 帧 `wglSwapIntervalEXT(0)` 关垂直同步（不关就量到 60fps 的整数倍）、
   **前 30 帧预热**（脚本编译、纹理上传、着色器编译都在头几帧）、满 N 帧写
   `polydraw_bench.txt` 再退；
5. **跳过那句礼让 `Sleep(1)`** —— 主循环里"脚本没有 `@f` 片元段就 `Sleep(1)`"，
   在默认定时器粒度下**实际睡 15.6ms**，把所有无着色器的脚本钉死在 ~63.4fps。
   第一趟量出来十几份整整齐齐 15.75~15.79ms，全是这一句，不是渲染成本 ——
   跳过之后 `examples/opengl/28_peaks` 从 15.795 降到 **0.107ms/帧**。

插桩的注释一律用英文，与原文的风格一致；每一处都带 `//Omni benchmark` 记号，
`git diff 559ed7a` 一眼看得见加了什么。

## 这根尺子量出来的几个数（320×240）

| 脚本 | 原版 ms/帧 |
| --- | --- |
| `tigrou/balls2k` | 0.636 |
| `ken/drawsph` | 0.823 |
| `tigrou/snake tube` | 1.331 |
| `tigrou/disco ball` | 5.656 |

整份语料 103 份里原版自己低于 60fps 的只有两份（`tigrou/tree` 10.288ms、
`ken/curvybuild` 32.353ms）——"这门语言本来就是实时的"这句话有了具体的数。
