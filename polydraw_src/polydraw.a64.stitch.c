/* polydraw.a64.stitch.c —— arm64 / macOS 的缝合文件。
 *
 * 与 `polydraw.stitch.c` 的差别只有一处：把 `kputs` 改名，真名归
 * `port/a64/pd_cons_a64.c` —— 它把 polydraw 的控制台输出**转到 stderr**。
 *
 * 为什么非得这样：`kputs`（`pd/pd_cons.c:4`）是往编辑器那个控制台窗口写的，
 * 而我们的窗口全是空壳 —— 于是 polydraw 的全部诊断（着色器编译错误、
 * 脚本编译错误、`compile frag#0` 这种进度）**一个字都看不见**。
 * 查"画面全黑"的时候这等于闭着眼睛。
 */
#include "pd/pd_head.h"
#include "pd/pd_ini.c"
#define kputs kputs_win32
#include "pd/pd_cons.c"
#undef kputs
#include "../port/a64/pd_cons_a64.c"
#include "pd/pd_hilite.c"

/* 纹理上传那一路的诊断（`PD_TEXDBG=1`）—— 定义留在这份里用真名，
   宏一开，后面那些 pd 文件里的**调用点**就走包了一层的那个。 */
#include "../port/a64/pd_gl_texdbg.c"
#define glBindTexture    pd_dbg_glBindTexture
#define glTexImage2D     pd_dbg_glTexImage2D
#define glTexSubImage2D  pd_dbg_glTexSubImage2D
#define glTexParameteri  pd_dbg_glTexParameteri

#include "pd/pd_wingl.c"
#include "pd/pd_font.c"
#include "pd/pd_noise.c"
/* 第 24 个洞：`kglsettexarray*` 那句范围检查把 64 位指针截成 32 位（`(int)p`），
   于是 `glsettex(贴图号,数组,…)` 一声不响地回 -1、贴图从来没上传过。
   原文那三个名字改掉，真名归 `port/a64/pd_gl_settexarr_a64.c` —— 它要
   `tex[]`/`gbmp`/`CreateEmptyTexture` 那些 file-static，所以必须在同一个翻译单元里、
   紧跟在 `pd_host_gl.c` 后头。`pd_script.c` 里那张 myext[] 于是指到修好的那一份。 */
#define kglsettexarray1 kglsettexarray1_win32
#define kglsettexarray2 kglsettexarray2_win32
#define kglsettexarray3 kglsettexarray3_win32
#include "pd/pd_host_gl.c"
#undef kglsettexarray1
#undef kglsettexarray2
#undef kglsettexarray3
#include "../port/a64/pd_gl_settexarr_a64.c"
#include "pd/pd_midi.c"
#include "pd/pd_zip.c"
#include "pd/pd_script.c"
#include "pd/pd_find.c"
#include "pd/pd_win.c"

/* GUI 那条腿往 polydraw 里喂输入的唯一通道 —— `dkeystatus`/`dbstatus` 是
   `pd_head.h:354` 里的 static，只有这个翻译单元看得见，所以桥必须放在这儿。 */
#include "../port/a64/pd_gui_bridge.c"
