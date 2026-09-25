/* eval.a64.stitch.c —— arm64 / macOS 的缝合文件。
 *
 * 与 `eval.stitch.c` 的差别只有三处，全是**新材料**，原文的字节一个都没改：
 *
 *   1. `eval/kasm_main.c` 换成 `port/a64/kasm_main_a64.c` —— 原文那一份在
 *      COMPILE==0 上写的是 `??? not implemented .. need to fix .. sorry :/`，编不过；
 *   2. 插进 `port/a64/pd_a64_jit.c`（真的 arm64 codestub）与 `pd_a64_api.c`
 *      （`kasm87free` / `kasm87jumpback` 的 arm64 版）；
 *   3. 三个 `#define` 改名：**改名写在这份文件里，不在原文里**。
 *      - `kasm87c_copyglob2struct` -> 我们的外壳（原文那一份照旧跑，回来之后把
 *        "直接交回解释器入口地址"那个 hack 换成一格真 thunk）；
 *      - `kasm87free` / `kasm87jumpback` -> `*_x86`（那两份读的是 COMPILE!=0 才有的
 *        头字，在 arm64 上是死代码），真名归 `pd_a64_api.c`。
 *
 * 为什么 `#define` 能放在两个 `#include` 中间就成：宏是按记号流展开的，
 * 定义点在 `kasm_interp.c` 之后、`kasm_comp.c` 之前，于是**定义**用原名、
 * **调用点**用新名 —— 正好是要的那一刀。
 */
#include "eval/kasm_head.h"
#include "eval/kasm_state.c"
#include "eval/kasm_cpu.c"
#include "eval/kasm_math.c"
#include "eval/kasm_dbg.c"
#include "eval/kasm_name.c"
#include "eval/kasm_grow.c"
#include "eval/kasm_parse.c"
#include "eval/kasm_emit.c"
#include "eval/kasm_opt.c"
#include "eval/kasm_interp.c"

#include "../port/a64/pd_a64_jit.c"

#define kasm87c_copyglob2struct pd_a64_copyglob2struct
#define kasm87free              kasm87free_x86
#define kasm87jumpback          kasm87jumpback_x86
#include "eval/kasm_comp.c"
#undef kasm87free
#undef kasm87jumpback

#include "../port/a64/pd_a64_api.c"
#include "../port/a64/kasm_main_a64.c"
