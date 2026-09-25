/* polydraw.c —— 由 `omni c split` 生成的缝合文件（ADR-0046）。
 * 原文一个字节都没改：这几份 `#include` 的字节接起来**等于**原文，次序就是这里的次序。
 * 为什么走 unity build 而不是各编成 .o：拆出来的模块里那些 `static` 只在原来那一个
 * 翻译单元里可见，要分开编就得去掉 `static` 并补 `extern` —— 那是改原文，不许。
 */
#include "pd/pd_head.h"
#include "pd/pd_ini.c"
#include "pd/pd_cons.c"
#include "pd/pd_hilite.c"
#include "pd/pd_wingl.c"
#include "pd/pd_font.c"
#include "pd/pd_noise.c"
#include "pd/pd_host_gl.c"
#include "pd/pd_midi.c"
#include "pd/pd_zip.c"
#include "pd/pd_script.c"
#include "pd/pd_find.c"
#include "pd/pd_win.c"
