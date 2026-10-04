# 两台 JIT：gasm[] → A64 / X64 机器码

> 背景、逐个洞的证据与 A/B 账在 [port/README.md](../port/README.md)。

## 为什么需要它

原文的 `kasm87`（EVAL 编译器）只有两种形态：x86 上的 x87 机器码（`COMPILE==1`），
与"纯 C 解释器"（`COMPILE==0`，Ken 注释里写着 *"Virtual machine (PowerPC)"*）。
后者那条路**从来没通过**：多函数脚本必崩、清理路径 `free()` 野地址、变参宿主调用
按 `double(...)` 强转……（见 [porting-holes.md](porting-holes.md) #1~#7）。

arm64 上 x87 不存在，于是自己写：**同一串 `gasm[]` 三地址字节码**，一格 `kcd`
（已编译的脚本函数）编一次真机器码，之后每帧直接跳进去。挂点只有 `pd_a64_fill`
里一句 —— 编不出来就退回解释器，答案照旧对。

## thunk：把"函数指针"交回给原文

原文把脚本当 C 函数指针用（`double (*)(double,...)`，真变参）。移植层给每格 kcd
造一小片入口代码，自己 `mmap` 一页：

* **arm64**（`port/a64/pd_a64_jit.c`）：14 条指令、全立即数（不需要重定位）。
  arm64 macOS 不给 RWX —— 码页写时 RW、跑时 RX；`kasm87free`/`kasm87jumpback`
  的记账挂在 thunk 那一格的尾巴上（原文会去 `free()` 一个不是 malloc 来的地址，
  见洞 #3）；
* **x86-64**：36 字节，`movabs r10=&gkasm87cptr / movabs r11=kcd / mov [r10],r11 /
  movabs r10=entry / jmp r10`。**只许用 r10/r11**：SysV 下 `kasm87c` 是变参函数，
  callee 的 `va_start` 要读 `al`（用了几个向量寄存器）—— 拿 rax 当草稿纸就踩了。

## 发射器（`port/a64/pd_a64_jitc.c` / `port/x64/pd_x64_jitc.c`）

把 `gasm[]` 逐条吐成机器码。公共骨架（kcd 缓存、退路、三把开关）两边共用，
各写各的编码。要点：

* **宿主调用按 AAPCS64 摆实参**：`d`（double）走 d0..d7、`D`（double *）/`C`（char *）
  走 x0..x7，**两条独立序列、任意次序都对** —— 解释器那张按原型串枚举的 switch
  要求"指针必须是后缀"（洞 #20/#22 的根），JIT 这边没有那道门；
* **脚本自己的函数**（`pd_a64_owns` 认出）：把操作数摆成 `double *p[17]` 那张表
  （栈上 `PD_POFF` 那一段），原样递给 `pd_a64_call_script`；而它**自己也要问一句
  JIT**（递归的唯一入口，不问的话被调方永远在解释器上跑 —— `fib(20)` 先头只快
  1.18 倍就是这个洞）；
* **x86-64 的三个语义坑**（都是语义，不是编码错）：
  * `ucomisd` 无序时 ZF/PF/CF 全置 1 —— `a<b` **不能**写 `setb`（NaN 会给出 1），
    要把操作数反过来比、用 `seta`；`==` 要 `sete && setnp`、`!=` 要 `setne || setp`
    （arm64 的 NE 天然含"无序"，与 C 一致）；
  * `MINSD/MAXSD`"无序取第二个操作数"恰好等于 arm64 `fcsel …,MI/GT` 的语义
    （含 ±0 那格），MIN/MAX 一条指令不用分支；
  * `roundsd`（floor/ceil/trunc 与 `%`）是 **SSE4.1**：`pd_jit_build` 开头
    `__builtin_cpu_supports("sse4.1")`，没有就整份不编、退回解释器。

## 差分判据与三把查错开关

**判据是差分，不是手算期望值**：

* `PD_JIT=2` —— 每次调用**两条路都跑一趟**、位级对不上就印。查"算错了"最快的一格；
* `PD_JITMAX=n` —— 只编指令数 ≤ n 的 kcd（每格脚本函数是一格 kcd），二分"哪份函数的锅"；
* `PD_JITNO=f1,f2,…` / `PD_JITFB=a,b` —— 拒编含这几号指令的整份 / 按指令区间二分，
  二分"哪族指令的锅"。前提是**"单条指令走解释器"的退路**（`pd_a64_jit_one`）
  —— 没有它，一族一关就等于整份退回，没有信息量。

`bench/test-ops.sh` 的 83 行（含 12 行 NaN 语义）拿这三把尺子交叉：arm64 JIT =
x64 解释器 = x64 JIT 逐行相同；同一个二进制 `PD_JIT=0` 与 `=1` 各跑一趟语料
**逐份判定一处真差都没有**。

## 账

同一台机器的微基准（min-of-batches，值逐位相同）：

```text
(x){s=0;for(i=0;i<1000;i++)s=s+sqrt(i*x);s}   解释器 56.8 µs -> JIT 5.2 µs   （10.9x）
fib(20)（脚本函数递归）                        解释器 1280 µs -> JIT 511 µs
GUI 实测                                      texture 60.8 -> 120.5 fps
                                              clock   59.8 -> 120.0 fps
ken/balls.pss --gui（JIT + 攒批）              30 -> 115 fps
```

**默认是开的**（`PD_JIT=1`）。JIT 吐的码在 x86-64 上还要被 Rosetta 再翻一次，
所以 x64 那两条腿的速度数字不算数。

## 还欠的

* **寄存器分配** —— 现在每条指令"读内存-算-写内存"，省的是"算三个地址 +
  一趟 switch + 一层间接"，不是寄存器；
* 其余（`n > 8` 的宿主调用等）见 `pd_a64_jitc.c` 头注的清单。
