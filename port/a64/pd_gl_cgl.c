/* port/a64/pd_gl_cgl.c —— `wgl*` 那一族的实现（**上下文**那一格按平台分岔）。
 *
 * * macOS：CGL 离屏上下文。为什么用 CGL 而不是 GLFW/NSOpenGL —— 不需要窗口就能有
 *   上下文，而且不碰 AppKit（不用主线程、不用 run loop）。出图那条路要的正是这个；
 * * 别的平台（linux x86-64 那条腿）：**GLFW 的不可见窗口**
 *   （`pd_gui_open_offscreen`，与 GUI 那条腿同一份源码）。GLX 自己那套 pbuffer
 *   样板代码不写 —— "GLFW 跨平台共用"本来就是这条腿的目标之一。
 *
 * 一格上下文 + 一张 FBO：`polydraw` 画到默认帧缓冲，我们把"默认"换成 FBO，
 * 于是 `glReadPixels` 读出来就是要存的那张图。
 *
 * 注意：macOS 的 legacy GL 最高到 **2.1**（core profile 才有 3.2+，但 core 里
 * 没有固定管线，而 polydraw 的 `glBegin/glEnd` 一族要固定管线）。所以那边走
 * legacy profile —— 2.1 够 polydraw 用（它自己的着色器是 GLSL 1.20）。
 * mesa 那边给的是 4.5 兼容档（2.1 的超集），同一份代码照跑。
 */
#if !defined(__APPLE__)
/* 我们这一份要的是**真**的 GL 函数，不是 pd_head.h 那张指针表 —— 所以在包
   winshim 的 `gl/gl.h` 之前先把那 43 行让位关掉（见那一份的注）。 */
#define PD_NO_GL_RENAME 1
#endif
#include <windows.h>
#if defined(__APPLE__)
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#else
/* mesa：`GL_GLEXT_PROTOTYPES` 才有 FBO 那一族的原型（EXT 版 mesa 也导出）。 */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
/* 无头上下文那一格（port/x64/pd_gl_egl.c，EGL surfaceless + llvmpipe）——
   出图那条路的**默认**。GLFW 那个不可见窗口只是退路（EGL 开不出来时）。 */
extern int pd_egl_open (void);
extern int pd_egl_on (void);
extern int pd_egl_make_current (void);
extern void *pd_egl_procaddr (const char *nm);
/* GLFW 的不可见窗口（port/a64/pd_gui_glfw.c），要 DISPLAY。 */
extern int pd_gui_open_offscreen (void);
#endif
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
static CGLContextObj pd_cgl = 0;
#else
static int pd_cgl = 0;            /* 非 macOS：只是"开没开"的标志 */
#endif
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

/* ── GUI 那条腿（`port/a64/pd_gui_glfw.c`）——
   开了 `--gui` 就把上下文交给窗口，渲染路径一个字不改（照旧画进我们的 FBO），
   每帧收尾多做一步：把 viewport 那一块 blit 到窗口的 0 号帧缓冲。 */
extern int pd_gui_on (void);
extern int pd_gui_open (void);
extern int pd_gui_make_current (void);
extern void pd_gui_present (unsigned int fbo, const int *vp);
#if !defined(__APPLE__)
extern void *pd_gui_procaddr (const char *nm);
#endif

