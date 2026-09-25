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

static void pd_dbg_glBindTexture (GLenum tar, GLuint tex)
{
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
