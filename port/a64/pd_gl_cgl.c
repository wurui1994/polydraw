/* port/a64/pd_gl_cgl.c —— `wgl*` 那一族在 macOS 上的实现（CGL 离屏上下文）。
 *
 * 为什么用 CGL 而不是 GLFW/NSOpenGL：不需要窗口就能有上下文，而且不碰 AppKit
 * （不用主线程、不用 run loop）。出图那条路要的正是这个。
 *
 * 一格上下文 + 一张 FBO：`polydraw` 画到默认帧缓冲，我们把"默认"换成 FBO，
 * 于是 `glReadPixels` 读出来就是要存的那张图。
 *
 * 注意：macOS 的 legacy GL 最高到 **2.1**（core profile 才有 3.2+，但 core 里
 * 没有固定管线，而 polydraw 的 `glBegin/glEnd` 一族要固定管线）。所以这儿走
 * legacy profile —— 2.1 够 polydraw 用（它自己的着色器是 GLSL 1.20）。
 */
#include <windows.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CGLContextObj pd_cgl = 0;
static GLuint pd_fbo = 0, pd_color = 0, pd_depth = 0;
static int pd_fbw = 0, pd_fbh = 0;

/* 帧缓冲按需重建（polydraw 改分辨率时会重新 glViewport，但 FBO 得我们自己跟）。 */
int pd_gl_resize (int w, int h)
{
	if (!pd_cgl) return(0);
	if ((w == pd_fbw) && (h == pd_fbh) && pd_fbo) return(1);
	if (pd_fbo) { glDeleteFramebuffersEXT(1,&pd_fbo); glDeleteRenderbuffersEXT(1,&pd_color); glDeleteRenderbuffersEXT(1,&pd_depth); }
	glGenFramebuffersEXT(1,&pd_fbo);
	glGenRenderbuffersEXT(1,&pd_color);
	glGenRenderbuffersEXT(1,&pd_depth);
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT,pd_color);
	glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT,GL_RGBA8,w,h);
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT,pd_depth);
	glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT,GL_DEPTH_COMPONENT24,w,h);
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,pd_fbo);
	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_RENDERBUFFER_EXT,pd_color);
	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT,GL_DEPTH_ATTACHMENT_EXT,GL_RENDERBUFFER_EXT,pd_depth);
	if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT)
		{ fprintf(stderr,"pd_gl: FBO 不完整（%dx%d）\n",w,h); return(0); }
	pd_fbw = w; pd_fbh = h;
	glViewport(0,0,w,h);
	return(1);
}

/* 给调试器用的探针：`expr (void)pd_gl_probe("tag")` —— 把当帧的 GL 关键状态印出来。
   （lldb 里直接调 glGetIntegerv 要写一堆强转，包一层省事。） */
void pd_gl_probe (const char *tag)
{
	GLint fb = 0, pr = 0, dt = 0, cf = 0, bl = 0, vp[4] = {0,0,0,0};
	GLdouble mv[16], pj[16];
	glGetIntegerv(0x8CA6/*FRAMEBUFFER_BINDING_EXT*/,&fb);
	glGetIntegerv(0x8B8D/*CURRENT_PROGRAM*/,&pr);
	glGetIntegerv(GL_DEPTH_TEST,&dt);
	glGetIntegerv(GL_CULL_FACE,&cf);
	glGetIntegerv(GL_BLEND,&bl);
	glGetIntegerv(GL_VIEWPORT,vp);
	glGetDoublev(GL_MODELVIEW_MATRIX,mv);
	glGetDoublev(GL_PROJECTION_MATRIX,pj);
	fprintf(stderr,"[probe %s] fb=%d(ours=%u) prog=%d depth=%d cull=%d blend=%d vp=%d,%d,%d,%d err=%04x\n",
		tag,fb,pd_fbo,pr,dt,cf,bl,vp[0],vp[1],vp[2],vp[3],(unsigned)glGetError());
	fprintf(stderr,"[probe %s] mv[0,5,10,12,13,14]=%g,%g,%g,%g,%g,%g  pj[0,5,10,11,14]=%g,%g,%g,%g,%g\n",
		tag,mv[0],mv[5],mv[10],mv[12],mv[13],mv[14],pj[0],pj[5],pj[10],pj[11],pj[14]);
}

int pd_gl_size (int *w, int *h) { if (w) *w = pd_fbw; if (h) *h = pd_fbh; return(pd_fbo != 0); }

