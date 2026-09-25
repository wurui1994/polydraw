
//--------------------------------------------------------------------------------------------------

double __cdecl qglAlphaEnable (double d)
{
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
	return 0;
}
double __cdecl qglAlphaDisable (double d)
{
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	return 0;
}
double __cdecl qglQuad (double alpha)
{
	glPushAttrib(GL_DEPTH_BUFFER_BIT|GL_COLOR_BUFFER_BIT);
	glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0,oglxres,oglyres,0,-1,1);
	glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();

	glDisable(GL_DEPTH_TEST);
	if (alpha == 0.0) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA); }
	if (alpha == 1.0) { glDisable(GL_BLEND); }

	glBegin(GL_QUADS);
	glTexCoord2f(0,1); glVertex2f(      0,      0);
	glTexCoord2f(1,1); glVertex2f(oglxres,      0);
	glTexCoord2f(1,0); glVertex2f(oglxres,oglyres);
	glTexCoord2f(0,0); glVertex2f(      0,oglyres);
	glEnd();

	glMatrixMode(GL_PROJECTION); glPopMatrix();
	glMatrixMode(GL_MODELVIEW); glPopMatrix();
	glPopAttrib();
	return 0;
}

static double __cdecl setshader_int (int sh0, int sh1, int sh2)
{
	char tbuf[4096];
	int i, j;

	if ((usearbasm) || (usearbasmonly))
	{
		if ((unsigned)sh0 >= (unsigned)shadn[0]) sh0 = 0;
		if ((unsigned)sh2 >= (unsigned)shadn[2]) sh2 = 0;
		((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(GL_VERTEX_PROGRAM_ARB  ,shad[0][sh0]);
		((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(GL_FRAGMENT_PROGRAM_ARB,shad[2][sh2]);
		return(0.0);
	}

	for(i=0;i<shadprogn;i++)
		if ((shadprogi[i].v == sh0) && (shadprogi[i].g == sh1) && (shadprogi[i].f == sh2))
		{
			if (!shadprogi[i].ishw) { gcurshader = 0; return(-1.0); }
			((PFNGLUSEPROGRAMPROC)glfp[glUseProgram])(shadprog[i]); gcurshader = i; return(0.0);
		}
	if (shadprogn >= PROGMAX) return(0.0); //silent error :/

	if ((unsigned)sh0 >= (unsigned)shadn[0]) sh0 = 0;
	if ((unsigned)sh1 >= (unsigned)shadn[1]) sh1 =-1;
	if ((unsigned)sh2 >= (unsigned)shadn[2]) sh2 = 0;

	gcurshader = shadprogn;
	shadprogi[shadprogn].v = sh0;
	shadprogi[shadprogn].g = sh1;
	shadprogi[shadprogn].f = sh2;
	shadprogi[shadprogn].ishw = 1;

	shadprog[shadprogn] = ((PFNGLCREATEPROGRAMPROC)glfp[glCreateProgram])();
					  ((PFNGLATTACHSHADERPROC)glfp[glAttachShader])(shadprog[shadprogn],shad[0][sh0]);
	if (sh1 >= 0) ((PFNGLATTACHSHADERPROC)glfp[glAttachShader])(shadprog[shadprogn],shad[1][sh1]);
					  ((PFNGLATTACHSHADERPROC)glfp[glAttachShader])(shadprog[shadprogn],shad[2][sh2]);

	if ((sh1 >= 0) && (glfp[glProgramParameteri]))
	{
			//Example: @g,GL_TRIANGLES,GL_TRIANGLE_STRIP,1024:myname
		//glGetIntegerv(GL_MAX_GEOMETRY_UNIFORM_COMPONENTS,&n); //2048
		//glGetIntegerv(GL_MAX_GEOMETRY_OUTPUT_VERTICES,&n); //1024
		//glGetIntegerv(GL_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS,&n); //1024
		i = geo2blocki[sh1];
		((PFNGLPROGRAMPARAMETERIEXTPROC)glfp[glProgramParameteri])(shadprog[shadprogn],GL_GEOMETRY_INPUT_TYPE_EXT,tsec[i].geo_in); //GL_POINTS/GL_LINES/GL_LINES_ADJACENCY/GL_TRIANGLES/GL_TRIANGLES_ADJACENCY
		((PFNGLPROGRAMPARAMETERIEXTPROC)glfp[glProgramParameteri])(shadprog[shadprogn],GL_GEOMETRY_OUTPUT_TYPE_EXT,tsec[i].geo_out); //GL_POINTS/GL_LINE_STRIP/GL_TRIANGLE_STRIP
		((PFNGLPROGRAMPARAMETERIEXTPROC)glfp[glProgramParameteri])(shadprog[shadprogn],GL_GEOMETRY_VERTICES_OUT_EXT,tsec[i].geo_nverts); //min max=1024 ?
	}

	((PFNGLLINKPROGRAMPROC)glfp[glLinkProgram])(shadprog[shadprogn]);
	((PFNGLGETPROGRAMIVPROC)glfp[glGetProgramiv])(shadprog[shadprogn],GL_LINK_STATUS,&i);
		//NOTE:must get infolog anyway because driver doesn't consider running in SW an error.
	((PFNGLGETINFOLOGARBPROC)glfp[glGetInfoLogARB])(shadprog[shadprogn],sizeof(tbuf),0,tbuf);
	j = (strstr(tbuf,"software") != 0);
	if ((!i) || (j)) //the string of evil..
	{
		if (!i) kputs(tbuf,1);
		if (j) kputs("Shader won't run in HW! Execution denied. :/",1);
		shadprogi[shadprogn].ishw = 0;
		shadprogn++;
		return(-1.0);
	}

	((PFNGLUSEPROGRAMPROC)glfp[glUseProgram])(shadprog[shadprogn]);

		//Note: Get*Uniform*() must be called after glUseProgram() to work properly
	((PFNGLUNIFORM1IPROC)glfp[glUniform1i])(((PFNGLGETUNIFORMLOCATIONPROC)glfp[glGetUniformLocation])(shadprog[shadprogn],"tex0"),0);
	((PFNGLUNIFORM1IPROC)glfp[glUniform1i])(((PFNGLGETUNIFORMLOCATIONPROC)glfp[glGetUniformLocation])(shadprog[shadprogn],"tex1"),1);
	((PFNGLUNIFORM1IPROC)glfp[glUniform1i])(((PFNGLGETUNIFORMLOCATIONPROC)glfp[glGetUniformLocation])(shadprog[shadprogn],"tex2"),2);
	((PFNGLUNIFORM1IPROC)glfp[glUniform1i])(((PFNGLGETUNIFORMLOCATIONPROC)glfp[glGetUniformLocation])(shadprog[shadprogn],"tex3"),3);

	shadprogn++;
	return(0.0);
}
double __cdecl qglsetshader  (double d) { return(setshader_int(0,-1,(int)d)); }
double __cdecl kglsetshader3 (char *st0, char *st1, char *st2)
{
	int i, j, shi[3] = {-1,-1,-1};
	for(i=0;i<tsecn;i++)
	{
		j = ((int)tsec[i].typ)-1; if (j < 0) continue;
		if (((j == 0) && (!stricmp(tsec[i].nam,st0))) ||
			 ((j == 1) && (!stricmp(tsec[i].nam,st1))) ||
			 ((j == 2) && (!stricmp(tsec[i].nam,st2)))) shi[j] = tsec[i].cnt;
	}
	return(setshader_int(shi[0],shi[1],shi[2]));
}
double __cdecl kglsetshader2 (char *st0, char *st1) { return(kglsetshader3(st0,"",st1)); }

static const int cubemapconst[6] =
{
	GL_TEXTURE_CUBE_MAP_POSITIVE_X, GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
	GL_TEXTURE_CUBE_MAP_POSITIVE_Y, GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
	GL_TEXTURE_CUBE_MAP_POSITIVE_Z, GL_TEXTURE_CUBE_MAP_NEGATIVE_Z,
};
static const int cubemapindex[6] = {1,3,4,5,0,2};
static void CreateEmptyTexture (int itex, int xs, int ys, int zs, int icoltype)
{
	int i, internalFormat, format, type;

	tex[itex].tar = GL_TEXTURE_3D;
	if (zs == 1)
	{
		tex[itex].tar = GL_TEXTURE_2D;
		if (xs*6 == ys)
		{
			tex[itex].tar = GL_TEXTURE_CUBE_MAP;
			icoltype = (icoltype&~0xf00)|KGL_CLAMP_TO_EDGE;
			if ((icoltype&0xf0) >= KGL_MIPMAP) icoltype = (icoltype&~0xf0)|KGL_LINEAR;
		}
		else if (ys == 1) { tex[itex].tar = GL_TEXTURE_1D; }
	}
	tex[itex].sizx = xs;
	tex[itex].sizy = ys;
	tex[itex].sizz = zs;
	tex[itex].coltype = icoltype;

	glBindTexture(tex[itex].tar,itex);
	switch (icoltype&0xf0)
	{
		case KGL_LINEAR: default:
			glTexParameteri(tex[itex].tar,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
			glTexParameteri(tex[itex].tar,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
			break;
		case KGL_NEAREST:
			glTexParameteri(tex[itex].tar,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
			glTexParameteri(tex[itex].tar,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
			break;
		case KGL_MIPMAP0: case KGL_MIPMAP1: case KGL_MIPMAP2: case KGL_MIPMAP3:
			switch(icoltype&0xf0)
			{
				case KGL_MIPMAP0: glTexParameteri(tex[itex].tar,GL_TEXTURE_MIN_FILTER,GL_NEAREST_MIPMAP_NEAREST); break;
				case KGL_MIPMAP1: glTexParameteri(tex[itex].tar,GL_TEXTURE_MIN_FILTER,GL_NEAREST_MIPMAP_LINEAR); break;
				case KGL_MIPMAP2: glTexParameteri(tex[itex].tar,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_NEAREST); break;
				case KGL_MIPMAP3: glTexParameteri(tex[itex].tar,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR); break;
			}

			//#define GL_TEXTURE_MIN_LOD      0x813A
			//#define GL_TEXTURE_MAX_LOD      0x813B
			//#define GL_TEXTURE_BASE_LEVEL   0x813C
			//#define GL_TEXTURE_MAX_LEVEL    0x813D
			//#define GL_MAX_TEXTURE_LOD_BIAS 0x84FD
			//#define GL_TEXTURE_LOD_BIAS     0x8501
			//#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
			//glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,0);
			//glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,1);
			//glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_LOD,0);
			//glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LOD,1);
			//glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_LOD_BIAS,-4);
			//glTexEnvi(GL_TEXTURE_ENV,GL_MAX_TEXTURE_LOD_BIAS,-4);
			//glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_MAX_ANISOTROPY_EXT,1.0);

			glTexParameteri(tex[itex].tar,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
			break;
	}
	switch(icoltype&0xf00)
	{
		case KGL_REPEAT: default:
			glTexParameteri(tex[itex].tar,GL_TEXTURE_WRAP_S,GL_REPEAT);
			glTexParameteri(tex[itex].tar,GL_TEXTURE_WRAP_T,GL_REPEAT);
			glTexParameteri(tex[itex].tar,GL_TEXTURE_WRAP_R,GL_REPEAT);
			break;
		case KGL_MIRRORED_REPEAT:
			glTexParameteri(tex[itex].tar,GL_TEXTURE_WRAP_S,GL_MIRRORED_REPEAT);
			glTexParameteri(tex[itex].tar,GL_TEXTURE_WRAP_T,GL_MIRRORED_REPEAT);
			glTexParameteri(tex[itex].tar,GL_TEXTURE_WRAP_R,GL_MIRRORED_REPEAT);
			break;
		case KGL_CLAMP:
			glTexParameteri(tex[itex].tar,GL_TEXTURE_WRAP_S,GL_CLAMP);
			glTexParameteri(tex[itex].tar,GL_TEXTURE_WRAP_T,GL_CLAMP);
			glTexParameteri(tex[itex].tar,GL_TEXTURE_WRAP_R,GL_CLAMP);
			break;
		case KGL_CLAMP_TO_EDGE:
			glTexParameteri(tex[itex].tar,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
			glTexParameteri(tex[itex].tar,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
			glTexParameteri(tex[itex].tar,GL_TEXTURE_WRAP_R,GL_CLAMP_TO_EDGE);
			break;
	}
	switch(icoltype&15)
	{
		case KGL_BGRA32: internalFormat =                   4; format =  GL_BGRA_EXT; type = GL_UNSIGNED_BYTE; break;
		case KGL_CHAR:   internalFormat =       GL_LUMINANCE8; format = GL_LUMINANCE; type = GL_UNSIGNED_BYTE; break;
		case KGL_SHORT:  internalFormat =      GL_LUMINANCE16; format = GL_LUMINANCE; type =GL_UNSIGNED_SHORT; break;
		case KGL_INT:    internalFormat = GL_LUMINANCE32I_EXT; format = GL_LUMINANCE; type =  GL_UNSIGNED_INT; break;
		case KGL_FLOAT:  internalFormat = GL_LUMINANCE32F_ARB; format = GL_LUMINANCE; type =         GL_FLOAT; break;
		case KGL_VEC4:   internalFormat =      GL_RGBA32F_ARB; format =      GL_RGBA; type =         GL_FLOAT; break;
	}
	switch(tex[itex].tar)
	{
		case GL_TEXTURE_1D: glTexImage1D(tex[itex].tar,0,internalFormat,tex[itex].sizx,               0,format,type,0); break;
		case GL_TEXTURE_2D: glTexImage2D(tex[itex].tar,0,internalFormat,tex[itex].sizx,tex[itex].sizy,0,format,type,0); break;
		case GL_TEXTURE_3D: ((PFNGLTEXIMAGE3DPROC)glfp[glTexImage3D])(tex[itex].tar,0,internalFormat,tex[itex].sizx,tex[itex].sizy,tex[itex].sizz,0,format,type,0); break;
		case GL_TEXTURE_CUBE_MAP:
			for(i=0;i<6;i++) { glTexImage2D(cubemapconst[i],0,internalFormat,tex[itex].sizx,tex[itex].sizx,0,format,type,0); } break;
	}
}
static int glastcap = 0;
double __cdecl qglCapture (double dcaptexsiz)
{
	int i, j;
	i = (int)dcaptexsiz; if ((i > 0) && (i <= 8192)) captexsiz = i;
	i = min(oglxres,oglyres);
	if (i < captexsiz) //FIXME:ugly hack; use FBO to support full requested size?
		{ for(captexsiz=1;(captexsiz<<1)<=i;captexsiz<<=1); }

	glViewport(0,0,captexsiz,captexsiz);

	glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); gluPerspective(45,1,0.1,1000.0);
	glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();

	glScalef(oglyres/(float)oglxres,1,1);
	glastcap = 0;
	return 0;
}
GLuint gmyfb = 0; //Nice article about GPGPU in shaders: http://www.mathematik.tu-dortmund.de/~goeddeke/gpgpu/tutorial.html
double __cdecl kglCapture (double dtex, double xsiz, double ysiz, double dcoltype)
{
	int i, itex, xs, ys, internalFormat, format, type, icoltype;

	itex = (int)dtex; icoltype = (int)dcoltype; if ((icoltype&15) >= KGL_NUM) icoltype &= ~15;
	if ((xsiz < 1.0) || (ysiz < 1.0) || (xsiz*ysiz > 67108864.0)) return(-1.0);
	xs = (int)xsiz; ys = (int)ysiz;
	if ((!glfp[glGenFramebuffersEXT]) || (!glfp[glBindFramebufferEXT]) || (!glfp[glFramebufferTexture2DEXT]))
	{
		kputs("Sorry, this HW doesn't support glcapture(,,,,) :/",1);
		gshadercrashed = 1; return(-1.0);
	}

	if (!gmyfb) ((PFNGLGENFRAMEBUFFERSEXTPROC)glfp[glGenFramebuffersEXT])(1,&gmyfb); //create FBO/offscreen framebuf
	((PFNGLBINDFRAMEBUFFEREXTPROC)glfp[glBindFramebufferEXT])(GL_FRAMEBUFFER_EXT,gmyfb);

	if ((tex[itex].sizx != captexsiz) || (tex[itex].sizy != captexsiz) || (tex[itex].sizz != 1) || (tex[itex].coltype != icoltype))
		CreateEmptyTexture(itex,xs,ys,1,icoltype);

	//tex[itex].tar = GL_TEXTURE_RECTANGLE_ARB;
	//glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_REPLACE); //necessary?

	//glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
	((PFNGLFRAMEBUFFERTEXTURE2DEXTPROC)glfp[glFramebufferTexture2DEXT])(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT/*0..3*/,tex[itex].tar,itex,0);

	glViewport(0,0,xs,ys);

	glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); gluPerspective(45,1,0.1,1000.0);
	glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();

	glScalef(ys/(float)xs,1,1);

	glastcap = 1;
	return(0);
}
double __cdecl qglEndCapture (double dtex)
{
	int itex;
	itex = (int)dtex; if ((unsigned)itex >= (unsigned)MAXUSERTEX) return(-1.0);
	tex[itex].nam[0] = 0;

	if (glastcap)
	{
		glViewport(0,0,oglxres,oglyres);
		glMatrixMode(GL_PROJECTION); glPopMatrix();
		glMatrixMode(GL_MODELVIEW); glPopMatrix();
		((PFNGLBINDFRAMEBUFFEREXTPROC)glfp[glBindFramebufferEXT])(GL_FRAMEBUFFER_EXT,0); //restore
		return(0.0);
	}

	if ((tex[itex].sizx != captexsiz) || (tex[itex].sizy != captexsiz) || (tex[itex].sizz != 1) || (tex[itex].coltype != KGL_BGRA32))
		CreateEmptyTexture(itex,captexsiz,captexsiz,1,KGL_BGRA32);

	glBindTexture(tex[itex].tar,(int)itex);
	glCopyTexImage2D(tex[itex].tar,0,GL_RGBA,0,0,tex[itex].sizx,tex[itex].sizy,0);

	tex[itex].nam[0] = 0;

	glViewport(0,0,oglxres,oglyres);
	glMatrixMode(GL_PROJECTION); glPopMatrix();
	glMatrixMode(GL_MODELVIEW); glPopMatrix();

	glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
	return 0;
}

double __cdecl kglsettex2 (double dtex, char *st, double dcolmode)
{
	int i, x, y, gotpic, itex, icolmode, leng, xsiz, ysiz;
	char *buf;

	itex = (int)dtex; if ((unsigned)itex >= (unsigned)MAXUSERTEX) return(-1.0);
	icolmode = ((int)dcolmode)&~15;
	if (strlen(st) > MAX_PATH-1) return(-2.0);
	if ((!stricmp(tex[itex].nam,st)) && (tex[itex].coltype == icolmode)) return(0.0);
	strcpy(tex[itex].nam,st);

	gotpic = 0; xsiz = 32; ysiz = 32;
	do
	{
		if (!kzopen(st)) break;
		leng = kzfilelength();
		buf = (char *)malloc(leng); if (!buf) { kzclose(); break; }
		if (kzread(buf,leng) < leng) { free(buf); kzclose(); break; }
		kzclose();
		gotpic = kpgetdim(buf,leng,&xsiz,&ysiz);
	} while (0);

	if ((tex[itex].sizx != xsiz) || (tex[itex].sizy != ysiz) || (tex[itex].sizz != 1) || (tex[itex].coltype != icolmode))
		CreateEmptyTexture(itex,xsiz,ysiz,1,icolmode);

	i = xsiz*ysiz*4;
	if (i > gbmpmal) { gbmpmal = i; gbmp = (char *)realloc(gbmp,gbmpmal); }

	if (!gotpic)
	{
		static const int imagenotfoundbmp[32] = //generate placeholder image
		{
			0x7ce39138,0x05145b10,0x04145510,0x3dd45110,0x0517d110,0x05145110,0x7de45138,0x00000000, //"IMAGE"
			0x01f39100,0x00445300,0x00445500,0x00445900,0x00445100,0x00445100,0x00439100,0x00000000, //" NOT "
			0x3d144e7c,0x45345104,0x45545104,0x4594513c,0x45145104,0x45145104,0x3d138e04,0x00000000, //"FOUND"
			0x00400000,0x00200000,0x00200600,0x0027c600,0x00200000,0x00200600,0x00400600,0x00000000, //" :-( "
		};

		buf = gbmp;
		for(y=0;y<ysiz;y++)
			for(x=0;x<xsiz;x++,buf+=4)
			{
				if (imagenotfoundbmp[y]&(1<<x)) { *(int *)buf = 0xf0102030; continue; }
				*(int *)gbmp = (((rand()<<15)+rand())&0x1f1f1f)+0xff506070;
			}
	}
	else
	{
		kprender(buf,leng,(INT_PTR)gbmp,tex[itex].sizx*4,min(xsiz,tex[itex].sizx),min(ysiz,tex[itex].sizy),0,0);
	}

	glBindTexture(tex[itex].tar,itex);
	switch(tex[itex].tar)
	{
		case GL_TEXTURE_2D:
			glTexSubImage2D(tex[itex].tar,0,0,0,tex[itex].sizx,tex[itex].sizy,GL_BGRA_EXT,GL_UNSIGNED_BYTE,gbmp);
			if ((icolmode&0xf0) >= KGL_MIPMAP) gluBuild2DMipmaps(tex[itex].tar,4,tex[itex].sizx,tex[itex].sizy,GL_BGRA_EXT,GL_UNSIGNED_BYTE,gbmp);
			break;
		case GL_TEXTURE_CUBE_MAP:
			for(i=0;i<6;i++) glTexSubImage2D(cubemapconst[i],0,0,0,tex[itex].sizx,tex[itex].sizx,GL_BGRA_EXT,GL_UNSIGNED_BYTE,gbmp+tex[itex].sizx*tex[itex].sizx*4*cubemapindex[i]);
			break;
	}

	if (gotpic) free(buf);

	return(0.0);
}
double __cdecl kglsettex (double dtex, char *st) { return(kglsettex2(dtex,st,(double)(KGL_MIPMAP+KGL_REPEAT))); }

double __cdecl kglgettexarray2 (double dtex, double *p, double dxsiz, double dysiz, double coltype)
{
	int i, xs, ys, itex, evalvalperpix, glbyteperpix, internalFormat, format, type;

	itex = (int)dtex; if ((unsigned)itex >= (unsigned)MAXUSERTEX) return(-1.0);
	if ((dxsiz < 1.0) || (dysiz < 1.0) || (dxsiz*dysiz > 67108864.0)) return(-1.0);
	xs = (int)dxsiz; ys = (int)dysiz;
	if (xs*ys > tex[itex].sizx*tex[itex].sizy) return(-1.0);

	switch(tex[itex].coltype&15)
	{
		case KGL_BGRA32: evalvalperpix = 1; glbyteperpix = 4; break;
		case KGL_CHAR:   evalvalperpix = 1; glbyteperpix = 1; break;
		case KGL_SHORT:  evalvalperpix = 1; glbyteperpix = 2; break;
		case KGL_INT:    evalvalperpix = 1; glbyteperpix = 4; break;
		case KGL_FLOAT:  evalvalperpix = 1; glbyteperpix = 4; break;
		case KGL_VEC4:   evalvalperpix = 4; glbyteperpix =16; break;
	}

	if ((((int)p) < ((int)gevalfunc)) || (((int)p)+((xs*ys*evalvalperpix)<<3) > ((int)gevalfunc)+gevalfuncleng)) return(-1.0);

	i = xs*ys*glbyteperpix;
	if (i > gbmpmal) { gbmpmal = i; gbmp = (char *)realloc(gbmp,gbmpmal); }

	glBindTexture(tex[itex].tar,itex);
	switch(tex[itex].coltype&15)
	{
		case KGL_BGRA32: internalFormat =                   4; format =  GL_BGRA_EXT; type = GL_UNSIGNED_BYTE; break;
		case KGL_CHAR:   internalFormat =       GL_LUMINANCE8; format = GL_LUMINANCE; type = GL_UNSIGNED_BYTE; break;
		case KGL_SHORT:  internalFormat =      GL_LUMINANCE16; format = GL_LUMINANCE; type =GL_UNSIGNED_SHORT; break;
		case KGL_INT:    internalFormat = GL_LUMINANCE32I_EXT; format = GL_LUMINANCE; type =  GL_UNSIGNED_INT; break;
		case KGL_FLOAT:  internalFormat = GL_LUMINANCE32F_ARB; format = GL_LUMINANCE; type =         GL_FLOAT; break;
		case KGL_VEC4:   internalFormat =      GL_RGBA32F_ARB; format =      GL_RGBA; type =         GL_FLOAT; break;
	}
	glGetTexImage(tex[itex].tar,0,format,type,gbmp);

		//preferred method .. doesn't work :/
	//glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
	//glReadPixels(0,0,tex[itex].sizx,tex[itex].sizy,GL_BGRA_EXT,GL_UNSIGNED_BYTE,gbmp);

	switch(tex[itex].coltype&15)
	{
		case KGL_BGRA32: for(i=xs*ys  -1;i>=0;i--) p[i] = (double)*(unsigned int *)((i<<2) + gbmp); break;
		case KGL_CHAR:   for(i=xs*ys  -1;i>=0;i--) p[i] = (double)*(unsigned char *)(i     + gbmp); break;
		case KGL_SHORT:  for(i=xs*ys  -1;i>=0;i--) p[i] = (double)*(unsigned short *)((i<<1)+gbmp); break;
		case KGL_INT:    for(i=xs*ys  -1;i>=0;i--) p[i] = (double)*(unsigned int *)((i<<2) + gbmp); break;
		case KGL_FLOAT:  for(i=xs*ys  -1;i>=0;i--) p[i] = (double)*(       float *)((i<<2) + gbmp); break;
		case KGL_VEC4:   for(i=xs*ys*4-1;i>=0;i--) p[i] = (double)*(       float *)((i<<2) + gbmp); break;
	}
	return(0.0);
}

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

	if ((((int)p) < ((int)gevalfunc)) || (((int)p)+((xs*ys*zs*evalvalperpix)<<3) > ((int)gevalfunc)+gevalfuncleng)) return(-1.0);

	switch(icoltype&15)
	{
		case KGL_BGRA32: for(i=xs*ys*zs  -1;i>=0;i--) *(unsigned int *)((i<<2) + gbmp) = (unsigned int)(p[i]); break;
		case KGL_CHAR:   for(i=xs*ys*zs  -1;i>=0;i--) *(unsigned char *)( i    + gbmp) = (unsigned char)(p[i]); break;
		case KGL_SHORT:  for(i=xs*ys*zs  -1;i>=0;i--) *(unsigned short *)((i<<1)+gbmp) = (unsigned short)(p[i]); break;
		case KGL_INT:    for(i=xs*ys*zs  -1;i>=0;i--) *(unsigned int *)((i<<2) + gbmp) = (unsigned int)(p[i]); break;
		case KGL_FLOAT:  for(i=xs*ys*zs  -1;i>=0;i--) *(       float *)((i<<2) + gbmp) = (       float)(p[i]); break;
		case KGL_VEC4:   for(i=xs*ys*zs*4-1;i>=0;i--) *(       float *)((i<<2) + gbmp) = (       float)(p[i]); break;
	}

	glBindTexture(tex[itex].tar,itex);
	switch(icoltype&15)
	{
		case KGL_BGRA32: internalFormat =                   4; format =  GL_BGRA_EXT; type = GL_UNSIGNED_BYTE; break;
		case KGL_CHAR:   internalFormat =       GL_LUMINANCE8; format = GL_LUMINANCE; type = GL_UNSIGNED_BYTE; break;
		case KGL_SHORT:  internalFormat =      GL_LUMINANCE16; format = GL_LUMINANCE; type =GL_UNSIGNED_SHORT; break;
		case KGL_INT:    internalFormat = GL_LUMINANCE32I_EXT; format = GL_LUMINANCE; type =  GL_UNSIGNED_INT; break;
		case KGL_FLOAT:  internalFormat = GL_LUMINANCE32F_ARB; format = GL_LUMINANCE; type =         GL_FLOAT; break;
		case KGL_VEC4:   internalFormat =      GL_RGBA32F_ARB; format =      GL_RGBA; type =         GL_FLOAT; break;
	}
	switch(tex[itex].tar)
	{
		case GL_TEXTURE_1D: glTexSubImage1D(tex[itex].tar,0,0    ,tex[itex].sizx               ,format,type,gbmp); break;
		case GL_TEXTURE_2D: glTexSubImage2D(tex[itex].tar,0,0,0  ,tex[itex].sizx,tex[itex].sizy,format,type,gbmp);
			if (((icoltype&0xf0) >= KGL_MIPMAP) && ((icoltype&15) == KGL_BGRA32)) gluBuild2DMipmaps(tex[itex].tar,4,tex[itex].sizx,tex[itex].sizy,GL_BGRA_EXT,GL_UNSIGNED_BYTE,gbmp);
			break;
		case GL_TEXTURE_3D: ((PFNGLTEXSUBIMAGE3DPROC)glfp[glTexSubImage3D])(tex[itex].tar,0,0,0,0,tex[itex].sizx,tex[itex].sizy,tex[itex].sizz,format,type,gbmp); break;
		case GL_TEXTURE_CUBE_MAP:
			for(i=0;i<6;i++) glTexSubImage2D(cubemapconst[i],0,0,0,tex[itex].sizx,tex[itex].sizx,format,type,gbmp+tex[itex].sizx*tex[itex].sizx*glbyteperpix*cubemapindex[i]);
			break;
	}

	return(0.0);
}
double __cdecl kglsettexarray2 (double dtex, double *p, double dxsiz, double dysiz, double coltype) { return(kglsettexarray3(dtex,p,dxsiz,dysiz,1.0,coltype)); }
double __cdecl kglsettexarray1 (double dtex, double *p, double dxsiz, double coltype)               { return(kglsettexarray3(dtex,p,dxsiz,  1.0,1.0,coltype)); }

double __cdecl qglBindTex (double dtex)
{
	int itex;
	itex = (int)dtex; if ((unsigned)itex >= (unsigned)MAXUSERTEX) return(-1.0);
	glBindTexture(tex[itex].tar,itex);
	return 0;
}
double __cdecl kglActiveTex (double texunit)
{
	int itexunit = ((int)texunit)&3;
	if (glfp[glActiveTexture]) ((PFNGLACTIVETEXTUREPROC)glfp[glActiveTexture])(itexunit+GL_TEXTURE0);
	return(0);
}
double __cdecl kgluPerspective (double fovy, double xy, double z0, double z1)
{
	glMatrixMode(GL_PROJECTION); glLoadIdentity(); gluPerspective(fovy,xy,z0,z1);
	return(0.0);
}
double __cdecl qgluLookAt (double x, double y, double z, double px, double py, double pz, double ux, double uy, double uz)
	{ gluLookAt(x,y,z,px,py,pz,ux,uy,uz); return(0.0); }
double ksetfov (double fov) { gfov = tan(fov*PI/360.0)*atan((float)oglyres/(float)oglxres)*360.0/PI; return(gfov); }

double kglProgramLocalParam (double ind, double a, double b, double c, double d)
{
	((PFNGLPROGRAMLOCALPARAMETER4FARBPROC)glfp[glProgramLocalParameter4fARB])(GL_VERTEX_PROGRAM_ARB,(unsigned)ind,a,b,c,d);
	((PFNGLPROGRAMLOCALPARAMETER4FARBPROC)glfp[glProgramLocalParameter4fARB])(GL_FRAGMENT_PROGRAM_ARB,(unsigned)ind,a,b,c,d);
	return(0.0);
}
double kglProgramEnvParam (double ind, double a, double b, double c, double d)
{
	((PFNGLPROGRAMENVPARAMETER4FARBPROC)glfp[glProgramEnvParameter4fARB])(GL_VERTEX_PROGRAM_ARB,(unsigned)ind,a,b,c,d);
	((PFNGLPROGRAMENVPARAMETER4FARBPROC)glfp[glProgramEnvParameter4fARB])(GL_FRAGMENT_PROGRAM_ARB,(unsigned)ind,a,b,c,d);
	return(0.0);
}

double kglGetUniformLoc (char *shadvarnam)
{
	int i;
	i = ((PFNGLGETUNIFORMLOCATIONPROC)glfp[glGetUniformLocation])(shadprog[gcurshader],shadvarnam);
	return((double)i);
}
double kglUniform1f (double sh, double v0)                                  { ((PFNGLUNIFORM1FPROC)glfp[glUniform1f])((int)sh,(float)v0);                               return(0.0); }
double kglUniform2f (double sh, double v0, double v1)                       { ((PFNGLUNIFORM2FPROC)glfp[glUniform2f])((int)sh,(float)v0,(float)v1);                     return(0.0); }
double kglUniform3f (double sh, double v0, double v1, double v2)            { ((PFNGLUNIFORM3FPROC)glfp[glUniform3f])((int)sh,(float)v0,(float)v1,(float)v2);           return(0.0); }
double kglUniform4f (double sh, double v0, double v1, double v2, double v3) { ((PFNGLUNIFORM4FPROC)glfp[glUniform4f])((int)sh,(float)v0,(float)v1,(float)v2,(float)v3); return(0.0); }
double kglUniform1i (double sh, double v0)                                  { ((PFNGLUNIFORM1IPROC)glfp[glUniform1i])((int)sh,(int)v0);                                 return(0.0); }
double kglUniform2i (double sh, double v0, double v1)                       { ((PFNGLUNIFORM2IPROC)glfp[glUniform2i])((int)sh,(int)v0,(int)v1);                         return(0.0); }
double kglUniform3i (double sh, double v0, double v1, double v2)            { ((PFNGLUNIFORM3IPROC)glfp[glUniform3i])((int)sh,(int)v0,(int)v1,(int)v2);                 return(0.0); }
double kglUniform4i (double sh, double v0, double v1, double v2, double v3) { ((PFNGLUNIFORM4IPROC)glfp[glUniform4i])((int)sh,(int)v0,(int)v1,(int)v2,(int)v3);         return(0.0); }
#define MAXUNIFVALNUM 4096
double kglUniform1fv (double sh, double num, double *vals)
{
	int i, inum; float *fvals;
	inum = min((int)num,MAXUNIFVALNUM); if (inum < 0) return(0.0);
	fvals = (float *)_alloca(inum*sizeof(fvals[0])); if (!fvals) return(0.0);
	for(i=0;i<inum;i++) fvals[i] = (float)vals[i];
	((PFNGLUNIFORM1FVPROC)glfp[glUniform1fv])((int)sh,inum,fvals);
	return(0.0);
}
double kglUniform2fv (double sh, double num, double *vals)
{
	int i, inum; float *fvals;
	inum = min((int)num,MAXUNIFVALNUM); if (inum < 0) return(0.0);
	fvals = (float *)_alloca(inum*sizeof(fvals[0])); if (!fvals) return(0.0);
	for(i=0;i<inum;i++) fvals[i] = (float)vals[i];
	((PFNGLUNIFORM2FVPROC)glfp[glUniform2fv])((int)sh,inum,fvals);
	return(0.0);
}
double kglUniform3fv (double sh, double num, double *vals)
{
	int i, inum; float *fvals;
	inum = min((int)num,MAXUNIFVALNUM); if (inum < 0) return(0.0);
	fvals = (float *)_alloca(inum*sizeof(fvals[0])); if (!fvals) return(0.0);
	for(i=0;i<inum;i++) fvals[i] = (float)vals[i];
	((PFNGLUNIFORM3FVPROC)glfp[glUniform3fv])((int)sh,inum,fvals);
	return(0.0);
}
double kglUniform4fv (double sh, double num, double *vals)
{
	int i, inum; float *fvals;
	inum = min((int)num,MAXUNIFVALNUM); if (inum < 0) return(0.0);
	fvals = (float *)_alloca(inum*sizeof(fvals[0])); if (!fvals) return(0.0);
	for(i=0;i<inum;i++) fvals[i] = (float)vals[i];
	((PFNGLUNIFORM4FVPROC)glfp[glUniform4fv])((int)sh,inum,fvals);
	return(0.0);
}
double kglUniform1iv (double sh, double num, double *vals)
{
	int i, inum; int *ivals;
	inum = min((int)num,MAXUNIFVALNUM); if (inum < 0) return(0.0);
	ivals = (int *)_alloca(inum*sizeof(ivals[0])); if (!ivals) return(0.0);
	for(i=0;i<inum;i++) ivals[i] = (int)vals[i];
	((PFNGLUNIFORM1IVPROC)glfp[glUniform1iv])((int)sh,inum,ivals);
	return(0.0);
}
double kglUniform2iv (double sh, double num, double *vals)
{
	int i, inum; int *ivals;
	inum = min((int)num,MAXUNIFVALNUM); if (inum < 0) return(0.0);
	ivals = (int *)_alloca(inum*sizeof(ivals[0])); if (!ivals) return(0.0);
	for(i=0;i<inum;i++) ivals[i] = (int)vals[i];
	((PFNGLUNIFORM2IVPROC)glfp[glUniform2iv])((int)sh,inum,ivals);
	return(0.0);
}
double kglUniform3iv (double sh, double num, double *vals)
{
	int i, inum; int *ivals;
	inum = min((int)num,MAXUNIFVALNUM); if (inum < 0) return(0.0);
	ivals = (int *)_alloca(inum*sizeof(ivals[0])); if (!ivals) return(0.0);
	for(i=0;i<inum;i++) ivals[i] = (int)vals[i];
	((PFNGLUNIFORM3IVPROC)glfp[glUniform3iv])((int)sh,inum,ivals);
	return(0.0);
}
double kglUniform4iv (double sh, double num, double *vals)
{
	int i, inum; int *ivals;
	inum = min((int)num,MAXUNIFVALNUM); if (inum < 0) return(0.0);
	ivals = (int *)_alloca(inum*sizeof(ivals[0])); if (!ivals) return(0.0);
	for(i=0;i<inum;i++) ivals[i] = (int)vals[i];
	((PFNGLUNIFORM4IVPROC)glfp[glUniform4iv])((int)sh,inum,ivals);
	return(0.0);
}

double kglGetAttribLoc (char *shadvarnam)
{
	int i;
	i = ((PFNGLGETATTRIBLOCATIONPROC)glfp[glGetAttribLocation])(shadprog[gcurshader],shadvarnam);
	return((double)i);
}
double kglVertexAttrib1f (double sh, double v0)                                  { ((PFNGLVERTEXATTRIB1FPROC)glfp[glVertexAttrib1f])((int)sh,(float)v0);                               return(0.0); }
double kglVertexAttrib2f (double sh, double v0, double v1)                       { ((PFNGLVERTEXATTRIB2FPROC)glfp[glVertexAttrib2f])((int)sh,(float)v0,(float)v1);                     return(0.0); }
double kglVertexAttrib3f (double sh, double v0, double v1, double v2)            { ((PFNGLVERTEXATTRIB3FPROC)glfp[glVertexAttrib3f])((int)sh,(float)v0,(float)v1,(float)v2);           return(0.0); }
double kglVertexAttrib4f (double sh, double v0, double v1, double v2, double v3) { ((PFNGLVERTEXATTRIB4FPROC)glfp[glVertexAttrib4f])((int)sh,(float)v0,(float)v1,(float)v2,(float)v3); return(0.0); }

double kglCullFace (double mode)
{
	int imode; imode = (int)mode;

	if (imode == GL_NONE) { glDisable(GL_CULL_FACE); return(0.0); }
	glEnable(GL_CULL_FACE);
	glCullFace(imode);
	glFrontFace(GL_CW);
	return(0.0);
}

double kglBlendFunc (double sfactor, double dfactor)
{
	glEnable(GL_BLEND);
	glBlendFunc(sfactor,dfactor);
	return(0.0);
}
double kglEnable (double d) { glEnable(d); return(0); }
double kglDisable (double d) { glDisable(d); return(0); }

double mysleep (double ms)
{
	int i = ((int)ms);
	i = min(max(i,0),1000);
	Sleep(i);
	return(0.0);
}

double glswapinterval (double val)
{
	((PFNWGLSWAPINTERVALEXTPROC)glfp[wglSwapIntervalEXT])((int)val);
	return(0.0);
}

static int ginstartklock = 0;
double glklockstart (double _)
{
	if (supporttimerquery)
	{
		if (ginstartklock) ((PFNGLENDQUERYPROC)glfp[glEndQuery])(GL_TIME_ELAPSED_EXT); else ginstartklock = 1;
		((PFNGLBEGINQUERYPROC)glfp[glBeginQuery])(GL_TIME_ELAPSED_EXT,queries[0]);
	}
	return(0.0);
}

double glklockelapsed (double _)
{
	GLuint64EXT qdtim;
	GLuint dtim;
	GLint got;

	if (!supporttimerquery) return(-1.0);
	if (!ginstartklock) { return(-2.0); } ginstartklock = 0;
	((PFNGLENDQUERYPROC)glfp[glEndQuery])(GL_TIME_ELAPSED_EXT);
	do { ((PFNGLGETQUERYOBJECTIVPROC)glfp[glGetQueryObjectiv])(queries[0],GL_QUERY_RESULT_AVAILABLE,&got); } while (!got);
	if (glfp[glGetQueryObjectui64vEXT])
	{
		((PFNGLGETQUERYOBJECTUI64VEXTPROC)glfp[glGetQueryObjectui64vEXT])(queries[0],GL_QUERY_RESULT,&qdtim);
		return(((double)qdtim)*1e-9);
	}
	((PFNGLGETQUERYOBJECTUIVPROC)glfp[glGetQueryObjectuiv])(queries[0],GL_QUERY_RESULT,&dtim);
	return(((double)dtim)*1e-9);
}

double myklock (double d)
{
	__int64 q;
	int i = (int)d;
	if (!i) { QueryPerformanceCounter((LARGE_INTEGER *)&q); return(((double)(q-qtim0))/((double)qper)); }
	if (labs(i) < 10)
	{
			//t = klock(0); //0=seconds since compile, <0=UTC time, >0=local time
			//For example: 2009070414301725 is: July 4, 2009, 2:30pm + 17.25 seconds
			//Nice test program:
			//   cls(0); moveto(0,100);
			//   for(i=-9;i<=9;i++) printf("klock(%+2g) = %f\n",i,klock(i));
		SYSTEMTIME tim;
		if (i < 0) { GetSystemTime(&tim); i = -i; } else GetLocalTime(&tim);
		switch(i)
		{
			case 1:
			{
				__int64 q = ((__int64)tim.wYear        )*10000000000000I64 +
								((__int64)tim.wMonth       )*100000000000I64 +
								((__int64)tim.wDay         )*1000000000I64 +
								((__int64)tim.wHour        )*10000000I64 +
								((__int64)tim.wMinute      )*100000I64 +
								((__int64)tim.wSecond      )*1000I64 +
								((__int64)tim.wMilliseconds);
				 return(((double)q)*.001); //YYYYMMDDHHMMSS.sss
			}
			case 2: return((double)tim.wYear);
			case 3: return((double)tim.wMonth);
			case 4: return((double)tim.wDayOfWeek);
			case 5: return((double)tim.wDay);
			case 6: return((double)tim.wHour);
			case 7: return((double)tim.wMinute);
			case 8: return((double)tim.wSecond);
			case 9: return((double)tim.wMilliseconds);
		}
	}
	return(0.0);
}