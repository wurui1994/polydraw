/* port/x64/pd_gui_stub.c —— 没有 GLFW 的那一档：GUI 一律"没开"。
 *
 * 谁用：**osx x86-64（Rosetta 2）那条腿**。这台机器上 homebrew 那份
 * `libglfw.dylib` 是 arm64 的，连不进 x86_64 的可执行文件；而 macOS 上出图那条路
 * 走的是 **CGL**（不要窗口、不要 GLFW），所以 GUI 那一族桩掉就能编、能出图、
 * 能跑 JIT 判据。要 x64 的真窗口得先有一份 x86_64 的 GLFW
 * （装一套 Intel homebrew 或者自己编一份），那不在这一刀里 ——
 * **arm64 原生那条腿的 GUI 是通的**（`bench/gui-a64.sh` 4/4），这一格只是省掉依赖。
 *
 * 每一格的返回值都照"没开 GUI"那条路写：`pd_gui_on()` 回 0，于是
 * `pd_gl_cgl.c` 走 CGL 离屏、`pd_win_a64.c` 的 `GetCursorPos` 报画面中心
 * （那一格很要紧 —— 报 0,0 会让 `ken/orthoglobe.pss` 整片贴在相机平面上，见
 * `port/README.md` 里那一条）。
 */
#include <stdio.h>

int  pd_gui_on (void) { return(0); }
void pd_gui_enable (int w, int h)
	{ (void)w;(void)h; fprintf(stderr,"[gui] 这一份二进制没连 GLFW（osx x86-64），--gui 不可用\n"); }
int  pd_gui_open (void) { return(0); }
int  pd_gui_open_offscreen (void) { return(0); }   /* macOS 上走 CGL，压根不会问到这儿 */
int  pd_gui_make_current (void) { return(0); }
int  pd_gui_mouse (int *x, int *y) { (void)x;(void)y; return(0); }
int  pd_gui_fbsize (int *w, int *h) { (void)w;(void)h; return(0); }
int  pd_gui_title (const char *s) { (void)s; return(0); }
int  pd_gui_closing (void) { return(0); }
void pd_gui_present (unsigned int fbo, const int *vp) { (void)fbo;(void)vp; }
void pd_gui_close (void) {}
void *pd_gui_procaddr (const char *nm) { (void)nm; return(0); }
