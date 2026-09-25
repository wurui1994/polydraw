

//--------------------------------------------------------------------------------------------------
	//Tom Dobrowolski's noise algo

static __forceinline float fgrad (int h, float x, float y, float z)
{
	switch (h&15)
	{
		case  0: return( x+y  );
		case  1: return(-x+y  );
		case  2: return( x-y  );
		case  3: return(-x-y  );
		case  4: return( x  +z);
		case  5: return(-x  +z);
		case  6: return( x  -z);
		case  7: return(-x  -z);
		case  8: return(   y+z);
		case  9: return(  -y+z);
		case 10: return(   y-z);
		case 11: return(  -y-z);
		case 12: return( x+y  );
		case 13: return(-x+y  );
		case 14: return(   y-z);
		case 15: return(  -y-z);
	}
	return(0);
}

static int noisep[512], lut3m2[1024];
static void noiseinit ()
{
	int i, j, k;
	float f;

	for(i=256-1;i>=0;i--) noisep[i] = i;
	for(i=256-1;i> 0;i--) { j = ((rand()*(i+1))>>15); k = noisep[i]; noisep[i] = noisep[j]; noisep[j] = k; }
	for(i=256-1;i>=0;i--) noisep[i+256] = noisep[i];

	for(i=1024-1;i>=0;i--) { f = ((float)i)/1024.f; lut3m2[i] = (int)(((3.f - 2.f*f)*f*f)*1024.f); }
}

#if defined(_MSC_VER)
static __forceinline void dtol (double f, int *a)
{
	_asm
	{
		mov eax, a
		fld f
		fistp dword ptr [eax]
	}
}
#else
static void dtol (double f, int *a) { a = (int)f; }
#endif

static double noise1d (double fx)
{
	int i, l[1];
	float p[1], t[1], f[2];

	dtol(fx-.5,&l[0]); p[0] = fx-((float)l[0]); l[0] &= 255; t[0] = (3.0 - 2.0*p[0])*p[0]*p[0];
	f[0] = fgrad(noisep[noisep[noisep[l[0]  ]]],p[0]  ,0,0);
	f[1] = fgrad(noisep[noisep[noisep[l[0]+1]]],p[0]-1,0,0);
	return((f[1]-f[0])*t[0] + f[0]);
}

static double noise2d (double fx, double fy)
{
	int i, l[2], a[4];
	float p[2], t[2], f[4];

	dtol(fx-.5,&l[0]); p[0] = fx-((float)l[0]); l[0] &= 255; t[0] = (3.0 - 2.0*p[0])*p[0]*p[0];
	dtol(fy-.5,&l[1]); p[1] = fy-((float)l[1]); l[1] &= 255; t[1] = (3.0 - 2.0*p[1])*p[1]*p[1];
	i = noisep[l[0]  ]; a[0] = noisep[i+l[1]]; a[2] = noisep[i+l[1]+1];
	i = noisep[l[0]+1]; a[1] = noisep[i+l[1]]; a[3] = noisep[i+l[1]+1];
	f[0] = fgrad(noisep[a[0]],p[0]  ,p[1],0);
	f[1] = fgrad(noisep[a[1]],p[0]-1,p[1],0); p[1]--;
	f[2] = fgrad(noisep[a[2]],p[0]  ,p[1],0);
	f[3] = fgrad(noisep[a[3]],p[0]-1,p[1],0);
	f[0] = (f[1]-f[0])*t[0] + f[0];
	f[1] = (f[3]-f[2])*t[0] + f[2];
	return((f[1]-f[0])*t[1] + f[0]);
}

static double noise3d (double fx, double fy, double fz)
{
	int i, l[3], a[4];
	float p[3], t[3], f[8];

	dtol(fx-.5,&l[0]); p[0] = fx-((float)l[0]); l[0] &= 255; t[0] = (3.0 - 2.0*p[0])*p[0]*p[0];
	dtol(fy-.5,&l[1]); p[1] = fy-((float)l[1]); l[1] &= 255; t[1] = (3.0 - 2.0*p[1])*p[1]*p[1];
	dtol(fz-.5,&l[2]); p[2] = fz-((float)l[2]); l[2] &= 255; t[2] = (3.0 - 2.0*p[2])*p[2]*p[2];
	i = noisep[l[0]  ]; a[0] = noisep[i+l[1]]; a[2] = noisep[i+l[1]+1];
	i = noisep[l[0]+1]; a[1] = noisep[i+l[1]]; a[3] = noisep[i+l[1]+1];
	f[0] = fgrad(noisep[a[0]+l[2]  ],p[0]  ,p[1]  ,p[2]);
	f[1] = fgrad(noisep[a[1]+l[2]  ],p[0]-1,p[1]  ,p[2]);
	f[2] = fgrad(noisep[a[2]+l[2]  ],p[0]  ,p[1]-1,p[2]);
	f[3] = fgrad(noisep[a[3]+l[2]  ],p[0]-1,p[1]-1,p[2]); p[2]--;
	f[4] = fgrad(noisep[a[0]+l[2]+1],p[0]  ,p[1]  ,p[2]);
	f[5] = fgrad(noisep[a[1]+l[2]+1],p[0]-1,p[1]  ,p[2]);
	f[6] = fgrad(noisep[a[2]+l[2]+1],p[0]  ,p[1]-1,p[2]);
	f[7] = fgrad(noisep[a[3]+l[2]+1],p[0]-1,p[1]-1,p[2]);
	f[0] = (f[1]-f[0])*t[0] + f[0];
	f[1] = (f[3]-f[2])*t[0] + f[2];
	f[2] = (f[5]-f[4])*t[0] + f[4];
	f[3] = (f[7]-f[6])*t[0] + f[6];
	f[0] = (f[1]-f[0])*t[1] + f[0];
	f[1] = (f[3]-f[2])*t[1] + f[2];
	return((f[1]-f[0])*t[2] + f[0]);
}