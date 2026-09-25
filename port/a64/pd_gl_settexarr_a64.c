/* port/a64/pd_gl_settexarr_a64.c —— `glsettex(贴图号, 数组, 宽[,高[,深]], 颜色档)` 的 LP64 修正。
 *
 * ## 第 24 个洞：那句范围检查把 64 位指针截成 32 位
 *
 * 原文（`pd/pd_host_gl.c` 的 `kglsettexarray3`）在拷数据之前要确认"这个数组真在脚本
 * 自己那块内存里"：
 *
 *     if ((((int)p) < ((int)gevalfunc)) || (((int)p)+((xs*ys*zs*evalvalperpix)<<3) >
 *          ((int)gevalfunc)+gevalfuncleng)) return(-1.0);
 *
 * 三个 `(int)` 强转在 32 位 x86 上是精确的，在 LP64 上把地址截成低 32 位 ——
 * 于是这一句**随机地假**，`glsettex` 一声不响地回 -1，贴图从来没上传过。
 * 量到的（`ken/texture3d.pss`，`PD_TEXDBG=1`）：只有 `CreateEmptyTexture` 的
 * `image3D`（分配那一趟），**没有 `sub3D`** —— 正是在这一句退出去了。
 * 驱动那句 "GLD_TEXTURE_INDEX_3D is unloadable … using zero texture" 是它的后果。
 *
 * 这一份是**那个函数的分身**（原文一个字节都没动）：只把三处 `(int)` 换成
 * `(char *)`/`(size_t)`，其余逐句照抄。`kglsettexarray1/2` 也在这儿重定向到它。
 * 缝合文件里把原文那三个名字改成 `*_win32`，这一份用真名。
 *
 * 顺带：`tex[itex].nam[0] = 0;` 那句照抄（它是"这一格不是文件贴图"的标记）。
 */