HGLRC wglCreateContext (HDC dc)
{
	(void)dc;
	/* GUI 档：窗口自己带上下文，**不要**再开一个离屏的 —— 两个上下文之间
	   对象不共享，FBO 就 blit 不过去了。 */
	if (pd_gui_on()) { if (!pd_gui_open()) return(0); return((HGLRC)1); }
#if defined(__APPLE__)
	{
		CGLPixelFormatAttribute at[] = {
			kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
			kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
			kCGLPFADepthSize, (CGLPixelFormatAttribute)24,
			(CGLPixelFormatAttribute)0 };
		CGLPixelFormatObj pf = 0;
		GLint n = 0;
		if (pd_cgl) return((HGLRC)pd_cgl);
		if (CGLChoosePixelFormat(at,&pf,&n) != kCGLNoError) { fprintf(stderr,"pd_gl: CGLChoosePixelFormat 失败\n"); return(0); }
		if (CGLCreateContext(pf,0,&pd_cgl) != kCGLNoError) { CGLDestroyPixelFormat(pf); fprintf(stderr,"pd_gl: CGLCreateContext 失败\n"); return(0); }
		CGLDestroyPixelFormat(pf);
		return((HGLRC)pd_cgl);
	}
#else
	/* 出图那条路：**先要无头的**（EGL surfaceless + llvmpipe，不要 DISPLAY），
	   开不出来才退到 GLFW 的不可见窗口（那一条要 X）。 */
	if (pd_cgl) return((HGLRC)1);
	if (pd_egl_open()) { pd_cgl = 1; return((HGLRC)1); }
	if (!pd_gui_open_offscreen()) return(0);
	pd_cgl = 1;
	return((HGLRC)1);
#endif
}

