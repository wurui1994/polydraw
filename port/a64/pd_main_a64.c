/* port/a64/pd_main_a64.c —— 出图的入口（arm64 macOS）。
 *
 * 思路：**不重写 polydraw 的帧循环，去喂它**。`pd_win.c:699` 那个
 * `while(1){ PeekMessage…; …; GetWindowText(hWndEdit,…); setShaders; Draw; SwapBuffers; }`
 * 原文一个字节都没改，我们只做三件事：
 *
 *   1. `main()` 把 `.pss` 读进一格全局，然后调原文的 `WinMain`；
 *   2. 假的编辑框：`GetWindowText` 回那格全局 —— 于是 `Draw` 拿到的就是脚本正文；
 *   3. `SwapBuffers`（在 `pd_gl_cgl.c`）当"一帧画完"的钩子：数帧、计时、
 *      到点了 `glReadPixels` 写 PNG。
 *
 * 命令行走原文认的那几个（`/bench:N` 是我们先前给 polydraw.c 加的插桩，
 * 它自己就会在第 30 帧起计时、跑满 N 帧后退出）：
 *
 *   polydraw_a64 ken/balls.pss --frames 60 --size 640x480 --out out.png
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int WinMain (HINSTANCE, HINSTANCE, LPSTR, int);
extern void pd_gl_shoot_at (long frame, const char *path);
/* GUI 那条腿（`port/a64/pd_gui_glfw.c`）。 */
extern void pd_gui_enable (int w, int h);
extern int pd_gui_open (void);
extern int pd_gui_fbsize (int *w, int *h);
extern void pd_gui_close (void);

/* 那格全局：脚本正文。`GetWindowText` 就回它。 */
static char *pd_text = 0;
static long pd_textlen = 0;

int GetWindowText (HWND h, LPSTR buf, int n)
{
	long l;
	(void)h;
	if (!n) return(0);
	if (!pd_text) { buf[0] = 0; return(0); }
	l = pd_textlen; if (l > n-1) l = n-1;
	memcpy(buf,pd_text,(size_t)l); buf[l] = 0;
	return((int)l);
}

static int pd_load (const char *path)
{
	FILE *f = fopen(path,"rb");
	long n;
	if (!f) { fprintf(stderr,"打不开 %s\n",path); return(0); }
	fseek(f,0,SEEK_END); n = ftell(f); fseek(f,0,SEEK_SET);
	pd_text = (char *)malloc((size_t)n+1);
	if (!pd_text) { fclose(f); return(0); }
	pd_textlen = (long)fread(pd_text,1,(size_t)n,f);
	pd_text[pd_textlen] = 0;
	fclose(f);
	return(1);
}

int main (int argc, char **argv)
{
	char cmd[1024], *src = 0, *out = "out.png";
	int i, frames = 60, w = 640, h = 480, gui = -1;

	for(i=1;i<argc;i++)
	{
		if (!strcmp(argv[i],"--frames") && (i+1 < argc)) { frames = atoi(argv[++i]); if (gui < 0) gui = 0; continue; }
		if (!strcmp(argv[i],"--out")    && (i+1 < argc)) { out = argv[++i]; if (gui < 0) gui = 0; continue; }
		if (!strcmp(argv[i],"--size")   && (i+1 < argc)) { sscanf(argv[++i],"%dx%d",&w,&h); continue; }
		if (!strcmp(argv[i],"--gui"))    { gui = 1; continue; }
		if (!strcmp(argv[i],"--render")) { gui = 0; continue; }
		if (argv[i][0] != '-') { src = argv[i]; continue; }
		fprintf(stderr,"不认的旗子：%s\n",argv[i]); return(64);
	}
	if (!src)
	{
		printf("用法: polydraw_a64 脚本.pss [--gui|--render] [--frames N] [--size WxH] [--out 图.png]\n");
		printf("      不给旗子 = --gui（开窗口实时跑，与原版双击一样）；--render 是离屏出图。\n");
		return(64);
	}
	/* **不给旗子就开窗口**。理由是"别默默地干等"：离屏那条腿要跑满 `frames+30` 帧才写
	   PNG，中间既没有窗口也没有一行输出 —— 在解释器那条路上（还没有 arm64 JIT）
	   一帧就是几百毫秒到秒级，于是 `polydraw_a64 ken/balls.pss` 看着像死循环。
	   原版双击起来就是个窗口，所以默认跟它一致。给了 `--frames`/`--out`/`--render`
	   才走离屏（判据脚本全都给）。 */
	if (gui < 0) gui = 1;
	if (!pd_load(src)) return(66);

	/* GUI 档：开真窗口、实时跑到关窗为止（`port/a64/pd_gui_glfw.c`）。
	   与出图那条腿共用同一条渲染路径 —— polydraw 照旧画进我们的 FBO，
	   每帧收尾 blit 到窗口。所以 GUI 不给 `/bench:N`（那是"跑满 N 帧就退"）。 */
	if (gui) pd_gui_enable(w,h);
	/* 先把窗口开起来，然后**按帧缓冲的真实尺寸**告诉 polydraw（`/WxH`）——
	   Retina 上帧缓冲是窗口尺寸的两倍，按窗口尺寸给的话画面只铺满四分之一。 */
	if (gui && pd_gui_open()) pd_gui_fbsize(&w,&h);

	/* 最后一帧存图。`/bench:N` 的口径是"30 帧暖机 + N 帧计时"，所以总帧数是 N+30。 */
	/* PD_SHOT=n 存第 n 帧（调试用）。 */
	if (!gui)
	{
		const char *sh = getenv("PD_SHOT");
		/* 默认存**最后一帧**（`/bench:N` 的口径是 30 帧暖机 + N 帧计时）。
		   `PD_SHOT=n` 可以改成任意一帧。 */
		pd_gl_shoot_at(sh ? atol(sh) : (long)frames+29,out);
	}

	/* 交给原文的 WinMain。`/bench:N` 让它别等焦点、别 Sleep(1)，并自己计时。 */
	if (gui) snprintf(cmd,sizeof(cmd),"/%dx%d",w,h);
	else     snprintf(cmd,sizeof(cmd),"/bench:%d /%dx%d",frames,w,h);
	i = WinMain(0,0,cmd,1 /*SW_SHOWNORMAL*/);
	if (gui) pd_gui_close();
	return(i);
}
