# 拆分系统：`omni c split` 与关联项目 Omni

> 移植的第一步不是改代码，是**把原始代码拆开**：`polydraw_src/` 只有三个文件，
> `eval.c` 6122 行 / 238KB、`polydraw.c` 3661 行 / 148KB —— 不拆开，谁也没法在
> 这种粒度上安全地动它。拆分三提交：`d1f0948`（拆 32 个模块，逐字节复原已验）、
> `de56cde`（按切点规划 + 缝合文件）、`03fef4a`（挪四处切点让 `#if` 配平）。

## 关联项目：Omni

拆分工具不是给 polydraw 写的一次性脚本，是 **Omni**（工具链仓库，
`github.com/wurui1994/omni`，镜像 `gitee.com/clover1994/omni`）CLI 里的一项
**通用 C 前端能力**。设计见 Omni 仓库 `docs/design/adr-0046-c-split.md`（ADR-0046），
实现在 `src/core/frontend-c/split.js`，自带判据测试 `tests/c/split/run.js`
（拿 Omni 自己仓库里的真实 `.c` 过 `--check`）。

```text
usage: omni c split FILE.c --scan | --map FILE.split -o DIR
  --scan        印清单：种类 名字 行号 字节数（不切）
  --map VALUE   描述文件：目标文件<TAB>种类<TAB>符号名<TAB>行号
  --check VALUE 只验复原，不落盘
```

polydraw 这边的 ADR 编号（`port/README.md` 里的 ADR-0045"arm64 真 JIT"等）
与 Omni 的 ADR 序列是同一本账。

## 五条原则（ADR-0046 的约束，一条比一条硬）

1. **不许动原始代码**。拆分是"把同样的字节分到几个文件里"，不是重写；
2. **语法层定边界**（AST），不按行数、不用正则找 `^}`；
3. **能逐字节复原**：把拆出来的片段按记录次序接回去**逐字节等于原文** ——
   注释、空行、`#if 0` 里那段 nmake 头、行尾 CRLF 全都要在。这一条是支点：
   有了它，"拆得对不对"是一句**可执行**的话；
4. **描述文件就是规划**：哪个函数去哪个文件，是人做的决定，不是启发式输出；
5. **通用工具**，先做 C，不做 C++（mangling 与模板会毁掉"抄声明字节"那招）。

关键决定：**用语法层定边界，用原文的字节做内容**。拆分不重新打印 AST ——
打印出来的 C 永远不会逐字节等于原文。所以解析只回答"顶层声明从第几个字节
开始、到第几个字节结束"，每格产物是原文的一个**闭区间切片**。全部切片构成
原文的一个**划分**（不重不漏），复原只是按序 `concat` ——
`Buffer.concat(pieces).equals(original)` 是硬线，"逐字节复原"因此**结构上成立**，
不是"努力做到"。

## 实现要点

* **在原始 token 流上扫，不预处理**：预处理之后 `#if 0` 那段就不存在了，复原时
  那 8 行 nmake 头就丢了。扫描器吃词法层（tccpp 那一半），`#...` 整行当一格，
  `#if 0` 整段收成 `skip`，**不宏展开、不查头文件** —— 于是也与平台无关
  （`windows.h` 在拆分的机器上不存在，无所谓）；
* 每格 `chunk = {file, kind, name, start, end, lead}`：`lead` 是上一格结束到这格
  开始之间的全部字节（注释、空行、`#define`、`#if` 块），**跟着后面那格走**
  —— 注释描述的是它下面那个函数，这是 C 的通例；`kind` ∈ func / proto / var /
  type / pp / skip / tail；
* `split` 只有三种动作：`slice` / `concat` / 生成新头。头是唯一"打印"的地方，
  连它也是**抄声明符那段字节**再补 `;`（所以 `double (*f)(double,...)` 不会被
  类型打印器改写）；`static` 一律不进头，非 static **只有被别的模块引用到**才进
  （引用关系是扫描器给的）。模块第一行新加的 `#include` 属于新材料，`manifest`
  里记 `synth` 标记，`--check` 时跳过；
* `*.manifest.json` 记复原次序，`--check` 拉链回原文。

## 三段工作流：中间那段是人