BOOL wglMakeCurrent (HDC dc, HGLRC rc)
{
	(void)dc;
	if (pd_gui_on())
	{
		if (!rc) return(1);
		if (!pd_gui_make_current()) return(0);
		/* GUI 档**不建 FBO**：直接画进窗口那张默认帧缓冲。
		   于是 `pd_bindfb_wrap` 把"绑 0"映到 pd_fbo(=0) 正好是原意，
		   polydraw 自己那些 render-to-texture 的 FBO 也照常工作。
		   （离屏那条路非得有 FBO 是因为**离屏根本没有 0 号那张**。） */
		if (getenv("PD_GUIDBG")) fprintf(stderr,"[gui] wglMakeCurrent：直接画进窗口（不建 FBO）\n");
		return(1);
	}
#if defined(__APPLE__)
	if (!rc) { CGLSetCurrentContext(0); return(1); }
	if (CGLSetCurrentContext((CGLContextObj)rc) != kCGLNoError) return(0);
#else
	if (!rc) return(1);
	if (pd_egl_on()) { if (!pd_egl_make_current()) return(0); }
	else if (!pd_gui_make_current()) return(0);
#endif
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
#if defined(__APPLE__)
	if ((CGLContextObj)rc == pd_cgl) { CGLDestroyContext(pd_cgl); pd_cgl = 0; pd_fbo = 0; }
#else
	pd_fbo = 0;
#endif
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

/* 三维贴图那两格的诊断（`PD_TEXDBG=1`）：它们走 `glfp[]` 那张表，`pd_gl_texdbg.c`
   里 `#define` 那一招钩不到 —— 只能在**解析那一刻**把落点换成包了一层的。 */
static void (*pd_real_teximg3d)(GLenum,GLint,GLint,GLsizei,GLsizei,GLsizei,GLint,GLenum,GLenum,const void *) = 0;
static void (*pd_real_texsub3d)(GLenum,GLint,GLint,GLint,GLint,GLsizei,GLsizei,GLsizei,GLenum,GLenum,const void *) = 0;

static void pd_teximg3d_wrap (GLenum tar, GLint lev, GLint ifmt, GLsizei w, GLsizei h, GLsizei d,
	GLint bord, GLenum fmt, GLenum typ, const void *px)
{
	if (pd_real_teximg3d) pd_real_teximg3d(tar,lev,ifmt,w,h,d,bord,fmt,typ,px);
	if (getenv("PD_TEXDBG"))
		fprintf(stderr,"[tex] image3D tar=%x lev=%d ifmt=%d %dx%dx%d fmt=%x typ=%x px=%p err=%x\n",
			tar,lev,ifmt,(int)w,(int)h,(int)d,fmt,typ,px,glGetError());
}
static void pd_texsub3d_wrap (GLenum tar, GLint lev, GLint xo, GLint yo, GLint zo,
	GLsizei w, GLsizei h, GLsizei d, GLenum fmt, GLenum typ, const void *px)
{
	if (pd_real_texsub3d) pd_real_texsub3d(tar,lev,xo,yo,zo,w,h,d,fmt,typ,px);
	if (getenv("PD_TEXDBG"))
		fprintf(stderr,"[tex] sub3D tar=%x lev=%d at=%d,%d,%d %dx%dx%d fmt=%x typ=%x px=%p err=%x\n",
			tar,lev,xo,yo,zo,(int)w,(int)h,(int)d,fmt,typ,px,glGetError());
}

/* ── 着色器源码前面塞一行 `#extension`（`GL_EXT_gpu_shader4`）──────────────────
 *
 * macOS 的 legacy profile 只到 **GLSL 1.20**，而 1.20 没有整数位运算，也没有
 * int -> float 的隐式提升。语料里 `ken/gspiral.pss` 正好要这两样：
 *   `f = dot(vec4(i&255,(i>>8)&255,…),…)`  -> `'&' does not operate on 'int' and 'int'`
 *   `f*f*npoints*(1.0/16.0)`（npoints 是 uniform int）-> `'*' … 'float' and 'int'`
 * 先前这一份记成"GLSL 1.20 的上限、要换 core profile"——**那是错的**：
 * 这台机器的 2.1 上下文**有 `GL_EXT_gpu_shader4`**（探针量过，还有
 * `GL_ARB_shader_texture_lod`），那个扩展给的正是位运算 + 整型提升 + 片元里的
 * `texture2DLod`。所以只要在源码最前面加一行 `#extension … : enable` 就行，
 * 一个字的脚本都不用改。
 *
 * 两个细节：`#extension` 必须在任何非预处理记号之前（所以塞在最前面）；
 * 塞完要补一句 `#line 1`，不然 polydraw 那个 `glsl_geterrorlines`
 * 报的行号会整体偏移。`: enable` 而不是 `require` —— 不支持的机器上不会当场报错。
 */
static void (*pd_real_shadersrc)(GLuint, GLsizei, const GLchar **, const GLint *) = 0;

static void pd_shadersrc_wrap (GLuint sh, GLsizei cnt, const GLchar **str, const GLint *len)
{
	static const char *pre =
		"#extension GL_EXT_gpu_shader4 : enable\n"
		"#extension GL_ARB_shader_texture_lod : enable\n"
		"#line 1\n";
	static char *buf = 0; static long bufn = 0;
	const char *src, *ver;
	long l0, l1, at = 0;
	if (!pd_real_shadersrc) return;
	/* 一段以上、或者给了长度表的那种：原样转（语料里 polydraw 只发一段、len=0）。 */
	if ((cnt != 1) || (len) || (!str) || (!str[0])) { pd_real_shadersrc(sh,cnt,str,len); return; }
	src = str[0];
	/* **`#version` 必须是第一句**（几何着色器那一族 polydraw 自己会写 `#version …`）——
	   所以有它的时候塞在那一行**后头**，没有就塞最前面。踩过：一律塞最前面 ⇒
	   `geo_test` / `geo_duptris` 报 "#version must occur before any other statement"。 */
	ver = strstr(src,"#version");
	if (ver)
	{
		const char *nl = strchr(ver,'\n');
		at = nl ? (long)(nl+1-src) : (long)strlen(src);
	}
	l0 = (long)strlen(pre); l1 = (long)strlen(src);
	if (l0+l1+1 > bufn) { bufn = l0+l1+1; buf = (char *)realloc(buf,(size_t)bufn); }
	if (!buf) { pd_real_shadersrc(sh,cnt,str,len); return; }
	memcpy(buf,src,(size_t)at);
	memcpy(&buf[at],pre,(size_t)l0);
	memcpy(&buf[at+l0],&src[at],(size_t)(l1-at)+1);
	{ const GLchar *one = buf; pd_real_shadersrc(sh,1,&one,0); }
}

/* `wglGetProcAddress`：polydraw 靠它填那张 GL 2.0 的函数指针表。
   macOS 没有对应的 API —— 但 legacy GL 的符号全在 OpenGL.framework 里，
   所以 `dlsym(RTLD_DEFAULT, 名字)` 就是答案。
   别的平台上扩展函数不一定是导出符号，所以 dlsym 之后再退到 GLFW 那一格
   （底下是 `glXGetProcAddress`）。 */
static void *pd_sym (const char *nam)
{
	void *p = dlsym(RTLD_DEFAULT,nam);
#if !defined(__APPLE__)
	/* 扩展函数不一定是 libGL 的导出符号。无头那条路问 EGL，窗口那条路问 GLFW
	   （底下是 glXGetProcAddress）。**次序不能反**：GLFW 这条路上没 init 时
	   `glfwGetProcAddress` 是不能问的。 */
	if (!p) p = pd_egl_procaddr(nam);
	if (!p) p = pd_gui_procaddr(nam);
#endif
	return(p);
}

void *wglGetProcAddress (LPCSTR nam)
{
	void *p = pd_sym(nam);
	if ((!strcmp(nam,"glShaderSource") || !strcmp(nam,"glShaderSourceARB")) && p)
		{ pd_real_shadersrc = (void (*)(GLuint,GLsizei,const GLchar **,const GLint *))p;
		  return((void *)pd_shadersrc_wrap); }
	if (!strcmp(nam,"glTexImage3D") && p)
		{ pd_real_teximg3d = (void (*)(GLenum,GLint,GLint,GLsizei,GLsizei,GLsizei,GLint,GLenum,GLenum,const void *))p;
		  return((void *)pd_teximg3d_wrap); }
	if (!strcmp(nam,"glTexSubImage3D") && p)
		{ pd_real_texsub3d = (void (*)(GLenum,GLint,GLint,GLint,GLint,GLsizei,GLsizei,GLsizei,GLenum,GLenum,const void *))p;
		  return((void *)pd_texsub3d_wrap); }
	if (!strcmp(nam,"glBindFramebufferEXT") || !strcmp(nam,"glBindFramebuffer"))
	{
		if (!p) p = pd_sym("glBindFramebufferEXT");
		if (p) { pd_real_bindfb = (void (*)(GLenum,GLuint))p; return((void *)pd_bindfb_wrap); }
	}
	if (!p)
	{
		/* 有几个在 macOS 上只有 `*EXT`/`*ARB` 那一版（FBO 一族就是）。 */
		char buf[256];
		snprintf(buf,sizeof(buf),"%sEXT",nam); p = pd_sym(buf);
		if (!p) { snprintf(buf,sizeof(buf),"%sARB",nam); p = pd_sym(buf); }
		if (!p)
		{
			/* 再试一次去掉 EXT/ARB 后缀的名字（macOS 上有些只有无后缀那一版）。 */
			long l = (long)strlen(nam);
			if ((l > 3) && !strcmp(&nam[l-3],"EXT")) { snprintf(buf,sizeof(buf),"%.*s",(int)(l-3),nam); p = pd_sym(buf); }
			else if ((l > 3) && !strcmp(&nam[l-3],"ARB")) { snprintf(buf,sizeof(buf),"%.*s",(int)(l-3),nam); p = pd_sym(buf); }
		}
	}
	if (!p && getenv("PD_GLDBG")) fprintf(stderr,"[proc] 没找到 %s\n",nam);
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
	/* GUI 档：swap + poll（事件喂回 polydraw 那一边）。GUI 不走 FBO，
	   画面已经在窗口那张默认帧缓冲里了，所以不用 blit。 */
	if (getenv("PD_GUIDBG") && (pd_frame < 3))
		fprintf(stderr,"[gui] SwapBuffers 第 %ld 帧 gui=%d fbo=%u\n",(long)pd_frame-1,pd_gui_on(),pd_fbo);
	if (pd_gui_on())
	{
		GLint vv[4] = {0,0,0,0};
		if (pd_fbo) { glGetIntegerv(GL_VIEWPORT,vv); }
		pd_gui_present(pd_fbo,(const int *)vv);
	}
	return(1);
}

/* polydraw 第一帧会 `wglGetProcAddress("wglSwapIntervalEXT")` 然后直接调 ——
   离屏没有 vsync 可关，但那个指针不能是空的（空了就跳到 0）。 */
void wglSwapIntervalEXT (int n) { (void)n; }
HDC GetDC (HWND h) { (void)h; return((HDC)1); }
int ReleaseDC (HWND h, HDC d) { (void)h;(void)d; return(1); }
