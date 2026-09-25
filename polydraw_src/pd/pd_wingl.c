
//--------------------------------------------------------------------------------------------------

static HANDLE /*gmainthread,*/ gthand, ghevent[3];
static double (__cdecl *gevalfunc)(void) = 0;
static int gevalfuncleng = 0;
static int showtimeout = 0;
static unsigned int __stdcall watchthread (void *_)
{
	while (1)
	{
		WaitForSingleObject(ghevent[0],INFINITE);

			//if script takes too long, temporarily apply self-modifying code to force it to finish much faster
		if (WaitForSingleObject(ghevent[1],popts.timeout) == WAIT_TIMEOUT)
		{
			showtimeout = 1;
			kasm87jumpback(gevalfunc,0);
			WaitForSingleObject(ghevent[1],INFINITE);
			kasm87jumpback(gevalfunc,1);
		}

		SetEvent(ghevent[2]);
	}
}

HMENU gmenu = 0;
static short *menustart (short *sptr) { *sptr++ = 0; *sptr++ = 0; return(sptr); } //MENUITEMTEMPLATEHEADER
static short *menuadd (short *sptr, char *st, int flags, int id)
{
	*sptr++ = flags; //MENUITEMTEMPLATE
	if (!(flags&MF_POPUP)) *sptr++ = id;
	sptr += MultiByteToWideChar(CP_ACP,0,st,-1,(LPWSTR)sptr,strlen(st)+1);
	return(sptr);
}

#if 0
#include <gl/glu.h>
#pragma comment(lib,"glu32.lib")
#else
	//GLU replacements..
static void gluPerspective (double fovy, double xy, double z0, double z1)
	{ fovy = tan(fovy*(PI/360.0))*z0; xy *= fovy; glFrustum(-xy,xy,-fovy,fovy,z0,z1); }
static void gluLookAt (double px, double py, double pz, double fx, double fy, double fz, double ux, double uy, double uz)
{
	double t, r[3], d[3], f[3], mat[16];

	f[0] = px-fx; f[1] = py-fy; f[2] = pz-fz;
	t = 1.0/sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]); f[0] *= t; f[1] *= t; f[2] *= t;

	r[0] = f[2]*uy - f[1]*uz;
	r[1] = f[0]*uz - f[2]*ux;
	r[2] = f[1]*ux - f[0]*uy;
	t = 1.0/sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]); r[0] *= t; r[1] *= t; r[2] *= t;

	d[0] = f[1]*r[2] - f[2]*r[1];
	d[1] = f[2]*r[0] - f[0]*r[2];
	d[2] = f[0]*r[1] - f[1]*r[0];

	mat[0] = r[0]; mat[4] = r[1]; mat[ 8] = r[2]; mat[12] = -(mat[0]*px + mat[4]*py + mat[ 8]*pz);
	mat[1] = d[0]; mat[5] = d[1]; mat[ 9] = d[2]; mat[13] = -(mat[1]*px + mat[5]*py + mat[ 9]*pz);
	mat[2] = f[0]; mat[6] = f[1]; mat[10] = f[2]; mat[14] = -(mat[2]*px + mat[6]*py + mat[10]*pz);
	mat[3] =  0.0; mat[7] =  0.0; mat[11] =  0.0; mat[15] = 1.0;
	glLoadMatrixd(mat);
}
static int gluBuild2DMipmaps (GLenum target, GLint components, GLint xs, GLint ys, GLenum format, GLenum type, const void *data)
{
	unsigned char *wptr, *rptr, *rptr2;
	int i, x, y,  nxs, nys, xs4, nxs4;

	for(i=1;(xs|ys)&~1;i++,xs=nxs,ys=nys)
	{
		nxs = max(xs>>1,1); nys = max(ys>>1,1); xs4 = (xs<<2); nxs4 = (nxs<<2); //from GL_ARB_texture_non_power_of_two spec
		wptr = (unsigned char *)data; rptr = (unsigned char *)data;
		for(y=0;y<nys;y++,wptr+=nxs4,rptr+=xs4*2)
			for(x=0;x<nxs4;x++)
			{
				rptr2 = &rptr[(x&~3)+x];
				wptr[x] = (((int)rptr2[  0] + (int)rptr2[    4] +
								(int)rptr2[xs4] + (int)rptr2[xs4+4] + 2)>>2);
			}
		glTexImage2D   (target,i,4  ,nxs,nys,0,format,type,data); //loading 1st time
	 //glTexSubImage2D(target,i,0,0,nxs,nys  ,format,type,data); //overwrite old texture
	}
	return(0);
}
#endif