1. `omni c split --scan eval.c` —— 摊家底：每格顶层声明的种类、名字、行号、字节数；
2. **人照清单写 `eval.split`**（签进仓库）：一行 `目标文件<TAB>种类<TAB>符号名<TAB>行号`，
   `#` 开头是注释（表头就是那份"从这一行起属于哪个文件 + 这一段是什么"的规划）。
   **每一格都要指派**，没指派的格子 `--map` 直接报错退出 —— 不给"剩下的扔一处"的口子；
   行号是消歧用的（`eval.c` 里两个 `kasm87err`、四个 `rdtsc64`）；
3. `omni c split --map eval.split eval.c -o DIR/` —— 照规划切 + 生成头，
   `--check` 验逐字节复原。

规划的依据按优先级：**作者自己划的段落**（`//---- KASM87 BEGINS ----`、
polydraw.c 里 `//---------` 那 12 段）、**职责**（解析/优化/发码/解释是四件事）、
**引用关系**（只被一族用到的表跟着那族走）。

## 两道闸（都是真踩出来的）

"复原逐字节"证明不了"拆完还能编"，所以 `--map` 还有两件**结构上**必须成立的事，

| 闸 | 退出码 | 要求 |
| --- | --- | --- |
| `contiguity` | 66 | 同一产物的格子必须**连成一段区间** —— 缝合是"一份 include 一次"，不连续就等于悄悄重排声明次序（头一版按名字指派，`eval.c` 摊成 69 段） |
| `ppBalance` | 67 | 每份产物里 `#if/#ifdef` 的净深度必须是 0 —— `#include` 的边界**劈不开**条件段（踩过四对：`#ifdef _MSC_VER`、`#if (COMPILE==0)`、`#ifdef BIGENDIAN`、`mulshr24` 那条 `#if/#elif` 链，一条命令都编不过） |

修法不是放宽闸门，是**挪切点**：把开条件那一格（连同到 `#endif` 的全部格子）
归到后面那份里 —— 四处各挪 1~18 行，`kp_jpg.c` 顺带从"JPEG 的函数"扩成
"JPEG 的全局 + 函数"，那本来就是更像话的切法。

## polydraw 这边的产物

原文三份**原封不动**地留着（`eval.c` / `polydraw.c` / `kplib.c`），旁边各带三样新材料：

| 材料 | 作用 |
| --- | --- |
| `*.split`（253 / 384 / 250 行） | 规划本身，签在仓库里、可读可审 |
| `*.manifest.json` | 复原次序表（`--check` 拿它拉链回原文） |
| `*.stitch.c` / `*.a64.stitch.c` | 缝合文件：一串 `#include`，接起来**等于原文** |

拆出的模块：`eval/` 14 份（kasm_head.h + 13，`kasm_parse.c` 56KB、`kasm_comp.c`
连 `kasm87comp` 62KB）、`pd/` 13 份（作者划的 12 段 + pd_head.h）、`kp/` 9 份
（kp_head.h + 8）。三份 `--check` 逐字节复原：**238471 / 151594 / 110098 字节**。

为什么缝合走 **unity build**（一份 `#include` 一次）而不是各编成 .o：拆出的模块里
那些 `static` 只在原翻译单元里可见，分开编就得去掉 `static` 补 `extern` ——
那是改原文，不许。编译器看见的记号流与原文一模一样，要退回原文就去掉那个 `.stitch`。

而 `*.a64.stitch.c` 是移植层的挂接点：在同一个"一串 include"的形状上**换掉某一格**
（指到 `port/a64/`）或在两个 include 之间插 `#define` —— 见
[architecture.md](architecture.md)"一条规矩与三种手段"。

## 拆分的红利（顺手拿到的）

* `eval_test.c` 的 `main` 是 Ken 自己的独立测试程序，**不该编进 polydraw** ——
  拆开之后这件事第一次变得明显；
* `mysrand` 在 `eval.c` 与 `polydraw.c` 各定义一次（原版靠 `/FORCE:MULTIPLE` 链）：
  它落在不编进 polydraw 的 `eval_test.c` 里，那个链接开关反而可以去了；
* `kasm_emit.c`（`gasm[]` → x87 机器码那 62KB）被整个隔离出来 —— 正是移植/JIT
  工作要对照的那一份；
* 模块边界成了 a64 缝合与按格替换的"格子线"：`port/README.md` 里每一条
  "换掉某一格"引用的 `kasm_main_a64.c`、`kasm_interp.c` 那些行号，全是拆分给的。
