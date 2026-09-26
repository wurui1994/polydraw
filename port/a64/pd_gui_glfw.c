/* port/a64/pd_gui_glfw.c —— arm64 macOS 上的 GUI（真窗口 + 实时循环）。
 *
 * ## 形状：**不重写渲染路径**，只把离屏那张 FBO 端到窗口上
 *
 * 出图那条腿已经通了（polydraw 画进我们建的 FBO，`SwapBuffers` 是"一帧画完"的钩子）。
 * GUI 这一格要做的只有三件事，其它一律照旧：
 *   1. 开一个窗口，并让**它的上下文**成为唯一的 GL 上下文（`wglCreateContext` /
 *      `wglMakeCurrent` 在 GUI 档下转到这儿）—— 于是 FBO 就建在窗口的上下文里，
 *      往默认帧缓冲 blit 不用跨上下文共享对象；
 *   2. 每帧把 FBO 的 viewport 那一块 blit 到窗口的 0 号帧缓冲，然后 swap + poll；
 *   3. 把事件喂回 polydraw：鼠标位置走 `GetCursorPos`（本文件导出 pd_gui_mouse），
 *      鼠标键与键盘走 `pd_in_button` / `pd_in_key`（那两个在
 *      `port/a64/pd_gui_bridge.c` 里 —— `dkeystatus`/`dbstatus` 是 static，
 *      只有 polydraw 那个翻译单元看得见）。
 *
 * ## 为什么用 GLFW 而不是 Cocoa
 *
 * 要的是"等价实现"，不是原样照抄 win32 的窗口/菜单/编辑器。GLFW 在这台机器上是
 * 现成的（`/opt/homebrew/lib/libglfw.dylib`），而且它给的是**legacy（2.1）上下文**
 * —— 正好是 polydraw 要的（`glBegin/glEnd` 那一族要固定管线，core profile 没有）。
 * 不传 `GLFW_OPENGL_FORWARD_COMPAT` / `GLFW_OPENGL_PROFILE` 就是 2.1。
 *
 * 一个坑记在这儿：**glfwInit 必须在主线程且栈是常规大小**。Omni 那边踩过
 * "在大栈线程上 glfwInit 直接 SIGTRAP"；这条腿 `main` 就是主线程，没问题。
 *
 * ## DOS 扫描码
 *
 * 脚本读的是 `keystatus[0xcd]`（右方向键）这种 **DOS/DirectInput 扫描码**，
 * 不是 GLFW 的键码。下面那张表按 polydraw 自带例子里实际用到的键列的
 * （方向键/WASD/空格/Shift/Ctrl/Esc/回车/Tab/数字/字母），够跑语料。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLFW/glfw3.h>

/* pd_gui_bridge.c 里那两个（在 polydraw 那个翻译单元里）。 */
void pd_in_key (int scan, int down);
void pd_in_button (int bit, int down);

/* pd_gl_cgl.c 那边要的。 */
int pd_gl_size (int *w, int *h);

static GLFWwindow *pd_win = 0;
static int pd_gui_want = -1;
static int pd_gui_w = 640, pd_gui_h = 480;
static double pd_gui_mx = 0, pd_gui_my = 0;

int pd_gui_on (void)
{
	if (pd_gui_want < 0) pd_gui_want = 0;
	return(pd_gui_want);
}
void pd_gui_enable (int w, int h)
{
	pd_gui_want = 1;
	if (w > 0) pd_gui_w = w;
	if (h > 0) pd_gui_h = h;
	if (getenv("PD_GUIDBG")) fprintf(stderr,"[gui] 开 GUI 档 %dx%d\n",pd_gui_w,pd_gui_h);
}