HGLRC wglCreateContext (HDC dc)
{
	CGLPixelFormatAttribute at[] = {
		kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
		kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
		kCGLPFADepthSize, (CGLPixelFormatAttribute)24,
		(CGLPixelFormatAttribute)0 };
	CGLPixelFormatObj pf = 0;
	GLint n = 0;
	(void)dc;
	if (pd_cgl) return((HGLRC)pd_cgl);
	if (CGLChoosePixelFormat(at,&pf,&n) != kCGLNoError) { fprintf(stderr,"pd_gl: CGLChoosePixelFormat 失败\n"); return(0); }
	if (CGLCreateContext(pf,0,&pd_cgl) != kCGLNoError) { CGLDestroyPixelFormat(pf); fprintf(stderr,"pd_gl: CGLCreateContext 失败\n"); return(0); }
	CGLDestroyPixelFormat(pf);
	return((HGLRC)pd_cgl);
}

BOOL wglMakeCurrent (HDC dc, HGLRC rc)
{
	(void)dc;
	if (!rc) { CGLSetCurrentContext(0); return(1); }
	if (CGLSetCurrentContext((CGLContextObj)rc) != kCGLNoError) return(0);
	/* **一次建足够大**（不按 viewport 重建）：重建会把已经画进去的内容丢掉，
	   而 polydraw 是先画、后我们才知道 viewport 多大 —— 踩过一次，第 0 帧的画面
	   就是这么丢的。存图时只取 viewport 那一块。 */
	if (!pd_fbo) pd_gl_resize(2048,1536);
	else glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,pd_fbo);
	return(1);
}

BOOL wglDeleteContext (HGLRC rc)
{
	if (!rc) return(0);
	if ((CGLContextObj)rc == pd_cgl) { CGLDestroyContext(pd_cgl); pd_cgl = 0; pd_fbo = 0; }
	return(1);
}

/* ── 把"绑定 0 号帧缓冲"翻成"绑定我们那张 FBO" ──
 *
 * 离屏根本没有"0"那张。而 polydraw 在 `pd_host_gl.c:301` 用 `…(GL_FRAMEBUFFER_EXT,0)`
 * 去"restore"（`printg_init` 与每次 `glsettex` 都走这条），一绑就把我们那张换下来 ——
 * 后面画的东西全进了不存在的默认帧缓冲，于是**存出来的图全透明黑**。
 * 量到过：`qglEnd` 那一刻 `FRAMEBUFFER_BINDING` 是 0、viewport 是 0,0,0,0。
 *
 * polydraw 这一族是走 `wglGetProcAddress` 填的函数指针表，所以在那儿掉包就行。
 */
static void (*pd_real_bindfb)(GLenum, GLuint) = 0;
static void pd_bindfb_wrap (GLenum target, GLuint fb)
{
	if (!fb) fb = pd_fbo;
	if (getenv("PD_FBDBG")) fprintf(stderr,"[bind] frame=%ld -> %u\n",(long)0,fb);
	if (pd_real_bindfb) pd_real_bindfb(target,fb);
}

/* `wglGetProcAddress`：polydraw 靠它填那张 GL 2.0 的函数指针表。
   macOS 没有对应的 API —— 但 legacy GL 的符号全在 OpenGL.framework 里，
   所以 `dlsym(RTLD_DEFAULT, 名字)` 就是答案。 */
void *wglGetProcAddress (LPCSTR nam)
{
	void *p = dlsym(RTLD_DEFAULT,nam);
	if (!strcmp(nam,"glBindFramebufferEXT") || !strcmp(nam,"glBindFramebuffer"))
	{
		if (!p) p = dlsym(RTLD_DEFAULT,"glBindFramebufferEXT");
		if (p) { pd_real_bindfb = (void (*)(GLenum,GLuint))p; return((void *)pd_bindfb_wrap); }
	}
	if (!p)
	{
		/* 有几个在 macOS 上只有 `*EXT`/`*ARB` 那一版（FBO 一族就是）。 */
		char buf[256];
		snprintf(buf,sizeof(buf),"%sEXT",nam); p = dlsym(RTLD_DEFAULT,buf);
		if (!p) { snprintf(buf,sizeof(buf),"%sARB",nam); p = dlsym(RTLD_DEFAULT,buf); }
	}
	return(p);
}

int ChoosePixelFormat (HDC dc, const PIXELFORMATDESCRIPTOR *p) { (void)dc;(void)p; return(1); }
BOOL SetPixelFormat (HDC dc, int i, const PIXELFORMATDESCRIPTOR *p) { (void)dc;(void)i;(void)p; return(1); }
/* ── 存图：一个最小的 PNG 写出器 ──
 * 为什么自己写：kplib 只**读** PNG，不写。deflate 那一半用"存储块"（不压缩）——
 * 图是给人看与逐像素对照用的，体积不重要，少一个依赖更要紧。 */