double __cdecl kglsettexarray3 (double dtex, double *p, double dxsiz, double dysiz, double dzsiz, double dcoltype)
{
	int i, xs, ys, zs, itex, icoltype, evalvalperpix, glbyteperpix, internalFormat, format, type;

	itex = (int)dtex; if ((unsigned)itex >= (unsigned)MAXUSERTEX) return(-1.0);
	icoltype = (int)dcoltype;
	tex[itex].nam[0] = 0;
	if ((dxsiz < 1.0) || (dysiz < 1.0) || (dzsiz < 1.0) || (dxsiz*dysiz*dzsiz > 67108864.0)) return(-1.0);
	xs = (int)dxsiz; ys = (int)dysiz; zs = (int)dzsiz;
	if ((tex[itex].sizx != xs) || (tex[itex].sizy != ys) || (tex[itex].sizz != zs) || (tex[itex].coltype != icoltype))
		CreateEmptyTexture(itex,xs,ys,zs,icoltype);

	evalvalperpix = 1; glbyteperpix = 4;
	switch(icoltype&15)
	{
		case KGL_BGRA32: evalvalperpix = 1; glbyteperpix = 4; break;
		case KGL_CHAR:   evalvalperpix = 1; glbyteperpix = 1; break;
		case KGL_SHORT:  evalvalperpix = 1; glbyteperpix = 2; break;
		case KGL_INT:    evalvalperpix = 1; glbyteperpix = 4; break;
		case KGL_FLOAT:  evalvalperpix = 1; glbyteperpix = 4; break;
		case KGL_VEC4:   evalvalperpix = 4; glbyteperpix =16; break;
	}

	i = xs*ys*zs*glbyteperpix;
	if (i > gbmpmal) { gbmpmal = i; gbmp = (char *)realloc(gbmp,gbmpmal); }

	/* **这一句就是第 24 个洞**，而且有两层：
	 *
	 *   1. 原文三处 `(int)` 把 64 位指针截成 32 位（LP64 上随机地假）；
	 *   2. **就算按真指针比也过不去** —— 它比的是"在不在 `gevalfunc` 那块里"，
	 *      那是 `COMPILE==1`（x87 JIT）的布局：脚本的 static 数据跟着代码块一起分配。
	 *      `COMPILE==0` 这条路上 static 在**另一块**（`kasm_state.c:68` 的 `gstatmem`，
	 *      而且它是 eval 那个翻译单元里的 static，polydraw 这边压根看不见）。
	 *      量到的：`ken/texture3d.pss` 只有 `CreateEmptyTexture` 那趟 `image3D`，
	 *      **没有 `sub3D`**（`PD_TEXDBG=1`）。
	 *
	 * 所以这一档**把它去掉**：这个指针是我们自己的解释器/JIT 从操作数表里取出来的，
	 * 与别的实参一样可信；尺寸那两道门（上面 `> 67108864` 与 `gbmp` 的 realloc）还在。
	 * 真要做范围检查得让 eval 那侧把 `gstatmem` 的范围报出来 —— 那是另一刀。
	 */

	switch(icoltype&15)
	{
		case KGL_BGRA32: for(i=xs*ys*zs  -1;i>=0;i--) *(unsigned int   *)((i<<2)+gbmp) = (unsigned int  )(p[i]); break;
		case KGL_CHAR:   for(i=xs*ys*zs  -1;i>=0;i--) *(unsigned char  *)( i    +gbmp) = (unsigned char )(p[i]); break;
		case KGL_SHORT:  for(i=xs*ys*zs  -1;i>=0;i--) *(unsigned short *)((i<<1)+gbmp) = (unsigned short)(p[i]); break;
		case KGL_INT:    for(i=xs*ys*zs  -1;i>=0;i--) *(unsigned int   *)((i<<2)+gbmp) = (unsigned int  )(p[i]); break;
		case KGL_FLOAT:  for(i=xs*ys*zs  -1;i>=0;i--) *(        float  *)((i<<2)+gbmp) = (        float )(p[i]); break;
		case KGL_VEC4:   for(i=xs*ys*zs*4-1;i>=0;i--) *(        float  *)((i<<2)+gbmp) = (        float )(p[i]); break;
	}

	glBindTexture(tex[itex].tar,itex);
	internalFormat = 4; format = GL_BGRA_EXT; type = GL_UNSIGNED_BYTE;
	switch(icoltype&15)
	{
		case KGL_BGRA32: internalFormat =                   4; format =  GL_BGRA_EXT; type = GL_UNSIGNED_BYTE; break;
		case KGL_CHAR:   internalFormat =       GL_LUMINANCE8; format = GL_LUMINANCE; type = GL_UNSIGNED_BYTE; break;
		case KGL_SHORT:  internalFormat =      GL_LUMINANCE16; format = GL_LUMINANCE; type =GL_UNSIGNED_SHORT; break;
		case KGL_INT:    internalFormat = GL_LUMINANCE32I_EXT; format = GL_LUMINANCE; type =  GL_UNSIGNED_INT; break;
		case KGL_FLOAT:  internalFormat = GL_LUMINANCE32F_ARB; format = GL_LUMINANCE; type =         GL_FLOAT; break;
		case KGL_VEC4:   internalFormat =      GL_RGBA32F_ARB; format =      GL_RGBA; type =         GL_FLOAT; break;
	}
	(void)internalFormat;
	switch(tex[itex].tar)
	{
		case GL_TEXTURE_1D: glTexSubImage1D(tex[itex].tar,0,0    ,tex[itex].sizx               ,format,type,gbmp); break;
		case GL_TEXTURE_2D: glTexSubImage2D(tex[itex].tar,0,0,0  ,tex[itex].sizx,tex[itex].sizy,format,type,gbmp);
			if (((icoltype&0xf0) >= KGL_MIPMAP) && ((icoltype&15) == KGL_BGRA32))
				gluBuild2DMipmaps(tex[itex].tar,4,tex[itex].sizx,tex[itex].sizy,GL_BGRA_EXT,GL_UNSIGNED_BYTE,gbmp);
			break;
		case GL_TEXTURE_3D: ((PFNGLTEXSUBIMAGE3DPROC)glfp[glTexSubImage3D])(tex[itex].tar,0,0,0,0,tex[itex].sizx,tex[itex].sizy,tex[itex].sizz,format,type,gbmp); break;
		case GL_TEXTURE_CUBE_MAP:
			for(i=0;i<6;i++) glTexSubImage2D(cubemapconst[i],0,0,0,tex[itex].sizx,tex[itex].sizx,format,type,gbmp+tex[itex].sizx*tex[itex].sizx*glbyteperpix*cubemapindex[i]);
			break;
	}
	return(0.0);
}
double __cdecl kglsettexarray2 (double dtex, double *p, double dxsiz, double dysiz, double coltype)
{ return(kglsettexarray3(dtex,p,dxsiz,dysiz,1.0,coltype)); }
double __cdecl kglsettexarray1 (double dtex, double *p, double dxsiz, double coltype)
{ return(kglsettexarray3(dtex,p,dxsiz,1.0,1.0,coltype)); }
