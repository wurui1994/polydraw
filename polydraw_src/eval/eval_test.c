#if 0 //To compile as a stand-alone test program, type "nmake eval.c"
!ifndef COMP
COMP=1
!endif
eval.exe: eval.c; cl eval.c /Ox /G6Fy /Gs /MD /nologo /DEVALTEST /DCOMPILE=$(COMP) /link /opt:nowin98 /nologo kernel32.lib
	del eval.obj
!if 0
#endif


//------------------------------------------ KASM87 ENDS ------------------------------------------

#ifdef EVALTEST

#if defined(_MSC_VER) && defined(_M_IX86)
static __forceinline __int64 rdtsc64 () { _asm rdtsc }
#elif defined(__GNUC__) && defined(__i386__)
static _inline __int64 rdtsc64 ()
{
	unsigned long long q;
	__asm__ __volatile__ ("rdtsc\n" : "=A" (q) : : "memory");
	return(q);
}
#elif defined(powerc) || defined(__POWERC__) || defined(__ppc__) || defined(ppc)
static _inline __int64 rdtsc64 ()
{
	//register unsigned long t; __asm__ __volatile__ ("mftb %0":"=r"(t)); return((__int64)t); //32-bit only

		//Code from: http://ozlabs.org/pipermail/linuxppc-dev/1999-October/003889.html
		//TimeBase resolution is 41.5Mhz on a 1.412Mhz MacMini
	unsigned long long q;
	unsigned long t;
	__asm__ __volatile__ ("\n\
1:    mftbu %1\n\
		mftb %L0\n\
		mftbu %0\n\
		cmpw %0,%1\n\
		bne 1b"
		: "=r" (q), "=r" (t));
	return(q);
}
#else
static __int64 rdtsc64 () { return(LL(0)); }
#endif

	//This function is useful for debugging FP stack overflows/underflows/leaks
void dumpfp ()
{
#ifdef _MSC_VER

	long i, j, fpreg[8];

	_asm fnstenv fpreg
	if ((fpreg[1]&64) || ((fpreg[2]&0xffff) != 0xffff))
	{
		printf("\n\nERROR: fp stack corrupt! (st() regs should be: ---)");
		j = (fpreg[1]>>11)&7;
		printf("\n     "); for(i=0;i<8;i++) printf("st%d ",(i-j)&7);
		printf("\nTag: ");
		for(i=0;i<8;i++)
		{
			switch((fpreg[2]>>((i&7)<<1))&3)
			{
				case 0: printf(" #  "); break;
				case 1: printf(" 0  "); break;
				case 2: printf("NaN "); break;
				case 3: printf("--- "); break;
			}
		}
		if (fpreg[1]&64)
		{
			if (fpreg[1]&512) printf("\nFP Stack overflow: too many fld!");
							 else printf("\nFP Stack underflow: too many fst!");
		}

		_asm //This code ruins "sticky" FP exception flag, so make sure it's after fnstenv
		{
			mov eax, 28
begfpstk:fdecstp
			fst dword ptr fpreg[eax]
			sub eax, 4
			jge short begfpstk
			fninit
		}
		for(i=0;i<8;i++) printf("\nst(%d) = 0x%08x, %f",i,fpreg[i],*(float *)&fpreg[i]);
		printf("\n\n");
	}
#endif
}

	//NOTE: Non-VC compilers only support up to 3 params - else crash.
void testcode (char *st, double *v, long vnum)
{
	//double (__cdecl *fptr)(double, ...);
	EVALFUNC fptr;
	double d;
	__int64 q0, q1, q2;
	long i, j, k;

	printf("%s =\n",st);

	ksrand(17); snormstat = 0;

	//fptr = (double (__cdecl *)(double, ...))kasm87(st);
	fptr = (EVALFUNC)kasm87(st);
	if (fptr) { kasm87_showdebug(3,debuf,sizeof(debuf)); printf("%s",debuf); }
		  else { puts(kasm87err); return; }
	printf("value:");

#ifndef _MSC_VER
	d = fptr(v[0],v[1],v[2]);
#else
	_asm
	{
		mov eax, vnum
		mov edx, v
beg:  push dword ptr [edx+eax*8-4]
		push dword ptr [edx+eax*8-8]
		sub eax, 1
		jg short beg
		call dword ptr fptr
		mov eax, vnum
		lea esp, [esp+eax*8]
		fstp qword ptr d
	}
#endif
	printf(" %.25g",d);
	dumpfp();

	q2 = LL(0x7fffffffffffffff);
	for(j=16;j;j--)
	{
		q0 = rdtsc64();
		for(i=256;i;i--)
		{
#ifndef _MSC_VER
			fptr(v[0],v[1],v[2]);
#else
			_asm
			{
				mov eax, vnum
				mov edx, v
	  beg2:  push dword ptr [edx+eax*8-4]
				push dword ptr [edx+eax*8-8]
				sub eax, 1
				jg short beg2
				call dword ptr fptr
				mov eax, vnum
				lea esp, [esp+eax*8]
				fstp qword ptr d
			}
#endif
		}
		q1 = rdtsc64();
		if (q1-q0 < q2) q2 = q1-q0;
	}
	printf(" %10" PRINTF64 " cc\n",q2>>8);

	kasm87free((void *)fptr);
}

