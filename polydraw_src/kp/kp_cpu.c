static unsigned int LSWAPIB (unsigned int a) { return(((a>>8)&0xff00)+((a&0xff00)<<8)+(a<<24)+(a>>24)); }
static unsigned short SSWAPIB (unsigned short a) { return((a>>8)+(a<<8)); }
#define LSWAPIL(a) (a)
#define SSWAPIL(a) (a)
#else
#define LSWAPIB(a) (a)
#define SSWAPIB(a) (a)
static unsigned int LSWAPIL (unsigned int a) { return(((a>>8)&0xff00)+((a&0xff00)<<8)+(a<<24)+(a>>24)); }
static unsigned short SSWAPIL (unsigned short a) { return((a>>8)+(a<<8)); }
#endif

#ifdef __GNUC__
#include <stdint.h>
#define INT_PTR intptr_t
#define UINT_PTR uintptr_t
#endif

#if !defined(_WIN32) && !defined(__DOS__)
#include <unistd.h>
#include <dirent.h>
typedef long long __int64;
static __inline int _lrotl (int i, int sh)
	{ return((i>>(-sh))|(i<<sh)); }
static __inline int filelength (int h)
{
	struct stat st;
	if (fstat(h,&st) < 0) return(-1);
	return(st.st_size);
}
#define _fileno fileno
#else
#include <io.h>
#endif

#if defined(__DOS__)
#include <dos.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif
#if !defined(max)
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif
#if !defined(min)
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif

#if defined(__GNUC__)
#define _inline inline
#endif

	//use GCC-specific extension to force symbol name to be something in particular to override underscoring.
#if defined(__GNUC__) && defined(__i386__) && !defined(NOASM)
#define ASMNAME(x) asm(x)
#else
#define ASMNAME(x)
#endif

static int bytesperline, xres, yres, globxoffs, globyoffs;
static INT_PTR frameplace;

static const int pow2mask[32] =
{
	0x00000000,0x00000001,0x00000003,0x00000007,
	0x0000000f,0x0000001f,0x0000003f,0x0000007f,
	0x000000ff,0x000001ff,0x000003ff,0x000007ff,
	0x00000fff,0x00001fff,0x00003fff,0x00007fff,
	0x0000ffff,0x0001ffff,0x0003ffff,0x0007ffff,
	0x000fffff,0x001fffff,0x003fffff,0x007fffff,
	0x00ffffff,0x01ffffff,0x03ffffff,0x07ffffff,
	0x0fffffff,0x1fffffff,0x3fffffff,0x7fffffff,
};
static const int pow2long[32] =
{
	0x00000001,0x00000002,0x00000004,0x00000008,
	0x00000010,0x00000020,0x00000040,0x00000080,
	0x00000100,0x00000200,0x00000400,0x00000800,
	0x00001000,0x00002000,0x00004000,0x00008000,
	0x00010000,0x00020000,0x00040000,0x00080000,
	0x00100000,0x00200000,0x00400000,0x00800000,
	0x01000000,0x02000000,0x04000000,0x08000000,
	0x10000000,0x20000000,0x40000000,0x80000000,
};

	//Hack for peekbits,getbits,suckbits (to prevent lots of duplicate code)
	//   0: PNG: do 12-byte chunk_header removal hack
	// !=0: ZIP: use 64K buffer (olinbuf)
static int zipfilmode;
typedef struct
{
	FILE *fil;   //0:no file open, !=0:open file (either stand-alone or zip)
	int comptyp; //0:raw data (can be ZIP or stand-alone), 8:PKZIP LZ77 *flate
	int seek0;   //0:stand-alone file, !=0: start of zip compressed stream data
	int compleng;//Global variable for compression FIFO
	int comptell;//Global variable for compression FIFO
	int leng;    //Uncompressed file size (bytes)
	int pos;     //Current uncompressed relative file position (0<=pos<=leng)
	int endpos;  //Temp global variable for kzread
	int jmpplc;  //Store place where decompression paused
	int i;       //For stand-alone/ZIP comptyp#0, this is like "uncomptell"
					  //For ZIP comptyp#8&btype==0 "<64K store", this saves i state
	int bfinal;  //LZ77 decompression state (for later calls)
} kzfilestate;
static kzfilestate kzfs;