/* GLFW 键码 -> DOS 扫描码。只列脚本用得到的那些，别的忽略。 */
static int pd_scan_of (int key)
{
	switch(key)
	{
		case GLFW_KEY_ESCAPE:       return(0x01);
		case GLFW_KEY_1:            return(0x02);
		case GLFW_KEY_2:            return(0x03);
		case GLFW_KEY_3:            return(0x04);
		case GLFW_KEY_4:            return(0x05);
		case GLFW_KEY_5:            return(0x06);
		case GLFW_KEY_6:            return(0x07);
		case GLFW_KEY_7:            return(0x08);
		case GLFW_KEY_8:            return(0x09);
		case GLFW_KEY_9:            return(0x0a);
		case GLFW_KEY_0:            return(0x0b);
		case GLFW_KEY_MINUS:        return(0x0c);
		case GLFW_KEY_EQUAL:        return(0x0d);
		case GLFW_KEY_BACKSPACE:    return(0x0e);
		case GLFW_KEY_TAB:          return(0x0f);
		case GLFW_KEY_Q:            return(0x10);
		case GLFW_KEY_W:            return(0x11);
		case GLFW_KEY_E:            return(0x12);
		case GLFW_KEY_R:            return(0x13);
		case GLFW_KEY_T:            return(0x14);
		case GLFW_KEY_Y:            return(0x15);
		case GLFW_KEY_U:            return(0x16);
		case GLFW_KEY_I:            return(0x17);
		case GLFW_KEY_O:            return(0x18);
		case GLFW_KEY_P:            return(0x19);
		case GLFW_KEY_ENTER:        return(0x1c);
		case GLFW_KEY_LEFT_CONTROL: return(0x1d);
		case GLFW_KEY_A:            return(0x1e);
		case GLFW_KEY_S:            return(0x1f);
		case GLFW_KEY_D:            return(0x20);
		case GLFW_KEY_F:            return(0x21);
		case GLFW_KEY_G:            return(0x22);
		case GLFW_KEY_H:            return(0x23);
		case GLFW_KEY_J:            return(0x24);
		case GLFW_KEY_K:            return(0x25);
		case GLFW_KEY_L:            return(0x26);
		case GLFW_KEY_LEFT_SHIFT:   return(0x2a);
		case GLFW_KEY_Z:            return(0x2c);
		case GLFW_KEY_X:            return(0x2d);
		case GLFW_KEY_C:            return(0x2e);
		case GLFW_KEY_V:            return(0x2f);
		case GLFW_KEY_B:            return(0x30);
		case GLFW_KEY_N:            return(0x31);
		case GLFW_KEY_M:            return(0x32);
		case GLFW_KEY_RIGHT_SHIFT:  return(0x36);
		case GLFW_KEY_SPACE:        return(0x39);
		case GLFW_KEY_UP:           return(0xc8);
		case GLFW_KEY_LEFT:         return(0xcb);
		case GLFW_KEY_RIGHT:        return(0xcd);
		case GLFW_KEY_DOWN:         return(0xd0);
		case GLFW_KEY_PAGE_UP:      return(0xc9);
		case GLFW_KEY_PAGE_DOWN:    return(0xd1);
		case GLFW_KEY_INSERT:       return(0xd2);
		case GLFW_KEY_DELETE:       return(0xd3);
	}
	return(-1);
}

static void pd_cb_key (GLFWwindow *w, int key, int sc, int act, int mods)
{
	int s;
	(void)w;(void)sc;(void)mods;
	if (act == GLFW_REPEAT) return;
	s = pd_scan_of(key);
	if (s >= 0) pd_in_key(s,(act == GLFW_PRESS));
}
static void pd_cb_button (GLFWwindow *w, int b, int act, int mods)
{
	(void)w;(void)mods;
	     if (b == GLFW_MOUSE_BUTTON_LEFT)   pd_in_button(0,(act == GLFW_PRESS));
	else if (b == GLFW_MOUSE_BUTTON_RIGHT)  pd_in_button(1,(act == GLFW_PRESS));
	else if (b == GLFW_MOUSE_BUTTON_MIDDLE) pd_in_button(2,(act == GLFW_PRESS));
}
static void pd_cb_cursor (GLFWwindow *w, double x, double y)
{
	(void)w; pd_gui_mx = x; pd_gui_my = y;
}

/* `GetCursorPos` 在 GUI 档下问这儿要位置。回 0 表示"没开 GUI，你按老规矩来"。
   报的是**帧缓冲坐标**：Retina 上帧缓冲是窗口尺寸的两倍，而 polydraw 那边的
   xres/yres 用的是帧缓冲尺寸（见 pd_main_a64.c 里 `/WxH` 那一句），两头要一致。

   **第一句那个 `pd_gui_on()` 不许省**：离屏那条路（linux）也会开一个 GLFW 窗口
   （只是不可见），于是 `pd_win != 0` 也成立 —— 少了这一句就会报"鼠标在 0,0"，
   而不是 `pd_win_a64.c` 里那句"没开 GUI 就报画面中心"。踩过：`ken/orthoglobe.pss`
   的几何是 `z = mousy/yres*4` / `glvertex(c,-s,-z)`，mousy=0 ⇒ z=0 ⇒ 整片贴在
   相机平面上被近裁面切掉 ⇒ **全透明黑**（而且 GL 一个错都不报）。 */
int pd_gui_mouse (int *x, int *y)
{
	int ww = 1, wh = 1, fw = 1, fh = 1;
	if (!pd_gui_on()) return(0);
	if (!pd_win) return(0);
	glfwGetWindowSize(pd_win,&ww,&wh);
	glfwGetFramebufferSize(pd_win,&fw,&fh);
	if (ww <= 0) ww = 1;
	if (wh <= 0) wh = 1;
	if (x) *x = (int)(pd_gui_mx*(double)fw/(double)ww);
	if (y) *y = (int)(pd_gui_my*(double)fh/(double)wh);
	return(1);
}