static double pilut[] = {3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3,2,3,8,4,6,2,6,4};
double __cdecl getdigpi (double d)
{
	if ((d < 0) || (d >= (sizeof(pilut)/sizeof(pilut[0])))) return(0.0);
	return(pilut[(long)d]);
}

static double goldratlut[] = {1,6,1,8,0,3,3,9,8,8,7,4,9,8,9,5};
double __cdecl getdiggoldrat (double d)
{
	if ((d < 0) || (d >= (sizeof(goldratlut)/sizeof(goldratlut[0])))) return(0.0);
	return(goldratlut[(long)d]);
}

double __cdecl getangle360 (double x, double y)
{
	double d;
	d = atan2(y,x)*180/PI;
	if (d < 0) d += 360;
	return(d);
}

double __cdecl func4d (double a, double b, double c, double d) { return(((a*10+b)*10+c)*10+d); }

double __cdecl sndfunc (double x)
{
	return(x);
}

double __cdecl mysrand (double val) { kholdrand = (long)val; return(0); }
double __cdecl printnum (double n) { printf("%g ",n); return(0); }

static double buf8[8], buf11[11], buf3_2[3][2] = {2,3,5,7,11,13};

	//C += A x B
double __cdecl crossprod (double  Ax, double  Ay, double  Az,
								  double  Bx, double  By, double  Bz,
								  double *Cx, double *Cy, double *Cz)
{
	(*Cx) += Ay*Bz - Az*By;
	(*Cy) += Az*Bx - Ax*Bz;
	(*Cz) += Ax*By - Ay*Bx;
	return(0);
}

double __cdecl printst (double d, char *st)
{
	long i;
	for(i=(long)d;i>0;i--) printf("\n|%s|",st);
	return(0);
}