static unsigned int pd_crc_tab[256];
static void pd_crc_init (void)
{
	unsigned int c; int n, k;
	for(n=0;n<256;n++)
	{
		c = (unsigned int)n;
		for(k=0;k<8;k++) c = (c&1) ? (0xEDB88320u^(c>>1)) : (c>>1);
		pd_crc_tab[n] = c;
	}
}
static unsigned int pd_crc (const unsigned char *p, long n, unsigned int c)
{
	long i;
	for(i=0;i<n;i++) c = pd_crc_tab[(c^p[i])&0xff]^(c>>8);
	return(c);
}
static void pd_be32 (unsigned char *p, unsigned int v)
{ p[0] = (unsigned char)(v>>24); p[1] = (unsigned char)(v>>16); p[2] = (unsigned char)(v>>8); p[3] = (unsigned char)v; }
static void pd_chunk (FILE *f, const char *tag, const unsigned char *d, long n)
{
	unsigned char hd[8]; unsigned int c;
	pd_be32(hd,(unsigned int)n); memcpy(&hd[4],tag,4);
	fwrite(hd,1,8,f);
	c = pd_crc((const unsigned char *)tag,4,0xffffffffu);
	if (n) { fwrite(d,1,(size_t)n,f); c = pd_crc(d,n,c); }
	pd_be32(hd,c^0xffffffffu); fwrite(hd,1,4,f);
}
static int pd_write_png (const char *path, const unsigned char *rgba, int w, int h)
{
	static const unsigned char sig[8] = {137,80,78,71,13,10,26,10};
	unsigned char ihdr[13], *raw, *z;
	long rawn, zn, at, i;
	unsigned int a = 1, b = 0;
	FILE *f;

	pd_crc_init();
	/* 扫描行：每行一个 filter 字节（0）+ w*4；**GL 的行序是自下而上**，翻过来。 */
	rawn = (long)h*((long)w*4+1);
	raw = (unsigned char *)malloc((size_t)rawn); if (!raw) return(0);
	for(i=0;i<h;i++)
	{
		unsigned char *d = &raw[i*((long)w*4+1)];
		d[0] = 0;
		memcpy(&d[1],&rgba[(long)(h-1-i)*(long)w*4],(size_t)w*4);
	}
	/* zlib：2 字节头 + 一串存储块 + adler32。 */
	zn = 2 + ((rawn+65534)/65535)*5 + rawn + 4;
	z = (unsigned char *)malloc((size_t)zn); if (!z) { free(raw); return(0); }
	z[0] = 0x78; z[1] = 0x01; at = 2;
	for(i=0;i<rawn;)
	{
		long n = rawn-i; if (n > 65535) n = 65535;
		z[at++] = (i+n >= rawn) ? 1 : 0;
		z[at++] = (unsigned char)(n&0xff);  z[at++] = (unsigned char)(n>>8);
		z[at++] = (unsigned char)(~n&0xff); z[at++] = (unsigned char)((~n>>8)&0xff);
		memcpy(&z[at],&raw[i],(size_t)n); at += n; i += n;
	}
	for(i=0;i<rawn;i++) { a = (a+raw[i])%65521u; b = (b+a)%65521u; }
	pd_be32(&z[at],(b<<16)|a); at += 4;

	f = fopen(path,"wb"); if (!f) { free(raw); free(z); return(0); }
	fwrite(sig,1,8,f);
	pd_be32(&ihdr[0],(unsigned int)w); pd_be32(&ihdr[4],(unsigned int)h);
	ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0; /* RGBA8 */
	pd_chunk(f,"IHDR",ihdr,13);
	pd_chunk(f,"IDAT",z,at);
	pd_chunk(f,"IEND",0,0);
	fclose(f); free(raw); free(z);
	return(1);
}

/* ── 一帧画完的钩子 ── */
static long pd_frame = 0, pd_shot = -1;
static const char *pd_shot_path = 0;
void pd_gl_shoot_at (long frame, const char *path) { pd_shot = frame; pd_shot_path = path; }