/* 窗口那张帧缓冲多大（Retina 上是窗口尺寸的两倍）。回 0 = 没开 GUI。 */int pd_gui_fbsize (int *w, int *h)
{
	int fw = 0, fh = 0;
	if (!pd_win) return(0);
	glfwGetFramebufferSize(pd_win,&fw,&fh);
	if (w) *w = fw;
	if (h) *h = fh;
	return(1);
}

/* 标题栏：`SetWindowText` 转到这儿（原文每秒写一次"文件名 + fps"）。 */
int pd_gui_title (const char *s)
{
	if (!pd_win || !s) return(0);
	glfwSetWindowTitle(pd_win,s);
	if (getenv("PD_GUIDBG")) fprintf(stderr,"[gui] 标题 %s\n",s);
	return(1);
}

/* GLFW 自己的错误回调 —— 不挂这一格的话 `glfwInit`/`glfwCreateWindow` 失败时
   只能看见一句"失败了"，看不见原因（linux 上第一次就是这么卡住的：
   GLFW 3.4 先探 Wayland，没有 `XDG_RUNTIME_DIR` 就报错）。 */
static void pd_cb_err (int code, const char *msg)
	{ fprintf(stderr,"[gui] GLFW 错误 %d：%s\n",code,msg ? msg : "?"); }

/* 平台**点明**（GLFW 3.4 起有这一格）：容器里没有 Wayland，让它别去探。 */
static void pd_glfw_prep (void)
{
	glfwSetErrorCallback(pd_cb_err);
#if defined(GLFW_PLATFORM) && defined(GLFW_PLATFORM_X11) && !defined(__APPLE__)
	if (glfwPlatformSupported(GLFW_PLATFORM_X11)) glfwInitHint(GLFW_PLATFORM,GLFW_PLATFORM_X11);
#endif
}

int pd_gui_open (void){
	if (pd_win) return(1);
	pd_glfw_prep();
	if (!glfwInit()) { fprintf(stderr,"[gui] glfwInit 失败\n"); return(0); }
	/* 什么都不设 = legacy（macOS 上给 2.1）—— polydraw 的固定管线要的正是它。 */
	glfwWindowHint(GLFW_DOUBLEBUFFER,GLFW_TRUE);
	glfwWindowHint(GLFW_DEPTH_BITS,24);
	pd_win = glfwCreateWindow(pd_gui_w,pd_gui_h,"polydraw (arm64 macOS)",0,0);
	if (!pd_win) { fprintf(stderr,"[gui] 开窗口失败\n"); glfwTerminate(); return(0); }
	glfwMakeContextCurrent(pd_win);
	glfwSwapInterval(1);
	glfwSetKeyCallback(pd_win,pd_cb_key);
	glfwSetMouseButtonCallback(pd_win,pd_cb_button);
	glfwSetCursorPosCallback(pd_win,pd_cb_cursor);
	if (getenv("PD_GUIDBG")) fprintf(stderr,"[gui] 窗口开好了 %dx%d\n",pd_gui_w,pd_gui_h);
	return(1);
}

/**
 * **看不见的那一格**：只要上下文、不要窗口。
 *
 * 谁用：macOS 上出图那条路用的是 CGL（不需要窗口就有上下文，也不碰 AppKit）；
 * 别的平台没有那种东西 —— GLX 要么给 pbuffer（一堆样板代码）要么就得有窗口。
 * 既然 GUI 那条腿已经把 GLFW 这条路走通了，离屏就复用它：开一个
 * `GLFW_VISIBLE=FALSE` 的窗口当上下文的载体。**渲染照旧进我们自己的 FBO**，
 * 所以这个窗口的尺寸/可见性一个字都不影响出来的图。
 *
 * 代价：linux 上出图也要 `DISPLAY`（docker 里就是 XQuartz 转发那一格）。
 * 真要完全无头的话下一刀是 EGL surfaceless —— 但那会再引一个依赖，
 * 而"GLFW 跨平台共用"这件事本身是这条腿的一个目标。
 */
int pd_gui_open_offscreen (void)
{
	if (pd_win) return(1);
	pd_glfw_prep();
	if (!glfwInit()) { fprintf(stderr,"[gui] glfwInit 失败（离屏档）\n"); return(0); }
	glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
	glfwWindowHint(GLFW_DOUBLEBUFFER,GLFW_TRUE);
	glfwWindowHint(GLFW_DEPTH_BITS,24);
	pd_win = glfwCreateWindow(64,64,"polydraw (offscreen)",0,0);
	if (!pd_win) { fprintf(stderr,"[gui] 离屏上下文开不出来（DISPLAY=%s）\n",getenv("DISPLAY") ? getenv("DISPLAY") : "未设"); glfwTerminate(); return(0); }
	glfwMakeContextCurrent(pd_win);
	if (getenv("PD_GUIDBG")) fprintf(stderr,"[gui] 离屏上下文开好了（不可见窗口）\n");
	return(1);
}