int main (int argc, char **argv)
{
	double v[256];
	long i, j, nparams;

	memset(v,0,sizeof(v));
	if (argc >= 2)
	{
		for(i=argc-2-1;i>=0;i--) v[i] = atof(argv[i+2]);
		testcode(argv[1],v,argc-2);
		return(0);
	}

	v[0] = 0.5; v[1] = 0.8; testcode("(x,y)cos(max(x,y)*PI)^2+sin(max(y,x)*PI)^2",v,2); //=1.0!
	puts("");
	v[0] = 2.0/3.0; testcode("(x)y=PI/2*x;z=y^2;(z/fact(5)-1/fact(3))*z*y+y",v,1); //~fcos
	puts("");

		//Simple example #1: Celsius to Fahrenheit converter
	{
	//double (__cdecl *c2f)(double, ...) = (double (__cdecl *)(double, ...))kasm87("(x)(x*(9/5))+32");
	EVALFUNC c2f = (EVALFUNC)kasm87("(x)(x*(9/5))+32");
	if (c2f)
	{
		printf("%gøC = %gøF\n",20.0,c2f(20.0));
		kasm87free(c2f);
	}
	}

		//Simple example #2: Hypotenuse calculator
	{
	EVALFUNC hypot = (EVALFUNC)kasm87("(cat,dog)cat*=cat;bozo=dog^2;sqrt(cat+bozo)");
	if (hypot)
	{
#ifndef _MSC_VER
		printf("hypot(%g,%g) = %g\n",3.0,4.0,hypot(3.0,4.0));
#else
		v[0] = 3.0; v[1] = 4.0; //v[2] = hypot(v[0],v[1]);
		_asm //Show how to call kasm87 code in ASM
		{
			push dword ptr [v+12]
			push dword ptr [v+8]
			push dword ptr [v+4]
			push dword ptr [v]
			call dword ptr [hypot]
			fstp qword ptr [v+16]
			add esp, 16
		}
		printf("hypot(%g,%g) = %g\n",v[0],v[1],v[2]);
#endif
		kasm87free(hypot);
	}
	}

		//Example #3: Passing user functions by pointer
	{
	EVALFUNC passfunc = (EVALFUNC)kasm87("(x,pifunc())pifunc(x)*10+pifunc(x+1)");
	if (passfunc)
	{
		double d;
		kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
		for(d=.5;d<5;d++) { printf("sillypifunc(%g) = %g\n",d,passfunc(d,getdigpi)); }
		kasm87free(passfunc);
	} else puts(kasm87err);
	}

		//Example #4: Passing 2 user functions by pointer
	{
	EVALFUNC passfunc = (EVALFUNC)kasm87("(x,goldfunc(),pifunc())pifunc(x)*1000+goldfunc(x)*100+pifunc(x)*10+goldfunc(x)");
	if (passfunc)
	{
		double d;
		kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
		for(d=.5;d<5;d++) { printf("sillydualfunc(%g) = %g\n",d,passfunc(d,getdiggoldrat,getdigpi)); }
		kasm87free(passfunc);
	} else puts(kasm87err);
	}

		//Example #5: Passing user functions with 2 variables by pointer
	{
	EVALFUNC passfunc = (EVALFUNC)kasm87("(x,y,ang2vec(,))ang2vec(x,y)");
	if (passfunc)
	{
		double x, y;
		puts("");
		for(i=1;i>=-1;i-=2)
		{
			x = 1; y = (double)i;
			printf("dumbanglefunc(%g,%g) = %g (should be %g)\n",x,y,passfunc(x,y,getangle360),getangle360(x,y));
		}
		kasm87free(passfunc);
	} else puts(kasm87err);
	}

		//Example #6: Passing variables by pointer
	{
	EVALFUNC passfunc = (EVALFUNC)kasm87("(ang,&x,&y)ang*=PI/180;x=cos(ang);y=sin(ang);");
	if (passfunc)
	{
		double a, x, y;
		kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
		a = 30; x = y = -17.0; passfunc(a,&x,&y); printf("getunitvector(%g) = %g,%g\n",a,x,y);
		printf("        [Should be: %g,%g]\n",cos(a*PI/180),sin(a*PI/180));
		kasm87free(passfunc);
	} else puts(kasm87err);
	}

		//Example #7: More with pointers
	{
	EVALFUNC passfunc = (EVALFUNC)kasm87("(x,y,&r,&g,&b)r=x+y;g=x*y;b=x/y;");
	if (passfunc)
	{
		double x, y, r, g, b;
		kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
		x = 4; y = 3; r = g = b = -17.0; passfunc(x,y,&r,&g,&b); printf("getcol(%g,%g) = %g,%g,%g\n",x,y,r,g,b);
		printf("  [Should be: %g,%g,%g]\n",x+y,x*y,x/y);
		kasm87free(passfunc);
	} else puts(kasm87err);
	}

		//Example #8: Passing arrays by pointers (indices must be hardcoded)
	{
	EVALFUNCP passfunc = (EVALFUNCP)kasm87("(a[3])a[0]+=a[1];a[1]+=a[2];");
	if (passfunc)
	{
		double a[3];
		kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
		a[0] = 1; a[1] = 2; a[2] = 3;
		printf("%g,%g,%g  <-Should be: 1 2 3\n",a[0],a[1],a[2]); passfunc(a);
		printf("%g,%g,%g  <-           3 5 3\n",a[0],a[1],a[2]); passfunc(a);
		printf("%g,%g,%g  <-           8 8 3\n",a[0],a[1],a[2]);
		kasm87free(passfunc);
	} else puts(kasm87err);
	}

		//Example #9: Compile with external library
	{
	double d;
	evalextyp myext[] =
	{
		{"LOCVAR"     ,&d         },
		{"PILUT[24]"  ,pilut      },
		{"GETDIGPI()" ,getdigpi   },
		{"GETANG(,)"  ,getangle360},
		{"FUNC4D(,,,)",func4d     },
	};
	kasm87addext(myext,sizeof(myext)/sizeof(myext[0]));
	{
	EVALFUNC libfunc = (EVALFUNC)kasm87("(x)func4d(pilut[0],getdigpi(1),getdigpi(2),pilut[3])+x*10000");
	if (libfunc)
	{
		puts("");
		kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);

		for(i=0;i<5;i++)
		{
			d = ((double)(((rand()&32767)*10)>>15));
			printf("%g: = ",d);
			printf("%5g ",libfunc(d));
			printf("\n");
		}

		//puts(""); for(d=0;d<(sizeof(pilut)/sizeof(pilut[0]));d++) printf("%g",libfunc(d));

		kasm87free(libfunc);
	} else puts(kasm87err);
	}
	kasm87addext(0,0);
	}

