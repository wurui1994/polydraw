/* eval.c —— 由 `omni c split` 生成的缝合文件（ADR-0046）。
 * 原文一个字节都没改：这几份 `#include` 的字节接起来**等于**原文，次序就是这里的次序。
 * 为什么走 unity build 而不是各编成 .o：拆出来的模块里那些 `static` 只在原来那一个
 * 翻译单元里可见，要分开编就得去掉 `static` 并补 `extern` —— 那是改原文，不许。
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
#include "eval/kasm_comp.c"
#include "eval/kasm_main.c"
#include "eval/eval_test.c"
