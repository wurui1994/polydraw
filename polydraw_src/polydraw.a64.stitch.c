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
#include "pd/pd_wingl.c"
#include "pd/pd_font.c"
#include "pd/pd_noise.c"
#include "pd/pd_host_gl.c"
#include "pd/pd_midi.c"
#include "pd/pd_zip.c"
#include "pd/pd_script.c"
#include "pd/pd_find.c"
#include "pd/pd_win.c"
