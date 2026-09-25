/* port/a64/pd_gui_bridge.c —— GUI 那条腿往 polydraw 里喂输入的唯一通道。
 *
 * 为什么非得有这一份：`dkeystatus[256]` / `dbstatus` / `dnumframes` 都是
 * `pd_head.h:354` 里的 **static** —— 只有 polydraw 那个翻译单元看得见。
 * 而窗口与事件在 `port/a64/pd_gui_glfw.c`（另一份 .o）里。所以在缝合文件**末尾**
 * 包这一份，用它导出三个非 static 的写入口，GLFW 那边照着调。
 *
 * 原文一个字节都没动：这是新材料，只有 arm64 的缝合文件包含它。
 */

/* 键：下标是 DOS/DirectInput 扫描码（脚本里就是 `keystatus[0xcd]` 这种）。 */
void pd_in_key (int scan, int down)
{
	if ((unsigned)scan >= 256) return;
	dkeystatus[scan] = (down ? 1.0 : 0.0);
}

/* 鼠标键：位 0/1/2 = 左/右/中，照 `pd_win.c:282-290` 那套加减法的口径。 */
void pd_in_button (int bit, int down)
{
	double m, half;
	if ((unsigned)bit >= 3) return;
	half = (double)(1<<bit);
	m = fmod(dbstatus,half*2.0);
	if (down) { if (m <  half) dbstatus += half; }
	else      { if (m >= half) dbstatus -= half; }
}

/* 给 GUI 那边看一眼当前帧号（窗口标题里印）。 */
double pd_in_numframes (void) { return(dnumframes); }

/* GUI 档：让**渲染窗格铺满整个窗口**。
 *
 * 原文的布局是"渲染占左上四分之一、编辑器占其余"（`pd_win.c:229`：
 * `oglxres = xres>>1`、`oglyres = oglxres*3/4`）。离屏出图无所谓（存图只取那一块），
 * 可 GUI 下画面就只落在窗口一角。原文自己留了这个开关 —— `popts.fullscreen`
 * （`pd_win.c:238` 那条分支：`oglxres = xres; oglyres = yres;`），所以按它就行，
 * 不用去动布局，也**不要**去包 `glViewport`（试过：`glcapture()` 那一族
 * 自己会设小视口，一律覆盖会把 `tigrou/clock.pss` 的渲染到纹理整条打断，
 * 整幅图全黑）。
 *
 * 调用点：`pd_win_a64.c` 的 `CreateWindow` 壳子 —— 它正好在 `resetwindows()`
 * 算布局**之前**（`pd_win.c:220`），所以这一格在那时候写进去还赶得上。
 */
void pd_in_fullscreen (int on) { popts.fullscreen = (on != 0); }