#if 0
		//Example #10: Extended variable names in quotes (put any "snd0.wav"&"snd1.wav" in directory)
	{
	double d;
	evalextyp myext[] =
	{
		{"\"snd0.wav\"()",sndfunc},
		{"\"snd1.wav\"()",sndfunc},
	};
	kasm87addext(myext,sizeof(myext)/sizeof(myext[0]));
	{
	EVALFUNC libfunc = (EVALFUNC)kasm87("(x)\"snd0.wav\"(x)+\"snd1.wav\"(x)");
	if (libfunc)
	{
		kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);

		for(i=0;i<5;i++)
		{
			d = ((double)(((rand()&32767)*10)>>15));
			printf("%g: = ",d);
			printf("%5g ",libfunc(d));
			printf("\n");
		}

		kasm87free(libfunc);
	} else puts(kasm87err);
	}
	kasm87addext(0,0);
	}
#endif

		//Example #11: Lookup table
	{
	double d, crc32[256];
	long j, k;
	for(i=255;i>=0;i--)
	{
		k = i; for(j=8;j;j--) k = ((unsigned long)k>>1)^((-(k&1))&0xedb88320);
		crc32[i] = (double)k;
	}
	{
	EVALFUNC lutfunc = (EVALFUNC)kasm87("(x,crc32[256])crc32[x]");
	if (lutfunc)
	{
		kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
		for(i=0;i<256;i+=85)
		{
			printf("crc32[%3d]: ",i);
			printf("%11.1f ",lutfunc((double)i,crc32));
			printf("%10d\n",(long)crc32[i]);
		}
		kasm87free(lutfunc);
	} else puts(kasm87err);
	}
	}

		//Example #12: Write buffer
	{
	double d, sq[8];
	{
	//EVALFUNC lutfunc = (EVALFUNC)kasm87("(sc,buf[8])i=7;do{buf[i]=i*i*sc;i--;}while(i>=0);0");
	EVALFUNC lutfunc = (EVALFUNC)kasm87("(sc,buf[8])for(i=0;i<8;i++)buf[i]=i*i*sc;0");
	if (lutfunc)
	{
		kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
		memset(sq,0,sizeof(double)*8); lutfunc(3.0,sq); for(i=0;i<8;i++) printf("%g ",sq[i]); printf("\n");
		printf("0 3 12 27 48 75 108 147 <- Should be\n");
		kasm87free(lutfunc);
	} else puts(kasm87err);
	}
	}

		//Example #13: User static buffer
	{
	double d;
	evalextyp myext[] = {"PRINTNUM()",printnum,"SRAND()",mysrand};
	kasm87addext(myext,sizeof(myext)/sizeof(myext[0]));
	{
	EVALFUNC arrfunc = (EVALFUNC)kasm87(
		"(dum) static buf[28]; srand(0);"
		"for(i=0;i<28;i++) buf[i] = i;"
		"for(i=28;i>1;i--) { j = int(i*rnd); z = buf[j]; buf[j] = buf[i-1]; buf[i-1] = z; }"
		"for(i=0;i<28;i++) printnum(buf[i]);"
		"0");
	if (arrfunc)
	{
		printf("\n");
		//kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
		arrfunc(0.0); printf("\n");
		kasm87free(arrfunc);
	} else puts(kasm87err);
	}
	}

		//Example #14: Test behavior of array index out of bounds
	{
	evalextyp myext[] = {"BUF8[8]",buf8,"BUF11[11]",buf11};
	kasm87addext(myext,sizeof(myext)/sizeof(myext[0]));
	{
	EVALFUNC boundtst = (EVALFUNC)kasm87("(dum) for(i=0;i<13;i++) { buf8[i] = i; buf11[i] = i; } 0");
	if (boundtst)
	{
		printf("\n");
		//kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
		for(i=0;i<8;i++) buf8[i] = -1.0;
		for(i=0;i<11;i++) buf11[i] = -1.0;
		boundtst(0.0);
		for(i=0;i<8;i++) printf("%g,",buf8[i]); printf("  ");
		for(i=0;i<11;i++) printf("%g,",buf11[i]); printf("\n");
		printf("8,9,10,11,12,5,6,7,  12,1,2,3,4,5,6,7,8,9,10 <- Should be\n");
		kasm87free(boundtst);
	} else puts(kasm87err);
	}
	kasm87addext(0,0);
	}

		//Example #15: External function passing by pointer
	{
	evalextyp myext[] =
	{
		{"CROSSPROD(,,,,,,&,&,&)",crossprod},
	};
	kasm87addext(myext,sizeof(myext)/sizeof(myext[0]));
	{
	EVALFUNCP makevec = (EVALFUNCP)kasm87("(&x,&y,&z)crossprod(y,z,x,z,x,y,&x,&y,&z);x*=10;y*=10;z*=10;");
	if (makevec)
	{
		puts("");
		kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
		v[0] = 2; v[1] = 3; v[2] = 5; makevec(&v[0],&v[1],&v[2]);
		printf("Should be:[130,40,-140]\n");
		printf("Result is: %g,%g,%g\n",v[0],v[1],v[2]);
		kasm87free(makevec);
	} else puts(kasm87err);
	}
	kasm87addext(0,0);
	}

		//Example #16: Eval function calling another Eval function
	{
	EVALFUNC rot2d, doubrot;
	rot2d = (EVALFUNC)kasm87("(a,&x,&y)a*=PI/180;c=cos(a);s=sin(a);ox=x;x=x*c-y*s;y=y*c+ox*s;");
	if (rot2d)
	{
		evalextyp myext[] = {{"ROT2D(,&,&)",rot2d}};
		//kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
		kasm87addext(myext,sizeof(myext)/sizeof(myext[0]));
		doubrot = (EVALFUNC)kasm87("(x,y,&x2,&y2)x2=x;y2=y;rot2d(45,&x2,&y2);x2++;rot2d(-45,&x2,&y2);");
		if (doubrot)
		{
			//kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
			doubrot(1.0,0.0,&v[0],&v[1]); printf("\nShould be:[1.70711,-0.707107]\nResult is: %g,%g\n",v[0],v[1]);
		} else puts(kasm87err);
	} else puts(kasm87err);
	kasm87free(doubrot);
	kasm87free(rot2d);
	kasm87addext(0,0);
	}

		//Example #17: Eval function calling another Eval function
	{
	EVALFUNC cubesum = (EVALFUNC)kasm87("(x,y) { return(cube(x)+cube(y)); } cube(x) { return(x*x*x); }");
	if (cubesum) { printf("\nShould be:[35]\nResult is: %g\n",cubesum(2.0,3.0)); } else puts(kasm87err);
	kasm87free(cubesum);
	kasm87addext(0,0);
	}

		//Example #18: Passing string pointer (07/18/2006)
	{
	evalextyp myext[] = {{"PRINTST(,$)",printst}};
	kasm87addext(myext,sizeof(myext)/sizeof(myext[0]));
	{
	EVALFUNC printme = (EVALFUNC)kasm87("(d,$st)printst(d+1,st);printst(d-1,\"[It, works! ( : }\");");
	//kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
	if (printme) printme(3,"Hello :)"); else puts(kasm87err);
	kasm87free(printme);
	}
	kasm87addext(0,0);
	}

		//Example #19: Passing multidimensional array (03/01/2007)
	{
	evalextyp myext[] = {{"BUF[3][2]",buf3_2}};
	kasm87addext(myext,sizeof(myext)/sizeof(myext[0]));
	{
	EVALFUNC multidim = (EVALFUNC)kasm87("(x)buf[3]+buf[1][0]+buf[2][1]+x;");
	//kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
	if (multidim) printf("\n\nEx.19: Value is:%g\n     [Should be:28.4]",multidim(3.4)); else puts(kasm87err);
	kasm87free(multidim);
	}
	kasm87addext(0,0);
	}

}

#endif

#if 0
!endif
#endif