//Initialized tables (can't be in union)
//jpg:                png:
//   crmul      16384    abstab10    4096
//   cbmul      16384    hxbit        472
//   dct         4608    pow2mask     128*
//   colclip     4096
//   colclipup8  4096
//   colclipup16 4096
//   unzig        256
//   pow2mask     128*
//   dcflagor      64

int kplib_palcol[256] ASMNAME("kplib_palcol"), kplib_paleng, kplib_bakcol, kplib_numhufblocks, kplib_zlibcompflags;
signed char kplib_coltype, kplib_bitdepth;
char *kplib_filterlist = 0;
int kplib_filterlistmal = 0;

//============================ KPNGILIB begins ===============================

//07/31/2000: KPNG.C first ported to C from READPNG.BAS
//10/11/2000: KPNG.C split into 2 files: KPNG.C and PNGINLIB.C
//11/24/2000: Finished adding support for coltypes 4&6
//03/31/2001: Added support for Adam7-type interlaced images
//Currently, there is no support for:
//   * 16-bit color depth
//   * Some useless ancillary chunks, like: gAMA(gamma) & pHYs(aspect ratio)

	//.PNG specific variables:
static int bakr = 0x80, bakg = 0x80, bakb = 0x80; //this used to be public...
static int gslidew = 0, gslider = 0, xm, xmn[4], xr0, xr1, xplc, yplc;
static INT_PTR nfplace;
static int clen[320], cclen[19], bitpos, filt, xsiz, ysiz;
static int xsizbpl, ixsiz, ixoff, iyoff, ixstp, iystp, intlac, nbpl, trnsrgb ASMNAME("trnsrgb");
static int ccind[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
static int hxbit[59][2], ibuf0[288], nbuf0[32], ibuf1[32], nbuf1[32];
static const unsigned char *filptr;
static unsigned char slidebuf[32768], opixbuf0[4], opixbuf1[4];
static unsigned char pnginited = 0, olinbuf[65536] ASMNAME("olinbuf"); //WARNING:max xres is: 65536/bpp-1
static int gotcmov = -2, abstab10[1024] ASMNAME("abstab10");

	//Variables to speed up dynamic Huffman decoding:
#define LOGQHUFSIZ0 9
#define LOGQHUFSIZ1 6
static int qhufval0[1<<LOGQHUFSIZ0], qhufval1[1<<LOGQHUFSIZ1];
static unsigned char qhufbit0[1<<LOGQHUFSIZ0], qhufbit1[1<<LOGQHUFSIZ1];

#if defined(__WATCOMC__) && !defined(NOASM)

static int bswap (int);
#pragma aux bswap =\
	".586"\
	"bswap eax"\
	parm [eax]\
	modify nomemory exact [eax]\
	value [eax]

static int bitrev (int, int);
#pragma aux bitrev =\
	"xor eax, eax"\
	"beg: shr ebx, 1"\
	"adc eax, eax"\
	"dec ecx"\
	"jnz short beg"\
	parm [ebx][ecx]\
	modify nomemory exact [eax ebx ecx]\
	value [eax]

static int testflag (int);
#pragma aux testflag =\
	"pushfd"\
	"pop eax"\
	"mov ebx, eax"\
	"xor eax, ecx"\
	"push eax"\
	"popfd"\
	"pushfd"\
	"pop eax"\
	"xor eax, ebx"\
	"mov eax, 1"\
	"jne menostinx"\
	"xor eax, eax"\
	"menostinx:"\
	parm nomemory [ecx]\
	modify exact [eax ebx]\
	value [eax]

static void cpuid (int, int *);
#pragma aux cpuid =\
	".586"\
	"cpuid"\
	"mov dword ptr [esi], eax"\
	"mov dword ptr [esi+4], ebx"\
	"mov dword ptr [esi+8], ecx"\
	"mov dword ptr [esi+12], edx"\
	parm [eax][esi]\
	modify exact [eax ebx ecx edx]\
	value

#elif defined(_MSC_VER) && !defined(NOASM)

static _inline unsigned int bswap (unsigned int a)
{
	_asm
	{
		mov eax, a
		bswap eax
	}
}

static _inline int bitrev (int b, int c)
{
	_asm
	{
		mov edx, b
		mov ecx, c
		xor eax, eax
 beg: shr edx, 1
		adc eax, eax
		sub ecx, 1
		jnz short beg
	}
}

static _inline int testflag (int c)
{
	_asm
	{
		mov ecx, c
		pushfd
		pop eax
		mov edx, eax
		xor eax, ecx
		push eax
		popfd
		pushfd
		pop eax
		xor eax, edx
		mov eax, 1
		jne menostinx
		xor eax, eax
		menostinx:
	}
}

static _inline void cpuid (int a, int *s)
{
	_asm
	{
		push ebx
		push esi
		mov eax, a
		cpuid
		mov esi, s
		mov dword ptr [esi+0], eax
		mov dword ptr [esi+4], ebx
		mov dword ptr [esi+8], ecx
		mov dword ptr [esi+12], edx
		pop esi
		pop ebx
	}
}

#elif defined(__GNUC__) && defined(__i386__) && !defined(NOASM)

static inline unsigned int bswap (unsigned int a)
{
	__asm__ __volatile__ ("bswap %0" : "+r" (a) : : "cc" );
	return a;
}

static inline int bitrev (int b, int c)
{
	int a;
	__asm__ __volatile__ (
		"xorl %%eax, %%eax\n\t0:\n\tshrl $1, %%edx\n\tadcl %%eax, %%eax\n\tsubl $1, %%ecx\n\tjnz 0b"
		: "+a" (a), "+d" (b), "+c" (c) : : "cc");
	return a;
}

static inline int testflag (int c)
{
	int a;
	__asm__ __volatile__ (
		"pushf\n\tpopl %%eax\n\tmovl %%eax, %%edx\n\txorl %%ecx, %%eax\n\tpushl %%eax\n\t"
		"popf\n\tpushf\n\tpopl %%eax\n\txorl %%edx, %%eax\n\tmovl $1, %%eax\n\tjne 0f\n\t"
		"xorl %%eax, %%eax\n\t0:"
		: "=a" (a) : "c" (c) : "edx","cc" );
	return a;
}

static inline void cpuid (int a, int *s)
{
	__asm__ __volatile__ (
		"pushl %%ebx\n\t"
		"cpuid\n\tmovl %%eax, (%%esi)\n\tmovl %%ebx, 4(%%esi)\n\t"
		"movl %%ecx, 8(%%esi)\n\tmovl %%edx, 12(%%esi)\n\t"
		"popl %%ebx\n\t"
		: "+a" (a) : "S" (s) : "ecx","edx","memory","cc");
}

#else

#ifdef _WIN64
#define inline
#endif

static inline unsigned int bswap (unsigned int a)
{
	return(((a&0xff0000)>>8) + ((a&0xff00)<<8) + (a<<24) + (a>>24));
}

static inline int bitrev (int b, int c)
{
	int i, j;
	for(i=1,j=0,c=(1<<c);i<c;i+=i) { j += j; if (b&i) j++; }
	return(j);
}

static inline int testflag (int c) { return(0); }

static inline void cpuid (int a, int *s) {}

#endif

	//Bit numbers of return value:
	//0:FPU, 4:RDTSC, 15:CMOV, 22:MMX+, 23:MMX, 25:SSE, 26:SSE2, 30:3DNow!+, 31:3DNow!
static int getcputype ()
{
	int i, cpb[4], cpid[4];
	if (!testflag(0x200000)) return(0);
	cpuid(0,cpid); if (!cpid[0]) return(0);
	cpuid(1,cpb); i = (cpb[3]&~((1<<22)|(1<<30)|(1<<31)));
	cpuid(0x80000000,cpb);
	if (((unsigned int)cpb[0]) > 0x80000000)
	{
		cpuid(0x80000001,cpb);
		i |= (cpb[3]&(1<<31));
		if (!((cpid[1]^0x68747541)|(cpid[3]^0x69746e65)|(cpid[2]^0x444d4163))) //AuthenticAMD
			i |= (cpb[3]&((1<<22)|(1<<30)));
	}
	if (i&(1<<25)) i |= (1<<22); //SSE implies MMX+ support
	return(i);
}