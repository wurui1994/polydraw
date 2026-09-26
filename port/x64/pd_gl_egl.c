/* port/x64/pd_gl_egl.c —— **无头**的 GL 上下文（EGL surfaceless + mesa llvmpipe）。
 *
 * ## 为什么要这一份
 *
 * 出图那条路本来就不该要显示器。macOS 上是 CGL（不要窗口、不碰 AppKit）；linux 上
 * 第一版借了 GUI 那条腿的 **GLFW 不可见窗口**，代价是"连出图也要一个 `DISPLAY`"——
 * 判据里得先起一格 Xvfb，容器跑 CI 时白添一个依赖。
 *
 * EGL 的 surfaceless 平台正好是这件事的标准答案：**没有 X、没有窗口、没有 surface**，
 * 上下文直接建出来，我们照旧画进自己那张 FBO。量到的（容器里，qemu 转译）：
 *
 *     EGL 1.5  vendor=Mesa Project
 *     GL_VERSION  4.6 (Compatibility Profile) Mesa 26.2.3
 *     GL_RENDERER llvmpipe (LLVM 22.1.8, 128 bits)
 *     固定管线 glBegin/glEnd 画进 FBO -> 中心像素 0,255,0,255
 *
 * **兼容档**这一点很要紧：polydraw 的 `glBegin/glEnd` 一族要固定管线，所以
 * `eglBindAPI(EGL_OPENGL_API)` 之后不传 profile 属性 —— mesa 给的就是 compat。
 * （传 core 的话固定管线全没了，与 macOS 上"不能用 core profile"是同一件事。）
 *
 * ## 三个坑
 *
 *   1. `eglGetDisplay(EGL_DEFAULT_DISPLAY)` 在没有 `DISPLAY` 时会去试 X11，
 *      然后 `eglInitialize` 回 **0x3001（EGL_NOT_INITIALIZED）**。必须点明平台：
 *      `eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, …)`（EGL 1.5 core），
 *      拿不到函数指针就退到 `setenv("EGL_PLATFORM","surfaceless")` + 老 API；
 *   2. `EGL_SURFACE_TYPE` 要写 `EGL_PBUFFER_BIT`（不是 WINDOW）—— surfaceless 上
 *      根本没有窗口那一类 config；
 *   3. 扩展函数的地址走 `eglGetProcAddress`（EGL 1.5 起它也认 GL 的名字）——
 *      **不能**再用 `glfwGetProcAddress`，GLFW 这条路上压根没 init。
 *
 * `PD_EGL=0` 关掉这一份（退回 GLFW 的不可见窗口，A/B 对照用）。
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

static EGLDisplay pd_egl_dpy = EGL_NO_DISPLAY;
static EGLContext pd_egl_ctx = EGL_NO_CONTEXT;

/* 这一份是不是已经在用了（`pd_gl_cgl.c` 查 proc 时要据此分流）。 */
int pd_egl_on (void) { return(pd_egl_ctx != EGL_NO_CONTEXT); }

int pd_egl_open (void)
{
	EGLConfig cfg;
	EGLint n = 0, maj = 0, min = 0;
	static const EGLint ca[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24,
		EGL_NONE };
	const char *off = getenv("PD_EGL");

	if (pd_egl_ctx != EGL_NO_CONTEXT) return(1);
	if ((off) && (atoi(off) == 0)) return(0);

	{
		PFNEGLGETPLATFORMDISPLAYEXTPROC getplat =
			(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
		if (getplat) pd_egl_dpy = getplat(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,0);
		if (pd_egl_dpy == EGL_NO_DISPLAY)
			{ setenv("EGL_PLATFORM","surfaceless",1); pd_egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY); }
	}
	if (pd_egl_dpy == EGL_NO_DISPLAY) { fprintf(stderr,"[egl] 拿不到 display\n"); return(0); }
	if (!eglInitialize(pd_egl_dpy,&maj,&min))
		{ fprintf(stderr,"[egl] eglInitialize 失败 %04x\n",eglGetError()); pd_egl_dpy = EGL_NO_DISPLAY; return(0); }
	if (!eglBindAPI(EGL_OPENGL_API)) { fprintf(stderr,"[egl] 这套 EGL 不给桌面 GL\n"); return(0); }
	if ((!eglChooseConfig(pd_egl_dpy,ca,&cfg,1,&n)) || (!n))
		{ fprintf(stderr,"[egl] eglChooseConfig 失败 %04x\n",eglGetError()); return(0); }
	/* 不传 profile 属性 = 兼容档（固定管线在）。 */
	pd_egl_ctx = eglCreateContext(pd_egl_dpy,cfg,EGL_NO_CONTEXT,0);
	if (pd_egl_ctx == EGL_NO_CONTEXT)
		{ fprintf(stderr,"[egl] eglCreateContext 失败 %04x\n",eglGetError()); return(0); }
	if (!eglMakeCurrent(pd_egl_dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,pd_egl_ctx))
		{ fprintf(stderr,"[egl] eglMakeCurrent 失败 %04x\n",eglGetError()); pd_egl_ctx = EGL_NO_CONTEXT; return(0); }
	if (getenv("PD_GLDBG")) fprintf(stderr,"[egl] surfaceless 上下文好了（EGL %d.%d）\n",maj,min);
	return(1);
}

int pd_egl_make_current (void)
{
	if (pd_egl_ctx == EGL_NO_CONTEXT) return(0);
	return(eglMakeCurrent(pd_egl_dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,pd_egl_ctx) ? 1 : 0);
}

void *pd_egl_procaddr (const char *nm)
{
	if ((!nm) || (pd_egl_ctx == EGL_NO_CONTEXT)) return(0);
	return((void *)eglGetProcAddress(nm));
}