int pd_gui_make_current (void){
	if (!pd_win) return(0);
	glfwMakeContextCurrent(pd_win);
	return(1);
}

/* `wglGetProcAddress` 的退路（非 macOS）：GL 的扩展函数不一定是 libGL 的导出符号，
   `dlsym` 查不到；GLFW 那一格底下就是 `glXGetProcAddress`/`eglGetProcAddress`，
   而且跨平台同一个名字。要求：调的时候得有当前上下文（polydraw 是先建上下文
   再填那张表，次序正好）。 */
void *pd_gui_procaddr (const char *nm)
{
	if (!nm) return(0);
	return((void *)glfwGetProcAddress(nm));
}

int pd_gui_closing (void)
{
	if (!pd_win) return(0);
	return(glfwWindowShouldClose(pd_win));
}

/* 一帧收尾：把 FBO 的 `vp` 那一块贴到窗口上，然后 swap + poll。
   贴法用 `glBlitFramebufferEXT`（EXT_framebuffer_blit，Apple 的 legacy GL 有）。 */
/* 一帧收尾：swap + poll。
 * GUI 档不走 FBO（直接画进窗口），所以 `fbo==0` 是常态 —— 那就只 swap + poll。
 * 留着 blit 那一档是给"以后要把离屏那张端上来"用的（`fbo != 0` 时才走）。
 * `PD_GUIDBG=1` 下每 30 帧读一次**窗口那张帧缓冲**的中心像素 —— 看不见窗口也能
 * 验"这一帧到底画了东西没有"。必须在 swap **之前**读：swap 之后后台缓冲未定义。 */
void pd_gui_present (unsigned int fbo, const int *vp)
{
	int fw = 0, fh = 0;
	if (!pd_win) return;
	glfwGetFramebufferSize(pd_win,&fw,&fh);
	if (fbo && vp && (vp[2] > 0) && (vp[3] > 0))
	{
		void (*blit)(int,int,int,int,int,int,int,int,unsigned int,unsigned int) =
			(void (*)(int,int,int,int,int,int,int,int,unsigned int,unsigned int))
			glfwGetProcAddress("glBlitFramebufferEXT");
		void (*bindfb)(unsigned int,unsigned int) =
			(void (*)(unsigned int,unsigned int))glfwGetProcAddress("glBindFramebufferEXT");
		if (blit && bindfb)
		{
			bindfb(0x8CA8/*READ_FRAMEBUFFER_EXT*/,fbo);
			bindfb(0x8CA9/*DRAW_FRAMEBUFFER_EXT*/,0);
			blit(vp[0],vp[1],vp[0]+vp[2],vp[1]+vp[3],0,0,fw,fh,
				0x00004000/*GL_COLOR_BUFFER_BIT*/,0x2600/*GL_NEAREST*/);
			bindfb(0x8CA8,0);
		}
	}
	if (getenv("PD_GUIDBG"))
	{
		/* 判据：**整张帧缓冲里非黑像素的个数**。抽样点靠不住 ——
		   `tigrou/clock.pss` 画的是黑底上的细线，3x3 那九个点一个都碰不上。
		   每 30 帧读一次（只在诊断档下），5MB 的回读不进常路。 */
		static long n = 0;
		if ((n++%30) == 0)
		{
			static unsigned char *buf = 0; static long bufn = 0;
			long need = (long)fw*(long)fh*4, i, cnt = 0;
			if (need > bufn) { free(buf); buf = (unsigned char *)malloc((size_t)need); bufn = buf ? need : 0; }
			if (buf)
			{
				glReadPixels(0,0,fw,fh,0x1908/*GL_RGBA*/,0x1401/*GL_UNSIGNED_BYTE*/,buf);
				for(i=0;i<need;i+=4) if (buf[i]|buf[i+1]|buf[i+2]) cnt++;
				fprintf(stderr,"[gui] 第 %ld 帧 非黑像素 %ld/%ld（窗口 %dx%d）\n",
					n-1,cnt,need/4,fw,fh);
			}
		}
	}
	glfwSwapBuffers(pd_win);
	glfwPollEvents();
	(void)pd_gl_size;
}

void pd_gui_close (void)
{
	if (!pd_win) return;
	glfwDestroyWindow(pd_win); pd_win = 0;
	glfwTerminate();
}