/* 没有窗口可以交换 —— 这儿就是"一帧画完"：该存图的那一帧把像素读出来写 PNG。 */
BOOL SwapBuffers (HDC dc)
{
	GLint vp[4];
	(void)dc;
	glFlush();
	/* polydraw 每帧 `glViewport(0,0,oglxres,oglyres)`（`pd_win.c:714`），而那是
	   **渲染窗格**的尺寸（编辑器占掉了另一半），不是我们建 FBO 时用的那个。
	   量到过：窗口 640x480 时 oglxres/oglyres 是 320x240 —— 不跟着改的话，
	   画面只落在 FBO 的左下角一块，存出来的图一大半是空的。
	   所以每帧收尾时按当前 viewport 把 FBO 对齐（下一帧生效，存图在第 30+ 帧，够了）。 */
	if (getenv("PD_GLDBG"))
	{
		GLint fb = -1, dr = -1; GLenum e = glGetError();
		glGetIntegerv(0x8CA6/*GL_FRAMEBUFFER_BINDING_EXT*/,&fb);
		glGetIntegerv(GL_DRAW_BUFFER,&dr);
		glGetIntegerv(GL_VIEWPORT,vp);
		{
			unsigned char c4[4] = {9,9,9,9};
			glReadPixels(pd_fbw/2,pd_fbh/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,c4);
			fprintf(stderr,"[gl] 中心像素 %d,%d,%d,%d\n",c4[0],c4[1],c4[2],c4[3]);
		}
		fprintf(stderr,"[gl] frame=%ld fb=%d(ours=%u) drawbuf=%04x vp=%d,%d,%d,%d err=%04x\n",
			(long)pd_frame,fb,pd_fbo,dr,vp[0],vp[1],vp[2],vp[3],(unsigned)e);
	}
	if (getenv("PD_GLPROBE"))
	{
		/* 二分：拿 polydraw **当前的**状态画同一个四边形，然后逐样撤掉状态再画，
		   看哪一样一撤就出来 —— 那就是吃掉画面的那一格。 */
		unsigned char c4[4];
		int k;
		for(k=0;k<4;k++)
		{
			const char *what = "原样";
			if (k == 1) { glUseProgram(0); what = "+去着色器"; }
			if (k == 2) { glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); what = "+去深度/剔除"; }
			if (k == 3) { glMatrixMode(GL_PROJECTION); glLoadIdentity();
			              glMatrixMode(GL_MODELVIEW);  glLoadIdentity(); what = "+单位矩阵"; }
			glBegin(GL_QUADS);
			glColor3d(0,1,0);
			glVertex3d(-1,-1,-2); glVertex3d(1,-1,-2); glVertex3d(1,1,-2); glVertex3d(-1,1,-2);
			glEnd();
			glFinish();
			glReadPixels(pd_fbw/2,pd_fbh/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,c4);
			fprintf(stderr,"[bisect] %-14s -> %d,%d,%d,%d err=%04x\n",what,c4[0],c4[1],c4[2],c4[3],(unsigned)glGetError());
		}
	}
	(void)vp;
	if ((pd_frame == pd_shot) && pd_shot_path && pd_fbo)
	{
		unsigned char *px = (unsigned char *)malloc((size_t)pd_fbw*(size_t)pd_fbh*4);
		if (px)
		{
			/* polydraw 自己也用 FBO（render-to-texture），画完会 bind 回 0 ——
			   而离屏根本没有0这张。读之前先把我们的绑回来。 */
			GLint vv[4] = {0,0,pd_fbw,pd_fbh};
			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,pd_fbo);
			/* FBO 是一次建的大张（见 pd_gl_resize 的注），**存图只取当前 viewport
			   那一块** —— 那才是 polydraw 的渲染窗格。 */
			glGetIntegerv(GL_VIEWPORT,vv);
			if ((vv[2] <= 0) || (vv[3] > pd_fbh) || (vv[2] > pd_fbw)) { vv[0] = vv[1] = 0; vv[2] = pd_fbw; vv[3] = pd_fbh; }
			glPixelStorei(GL_PACK_ALIGNMENT,1);
			glReadPixels(vv[0],vv[1],vv[2],vv[3],GL_RGBA,GL_UNSIGNED_BYTE,px);
			if (pd_write_png(pd_shot_path,px,vv[2],vv[3]))
				fprintf(stderr,"[png] %s  %dx%d（第 %ld 帧）\n",pd_shot_path,vv[2],vv[3],pd_frame);
			free(px);
		}
	}
	pd_frame++;
	return(1);
}

/* polydraw 第一帧会 `wglGetProcAddress("wglSwapIntervalEXT")` 然后直接调 ——
   离屏没有 vsync 可关，但那个指针不能是空的（空了就跳到 0）。 */
void wglSwapIntervalEXT (int n) { (void)n; }
HDC GetDC (HWND h) { (void)h; return((HDC)1); }
int ReleaseDC (HWND h, HDC d) { (void)h;(void)d; return(1); }
