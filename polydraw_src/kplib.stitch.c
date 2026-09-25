/* kplib.c —— 由 `omni c split` 生成的缝合文件（ADR-0046）。
 * 原文一个字节都没改：这几份 `#include` 的字节接起来**等于**原文，次序就是这里的次序。
 * 为什么走 unity build 而不是各编成 .o：拆出来的模块里那些 `static` 只在原来那一个
 * 翻译单元里可见，要分开编就得去掉 `static` 并补 `extern` —— 那是改原文，不许。
 */
#include "kp/kp_head.h"
#include "kp/kp_cpu.c"
#include "kp/kp_bits.c"
#include "kp/kp_png.c"
#include "kp/kp_jpg.c"
#include "kp/kp_misc.c"
#include "kp/kp_api.c"
#include "kp/kp_zip.c"
#include "kp/kp_load.c"