double __cdecl qglVertex2d    (double x, double y)                     { glVertex2d(x,y);          return(0.0); }
double __cdecl qglVertex3d    (double x, double y, double z)           { glVertex3d(x,y,z);        return(0.0); }
double __cdecl qglVertex4d    (double x, double y, double z, double w) { glVertex4d(x,y,z,w);      return(0.0); }
double __cdecl qglTexCoord2d  (double u, double v)                     { glTexCoord2d(u,v);        return(0.0); }
double __cdecl qglTexCoord3d  (double u, double v, double s)           { glTexCoord3d(u,v,s);      return(0.0); }
double __cdecl qglTexCoord4d  (double u, double v, double s, double t) { glTexCoord4d(u,v,s,t);    return(0.0); }
double __cdecl qglColor3d     (double x, double y, double z)           { glColor3d(x,y,z);         return(0.0); }
double __cdecl qglColor4d     (double x, double y, double z, double w) { glColor4d(x,y,z,w);       return(0.0); }
double __cdecl qglNormal3d    (double x, double y, double z)           { glNormal3d(x,y,z);        return(0.0); }

double __cdecl qglClear       (double mask)                            { if (mask) glClear(mask);
                                                                         else glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);  
                                                                         return(0.0); }
double __cdecl qglBegin       (double mode)                            { glBegin((int)mode);       return(0.0); }
double __cdecl qglEnd         (double _)                               { glEnd();                  return(0.0); }
double __cdecl qglPushMatrix  (double _)                               { glPushMatrix();           return(0.0); }
double __cdecl qglPopMatrix   (double _)                               { glPopMatrix();            return(0.0); }
double __cdecl qglMultMatrixd (double *m)                              { if (m) glMultMatrixd(m);  return(0.0); }
double __cdecl qglTranslated  (double x, double y, double z)           { glTranslated(x,y,z);      return(0.0); }
double __cdecl qglRotated     (double t, double x, double y, double z) { glRotated(t,x,y,z);       return(0.0); }
double __cdecl qglScaled      (double x, double y, double z)           { glScaled(x,y,z);          return(0.0); }
double __cdecl qglEndTex      (double _)                               {                           return(0.0); }
double __cdecl qglLineWidth   (double size)                            { glLineWidth(size);        return(0.0); }

double __cdecl kmyrgb         (double r, double g, double b)           { return((double)((min(max((int)r,0),255)<<16) + (min(max((int)g,0),255)<<8) + min(max((int)b,0),255))); }
double __cdecl kmyrgba        (double r, double g, double b, double a) { return((double)((min(max((int)a,0),255)<<24) + (min(max((int)r,0),255)<<16) + (min(max((int)g,0),255)<<8) + min(max((int)b,0),255))); }

static int myprintf_check (char *fmt)
{
	int i, inperc, inslash;

	if (!fmt) return(-1);

	inperc = 0; inslash = 0; //Filter out
	for(i=0;fmt[i];i++)
	{
		if (inslash) { inslash = 0; continue; }
		if (fmt[i] == '\\') { inslash = 1; continue; }
		if (fmt[i] == '%') { inperc ^= 1; continue; }
		if (!inperc) continue;

			//int types
		if ((fmt[i] == 'c') || (fmt[i] == 'C') || (fmt[i] == 'd') || (fmt[i] == 'i') ||
			 (fmt[i] == 'o') || (fmt[i] == 'u') || (fmt[i] == 'x') || (fmt[i] == 'X'))
			{ kputs("invalid %",1); return(0); }

			//double types
		if ((fmt[i] == 'e') || (fmt[i] == 'E') || (fmt[i] == 'f') || (fmt[i] == 'g') || (fmt[i] == 'G'))
			{ inperc = 0; continue; }

			//pointer types
		if ((fmt[i] == 'n') || (fmt[i] == 'p') || (fmt[i] == 's') || (fmt[i] == 'S') || (fmt[i] == 'Z'))
			{ kputs("invalid %",1); return(0); }
	}
	return(1);
}
static void myprintf_filter (char *st)
{
	int i, j, inslash;

		//Filter \\, \n, etc..
	inslash = 0;
	for(i=0,j=0;st[i];i++)
	{
		if (inslash)
		{
			inslash = 0;
			if (st[i] == 'b') { if (j) j--; continue; }
			if (st[i] == 'r') { st[j++] = 13; continue; }
			if (st[i] == 'n') { st[j++] = 10; continue; }
			if (st[i] == 't') { st[j++] = 9; continue; }
		} else if (st[i] == '\\') { inslash = 1; continue; }
		st[j++] = st[i];
	}
	st[j] = 0;
}

double __cdecl myprintf (char *fmt, ...)
{
	va_list arglist;
	char st[2048];

	if (!myprintf_check(fmt)) return(-1.0);

	va_start(arglist,fmt);
	_vsnprintf(st,sizeof(st),fmt,arglist);
	va_end(arglist);

	myprintf_filter(st);

	kputs(st,0);

	return(0.0);
}