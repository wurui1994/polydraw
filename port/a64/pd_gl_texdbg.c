/* port/a64/pd_gl_texdbg.c —— 纹理上传那一路的诊断（`PD_TEXDBG=1` 打开）。
 *
 * 为什么要它：`texture2D(tex0,…)` 在我们这儿回的是**常量白**，而参考（c_impl）
 * 画得出来。白色是 Apple 驱动对"贴图不完整/装不进去"的回答（cubetex 那份的日志里
 * 它自己说了：`unit 0 … is unloadable … using zero texture`）。要分清是
 * "根本没调 glTexImage2D" / "调了但驱动报错" / "调了也没错但采样仍白"，
 * 只能把这三个入口逐个记下来 —— 参数 + 紧跟着的 `glGetError()`。
 *
 * 用法：缝合文件里在 `#include "pd/pd_wingl.c"` **之前**包这一份，然后
 * `#define glTexImage2D pd_dbg_glTexImage2D` 等等 —— 于是**定义**留在这儿用真名，
 * 后面那些 pd 文件里的**调用点**走我们的。宏按记号流展开，所以这一招成立
 * （与 `kputs` / `kasm87c` 那两处同一个套路）。
 */
#include <stdio.h>
#include <stdlib.h>

static int pd_texdbg = -1;
static int pd_texdbg_on (void)
{
	if (pd_texdbg < 0) pd_texdbg = (getenv("PD_TEXDBG") != 0);
	return(pd_texdbg);
}

static const char *pd_gl_errnam (GLenum e)
{
	switch(e)
	{
		case GL_NO_ERROR:          return("-");
		case GL_INVALID_ENUM:      return("INVALID_ENUM");
		case GL_INVALID_VALUE:     return("INVALID_VALUE");
		case GL_INVALID_OPERATION: return("INVALID_OPERATION");
		case GL_OUT_OF_MEMORY:     return("OUT_OF_MEMORY");
	}
	return("?");
}

/**
 * **第 23 个洞：名字 0 那格贴图在 Apple 上装不进去**（这一格不是诊断，是真补丁）。
 *
 * polydraw 的贴图**不走 `glGenTextures`**：它拿"第几格用户贴图"当 GL 的名字用
 * （`glBindTexture(tex[itex].tar,itex)` —— `pd_host_gl.c` 里六处都是这么写的）。
 * 于是第 0 格贴图绑的是**名字 0**，那是"默认贴图对象"。Windows 那些驱动上往默认
 * 对象上传是许的（原版就这么跑），Apple 的 Metal 后端**不许**：
 *   `unit 0 GLD_TEXTURE_INDEX_3D is unloadable and bound to sampler type (Float)
 *    - using zero texture because texture unloadable`
 * 于是 `ken/texture3d.pss` 采出来全是 0。
 *
 * 这一层把**名字 0 换成一格真的 `glGenTextures` 名字**（按 target 各一格 ——
 * 同一个名字不许换 target）。别的名字（1、2、…）照旧：兼容档里"绑一个没用过的名字"
 * 本来就等于就地建一个对象。
 *
 * 为什么可以在这儿换：这份源码里 `glBindTexture` 的**每一处**调用点都是
 * `fontid`（`glGenTextures` 给的）或者 `tex[itex].tar,itex` —— **没有一处是
 * "绑 0 去解绑"**（grep 过全部 14 处）。所以"名字 0 一定是第 0 格用户贴图"成立。
 */
#define PD_TEXZ 8
static struct { GLenum tar; GLuint nam; } pd_texz[PD_TEXZ];
static int pd_texzn = 0;

static GLuint pd_zero_name (GLenum tar)
{
	int i;
	for(i=0;i<pd_texzn;i++) if (pd_texz[i].tar == tar) return(pd_texz[i].nam);
	if (pd_texzn >= PD_TEXZ) return(0);
	pd_texz[pd_texzn].nam = 0;
	glGenTextures(1,&pd_texz[pd_texzn].nam);
	if (!pd_texz[pd_texzn].nam) return(0);
	pd_texz[pd_texzn].tar = tar;
	if (pd_texdbg_on())
		fprintf(stderr,"[tex] 名字 0（target %x）换成真名 %u\n",tar,pd_texz[pd_texzn].nam);
	return(pd_texz[pd_texzn++].nam);
}

static void pd_dbg_glBindTexture (GLenum tar, GLuint tex)
{
	if (!tex) tex = pd_zero_name(tar);
	glBindTexture(tar,tex);
	if (pd_texdbg_on())
		fprintf(stderr,"[tex] bind tar=%x name=%u err=%s\n",tar,tex,pd_gl_errnam(glGetError()));
}

static void pd_dbg_glTexImage2D (GLenum tar, GLint lev, GLint ifmt, GLsizei w, GLsizei h,
	GLint bord, GLenum fmt, GLenum typ, const void *px)
{
	glTexImage2D(tar,lev,ifmt,w,h,bord,fmt,typ,px);
	if (pd_texdbg_on())
		fprintf(stderr,"[tex] image2D tar=%x lev=%d ifmt=%d %dx%d fmt=%x typ=%x px=%p err=%s\n",
			tar,lev,ifmt,(int)w,(int)h,fmt,typ,px,pd_gl_errnam(glGetError()));
}

static void pd_dbg_glTexSubImage2D (GLenum tar, GLint lev, GLint xo, GLint yo, GLsizei w, GLsizei h,
	GLenum fmt, GLenum typ, const void *px)
{
	glTexSubImage2D(tar,lev,xo,yo,w,h,fmt,typ,px);
	if (pd_texdbg_on())
		fprintf(stderr,"[tex] sub2D tar=%x lev=%d at=%d,%d %dx%d fmt=%x typ=%x px=%p err=%s\n",
			tar,lev,xo,yo,(int)w,(int)h,fmt,typ,px,pd_gl_errnam(glGetError()));
}

static void pd_dbg_glTexParameteri (GLenum tar, GLenum pn, GLint v)
{
	glTexParameteri(tar,pn,v);
	if (pd_texdbg_on())
		fprintf(stderr,"[tex] parami tar=%x pname=%x v=%x err=%s\n",tar,pn,v,pd_gl_errnam(glGetError()));
}

/* ── 试过并**退掉**的一刀：包 `glViewport` 让它撑满窗口 ──
 *
 * GUI 下画面只落在窗口一角（渲染窗格是 `xres>>1` x `oglxres*3/4`），一眼看着像是
 * 该把视口改大。**不行**：`glcapture()` 那一族（`pd_host_gl.c:245`）自己会设一个
 * 小视口去做渲染到纹理，一律覆盖就把那条路整条打断 —— `tigrou/clock.pss`
 * 整幅图全黑（量到过：整张帧缓冲非黑像素 0）。
 *
 * 正确的做法是按原文自己留的开关 `popts.fullscreen`（`pd_win.c:238`），
 * 见 `port/a64/pd_gui_bridge.c` 的 `pd_in_fullscreen`。
 */
