/* eval.a64.stitch.c —— arm64 / macOS 的缝合文件。
 *
 * 与 `eval.stitch.c` 的差别只有三处，全是**新材料**，原文的字节一个都没改：
 *
 *   1. `eval/kasm_main.c` 换成 `port/a64/kasm_main_a64.c` —— 原文那一份在
 *      COMPILE==0 上写的是 `??? not implemented .. need to fix .. sorry :/`，编不过；
 *   2. 插进 `port/a64/pd_a64_jit.c`（真的 arm64 codestub）与 `pd_a64_api.c`
 *      （`kasm87free` / `kasm87jumpback` 的 arm64 版）；
 *   3. 四个 `#define` 改名：**改名写在这份文件里，不在原文里**。
 *      - `kasm87c` / `kasm87cp` -> `*_x86`（那两份写参数区时"指针占 4 字节"，
 *        是 32 位 x86 的口径）；真名归 `pd_a64_parm.c`；
 *      - `kasm87c_copyglob2struct` -> 我们的外壳（原文那一份照旧跑，回来之后把
 *        参数区偏移加宽、把"直接交回解释器入口地址"那个 hack 换成一格真 thunk）；
 *      - `kasm87free` / `kasm87jumpback` -> `*_x86`（那两份读的是 COMPILE!=0 才有的
 *        头字，在 arm64 上是死代码），真名归 `pd_a64_api.c`。
 *
 * 为什么 `#define` 能放在两个 `#include` 中间就成：宏是按记号流展开的，
 * 定义点在 `kasm_interp.c` 之后、`kasm_comp.c` 之前，于是**定义**用原名、
 * **调用点**用新名 —— 正好是要的那一刀。
 */
#include "eval/kasm_head.h"
#include "eval/kasm_state.c"

/* ── 第 6 个洞：`KIMM` 必须是 long 常量 ──
 *
 * `kasm_state.c:36` 是 `#define KIMM 0xb0000000` —— 在 C 里这个字面量的类型是
 * **unsigned int**。而 `kasm_interp.c:36` 写的是
 *
 *     plst[((unsigned long)KIMM)>>28] = ((long) -KIMM);
 *
 * 意图是 `0 - KIMM`，好让后面 `plst[…] + r`（r = KIMM|下标）正好等于**下标**。
 * 可是 `-KIMM` 走的是**无符号**算术：结果是 `0x50000000`，不是 `-0xb0000000`。
 * 在 32 位上 `0x50000000 + 0xb0000000 + idx` 溢出回绕**正好**得到 idx；
 * 在 arm64 上不回绕 —— 下标变成 `0x100000000 + idx`，
 * `kcd->gevalext[那个下标].ptr` 当场读飞（Ken 自带的例子 #9 就崩在这儿）。
 *
 * 一行就够：把它换成 long 字面量，`-KIMM` 就走有符号算术了。
 * 那十几处 `(r&0xf0000000) == KIMM` 的比较不受影响（两边都升成 long，值一样）。
 */
#undef KIMM
#define KIMM 0xb0000000L

#include "eval/kasm_cpu.c"
#include "eval/kasm_math.c"
#include "eval/kasm_dbg.c"
#include "eval/kasm_name.c"
#include "eval/kasm_grow.c"
#include "eval/kasm_parse.c"
#include "eval/kasm_emit.c"
#include "eval/kasm_opt.c"

#define kasm87c     kasm87c_x86
#define kasm87cp    kasm87cp_x86
#define kasm87c_run kasm87c_run_x86
#include "eval/kasm_interp.c"
#undef kasm87c
#undef kasm87cp
#undef kasm87c_run

/* 次序：jit（thunk 与 pd_a64_owns）-> run（真的 kasm87c_run，要 pd_a64_owns）
   -> parm（真的 kasm87c/kasm87cp，要 kasm87c_run）。 */
#include "../port/a64/pd_a64_jit.c"
#include "../port/a64/pd_a64_run.c"
#include "../port/a64/pd_a64_parm.c"

#define kasm87c_copyglob2struct pd_a64_copyglob2struct
#define kasm87free              kasm87free_x86
#define kasm87jumpback          kasm87jumpback_x86
#include "eval/kasm_comp.c"
#undef kasm87free
#undef kasm87jumpback

#include "../port/a64/pd_a64_api.c"
#include "../port/a64/kasm_main_a64.c"

/* 尾巴上那个 main 有三档，**由宏挑**（而不是再复制两份缝合文件 —— 那种复制过一次，
   KIMM 那一行的修法就只进了主本，派生的两份悄悄旧着，polydraw 那条腿照旧崩）：
     * 什么都不定义 -> 不带 main，给 polydraw 连（原来的 eval.a64lib.stitch.c）；
     * `-DEVALTEST`      -> Ken 自带的那个 main；
     * `-DPD_EVAL_BENCH` -> port/a64/eval_bench.c（量尺，换掉那个永远印 0 cc 的）。 */
#if defined(PD_EVAL_BENCH)
#include "../port/a64/eval_bench.c"
#elif defined(EVALTEST)
#include "eval/eval_test.c"
#endif
