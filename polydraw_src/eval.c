#if 0 // To compile as a stand-alone test program, type "nmake eval.c"
!ifndef COMP
COMP=1
!endif
eval.exe: eval.c; cl eval.c /Ox /G6Fy /Gs /MD /nologo /DEVALTEST /DCOMPILE=$(COMP) /link /nologo kernel32.lib
	del eval.obj
!if 0
#endif

#if 0
==============================================================================
Eval general todo:

	! make params passed as pointers not require declaration&init
	! return start&end index in original text buffer of errors
	* support: func(&buf[var])
	* optimization bug: eval "static j;(){j=1;i=func(j);j=0;i}func(){j}"

	* Add relocation function for user.. or: call [rel0]; pop edx; add edx, ?;
	* Unusual behavior: rounding functions return ~9.2e18 when infinity is input:
		  printf("%f %f %f",ceil(rnd/0),floor(rnd/0),int(rnd/0));
	* ability to define arrays on local stack using 'auto' keyword (for multithread)
	* implement C functions: rand(), abort(), log10(), cosh(), sinh(), tanh()
	* implement switch statement&associated syntax (case, default)
	* type declarations: double, float, (int alias for long), long, short, char
		  Precedence: double, float, __uint64, __int64, ulong, long, ushort, short, uchar, char
	* function domain problems:
		/,%,FMOD slow for 0 but correct
		ACOS/ASIN (x < -1) or (x > 1) slow but correct
		EXP (very high/low numbers slow but correct)
		SQRT/LOG (negative numbers slow but correct)
	* Multithread problems (due to writing globals): RND,NRND
	* USERFUNC save/restore only necessary FPU registers (fix fldz/ffree stuff)
	* Write machine code primitives for PPC ... very low priority :P
	* Functions not ideally implemented on Linux: cpuid/testflag, kpow/krand/fact
	* KASM87C problem with inst6.kc: exp(-1/x) =    0 in KASM87
												exp(-1/x) = -inf in KASM87C

==============================================================================
04/17/2003. Ken Silverman`s x87 expression compiler. This code takes a math
	expression in the form of a string, compiles it to (somewhat) optimized x87
	code, and returns a function pointer so you can call it freely from your C
	code. When no longer needed, call `kasm87_free` to free the function's
	memory. All parameters are passed as double-precision floating point. The
	return value is also double.

Currently supported operators, functions, and statements:

	Parenthesis: (), Arrays: [], Blocks: {}, Const strings: "", Literal '"': \"
			 Assignment: = *= /= %= += -= ++ -- (only 1 allowed per statement)
	1-Param Operators: + - . 0 1 2 3 4 5 6 7 8 9 E PI NRND RND (variable names)
	2-Param Operators: ^ * / % + - < <= > >= == != && ||
	1-Param Functions: ABS ACOS ASIN ATAN ATN CEIL COS EXP FABS FACT FLOOR INT
							 LOG SGN SIN SQRT TAN UNIT
	2-Param Functions: ATAN2 FADD FMOD LOG MIN MAX POW
			 Statements: IF(expr){codetrue}
							 IF(expr){codetrue}ELSE{codefalse}
							 DO{code}WHILE(expr);
							 WHILE(expr){code}
							 FOR(precode;expr;postcode){code}
							 GOTO label;
							 RETURN expr;
							 BREAK;
							 CONTINUE;
							 ENUM{name(=expr),name(=expr),...};
							 STATIC name[expr]["...],name["],...;
							 label:
				Comments: // text (CR), /* text */

Requirements:
	CPU: Pentium or above
	OS: Microsoft Windows 98/ME/2K/XP
	Compiler: Microsoft Visual C/C++ 6.0 or above.

Compiling:
	At the command prompt, type "nmake eval.c". Or if you prefer the VC
	windows environment, select "Win32 Console application" and make sure
	"EVALTEST" is defined in the code. You can either do this in the makefile
	with the /D option or as a #define at the top of the program.

	It is easy to make this an externally callable library. Just copy the
	following function declarations into your code and make sure EVALTEST is
	NOT defined:

	//function:
	//   This is your function. The formatting is similar to C syntax, except
	//   for these differences:
	//
	// * Function name should be left blank
	// * Function parameters support the following types:
	//
	//      kasm87 syntax: Equivalent C syntax:       Description:
	//         a           double a                   pass-by-value variable
	//         &a          double &a                  pointer to double
	//         a[10]       double a[10]               pointer to array of doubles
	//         a()         double (*a)(double)        function pointer, 1 param
	//         a(,)        double (*a)(double,double) function pointer, 2 params...
	//         a(,,)       etc...
	//
	// * Array indices must be constants or enum names.
	// * Function pointers must only have pass-by-value double parameters
	// * Type declarations are not allowed in the function body. Any new
	//      variables are assumed to be `double`
	// * Use int() function to round towards 0.
	// * No {} needed around function body. Code begins after the first ()
	// * The last expression is the return value and it doesn`t need a ;
	// * Switch statement and associated syntax (case, default) not yet supported.
	//
	//   You can pass any number of variables to your function. With this, you
	//   specify the names and the order of your variables which are found
	//   inside mathexpression. Here`s an example:
	//
	//   "(x,y,z)sqrt(x*x+y*y+z*z)"
	//
	//   The (x,y,z) tells kasm87 that the function will have 3 parameters,
	//   with the first parameter called "x", etc...
	//
	//Returns either:
	//   1. Pointer to the newly generated C function (__cdecl format)
	//   2. NULL pointer if there was an error in parsing.
extern void *kasm87 (char *function);

	//Finds index to '(' of 1st function. -1 if simple form (no function blocks)
extern long kasm87_findfirstfuncparen (char *function);

	//If kasm87 returns a NULL pointer, an error string is stored in kasm87err.
extern char kasm87err[256];

	//The number of bytes allocated by the malloc in kasm87; error text markers
extern long kasm87leng, kasm87err0, kasm87err1;

	//mode=0: overwrite backward jumps to 0's
	//mode=1: restore backward jumps
extern void kasm87jumpback (void *, long mode);

	//Free memory of compiled function (does nothing if using kasm87c)
extern void kasm87free (void *);

extern void kasm87freeall ();

	//Returns string of compiled code based on showflags (call after kasm87)
	//Showflags:
	//   1: pseudo-asm
	//   2: machine code bytes
	//  (4: Intel asm)
extern void kasm87_showdebug (long showflags, char *debuf, long debuflng);

	//Specify list of external functions&variables to be recognized by future kasm87() calls.
	//With this, you no longer need to simulate global functions/variables by passing them as pointers.
typedef struct { char *nam; long *ptr; } evalextyp;
extern void kasm87addext (evalextyp *daeet, long n);

		//Note to self: How to relocate kasm87-generated code:
	myfunc2 = (double (*)(double,...))malloc(FUNCBYTEOFFS+kasm87leng);
	memcpy(myfunc2,myfunc,FUNCBYTEOFFS+kasm87leng);
		//kasm87-generated code is fully re-locatable except for this 1 necessary hack:
		//If 1st line is "mov edx, imm32", adjust offset for new code (actually data) offset
	if (((char *)myfunc2)[FUNCBYTEOFFS] == 0xba)
		*(long *)(((long)myfunc2)+FUNCBYTEOFFS+1) += ((long)myfunc2)-((long)myfunc);

	Speed analysis 02/22/2004:
	CHS ABS  1 :)
	+ - *    4 :)
	MIN MAX  7 :)
	SGN UNIT 9 :)
	< etc.. 10 :)
	RND     11 :)
	SQRT    15 :)
	/       18 :)
	&& ||   21 -
	FMOD    48 :(
	%       59 :(
	LOG    111 :(
	SIN    164 :(
	ATAN2  183 :(
	TAN    184 :(
	EXP    192 :(
	ATAN   192 :(
	NRND   203 :(
	COS    209 :(
	FLOOR  216 :(
	CEIL   226 :(
	LOG    232 :(
	ACOS   242 :(
	ASIN   250 :(
	POW ^  342 :(
	FACT   467 -

  ÚÄÄÄÄÄÄÄÄÄÄÄÒÄÄÄÄÂÄÄÄÄÄÂÄÄÄÄÄÄÄÄÄÄÄÄÂÄÄÄÄÂÄÄÄÄÄÄÄÄÄ¿
  ³Round mode:ºQB: ³CLIB:³C TYPECAST: ³FPU:³SSE:     ³
  ÆÍÍÍÍÍÍÍÍÍÍÍÎÍÍÍÍØÍÍÍÍÍØÍÍÍÍÍÍÍÍÍÍÍÍØÍÍÍÍØÍÍÍÍÍÍÍÍÍµ
  ³         0 ºFIX ³     ³(int)       ³    ³cvttss2si³ <- generate array index
  ³ near/even ºCINT³     ³(int):QIFIST³ftol³cvtss2si ³ <- calculation precision
  ³      -inf ºINT ³floor³            ³    ³         ³
  ³      +inf º    ³ceil ³            ³    ³         ³
  ÀÄÄÄÄÄÄÄÄÄÄÄÐÄÄÄÄÁÄÄÄÄÄÁÄÄÄÄÄÄÄÄÄÄÄÄÁÄÄÄÄÁÄÄÄÄÄÄÄÄÄÙ


06/24/2004: Compile time analysis:
					 ÚÄÄÄÄÄÄÄÂÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ¿
					 ³  MCC  ³ P4-2.8 comp/s ³
  ÚÄÄÄÄÄÄÄÄÄÄÄÄÄÅÄÄÄÄÄÄÄÅÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ´
  ³ perspmom.kc ³ 60.90 ³      45.7     ³
  ³ groufst2.kc ³ 24.70 ³     112.7     ³
  ³ poster.kc   ³ 20.50 ³     135.8     ³
  ³ moire.kc    ³ 18.60 ³     149.7     ³
  ³ normarea.kc ³ 10.80 ³     257.8     ³
  ³ goldball.kc ³  9.68 ³     287.6     ³
  ÀÄÄÄÄÄÄÄÄÄÄÄÄÄÁÄÄÄÄÄÄÄÁÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÙ

------------------------------------------------------------------------------
Ken`s official website: http://advsys.net/ken
==============================================================================
#endif

#include <string.h>
#ifdef _MSC_VER
#include <conio.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "eval.h"
//#include <float.h>
//#include "kdisasm.c"

#if !defined(max)
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#if !defined(min)
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

#if _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h> //for VirtualProtect()
#endif

//----------------------------------------- KASM87 BEGINS -----------------------------------------
#define COMPILE 0 // Virtual machine (PowerPC)

enum
{
	PARAM0 = 0,
	NUL = PARAM0,
	GOTO,
	RETURN,
	RND,
	NRND,
	PARAM1,
	NOP = PARAM1,
	MOV,
	NEGMOV,
	NEQU0,
	IF0,
	IF1,
	FABS,
	SGN,
	UNIT,
	FLOOR,
	CEIL,
	ROUND0,
	ROUND0_32,
	SIN,
	COS,
	TAN,
	ASIN,
	ACOS,
	ATAN,
	SQRT,
	EXP,
	FACT,
	LOG,
	PARAM2,
	TIMES = PARAM2,
	SLASH,
	PERC,
	PLUS,
	MINUS,
	LES,
	LESEQ,
	MOR,
	MOREQ,
	EQU,
	NEQU,
	LAND,
	LOR,
	POW,
	MIN,
	MAX,
	FADD,
	FMOD,
	ATAN2,
	LOGB,
	PEEK,
	PARAM3,
	POKE,
	POKETIMES,
	POKESLASH,
	POKEPERC,
	POKEPLUS,
	POKEMINUS,
	USERFUNC,
	PARAMEND
};
static unsigned char oprio[PARAMEND] = {0};
#define KEAX 0x00000000	   // -
#define KECX 0x10000000	   // Local variable (moved to KFST/KESP for compiled)
#define KEDX 0x20000000	   // Constants (doubles/strings/arrays)
#define KEBX 0x30000000	   // -
#define KESP 0x40000000	   // Function parameter
#define KEBP 0x50000000	   // -
#define KESI 0x60000000	   // -
#define KEDI 0x70000000	   // -
#define KEIP 0x80000000	   // Jump location for GOTO/USERFUNC/IF*
#define KFST 0x90000000	   // Floating point stack (lowest 4 local variables)
#define KPTR 0xa0000000	   // Pointer to function parameter (addressed by ESP)
#define KIMM 0xb0000000	   // Immediate address from evalextyp[?].ptr
#define KSTR 0xc0000000	   // String table (moved to end of KEDX for compiled)
#define KARR 0xd0000000	   // Array table (moved to end of KEDX for compiled)
#define KGLB 0xe0000000	   // Global static (behaves similar to KARR&KIMM, but separate list)
#define KUNUSED (KEDX + 1) // Make parameter act like constant (best for optimization) and not match anything

// min/max values for exp: -745.13321910194116528 (-log(2)*(1024+51)) and 709.78271289338396 (log(2)*1024)
#define PI 3.14159265358979323
#ifdef _MSC_VER
__declspec(align(16)) static long kexptval[4] = {0, 0x80000000, 0, 0};
#define LL(l) l##i64
#define PRINTF64 "I64d"
#else
//#define __cdecl __attribute__((cdecl))
#define __cdecl
#define _inline __inline__
#define LL(l) l##ll
#define PRINTF64 "lld"
typedef long long __int64;
#define _snprintf snprintf
#define lnglng(x) x##ll
#define stricmp strcasecmp
#endif

static const long pinf = 0x7f800000, ninf = 0xff800000, pind = 0x7fc00000, nind = 0xffc00000;
static const float posone = 1.f, negone = -1.f, pointfive = .5f, oneover2_31 = 1.f / 2147483648.f;
// static const float threeup51 = 6755399441055744;

//--------------------------------------------------
static long *funcst = 0; // for initial parsing of functions/global sections
static long maxfuncst = 0;

static long gstatmem = 0; // pointer to global static buffer

static evalextyp *gevalext = 0;
static long gevalextnum = 0;

// kasm87 parsing temp variables
static long maxops = 0, arrnum;
static long *gop, *gnext, globi, memnum; // maxops

static double *globval; // maxops
static long gccnt;

static char *gstring; // maxst
static long gstnum, maxst = 0;

typedef struct
{
	long i;
	double v;
} initval_t;
static initval_t *ginitval;
static long ginitvalnum, maxinitval = 0;

static long gecnt, gnumarg = 0, gnumglob = 0;
static double *gvl; // regnum*(recursion depth), stack space used by kasm87c only
static double *gvlp;

static long maxvars = 0, maxvarchars = 0;
static char *newvarnam; // maxvarchars (variable name buffer; strings separated by NULL terminator)
static long newvarhash[256], newvarhash_glob[256];
typedef struct
{
	long r;		 // pointer to register family & offset
	long maxind; // Maximum index for arrays (0 if not an array)
	long parnum; //>=0: # parameters for user functions. <0: not a function; # = 1's complement of # dimensions
	long proti;	 // For funcs/arrays: newvarnam index. FuncProto:{d=double,D=double*}, ArrayDims:{(~parnum)*4}
	long nami;	 // index to start of variable/function`s name string in newvarnam
	long hashn;	 // hash index for variable/function name (for faster string finding & function overloading)
} newvartyp;
static newvartyp *newvar;
static long newvarnum, newvarplc; // maxvars

static char *enumnam;
static long maxenumchars = 0, enumcharplc; // Enum name list (NULL terminator separators)
static double *enumval;
static long maxenum = 0, enumnum; // Enum value list

static long maxlabs = 0, maxlabchars = 0;
static char *newlabnam;								// maxlabchars
static long *newlabind, newlabnum, newlabplc;		// maxlabs
static long *labpat, *jumpat, *lablinum, numlabels; // maxlabs

typedef struct
{
	long addr, val;
} jumpback_t;
static jumpback_t *jumpback = 0;
static long jumpbacknum = 0, maxjumpbacks = 0;

#define MAXPARMS (1 + 2) // Output + #Inputs (for > 2 inputs, use rxi)
typedef struct
{
	long r;	 // register family (EAX,ECX,EDX,ESP,etc...) in highest 4 bits, and offset in lower 28 bits
	long q;	 // additional info (array index, which user function)
	long nv; // newvar index
} rtyp;
typedef struct
{
	long f;			  // function enum index
	long g;			  // additional info for function
	long n;			  // Number of inputs
	rtyp r[MAXPARMS]; // register description
	long rxi;		  // Register eXtra Index
} gasmtyp;
static gasmtyp *gasm; // maxops

static rtyp *rxi;
static long numrxi, maxrxi = 0;

#if 1//(COMPILE != 0)
#define FUNCBYTEOFFS 16	  // Should be multiple of 16 for alignment speed. Pointer to jumpback table.
#define CODEDATADIST 1024 // Number of bytes separate code and data blocks

// if (?.ind >= 0) ?.ptr = gevalext[?.ind].ptr (must look up later for user function pointers)
typedef struct
{
	long *lptr;
	long ind;
} patch_t;
static patch_t *patch = 0;
static long patchnum = 0, maxpatch = 0;
#else
#define FUNCBYTEOFFS 0
#endif

static long round0msk[2048][2];
//--------------------------------------------------
static long cputype = 0, cpuinited = 0;


static _inline long testflag(long c)
{
	return (0);
}
static _inline void cpuid(long a, long *s) { return; }

// Bit numbers of return value:
// 0:FPU, 4:RDTSC, 15:CMOV, 22:MMX+, 23:MMX, 25:SSE, 26:SSE2, 27:SSE3, 30:3DNow!+, 31:3DNow!
static long getcputype()
{
	long i, cpb[4], cpid[4];
	if (!testflag(0x200000))
		return (0);
	cpuid(0, cpid);
	if (!cpid[0])
		return (0);
	cpuid(1, cpb);
	i = (cpb[3] & ~((1 << 22) | (1 << 27) | (1 << 30) | (1 << 31)));
	if (cpb[2] & (1 << 0))
		i |= (1 << 27); // I hijack bit 27 for SSE3 detection
	cpuid(0x80000000, cpb);
	if (((unsigned long)cpb[0]) > 0x80000000)
	{
		cpuid(0x80000001, cpb);
		i |= (cpb[3] & (1 << 31));
		if (!((cpid[1] ^ 0x68747541) | (cpid[3] ^ 0x69746e65) | (cpid[2] ^ 0x444d4163))) // AuthenticAMD
			i |= (cpb[3] & ((1 << 22) | (1 << 30)));
	}
	if (i & (1 << 25))
		i |= (1 << 22); // SSE implies MMX+ support
	return (i);
}

//--------------------------------------------------
static unsigned char *compcode = 0;
long kasm87leng, kasm87err0, kasm87err1, kasm87optimize = 1;
char kasm87err[256] = "";

static long *texttrans = 0, texttransn, texttransmal = 0; // keep track of removed whitespace for adjusting kasm87err0 & kasm87err1

static long kholdrand = 1, snormstat = 0;
void ksrand(long val)
{
	kholdrand = val;
	snormstat = 0;
}

static long krand()
{
	kholdrand = (unsigned long)((kholdrand * (214013 * 2) + 2531011 * 2) >> 1);
	return (kholdrand);
}

static double nrnd()
{
	static double srand2;
	double x, y, r;

	// Box-Muller method (Good & fast)
	if (snormstat)
	{
		snormstat = 0;
		return (srand2);
	}
	do
	{
		x = ((double)(krand() - 1073741824)) * (oneover2_31 * 2.0); //-1 to 1
		y = ((double)(krand() - 1073741824)) * (oneover2_31 * 2.0); //-1 to 1
		r = x * x + y * y;
	} while (r >= 1);
	snormstat = 1;
	r = sqrt(-2.0 * log(r) / r);
	srand2 = x * r;
	return (y * r);
}

static double fact(double num)
{
	if ((num <= -.99999999999999996) || (num >= 170.6243769562767))
		return (*(float *)&pinf);
	num++; // 2^, 14*, 1/, 15+  (Ken optimized out most divides - wasn`t easy!)
	return (pow(num + 5.5, num + 0.5) * exp(-5.5 - num) *
			(((((((num * 2.506628275107298 + 83.8676043423952) * num + 1168.926494792211) * num +
				 8687.245297053594) *
					num +
				36308.29514770109) *
				   num +
			   80916.62789524846) *
				  num +
			  75122.63315304522) /
			 (((((((num + 21) * num + 175) * num + 735) * num + 1624) * num + 1764) * num + 720) * num)));
}

// Ken`s replacement for pow...
static double kpow(double x, double y)
{
	return pow(x, y);
}

static void getfuncnam(gasmtyp *g, char *st)
{
	switch (g->f)
	{
	case NUL:
		strcpy(st, "NUL");
		break;
	case GOTO:
		strcpy(st, "GOTO");
		break;
	case RETURN:
		strcpy(st, "RETURN");
		break;
	case RND:
		strcpy(st, "RND");
		break;
	case NRND:
		strcpy(st, "NRND");
		break;
	case NOP:
		strcpy(st, "NOP");
		break;
	case MOV:
		strcpy(st, "MOV");
		break;
	case NEGMOV:
		strcpy(st, "NEGMOV");
		break;
	case NEQU0:
		strcpy(st, "NEQU0");
		break;
	case IF0:
		strcpy(st, "IF0");
		break;
	case IF1:
		strcpy(st, "IF1");
		break;
	case FABS:
		strcpy(st, "FABS");
		break;
	case SGN:
		strcpy(st, "SGN");
		break;
	case UNIT:
		strcpy(st, "UNIT");
		break;
	case FLOOR:
		strcpy(st, "FLOOR");
		break;
	case ROUND0:
	case ROUND0_32:
		strcpy(st, "ROUND0");
		break;
	case CEIL:
		strcpy(st, "CEIL");
		break;
	case SIN:
		strcpy(st, "SIN");
		break;
	case COS:
		strcpy(st, "COS");
		break;
	case TAN:
		strcpy(st, "TAN");
		break;
	case ASIN:
		strcpy(st, "ASIN");
		break;
	case ACOS:
		strcpy(st, "ACOS");
		break;
	case ATAN:
		strcpy(st, "ATAN");
		break;
	case SQRT:
		strcpy(st, "SQRT");
		break;
	case EXP:
		strcpy(st, "EXP");
		break;
	case FACT:
		strcpy(st, "FACT");
		break;
	case LOG:
		strcpy(st, "LOG");
		break;
	case TIMES:
		strcpy(st, "*");
		break;
	case SLASH:
		strcpy(st, "/");
		break;
	case PERC:
		strcpy(st, "%%");
		break;
	case PLUS:
		strcpy(st, "+");
		break;
	case MINUS:
		strcpy(st, "-");
		break;
	case LES:
		strcpy(st, "<");
		break;
	case LESEQ:
		strcpy(st, "<=");
		break;
	case MOR:
		strcpy(st, ">");
		break;
	case MOREQ:
		strcpy(st, ">=");
		break;
	case EQU:
		strcpy(st, "==");
		break;
	case NEQU:
		strcpy(st, "!=");
		break;
	case LAND:
		strcpy(st, "&&");
		break;
	case LOR:
		strcpy(st, "||");
		break;

	case POW:
		strcpy(st, "POW");
		break;
	case MIN:
		strcpy(st, "MIN");
		break;
	case MAX:
		strcpy(st, "MAX");
		break;
	case FADD:
		strcpy(st, "FADD");
		break;
	case FMOD:
		strcpy(st, "FMOD");
		break;
	case ATAN2:
		strcpy(st, "ATAN2");
		break;
	case LOGB:
		strcpy(st, "LOGB");
		break;
	case PEEK:
		strcpy(st, "PEEK");
		break;

	case POKE:
		strcpy(st, "POKE");
		break;
	case POKETIMES:
		strcpy(st, "POKETIMES");
		break;
	case POKESLASH:
		strcpy(st, "POKESLASH");
		break;
	case POKEPERC:
		strcpy(st, "POKEPERC");
		break;
	case POKEPLUS:
		strcpy(st, "POKEPLUS");
		break;
	case POKEMINUS:
		strcpy(st, "POKEMINUS");
		break;
	case USERFUNC:
		if (g->g >= 0)
			strcpy(st, &newvarnam[newvar[g->g].nami]);
		else
			strcpy(st, "USERFUNC");
		break;

	default:
		st[0] = 0;
		break;
	}
}

static void getvarnam(rtyp reg, char *st)
{
	long l, m;

	if (reg.r == KUNUSED)
	{
		strcpy(st, "?");
		return;
	}
	switch (((unsigned long)reg.r) >> 28)
	{
	case (KECX >> 28):
		sprintf(st, "m%d", (reg.r & 0x0fffffff) >> 3);
		break;
	case (KSTR >> 28):
		reg.r = (reg.r & 0x0fffffff) + gccnt * 8 + KEDX;
		goto getvarnam_casekedx;
	case (KARR >> 28):
		reg.r = (reg.r & 0x0fffffff) + gccnt * 8 + gstnum + KEDX;
		goto getvarnam_casekedx;
	case (KEDX >> 28):
	getvarnam_casekedx:
		l = (reg.r & 0x0fffffff);
		if (l < (gccnt << 3))
		{
			sprintf(st, "%g", globval[l >> 3]);
		}
		else if (l < (gccnt << 3) + gstnum)
		{
			sprintf(st, "\"%s\"", &gstring[l - (gccnt << 3)]);
		}
		else
		{
			for (m = newvarnum - 1; m >= 0; m--)
				if (newvar[m].r == l - (gccnt << 3) - gstnum + (signed)KARR)
				{
					strcpy(st, &newvarnam[newvar[m].nami]);
					sprintf(&st[strlen(st)], "[%d]", reg.q);
					break;
				}
			if (m < 0)
				sprintf(st, "??0x%08x??", reg.r);
		}
		break;
	case (KESP >> 28):
	case (KPTR >> 28):
		if (((reg.r & 0x0fffffff) >> 3) < memnum)
		{
			sprintf(st, "m%d", (reg.r & 0x0fffffff) >> 3);
			break;
		} // No break intentional
	case (KIMM >> 28):
	case (KGLB >> 28):
		for (m = 0; m < gnumglob; m++)
			if (newvar[m].r == reg.r)
			{
				strcpy(st, &newvarnam[newvar[m].nami]);
				if ((newvar[m].maxind) || ((((unsigned long)reg.r) >> 28) == (KPTR >> 28)))
					sprintf(&st[strlen(st)], "[%d]", reg.q);
				break;
			}
		if (m >= gnumglob)
			sprintf(st, "??0x%08x??", reg.r);
		break;
	case (KEIP >> 28):
		sprintf(st, "l%d", reg.r & 0x0fffffff);
		break;
	case (KFST >> 28):
		sprintf(st, "r%d", (reg.r & 0x0fffffff) >> 3);
		break;
	default:
		sprintf(st, "?!0x%08x!?", reg.r);
		break;
	}
}

// showflags=1: pseudoasm
// showflags=2: machine code bytes
void kasm87_showdebug(long showflags, char *debuf, long debuflng)
{
	rtyp *rp;
	long i, j, k, didlab = 0;
	char st[4][64], st2[260], *cptr, *cptr2, *cp0, *cp1;

	if (debuflng <= 0)
		return;
	debuf[0] = 0;
	cp0 = debuf;
	cp1 = &debuf[debuflng - 1];
	cp1[0] = 0;
	if (showflags & 1)
	{
		for (i = 0; i < gecnt; i++)
		{
			for (j = 2; j >= 0; j--)
				getvarnam(gasm[i].r[j], st[j]);
			if (gasm[i].n >= 3)
				getvarnam(rxi[gasm[i].rxi], st[3]);
			else
				st[3][0] = 0;

			if (gasm[i].f == NUL)
			{
				k = _snprintf(cp0, cp1 - cp0, "%s:", st[0]);
				if (k >= 0)
					cp0 += k;
				didlab = 1;
				continue;
			}
			if (!didlab)
			{
				k = _snprintf(cp0, cp1 - cp0, "   ");
				if (k >= 0)
					cp0 += k;
			}
			else
				didlab = 0;
			if (gasm[i].f == IF0)
			{
				k = _snprintf(cp0, cp1 - cp0, "IF !(%s) GOTO %s\n", st[2], st[1]);
				if (k >= 0)
					cp0 += k;
				continue;
			}
			if (gasm[i].f == IF1)
			{
				k = _snprintf(cp0, cp1 - cp0, "IF (%s) GOTO %s\n", st[2], st[1]);
				if (k >= 0)
					cp0 += k;
				continue;
			}
			if (gasm[i].f == GOTO)
			{
				k = _snprintf(cp0, cp1 - cp0, "GOTO %s\n", st[1]);
				if (k >= 0)
					cp0 += k;
				continue;
			}
			if (gasm[i].f == RETURN)
			{
				k = _snprintf(cp0, cp1 - cp0, "RETURN %s\n", st[1]);
				if (k >= 0)
					cp0 += k;
				continue;
			}
			if (gasm[i].f == USERFUNC)
			{
				getfuncnam(&gasm[i], st2);
				k = _snprintf(cp0, cp1 - cp0, "%s = %s(", st[0], st2);
				if (k >= 0)
					cp0 += k;
				for (j = 0; j < gasm[i].n; j++)
				{
					if (j)
					{
						k = _snprintf(cp0, cp1 - cp0, ",");
						if (k >= 0)
							cp0 += k;
					}
					if (newvarnam[newvar[gasm[i].g].proti + j] == 'D')
					{
						k = _snprintf(cp0, cp1 - cp0, "&");
						if (k >= 0)
							cp0 += k;
					}
					if (newvarnam[newvar[gasm[i].g].proti + j] == 'C')
					{
						k = _snprintf(cp0, cp1 - cp0, "$");
						if (k >= 0)
							cp0 += k;
					}
					if (j < 2)
						rp = &gasm[i].r[j + 1];
					else
						rp = &rxi[gasm[i].rxi + j - 2];
					getvarnam(*rp, st[2]);
					k = _snprintf(cp0, cp1 - cp0, "%s", st[2]);
					if (k >= 0)
						cp0 += k;
				}
				k = _snprintf(cp0, cp1 - cp0, ")\n");
				if (k >= 0)
					cp0 += k;
				continue;
			}
			switch (gasm[i].f)
			{
			case RND:
			case NRND:
				getfuncnam(&gasm[i], st2);
				strcat(st2, "()");
				break;

			case MOV:
				strcpy(st2, "%s");
				break;
			case NEGMOV:
				strcpy(st2, "-%s");
				break;
			case NEQU0:
				strcpy(st2, "%s != 0");
				break;

			case FABS:
			case SGN:
			case UNIT:
			case FLOOR:
			case CEIL:
			case ROUND0:
			case ROUND0_32:
			case SIN:
			case COS:
			case TAN:
			case ASIN:
			case ACOS:
			case ATAN:
			case SQRT:
			case EXP:
			case FACT:
			case LOG:
				getfuncnam(&gasm[i], st2);
				strcat(st2, "(%s)");
				break;

			case TIMES:
			case SLASH:
			case PERC:
			case PLUS:
			case MINUS:
			case LES:
			case LESEQ:
			case MOR:
			case MOREQ:
			case EQU:
			case NEQU:
			case LAND:
			case LOR:
				strcpy(st2, "%s ");
				getfuncnam(&gasm[i], &st2[strlen(st2)]);
				strcat(st2, " %s");
				break;

			case POW:
			case MIN:
			case MAX:
			case FADD:
			case FMOD:
			case ATAN2:
			case LOGB:
			case PEEK:
				getfuncnam(&gasm[i], st2);
				strcat(st2, "(%s,%s)");
				break;

			case POKE:
			case POKETIMES:
			case POKESLASH:
			case POKEPERC:
			case POKEPLUS:
			case POKEMINUS:
				getfuncnam(&gasm[i], st2);
				strcat(st2, "(%s,%s,%s)");
				break;
			}
			k = _snprintf(cp0, cp1 - cp0, "%s = ", st[0]);
			if (k >= 0)
				cp0 += k;
			k = _snprintf(cp0, cp1 - cp0, st2, st[1], st[2], st[3]);
			if (k >= 0)
				cp0 += k;
			k = _snprintf(cp0, cp1 - cp0, "\n");
			if (k >= 0)
				cp0 += k;
		}
		if (didlab)
		{
			k = _snprintf(cp0, cp1 - cp0, "\n");
			if (k >= 0)
				cp0 += k;
		} // last label has no code
	}
	if ((showflags & 2) && (compcode))
	{
		for (i = 0; i < kasm87leng; i++)
		{
			if (!(i & 15))
			{
				if (i)
				{
					k = _snprintf(cp0, cp1 - cp0, "\n", i);
					if (k >= 0)
						cp0 += k;

					for (j = i + 1; j < kasm87leng; j++)
						if (compcode[i] != compcode[j])
							break;
					if (j - i >= 16)
					{
						j = ((j - i) & ~15);
						k = _snprintf(cp0, cp1 - cp0, "%06x  %02x * 0x%x\n", i, compcode[i], j);
						if (k >= 0)
							cp0 += k;
						if (j > 16)
						{
							k = _snprintf(cp0, cp1 - cp0, "..\n");
							if (k >= 0)
								cp0 += k;
						}
						i += j;
						if (i >= kasm87leng)
							break;
					}
				}
				k = _snprintf(cp0, cp1 - cp0, "%06x  ", i);
				if (k >= 0)
					cp0 += k;
			}
			k = _snprintf(cp0, cp1 - cp0, "%02x ", compcode[i]);
			if (k >= 0)
				cp0 += k;
		}
		k = _snprintf(cp0, cp1 - cp0, " (%d bytes)\n", kasm87leng);
		if (k >= 0)
			cp0 += k;
	}
	// else { k = _snprintf(cp0,cp1-cp0,"\n"); if (k >= 0) cp0 += k; }
	// if ((showflags&4) && (compcode)) { i = 0; cp0 = kdisasm((char *)compcode,kasm87leng,cp0,cp1-cp0,&i); }
}

static long isvarchar(unsigned char ch)
{
	static const long isvarcharbuf[8] = {0, 0x03ff0000, 0x87fffffe, 0x07fffffe, 0, 0, 0, 0};
#if defined(_M_IX86) || defined(__i386__)
	return (isvarcharbuf[ch >> 5] & (1 << ch)); // WARNING: Shift auto-modulo`d by 32 trick only works on Intel CPUs!
#elif 1
	return (isvarcharbuf[ch >> 5] & (1 << (ch & 31)));
#endif
	// return(((ch >= '0') && (ch <= '9')) || ((ch >= 'A') && (ch <= 'Z')) || (ch == '_') || ((ch >= 'a') && (ch <= 'z')));
}

static long getnewvarhash(const char *st)
{
	long i, hashind;
	char ch;

	for (i = 0, hashind = 0; st[i]; i++)
	{
		ch = st[i];
		if ((ch >= 'a') && (ch <= 'z'))
			ch -= 32;
		hashind = ch - hashind * 3;
	}
	return (hashind & (sizeof(newvarhash) / sizeof(newvarhash[0]) - 1));
}

static void checkfuncst(long i) // note: i is per long, not function
{
	if (i < maxfuncst)
		return;
	if (!i)
	{
		maxfuncst = 256;
	}
	else
	{
		while (i >= maxfuncst)
			maxfuncst += (maxfuncst >> 2);
	}
	// printf("maxfuncst=%d\n",maxfuncst);
	if (!(funcst = (long *)realloc(funcst, sizeof(funcst[0]) * maxfuncst)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
}

static void checkops(long i)
{
	if (i < maxops)
		return;
	if (!i)
	{
		maxops = 1024;
	}
	else
	{
		while (i >= maxops)
			maxops += (maxops >> 2);
	}
	// printf("maxops=%d\n",maxops);
	if (!(gop = (long *)realloc(gop, sizeof(long) * maxops)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
	if (!(gnext = (long *)realloc(gnext, sizeof(long) * maxops)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
	if (!(globval = (double *)realloc(globval, sizeof(double) * maxops)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
	if (!(gasm = (gasmtyp *)realloc(gasm, sizeof(gasmtyp) * maxops)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
}

static void checkstrings(long i)
{
	if (i < maxst)
		return;
	if (!i)
	{
		maxst = 1024;
	}
	else
	{
		while (i >= maxst)
			maxst += (maxst >> 2);
	}
	// printf("maxst=%d\n",maxst);
	if (!(gstring = (char *)realloc(gstring, sizeof(char) * maxst)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
}

static void checkinitvals(long i)
{
	if (i < maxinitval)
		return;
	if (!i)
	{
		maxinitval = 256;
	}
	else
	{
		while (i >= maxinitval)
			maxinitval += (maxinitval >> 2);
	}
	// printf("maxinitval=%d\n",maxinitval);
	if (!(ginitval = (initval_t *)realloc(ginitval, sizeof(initval_t) * maxinitval)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
}

static void checkrxi(long i)
{
	if (i < maxrxi)
		return;
	if (!i)
	{
		maxrxi = 256;
	}
	else
	{
		while (i >= maxrxi)
			maxrxi += (maxrxi >> 2);
	}
	// printf("maxrxi=%d\n",maxrxi);
	if (!(rxi = (rtyp *)realloc(rxi, sizeof(rtyp) * maxrxi)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
}

static void checkvarchars(long i)
{
	if (i < maxvarchars)
		return;
	if (!i)
	{
		maxvarchars = 1024;
	}
	else
	{
		while (i >= maxvarchars)
			maxvarchars += (maxvarchars >> 2);
	}
	// printf("maxvarchars=%d\n",maxvarchars);
	if (!(newvarnam = (char *)realloc(newvarnam, sizeof(char) * maxvarchars)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
}

static void checkvars(long i)
{
	if (i < maxvars)
		return;
	if (!i)
	{
		maxvars = 256;
	}
	else
	{
		while (i >= maxvars)
			maxvars += (maxvars >> 2);
	}
	// printf("maxvars=%d\n",maxvars);
	if (!(newvar = (newvartyp *)realloc(newvar, sizeof(newvartyp) * maxvars)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
}

static void checkenumchars(long i)
{
	if (i < maxenumchars)
		return;
	if (!i)
	{
		maxenumchars = 1024;
	}
	else
	{
		while (i >= maxenumchars)
			maxenumchars += (maxenumchars >> 2);
	}
	// printf("maxenumchars=%d\n",maxenumchars);
	if (!(enumnam = (char *)realloc(enumnam, sizeof(char) * maxenumchars)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
}

static void checkenum(long i)
{
	if (i < maxenum)
		return;
	if (!i)
	{
		maxenum = 256;
	}
	else
	{
		while (i >= maxenum)
			maxenum += (maxenum >> 2);
	}
	// printf("maxenum=%d\n",maxenum);
	if (!(enumval = (double *)realloc(enumval, sizeof(double) * maxenum)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
}

static void checklabchars(long i)
{
	if (i < maxlabchars)
		return;
	if (!i)
	{
		maxlabchars = 1024;
	}
	else
	{
		while (i >= maxlabchars)
			maxlabchars += (maxlabchars >> 2);
	}
	// printf("maxlabchars=%d\n",maxlabchars);
	if (!(newlabnam = (char *)realloc(newlabnam, sizeof(char) * maxlabchars)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
}

static void checklabs(long i)
{
	if (i < maxlabs)
		return;
	if (!i)
	{
		maxlabs = 256;
	}
	else
	{
		while (i >= maxlabs)
			maxlabs += (maxlabs >> 2);
	}
	// printf("maxlabs=%d\n",maxlabs);
	if (!(newlabind = (long *)realloc(newlabind, sizeof(long) * maxlabs)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
	if (!(labpat = (long *)realloc(labpat, sizeof(long) * maxlabs)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
	if (!(jumpat = (long *)realloc(jumpat, sizeof(long) * maxlabs)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
	if (!(lablinum = (long *)realloc(lablinum, sizeof(long) * maxlabs)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
}

static void checkjumpbacks(long i)
{
	if (i < maxjumpbacks)
		return;
	if (!i)
	{
		maxjumpbacks = 256;
	}
	else
	{
		while (i >= maxjumpbacks)
			maxjumpbacks += (maxjumpbacks >> 2);
	}
	// printf("maxjumpbacks=%d\n",maxjumpbacks);
	if (!(jumpback = (jumpback_t *)realloc(jumpback, sizeof(jumpback_t) * maxjumpbacks)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
}

#if (COMPILE != 0)
static void checkpatch(long i)
{
	if (i < maxpatch)
		return;
	if (!i)
	{
		maxpatch = 256;
	}
	else
	{
		while (i >= maxpatch)
			maxpatch += (maxpatch >> 2);
	}
	// printf("maxpatch=%d\n",maxpatch);
	if (!(patch = (patch_t *)realloc(patch, sizeof(patch_t) * maxpatch)))
	{
		globi = -1;
		strcpy(kasm87err, "ERROR: malloc failed");
		return;
	}
}
#endif

// Helper function to compare 2 parameters on gasm
static long gasmeq(rtyp g0, rtyp g1)
{
	return ((g0.r == g1.r) && (g0.q == g1.q));
}

static long skipparen(char *st, long z)
{
	long p = 0, inquotes = 0;
	for (; 1; z++)
	{
		if (!st[z])
			return (-1);
		if ((st[z] == '\"') && ((!z) || (st[z - 1] != '\\')))
			inquotes ^= 1;
		if (inquotes)
			continue;
		if (st[z] == '(')
		{
			p++;
			continue;
		}
		if (st[z] == ')')
		{
			p--;
			if (!p)
				break;
		}
	}
	return (z + 1); // Skip ')'
}

// findscope(): parses dangling else`s and other weird syntax missing {} properly
//  st: string pointer base
//   z: string start index
//  z0: index to scope start
//  z1: index to scope end (where to write NULL terminator)
// returns:index to 1st char of next statement (skips `}` when applicable)
static long findscope(char *st, long z, long *z0, long *z1)
{
	long p, ifcnt, zx0, zx1;

	if (st[z] == '{')
	{
		z++;
		(*z0) = z;
		p = 1;
		for (; 1; z++)
		{
			if (!st[z])
				return (-1);
			if (st[z] == '{')
			{
				p++;
				continue;
			}
			if (st[z] == '}')
			{
				p--;
				if (!p)
					break;
			}
		}
		(*z1) = z;
		z++; // Skip '}'
	}
	else
	{
		(*z0) = z;
		if ((!strncmp(&st[z], "IF", 2)) && (!isvarchar(st[z + 2])))
		{
			if ((z = skipparen(st, z + 2)) < 0)
				return (-1);
			if ((z = findscope(st, z, &zx0, &zx1)) < 0)
				return (-1);
			if ((!strncmp(&st[z], "ELSE", 4)) && (!isvarchar(st[z + 4])))
			{
				z += 4;
				if (st[z] == ' ')
					z++;
				if ((z = findscope(st, z, &zx0, &zx1)) < 0)
					return (-1);
			}
		}
		else if ((!strncmp(&st[z], "DO", 2)) && (!isvarchar(st[z + 2])))
		{
			if ((z = findscope(st, z + 2, &zx0, &zx1)) < 0)
				return (-1);
			if ((!strncmp(&st[z], "WHILE", 5)) && (!isvarchar(st[z + 5])))
			{
				if ((z = skipparen(st, z + 5)) < 0)
					return (-1);
				if (st[z] == ';')
					z++;
			}
			else
				return (-1);
		}
		else if ((!strncmp(&st[z], "WHILE", 5)) && (!isvarchar(st[z + 5])))
		{
			if ((z = skipparen(st, z + 5)) < 0)
				return (-1);
			if ((z = findscope(st, z, &zx0, &zx1)) < 0)
				return (-1);
		}
		else if ((!strncmp(&st[z], "FOR", 3)) && (!isvarchar(st[z + 3])))
		{
			if ((z = skipparen(st, z + 3)) < 0)
				return (-1);
			if ((z = findscope(st, z, &zx0, &zx1)) < 0)
				return (-1);
		}
		else
		{
			for (; 1; z++)
			{
				if (!st[z])
					return (-1);
				if (st[z] == ';')
				{
					z++;
					break;
				}
			}
		}
		(*z1) = z;
	}
	return (z);
}

void kasm87addext(evalextyp *daeet, long n)
{
	gevalext = daeet;
	gevalextnum = n;
}

// NOTE: newvar must be written and 0-terminated first before calling this!
// Writes each dimension to newvar[newvarnum]; returns total array size (error=0), and new z in daz
static void parsefunc(char *, long, long);
static long kasmoptimizations(long, long);
static long parse_dimensions(char *st, long *daz, long writenewvar)
{
	long i, j, k, p, z, arrind = 1;
	char *cptr;

	z = j = (*daz);
	if (st[j] == ' ')
		j++;
	while (st[j] == '[')
	{
		j++;
		for (z = j; st[z] != ']'; z++)
			if (!st[z])
			{
				strcpy(kasm87err, "ERROR: missing ]");
				return (0);
			}

#if 0
		i = -1; //First see if array index is an "enum" style name...
		for(k=0,cptr=enumnam;k<enumnum;k++,cptr=&cptr[p+1])
		{
			for(p=1;cptr[p];p++);
			if ((z-j == p) && (!strncmp(&st[j],cptr,p)))
				{ i = (long)enumval[k]; break; }
		}
		if (i < 0) { i = strtol(&st[j],(char **)&z,10); z -= (long)st; } //If no, get number
#else
		// This version handles expressions.. left old case for nostalgia?
		{
			long oglobi, ogecnt;
			char *nst;
			oglobi = globi;
			ogecnt = gecnt;

			nst = (char *)malloc(z - j + 1);
			if (!nst)
			{
				strcpy(kasm87err, "ERROR: malloc failed");
				return (0);
			}
			memcpy(nst, &st[j], z - j);
			nst[z - j] = 0;
			parsefunc(nst, -1, -1);
			free(nst);
			if (globi == -1)
				return (0);

			kasmoptimizations(ogecnt, 1);

			if ((gecnt - ogecnt != 1) || (gasm[ogecnt].f != MOV) || ((gasm[ogecnt].r[1].r & 0xf0000000) != KEDX))
			{
				st[z] = 0;
				sprintf(kasm87err, "ERROR: could not simplify dimension (%s) to constant", &st[j]);
				st[z] = ']';
				return (0);
			}
			i = ((long)ceil(globval[(gasm[ogecnt].r[1].r & 0x0fffffff) >> 3]));

			globi = oglobi;
			gecnt = ogecnt;
			gnext[oglobi] = 0x7fffffff;
		}
#endif
		if (i <= 0)
		{
			strcpy(kasm87err, "ERROR: invalid dimensions");
			return (0);
		}
		z++; // skip ']'

		if (writenewvar)
		{
			checkvarchars(newvarplc + 4);
			*(long *)&newvarnam[newvarplc] = i;
			newvarplc += 4; // i is size of array dimension
			newvar[newvarnum].parnum++;
		}

		arrind *= i;

		if (st[z] == ' ')
			z++;
		j = z;
	}
	(*daz) = z;
	return (arrind);
}

static long parse_enum(char *st, long z)
{
	double d;
	long k, p;

	z += 4;
	if (st[z] == ' ')
		z++;
	if (st[z] == '{')
	{
		d = 0.0;
		k = 0;
		z++;
		p = 1;
		for (; st[z]; z++)
		{
			if (st[z] == ' ')
				z++;
			if (st[z] == '=')
			{
				z++;
#if 0
				d = strtod(&st[z],(char **)&z); z -= (long)st;
#else
				// This version handles expressions.. left old case for nostalgia?
				{
					long oglobi, ogecnt, np;
					char *nst;
					oglobi = globi;
					ogecnt = gecnt;

					k = z;
					np = 0;
					do
					{
						if (!st[z])
						{
							strcpy(kasm87err, "ERROR: static init missing , or ;");
							return (-1);
						}
						if (st[z] == '{')
						{
							np++;
						}
						if (st[z] == '}')
						{
							np--;
							if (np < 0)
								break;
						}
						if (((st[z] == ';') || (st[z] == ',')) && (np <= 0))
							break;
						z++;
					} while (1);

					nst = (char *)malloc(z - k + 1);
					if (!nst)
					{
						strcpy(kasm87err, "ERROR: malloc failed");
						return (-1);
					}
					memcpy(nst, &st[k], z - k);
					nst[z - k] = 0;
					parsefunc(nst, -1, -1);
					free(nst);
					if (globi == -1)
						return (-1);

					kasmoptimizations(ogecnt, 1);

					if ((gecnt - ogecnt != 1) || (gasm[ogecnt].f != MOV) || ((gasm[ogecnt].r[1].r & 0xf0000000) != KEDX))
					{
						st[z] = 0;
						sprintf(kasm87err, "ERROR: could not simplify enum init (%s) to constant", &st[k]);
						st[z] = ']';
						return (-1);
					}
					d = globval[(gasm[ogecnt].r[1].r & 0x0fffffff) >> 3];

					globi = oglobi;
					gecnt = ogecnt;
					gnext[oglobi] = 0x7fffffff;
				}
#endif
				k = 1;
			}
			if (st[z] == ' ')
				z++;
			if ((st[z] == ',') || (st[z] == '}'))
			{
				checkenumchars(enumcharplc + 1);
				enumnam[enumcharplc++] = 0;
				checkenum(enumnum + 1);
				enumval[enumnum++] = d;
				d++;
				k = 0;
			}
			else
			{
				if (k)
				{
					strcpy(kasm87err, "ERROR: ENUM syntax incorrect");
					globi = -1;
					return (-1);
				}
				checkenumchars(enumcharplc + 1);
				enumnam[enumcharplc++] = st[z];
			}
			if (st[z] == '{')
			{
				p++;
				continue;
			}
			if (st[z] == '}')
			{
				p--;
				if (!p)
					break;
			}
		}
		if (st[z] != '}')
		{
			strcpy(kasm87err, "ERROR: missing }");
			globi = -1;
			return (-1);
		}
		z++; // Skip '}'
	}

	return (z);
}

// writes: newvar,newvarhash,newvarnam,newvarnum,newvarplc
// writes: ginitval,ginitvalnum
static long parse_static(char *st, long z)
{
	long i, j, k, p, arrind;
	char ch, *cptr;

	z += 7;
	do
	{
		if (st[z] == ' ')
			z++;
		if ((st[z] >= '0') && (st[z] <= '9'))
		{
			strcpy(kasm87err, "ERROR: bad static init syntax");
			globi = -1;
			return (-1);
		}

		cptr = &st[z];
		for (j = z; isvarchar(st[j]); j++)
			;
		if (j == z)
		{
			strcpy(kasm87err, "ERROR: static missing variable");
			globi = -1;
			return (-1);
		}

		ch = st[j];
		st[j] = 0;
		for (i = newvarhash[getnewvarhash(cptr)]; i >= 0; i = newvar[i].hashn)
			if (!strcmp(cptr, &newvarnam[newvar[i].nami]))
			{
				sprintf(kasm87err, "ERROR: %s already defined", cptr);
				st[j] = ch;
				globi = -1;
				return (-1);
			}
		st[j] = ch;

		checkvarchars(newvarplc + j - z + 2);
		checkvars(newvarnum + 1);
		newvar[newvarnum].nami = newvarplc;
		// Save new variable name&index to list
		// if (st[z] == '\"') { memcpy(&newvarnam[newvarplc],&st[z+1],j-z-2); newvarplc += j-z-2; }
		//               else
		{
			memcpy(&newvarnam[newvarplc], &st[z], j - z);
			newvarplc += j - z;
		}
		newvarnam[newvarplc++] = 0;

		newvar[newvarnum].proti = newvarplc;
		newvar[newvarnum].parnum = 0;

		z = j;
		arrind = parse_dimensions(st, &z, 1);
		if (!arrind)
		{
			globi = -1;
			return (-1);
		}

		if (st[z] == ' ')
			z++;
		if (st[z] == '=') // Parse static initializers
		{
			z++;
			j = 0;
			p = 0;
			do
			{
				while ((st[z] == '{') || (st[z] == ' '))
				{
					p += (st[z] == '{');
					z++;
				}

				checkinitvals(ginitvalnum + 1);
				ginitval[ginitvalnum].i = arrnum + j;

				k = z;
#if 0
				if ((st[z] == '0') && (st[z+1] == 'X')) //Handle HEX numbers
				{
					ginitval[ginitvalnum].v = strtoul(&st[z],(char **)&z,0);
					if (ginitval[ginitvalnum].v >= 2147483648.0) ginitval[ginitvalnum].v -= 4294967296.0;
				} else { ginitval[ginitvalnum].v = strtod(&st[z],(char **)&z); }
				z -= (long)st;
#else
				// This version handles expressions.. left old case for nostalgia?
				{
					long oglobi, ogecnt, np;
					char *nst;
					oglobi = globi;
					ogecnt = gecnt;

					np = 0;
					do
					{
						if (!st[z])
						{
							strcpy(kasm87err, "ERROR: static init missing , or ;");
							return (-1);
						}
						if (st[z] == '{')
						{
							np++;
						}
						if (st[z] == '}')
						{
							np--;
							if (np < 0)
								break;
						}
						if (((st[z] == ';') || (st[z] == ',')) && (np <= 0))
							break;
						z++;
					} while (1);

					nst = (char *)malloc(z - k + 1);
					if (!nst)
					{
						strcpy(kasm87err, "ERROR: malloc failed");
						return (-1);
					}
					memcpy(nst, &st[k], z - k);
					nst[z - k] = 0;
					parsefunc(nst, -1, -1);
					free(nst);
					if (globi == -1)
						return (-1);

					kasmoptimizations(ogecnt, 1);

					if ((gecnt - ogecnt != 1) || (gasm[ogecnt].f != MOV) || ((gasm[ogecnt].r[1].r & 0xf0000000) != KEDX))
					{
						st[z] = 0;
						sprintf(kasm87err, "ERROR: could not simplify static init (%s) to constant", &st[k]);
						st[z] = ']';
						return (-1);
					}
					ginitval[ginitvalnum].v = globval[(gasm[ogecnt].r[1].r & 0x0fffffff) >> 3];

					globi = oglobi;
					gecnt = ogecnt;
					gnext[oglobi] = 0x7fffffff;
				}
#endif
				if ((j >= (arrind << 3)) && (z > k))
				{
					sprintf(kasm87err, "ERROR: too many initializers for %s", &newvarnam[newvar[newvarnum].nami]);
					globi = -1;
					return (-1);
				}
				if (ginitval[ginitvalnum].v != 0.0)
					ginitvalnum++;
				j += 8;

				while ((st[z] == '}') || (st[z] == ' '))
				{
					p -= (st[z] == '}');
					z++;
					if (!p)
						break;
				}
				if (!p)
					break;

				if (st[z] != ',')
				{
					strcpy(kasm87err, "ERROR: bad static init syntax");
					globi = -1;
					return (-1);
				}
				z++;
			} while (1);
		}

		newvar[newvarnum].r = arrnum + KARR;
		arrnum += (arrind << 3);
		newvar[newvarnum].maxind = arrind;
		newvar[newvarnum].parnum ^= -1; // 1's complement
		k = getnewvarhash(&newvarnam[newvar[newvarnum].nami]);
		newvar[newvarnum].hashn = newvarhash[k];
		newvarhash[k] = newvarnum;
		newvarnum++;

		if (st[z] == ' ')
			z++;
		if ((st[z] != ';') && (st[z] != ','))
		{
			strcpy(kasm87err, "ERROR: bad static init syntax");
			globi = -1;
			return (-1);
		}
		z++; // skip ';' or ','
	} while (st[z - 1] == ',');
	return (z);
}

static void parsefunc(char *st, long breaklab, long contlab)
{
	rtyp *rp;
	gasmtyp tg;
	long i, j, k, z, oz, p, fmode, newvari, fparm, ogi, parm, negit, arrind, inquotes, isaddr;
	char ch, ch2, *cptr;

	// printf("|%s|\n",st); //Enable to debug expression splitting

	if (!st[0])
		st = "0";
	z = 0;
	fmode = NOP;
	newvari = -1;
	isaddr = 0;
prebegit:;
	ogi = globi;
	checkops(ogi + 1);
	gnext[ogi] = 0x7fffffff; // write ending val to gnext for null expressions (Ex:GOTO)
begit:;
	if (!z)
	{
		if ((st[z] == '-') || (st[z] == '+')) // Hack for first '-' or '+' to fix priority of "-x^2"
		{
			// Insert '0' before '-' or '+' if sign is 1st char in expression
			checkops(gecnt + 1);
			memset(&gasm[gecnt], 0, sizeof(gasmtyp));
			gasm[gecnt].r[0].r = globi * 8 + KECX;
			gasm[gecnt].r[1].r = gccnt * 8 + KEDX;
			gasm[gecnt].r[2].r = KUNUSED;
			gasm[gecnt].n = 1;
			gasm[gecnt].f = MOV;
			gecnt++;
			checkops(gccnt + 1);
			globval[gccnt++] = 0.0;
			checkops(globi + 2);
			gnext[globi] = globi + 1;
			globi++;
			gop[globi] = NUL;
		}
		negit = 1;
	}
	else
	{
		// Hack to make "2^(+3)" not translate as: "2+3"
		// handle unary '-' operator. 0:nothing special, 1:negate next node!
		for (negit = 1; (st[z] == '+') || (st[z] == '-'); z++)
			if (st[z] == '-')
				negit ^= ((+1) ^ (-1));
	}

	while (st[z])
	{
		oz = z;
		switch (st[z])
		{
		case '(':
			p = 1;
			z++;
			oz = z;
			parm = 1;
			i = globi;
			inquotes = 0;
			for (; st[z]; z++)
			{
				if ((st[z] == '\"') && (st[z - 1] != '\\'))
					inquotes ^= 1;
				if (inquotes)
					continue;
				if (st[z] == '(')
				{
					p++;
					continue;
				}
				if (st[z] == ')')
				{
					p--;
					if (!p)
						break;
				}
				if ((st[z] == ',') && (p == 1))
				{
					if (z == oz)
					{
						// Insert '0' if blank param
						checkops(gecnt + 1);
						memset(&gasm[gecnt], 0, sizeof(gasmtyp));
						gasm[gecnt].r[0].r = globi * 8 + KECX;
						gasm[gecnt].r[1].r = gccnt * 8 + KEDX;
						gasm[gecnt].r[2].r = KUNUSED;
						gasm[gecnt].n = 1;
						gasm[gecnt].f = MOV;
						gecnt++;
						checkops(gccnt + 1);
						globval[gccnt++] = 0.0;
						checkops(globi + 2);
						gnext[globi] = globi + 1;
						globi++;
						gop[globi] = NUL;
					}
					else
					{
						ch = st[z];
						st[z] = 0;
						parsefunc(&st[oz], breaklab, contlab);
						st[z] = ch;
						if (globi == -1)
							return;
					}
					parm++;
					oz = z + 1;
				}
			}
			if (z == oz)
			{
				// Insert '0' if blank param
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = globi * 8 + KECX;
				gasm[gecnt].r[1].r = gccnt * 8 + KEDX;
				gasm[gecnt].r[2].r = KUNUSED;
				gasm[gecnt].n = 1;
				gasm[gecnt].f = MOV;
				gecnt++;
				checkops(gccnt + 1);
				globval[gccnt++] = 0.0;
				checkops(globi + 2);
				gnext[globi] = globi + 1;
				globi++;
				gop[globi] = NUL;
			}
			else
			{
				ch = st[z];
				st[z] = 0;
				parsefunc(&st[oz], breaklab, contlab);
				st[z] = ch;
				if (globi == -1)
					return;
			}
			if (st[z] != ')')
			{
				strcpy(kasm87err, "ERROR: missing )");
				globi = -1;
				return;
			}
			z++; // Skip ')'

			if ((fmode == LOG) && (parm == 2))
				fmode = LOGB; // Choose function based on parm #

			fparm = -1;
			if (fmode < PARAM1)
				;
			else if ((fmode >= PARAM1) && (fmode < PARAM2))
				fparm = 1;
			else if ((fmode >= PARAM2) && (fmode < PARAM3))
				fparm = 2;
			else if ((fmode >= PARAM3) && (fmode != USERFUNC))
				;
			else if (newvari >= 0)
				fparm = newvar[newvari].parnum;

			// Hack to support function overloading :)
			tg.g = newvari;
			if ((fmode == USERFUNC) && (newvari >= 0) && (parm != fparm))
			{
				if (newvarnam[newvar[newvari].proti + fparm - 1] == 'e')
					fparm = parm;
				else
				{
					cptr = &newvarnam[newvar[newvari].nami];
					while ((parm != fparm) || (stricmp(cptr, &newvarnam[newvar[newvari].nami])))
					{
						newvari = newvar[newvari].hashn;
						if (newvari < 0)
						{
							fparm = -1;
							break;
						} // function with same # parms not found :/
						fparm = newvar[newvari].parnum;
					}
				}
			}
			if (parm != fparm)
			{
				strcpy(kasm87err, "ERROR: ");
				tg.f = fmode;
				getfuncnam(&tg, &kasm87err[strlen(kasm87err)]);
				sprintf(&kasm87err[strlen(kasm87err)], " doesn't take %d param", parm);
				if (parm != 1)
					strcat(kasm87err, "s");
				globi = -1;
				return;
			}

			if (fmode == USERFUNC)
			{
				long skip4ellipses = 0;
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = i * 8 + KECX;
				if (fparm > 2)
				{
					checkrxi(numrxi + fparm - 2);
					memset(&rxi[numrxi], 0, sizeof(rxi[0]) * (fparm - 2));
				}
				for (j = i, k = 0; k < fparm; j = gnext[j], k++)
				{
					if (k < 2)
						rp = &gasm[gecnt].r[k + 1];
					else
						rp = &rxi[numrxi + k - 2];
					if ((unsigned)j > (unsigned)globi)
					{
						strcpy(kasm87err, "ERROR: ");
						tg.f = fmode;
						getfuncnam(&tg, &kasm87err[strlen(kasm87err)]);
						sprintf(&kasm87err[strlen(kasm87err)], " param %d: pointer invalid", k + 1);
						globi = -1;
						return;
					}
					rp->r = j * 8 + KECX;
					if (newvarnam[newvar[newvari].proti + k] == 'e')
						skip4ellipses = 1;
					if (skip4ellipses)
						continue;
					if (newvarnam[newvar[newvari].proti + k] >= 'a')
						continue;
					for (p = gecnt - 1; p >= 0; p--) // pointer params (double* or char*) must not be an expression
						if (gasm[p].r[0].r == rp->r)
						{
							if (gasm[p].f != MOV)
							{
								strcpy(kasm87err, "ERROR: ");
								tg.f = fmode;
								getfuncnam(&tg, &kasm87err[strlen(kasm87err)]);
								sprintf(&kasm87err[strlen(kasm87err)], " param %d: pointer invalid", k + 1);
								globi = -1;
								return;
							}
							// if ((gasm[p].r[1].r&0xf0000000) == KEDX) break; //FIXFIXFIX
							(*rp) = gasm[p].r[1];
							break;
						}
				}
				if (fparm > 2)
				{
					gasm[gecnt].rxi = numrxi;
					numrxi += fparm - 2;
				}
				gasm[gecnt].f = fmode;
				if (newvari >= 0)
					gasm[gecnt].g = newvari;
				else
					gasm[gecnt].g = 0;
				gasm[gecnt].n = fparm;
				gecnt++;
			}
			else if (fmode != NOP)
			{
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = gasm[gecnt].r[1].r = i * 8 + KECX;
				if (fmode < PARAM2)
				{
					gasm[gecnt].n = 1;
					gasm[gecnt].r[2].r = KUNUSED;
				}
				else
				{
					gasm[gecnt].n = 2;
					gasm[gecnt].r[2].r = gnext[i] * 8 + KECX;
				}
				gasm[gecnt].f = fmode;
				if (newvari >= 0)
					gasm[gecnt].g = newvari;
				else
					gasm[gecnt].g = 0;
				gecnt++;
			}

			gnext[i] = globi;
			if (negit < 0)
			{
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = gasm[gecnt].r[1].r = i * 8 + KECX;
				gasm[gecnt].r[2].r = KUNUSED;
				gasm[gecnt].n = 1;
				gasm[gecnt].f = NEGMOV;
				gecnt++;
			}
			fmode = NOP;
			break;

			// Parse operators
		case '=':
			if (st[z + 1] == '=')
			{
				gop[globi] = EQU;
				z += 2;
				goto begit;
			}
			break;
		case '!':
			if (st[z + 1] == '=')
			{
				gop[globi] = NEQU;
				z += 2;
				goto begit;
			}
			break;
		case '&':
			if (st[z + 1] == '&')
			{
				gop[globi] = LAND;
				z += 2;
				goto begit;
			}
			if (isvarchar(st[z + 1]))
				isaddr = 1;
			z++;
			break;
		case '|':
			if (st[z + 1] == '|')
			{
				gop[globi] = LOR;
				z += 2;
				goto begit;
			}
			break;
		case '^':
			gop[globi] = POW;
			z++;
			goto begit;
		case '*':
			gop[globi] = TIMES;
			z++;
			goto begit;
		case '/':
			gop[globi] = SLASH;
			z++;
			goto begit;
		case '%':
			gop[globi] = PERC;
			z++;
			goto begit;
		case '+':
			gop[globi] = PLUS;
			z++;
			goto begit;
		case '-':
			gop[globi] = MINUS;
			z++;
			goto begit;
		case '<':
			if (st[z + 1] == '=')
			{
				gop[globi] = LESEQ;
				z += 2;
			}
			else
			{
				gop[globi] = LES;
				z++;
			}
			goto begit;
		case '>':
			if (st[z + 1] == '=')
			{
				gop[globi] = MOREQ;
				z += 2;
			}
			else
			{
				gop[globi] = MOR;
				z++;
			}
			goto begit;

			// Parse constants
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '.':
			checkops(gecnt + 1);
			memset(&gasm[gecnt], 0, sizeof(gasmtyp));
			gasm[gecnt].r[0].r = globi * 8 + KECX;
			gasm[gecnt].r[1].r = gccnt * 8 + KEDX;
			gasm[gecnt].r[2].r = KUNUSED;
			gasm[gecnt].n = 1;
			gasm[gecnt].f = MOV;
			gecnt++;
			checkops(gccnt + 1);
			if ((st[z] == '0') && (st[z + 1] == 'X')) // Handle HEX numbers
			{
				globval[gccnt] = strtoul(&st[z], (char **)&z, 0);
				if (globval[gccnt] >= 2147483648.0)
					globval[gccnt] -= 4294967296.0;
			}
			else
				globval[gccnt] = strtod(&st[z], (char **)&z) * (double)negit;
			gccnt++;
			z -= (long)st;
			checkops(globi + 2);
			gnext[globi] = globi + 1;
			globi++;
			gop[globi] = NUL;
			break;

			// Parse functions&statements
		case 'A':
			if (!strncmp(&st[z], "ACOS(", 5))
			{
				fmode = ACOS;
				z += 4;
			}
			else if (!strncmp(&st[z], "ASIN(", 5))
			{
				fmode = ASIN;
				z += 4;
			}
			else if (!strncmp(&st[z], "ATAN2(", 6))
			{
				fmode = ATAN2;
				z += 5;
			}
			else if (!strncmp(&st[z], "ATAN(", 5))
			{
				fmode = ATAN;
				z += 4;
			}
			else if (!strncmp(&st[z], "ATN(", 4))
			{
				fmode = ATAN;
				z += 3;
			}
			else if (!strncmp(&st[z], "ABS(", 4))
			{
				fmode = FABS;
				z += 3;
			}
			break;
		case 'B':
			if (!strncmp(&st[z], "BREAK;", 6))
			{
				z += 6;
				if (breaklab < 0)
				{
					sprintf(kasm87err, "ERROR: BREAK not allowed outside loop");
					globi = -1;
					return;
				}

				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].f = GOTO;
				gasm[gecnt].r[0].r = KUNUSED;
				gasm[gecnt].r[1].r = breaklab + KEIP;
				gasm[gecnt].n = 1;
				gecnt++;
			}
			break;
		case 'C':
			if (!strncmp(&st[z], "COS(", 4))
			{
				fmode = COS;
				z += 3;
			}
			else if (!strncmp(&st[z], "CEIL(", 5))
			{
				fmode = CEIL;
				z += 4;
			}
			else if (!strncmp(&st[z], "CONTINUE;", 9))
			{
				z += 9;
				if (contlab < 0)
				{
					sprintf(kasm87err, "ERROR: CONTINUE not allowed outside loop");
					globi = -1;
					return;
				}

				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].f = GOTO;
				gasm[gecnt].r[0].r = KUNUSED;
				gasm[gecnt].r[1].r = contlab + KEIP;
				gasm[gecnt].n = 1;
				gecnt++;
			}
			break;
		case 'D':
			if ((!strncmp(&st[z], "DO", 2)) && (!isvarchar(st[z + 2])))
			{
				z += 2;
				// l1: (fmode)
				//    <code_true>
				// l2: (fmode+1)
				//    if (expression) goto l1
				// l3: (fmode+2)

				// Reserve label #
				checklabs(numlabels + 3);
				fmode = numlabels;
				numlabels += 3; // fmode used for unrelated temp here

				// Insert label
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].f = NUL;
				gasm[gecnt].r[0].r = fmode + KEIP;
				gecnt++;

				if ((p = findscope(st, z, &oz, &z)) < 0)
				{
					strcpy(kasm87err, "ERROR: DO needs statement");
					globi = -1;
					return;
				}
				ch = st[z];
				st[z] = 0;
				parsefunc(&st[oz], fmode + 2, fmode + 1);
				st[z] = ch;
				if (globi == -1)
					return;
				z = p;

				// Insert label
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].f = NUL;
				gasm[gecnt].r[0].r = fmode + 1 + KEIP;
				gecnt++;

				if ((strncmp(&st[z], "WHILE", 5)) || (isvarchar(st[z + 5])) || (!st[z + 5]))
				{
					strcpy(kasm87err, "ERROR: DO needs WHILE");
					globi = -1;
					return;
				}
				z += 5;

				p = 1;
				z++;
				oz = z;
				i = globi;
				inquotes = 0;
				for (; st[z]; z++)
				{
					if ((st[z] == '\"') && ((!z) || (st[z - 1] != '\\')))
						inquotes ^= 1;
					if (inquotes)
						continue;
					if (st[z] == '(')
					{
						p++;
						continue;
					}
					if (st[z] == ')')
					{
						p--;
						if (!p)
							break;
					}
					if ((st[z] == ',') && (p == 1))
					{
						sprintf(kasm87err, "ERROR: DO takes 1 param");
						globi = -1;
						return;
					}
				}
				ch = st[z];
				st[z] = 0;
				parsefunc(&st[oz], breaklab, contlab);
				st[z] = ch;
				if (globi == -1)
					return;
				if (st[z] != ')')
				{
					strcpy(kasm87err, "ERROR: missing )");
					globi = -1;
					return;
				}
				z++; // Skip ')'

				if (st[z] != ';')
				{
					strcpy(kasm87err, "ERROR: missing ; after WHILE");
					globi = -1;
					return;
				}
				z++;

				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = KUNUSED;
				gasm[gecnt].r[1].r = fmode + KEIP;
				gasm[gecnt].r[2].r = i * 8 + KECX;
				gasm[gecnt].n = 2;
				gasm[gecnt].f = IF1;
				gecnt++;
				gnext[i] = globi;

				// Insert label
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].f = NUL;
				gasm[gecnt].r[0].r = fmode + 2 + KEIP;
				gecnt++;

				fmode = NOP;
			}
			break;
		case 'E':
			if (!strncmp(&st[z], "EXP(", 4))
			{
				fmode = EXP;
				z += 3;
			}
			else if ((!strncmp(&st[z], "ENUM", 4)) && (!isvarchar(st[z + 4])))
			{
				z = parse_enum(st, z);
				if (globi < 0)
					return;
			}
			break;
		case 'F':
			if (!strncmp(&st[z], "FABS(", 5))
			{
				fmode = FABS;
				z += 4;
			}
			else if (!strncmp(&st[z], "FACT(", 5))
			{
				fmode = FACT;
				z += 4;
			}
			else if (!strncmp(&st[z], "FADD(", 5))
			{
				fmode = FADD;
				z += 4;
			}
			else if (!strncmp(&st[z], "FLOOR(", 6))
			{
				fmode = FLOOR;
				z += 5;
			}
			else if (!strncmp(&st[z], "FMOD(", 5))
			{
				fmode = FMOD;
				z += 4;
			}
			else if (!strncmp(&st[z], "FOR(", 4))
			{
				z += 4;

				// Do Initial Condition of (;;)
				p = 1;
				oz = z;
				inquotes = 0;
				for (; st[z]; z++)
				{
					if ((st[z] == '\"') && ((!z) || (st[z - 1] != '\\')))
						inquotes ^= 1;
					if (inquotes)
						continue;
					if (st[z] == '(')
					{
						p++;
						continue;
					}
					if (st[z] == ')')
					{
						p--;
						if (!p)
						{
							strcpy(kasm87err, "ERROR: FOR needs 3 fields");
							globi = -1;
							return;
						}
						continue;
					}
					if ((st[z] == ';') && (p == 1))
						break;
				}
				if (st[z] != ';')
				{
					strcpy(kasm87err, "ERROR: FOR missing ;");
					globi = -1;
					return;
				}
				z++; // Skip ';'
				ch = st[z];
				st[z] = 0;
				parsefunc(&st[oz], breaklab, contlab);
				st[z] = ch;
				if (globi == -1)
					return;

				//   Init;
				// l1: (fmode)
				//   if !(expression) goto l3
				//   <code_true>
				// l2: (fmode+1)
				//   Iterate;
				//   goto l1
				// l3: (fmode+2)

				// Reserve label #
				checklabs(numlabels + 3);
				fmode = numlabels;
				numlabels += 3; // fmode used for unrelated temp here
				// Insert label
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].f = NUL;
				gasm[gecnt].r[0].r = fmode + KEIP;
				gecnt++;

				p = 1;
				oz = z;
				i = globi;
				inquotes = 0;
				for (; st[z]; z++)
				{
					if ((st[z] == '\"') && ((!z) || (st[z - 1] != '\\')))
						inquotes ^= 1;
					if (inquotes)
						continue;
					if (st[z] == '(')
					{
						p++;
						continue;
					}
					if (st[z] == ')')
					{
						p--;
						if (!p)
						{
							strcpy(kasm87err, "ERROR: FOR needs 3 fields");
							globi = -1;
							return;
						}
						continue;
					}
					if ((st[z] == ';') && (p == 1))
						break;
				}
				ch = st[z];
				st[z] = 0;
				parsefunc(&st[oz], breaklab, contlab);
				st[z] = ch;
				if (globi == -1)
					return;
				if (st[z] != ';')
				{
					strcpy(kasm87err, "ERROR: FOR missing ;");
					globi = -1;
					return;
				}
				z++; // Skip ';'

				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = KUNUSED;
				gasm[gecnt].r[1].r = fmode + 2 + KEIP;
				gasm[gecnt].r[2].r = i * 8 + KECX;
				gasm[gecnt].n = 2;
				gasm[gecnt].f = IF0;
				gecnt++;
				gnext[i] = globi;

				// Save 3rd param of (;;) as j&k for Iteration (which is done after {})
				j = z;
				p = 1;
				inquotes = 0;
				for (; st[z]; z++)
				{
					if ((st[z] == '\"') && ((!z) || (st[z - 1] != '\\')))
						inquotes ^= 1;
					if (inquotes)
						continue;
					if (st[z] == '(')
					{
						p++;
						continue;
					}
					if (st[z] == ')')
					{
						p--;
						if (!p)
							break;
					}
				}
				if (st[z] != ')')
				{
					strcpy(kasm87err, "ERROR: FOR missing )");
					globi = -1;
					return;
				}
				z++; // Skip ')'
				k = z;

				if ((p = findscope(st, z, &oz, &z)) < 0)
				{
					strcpy(kasm87err, "ERROR: FOR needs statement");
					globi = -1;
					return;
				}
				ch = st[z];
				st[z] = 0;
				parsefunc(&st[oz], fmode + 2, fmode + 1);
				st[z] = ch;
				if (globi == -1)
					return;
				z = p;

				// Insert label
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].f = NUL;
				gasm[gecnt].r[0].r = fmode + 1 + KEIP;
				gecnt++;

				// Do Iteration of (;;)
				ch2 = st[k - 1];
				st[k - 1] = ';';
				ch = st[k];
				st[k] = 0;
				parsefunc(&st[j], breaklab, contlab);
				st[k] = ch;
				st[k - 1] = ch2;
				if (globi == -1)
					return;

				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = KUNUSED;
				gasm[gecnt].r[1].r = fmode + KEIP;
				gasm[gecnt].n = 1;
				gasm[gecnt].f = GOTO;
				gecnt++;

				// Insert label
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].f = NUL;
				gasm[gecnt].r[0].r = fmode + 2 + KEIP;
				gecnt++;

				fmode = NOP;
			}
			break;
		case 'G':
			if (!strncmp(&st[z], "GOTO ", 5))
			{
				z += 5;
				for (j = z; isvarchar(st[j]); j++)
					;
				if (j - z <= 0)
				{
					sprintf(kasm87err, "ERROR: GOTO needs label");
					globi = -1;
					return;
				}

				ch = st[j];
				st[j] = 0;
				for (i = 0, cptr = newlabnam; i < newlabnum; i++, cptr = &cptr[k + 1])
				{
					for (k = 1; cptr[k]; k++)
						;
					if (!strcmp(&st[z], cptr))
						break;
				}
				if (i >= newlabnum)
				{
					checklabchars(newlabplc + j - z + 2);
					checklabs(numlabels + 1);
					strcpy(&newlabnam[newlabplc], &st[z]);
					newlabplc += j - z + 1;
					newlabind[newlabnum++] = (numlabels | 0x80000000);
					i = numlabels++;
				}
				else
					i = (newlabind[i] & 0x7fffffff);
				st[j] = ch;

				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = KUNUSED;
				gasm[gecnt].r[1].r = i + KEIP;
				gasm[gecnt].f = GOTO;
				gasm[gecnt].n = 1;
				gecnt++;

				z = j + 1; // skip label
			}
			break;
		case 'I':
			if (!strncmp(&st[z], "INT(", 4))
			{
				fmode = ROUND0;
				z += 3;
			}
			else if ((!strncmp(&st[z], "IF", 2)) && (!isvarchar(st[z + 2])) && (st[z + 2]))
			{
				z += 2;

				checklabs(numlabels + 1);
				fmode = numlabels;
				numlabels++; // fmode used for unrelated temp here

				p = 1;
				z++;
				oz = z;
				i = globi;
				inquotes = 0;
				for (; st[z]; z++)
				{
					if ((st[z] == '\"') && ((!z) || (st[z - 1] != '\\')))
						inquotes ^= 1;
					if (inquotes)
						continue;
					if (st[z] == '(')
					{
						p++;
						continue;
					}
					if (st[z] == ')')
					{
						p--;
						if (!p)
							break;
					}
					if (((st[z] == ',') && (p == 1)) || (st[z] == ';'))
					{
						strcpy(kasm87err, "ERROR: IF condition invalid");
						globi = -1;
						return;
					}
				}
				ch = st[z];
				st[z] = 0;
				parsefunc(&st[oz], breaklab, contlab);
				st[z] = ch;
				if (globi == -1)
					return;
				if (st[z] != ')')
				{
					strcpy(kasm87err, "ERROR: IF missing )");
					globi = -1;
					return;
				}
				z++; // Skip ')'

				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = KUNUSED;
				gasm[gecnt].r[1].r = fmode + KEIP;
				gasm[gecnt].r[2].r = i * 8 + KECX;
				gasm[gecnt].n = 2;
				gasm[gecnt].f = IF0;
				gecnt++;
				gnext[i] = globi;

				i = globi;
				if ((p = findscope(st, z, &oz, &z)) < 0)
				{
					strcpy(kasm87err, "ERROR: IF needs statement");
					globi = -1;
					return;
				}
				ch = st[z];
				st[z] = 0;
				parsefunc(&st[oz], breaklab, contlab);
				st[z] = ch;
				if (globi == -1)
					return;
				z = p;

				if ((!strncmp(&st[z], "ELSE", 4)) && (!isvarchar(st[z + 4])))
				{
					z += 4;
					// if !(expression) goto l1
					//    <code_true>
					//    goto l2
					// l1:
					//    <code_false>
					// l2:

					checklabs(numlabels + 2);
					// Insert goto so true case skips 'else' part
					checkops(gecnt + 1);
					memset(&gasm[gecnt], 0, sizeof(gasmtyp));
					gasm[gecnt].r[0].r = KUNUSED;
					gasm[gecnt].r[1].r = numlabels + KEIP;
					gasm[gecnt].f = GOTO;
					gasm[gecnt].n = 1;
					numlabels++;
					gecnt++;
					// Insert label to begin else part
					checkops(gecnt + 1);
					memset(&gasm[gecnt], 0, sizeof(gasmtyp));
					gasm[gecnt].f = NUL;
					gasm[gecnt].r[0].r = fmode + KEIP;
					gecnt++;
					fmode = numlabels - 1;

					i = globi;

					// Some cases to verify code with:
					//"(x)if(x<0)r=0;else if(x<1) r=x; else r=1;  r"
					//"(x)if(x<0)r=0;else{if(x<1) r=x; else r=1;} r"
					//"(x)if(x<0)r=0;else{if(x<1){r=x;}else{r=1;}}r"
					//"(x)if(x<0)r=0;else if(x<1) r=x; else{r=1;} r"
					if (st[z] == ' ')
						z++;
					oz = z;
					p = 0;
					for (;; z++)
					{
						if (!st[z])
						{
							strcpy(kasm87err, "ERROR: ELSE needs statement");
							globi = -1;
							return;
						}
						if (st[z] == '{')
						{
							p++;
							continue;
						}
						if (st[z] == '}')
							p--;
						if (((st[z] == '}') || (st[z] == ';')) && (p <= 0))
						{
							z++;
							if ((!strncmp(&st[z], "ELSE", 4)) && (!isvarchar(st[z + 4])))
							{
								z += 4 - 1;
								continue;
							}
							break;
						}
					}

					if (st[oz] == '{')
					{
						oz++;
						z--;
					}
					ch = st[z];
					st[z] = 0;
					parsefunc(&st[oz], breaklab, contlab);
					st[z] = ch;
					if (globi == -1)
						return;
					if (st[z] == '}')
						z++;
				}

				// Insert label at }
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].f = NUL;
				gasm[gecnt].r[0].r = fmode + KEIP;
				gecnt++;

				fmode = NOP;
			}
			break;
		case 'L':
			if (!strncmp(&st[z], "LOG(", 4))
			{
				fmode = LOG;
				z += 3;
			}
			break;
		case 'M':
			if (!strncmp(&st[z], "MIN(", 4))
			{
				fmode = MIN;
				z += 3;
			}
			else if (!strncmp(&st[z], "MAX(", 4))
			{
				fmode = MAX;
				z += 3;
			}
			break;
		case 'N':
			if ((!strncmp(&st[z], "NRND", 4)) && (!isvarchar(st[z + 4])))
			{
				z += 4;
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = globi * 8 + KECX;
				gasm[gecnt].r[1].r = gasm[gecnt].r[2].r = KUNUSED;
				gasm[gecnt].f = NRND;
				gecnt++;
				checkops(globi + 2);
				gnext[globi] = globi + 1;
				globi++;
				gop[globi] = NUL;
			}
			break;
		case 'P':
			if (!strncmp(&st[z], "POW(", 4))
			{
				fmode = POW;
				z += 3;
			}
			else if ((!strncmp(&st[z], "PI", 2)) && (!isvarchar(st[z + 2])))
			{
				z += 2;
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = globi * 8 + KECX;
				gasm[gecnt].r[1].r = gccnt * 8 + KEDX;
				gasm[gecnt].r[2].r = KUNUSED;
				gasm[gecnt].n = 1;
				gasm[gecnt].f = MOV;
				gecnt++;
				checkops(gccnt + 1);
				globval[gccnt++] = PI * (double)negit;
				checkops(globi + 2);
				gnext[globi] = globi + 1;
				globi++;
				gop[globi] = NUL;
			}
			break;
		case 'R':
			if ((!strncmp(&st[z], "RND", 3)) && (!isvarchar(st[z + 3])))
			{
				z += 3;
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = globi * 8 + KECX;
				gasm[gecnt].r[1].r = gasm[gecnt].r[2].r = KUNUSED;
				gasm[gecnt].f = RND;
				gecnt++;
				checkops(globi + 2);
				gnext[globi] = globi + 1;
				globi++;
				gop[globi] = NUL;
			}
			else if ((!strncmp(&st[z], "RETURN", 6)) && (!isvarchar(st[z + 6])))
			{
				z += 6;
				if (st[z] == ' ')
					z++;

				p = 0;
				oz = z;
				i = globi;
				inquotes = 0;
				for (; st[z]; z++)
				{
					if ((st[z] == '\"') && ((!z) || (st[z - 1] != '\\')))
						inquotes ^= 1;
					if (inquotes)
						continue;
					if (st[z] == '(')
					{
						p++;
						continue;
					}
					if (st[z] == ')')
					{
						p--;
						continue;
					}
					if ((p == 0) && (st[z] == ';'))
						break;
					if ((p == 1) && (st[z] == ','))
					{
						strcpy(kasm87err, "ERROR: RETURN ',' invalid syntax");
						globi = -1;
						return;
					}
				}
				ch = st[z];
				st[z] = 0;
				parsefunc(&st[oz], -1, -1);
				st[z] = ch;
				if (globi == -1)
					return;
				if (st[z] != ';')
				{
					strcpy(kasm87err, "ERROR: RETURN missing ;");
					globi = -1;
					return;
				}
				z++; // Skip ';'

				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = KUNUSED;
				gasm[gecnt].r[1].r = i * 8 + KECX;
				gasm[gecnt].r[2].r = KUNUSED;
				gasm[gecnt].n = 1;
				gasm[gecnt].f = RETURN;
				gecnt++;
			}
			break;
		case 'S':
			if (!strncmp(&st[z], "SQRT(", 5))
			{
				fmode = SQRT;
				z += 4;
			}
			// else if (!strncmp(&st[z],"SQR(",4))   { fmode = SQRT;  z += 3; }
			else if (!strncmp(&st[z], "SIN(", 4))
			{
				fmode = SIN;
				z += 3;
			}
			else if (!strncmp(&st[z], "SGN(", 4))
			{
				fmode = SGN;
				z += 3;
			}
			else if ((!strncmp(&st[z], "STATIC", 6)) && (!isvarchar(st[z + 6])))
			{
				z = parse_static(st, z);
				if (globi < 0)
					return;
				goto prebegit;
			}
			break;
		case 'T':
			if (!strncmp(&st[z], "TAN(", 4))
			{
				fmode = TAN;
				z += 3;
			}
			break;
		case 'U':
			if (!strncmp(&st[z], "UNIT(", 5))
			{
				fmode = UNIT;
				z += 4;
			}
			break;
		case 'W':
			if ((!strncmp(&st[z], "WHILE", 5)) && (!isvarchar(st[z + 5])) && (st[z + 5]))
			{
				z += 5;
				// l1: (fmode)
				//    if !(expression) goto l2
				//    <code_true>
				//    goto l1
				// l2: (fmode+1)

				// Reserve label #
				checklabs(numlabels + 2);
				fmode = numlabels;
				numlabels += 2; // fmode used for unrelated temp here
				// Insert label
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].f = NUL;
				gasm[gecnt].r[0].r = fmode + KEIP;
				gecnt++;

				p = 1;
				z++;
				oz = z;
				i = globi;
				inquotes = 0;
				for (; st[z]; z++)
				{
					if ((st[z] == '\"') && ((!z) || (st[z - 1] != '\\')))
						inquotes ^= 1;
					if (inquotes)
						continue;
					if (st[z] == '(')
					{
						p++;
						continue;
					}
					if (st[z] == ')')
					{
						p--;
						if (!p)
							break;
					}
					if ((st[z] == ',') && (p == 1))
					{
						strcpy(kasm87err, "ERROR: WHILE takes 1 param");
						globi = -1;
						return;
					}
				}
				ch = st[z];
				st[z] = 0;
				parsefunc(&st[oz], breaklab, contlab);
				st[z] = ch;
				if (globi == -1)
					return;
				if (st[z] != ')')
				{
					strcpy(kasm87err, "ERROR: WHILE missing )");
					globi = -1;
					return;
				}
				z++; // Skip ')'

				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = KUNUSED;
				gasm[gecnt].r[1].r = fmode + 1 + KEIP;
				gasm[gecnt].r[2].r = i * 8 + KECX;
				gasm[gecnt].n = 2;
				gasm[gecnt].f = IF0;
				gecnt++;
				gnext[i] = globi;

				if ((p = findscope(st, z, &oz, &z)) < 0)
				{
					strcpy(kasm87err, "ERROR: WHILE needs statement");
					globi = -1;
					return;
				}
				ch = st[z];
				st[z] = 0;
				parsefunc(&st[oz], fmode + 1, fmode);
				st[z] = ch;
				if (globi == -1)
					return;
				z = p;

				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = KUNUSED;
				gasm[gecnt].r[1].r = fmode + KEIP;
				gasm[gecnt].n = 1;
				gasm[gecnt].f = GOTO;
				gecnt++;

				// Insert label
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].f = NUL;
				gasm[gecnt].r[0].r = fmode + 1 + KEIP;
				gecnt++;

				fmode = NOP;
			}
			break;
		case '{':
			if ((p = findscope(st, z, &oz, &z)) < 0)
			{
				strcpy(kasm87err, "ERROR: missing }");
				globi = -1;
				return;
			}
			ch = st[z];
			st[z] = 0;
			parsefunc(&st[oz], breaklab, contlab);
			st[z] = ch;
			if (globi == -1)
				return;
			z = p;
			fmode = NOP;
			break;
		case ';':
		case ',':
			z++;
			fmode = NOP;
			break;
		case ' ':
			z++;
			break;
		default:
			break;
		}
		if (oz != z)
			continue; // Token found: no more processing

		// Undefined token... see if it's a variable/function name
		cptr = &st[z];
		if (st[z] == '\"') // Support filenames
		{
			for (z++; (st[z]) && ((st[z] != '\"') || (st[z - 1] == '\\')); z++)
				;
			if (st[z])
				z++;
		}
		else
		{
			while (isvarchar(st[z]))
				z++;
		}
		p = z;
		j = z - oz;
		ch = st[p];
		st[p] = 0;
		for (i = newvarhash[getnewvarhash(cptr)]; i >= 0; i = newvar[i].hashn)
		{
			// printf("|%s| vs. |%s|\n",cptr,&newvarnam[newvar[i].nami]); //nice for debugging
			if (!strcmp(cptr, &newvarnam[newvar[i].nami]))
				break;
		}
		st[p] = ch;

		if (i < 0)
			z = oz;
		else
		{
			if (st[z] == '[')
			{
				char *cptr2;

				p = 1;
				z++;
				oz = z;
				parm = 1; // pal[c[x][y][z]]
				for (; st[z]; z++)
				{
					if (st[z] == '[')
					{
						p++;
						continue;
					}
					if (st[z] == ']')
					{
						p--;
						if (p)
							continue;
						if ((st[z + 1] == ' ') && (st[z + 2] == '['))
							z++;
						if (st[z + 1] == '[')
						{
							parm++;
							continue;
						}
						break;
					}
				}
				if (!st[z])
				{
					sprintf(kasm87err, "ERROR: %s missing ]", &newvarnam[newvar[i].nami]);
					globi = -1;
					return;
				}
				if (z == oz)
				{
					sprintf(kasm87err, "ERROR: Blank param in %s[]", &newvarnam[newvar[i].nami]);
					globi = -1;
					return;
				}
				if (parm > (~newvar[i].parnum))
				{
					sprintf(kasm87err, "ERROR: Array (%s) too many dimensions", &newvarnam[newvar[i].nami]);
					globi = -1;
					return;
				}

				if (parm == 1)
				{
					// Doing enum/strtol check here is merely an optimization... (not necessary)
					arrind = -1; // First see if array index is an "enum" style name...
					for (k = 0, cptr2 = enumnam; k < enumnum; k++, cptr2 = &cptr2[p + 1])
					{
						for (p = 1; cptr2[p]; p++)
							;
						if ((z - oz == p) && (!strncmp(&st[oz], cptr2, p)))
						{
							arrind = (long)enumval[k];
							p = z;
							break;
						}
					}
					if (arrind < 0)
					{
						arrind = strtol(&st[oz], (char **)&p, 10);
						p -= (long)st;
					}
				}
				if (p == z)
				{
					if ((unsigned long)arrind >= (unsigned long)newvar[i].maxind)
					{
						sprintf(kasm87err, "ERROR: array index out of bounds (%s)", &newvarnam[newvar[i].nami]);
						globi = -1;
						return;
					}
					z++;
				}
				else // Array parameter is not enum, constant, or < 2 dimens; treat index as expression
				{
					k = globi;
#if 0
					ch = st[z]; st[z] = 0; parsefunc(&st[oz],breaklab,contlab); st[z] = ch; if (globi == -1) return;
					if (st[z] != ']') { sprintf(kasm87err,"ERROR: array (%s) missing ]",&newvarnam[newvar[i].nami]); globi = -1; return; }
					z++; //Skip ']'
#else
					p = 1;
					fparm = 0; // multidimensional arrays
					for (z = oz; st[z]; z++)
					{
						if (st[z] == '[')
						{
							p++;
							continue;
						}
						if (st[z] == ']')
						{
							p--;
							if (p)
								continue;
							p = globi;

							ch = st[z];
							st[z] = 0;
							parsefunc(&st[oz], breaklab, contlab);
							st[z] = ch;
							if (globi == -1)
								return;

							if (fparm < parm - 1)
							{
								long l; // if (parm > 2)

								// p = int(p);
								checkops(gecnt + 1);
								memset(&gasm[gecnt], 0, sizeof(gasmtyp));
								gasm[gecnt].r[0].r = gasm[gecnt].r[1].r = p * 8 + KECX;
								gasm[gecnt].r[2].r = KUNUSED;
								gasm[gecnt].n = 1;
								gasm[gecnt].f = ROUND0_32;
								gecnt++; //(array indices only need 32-bit precision)
								gnext[p] = globi;

								// p *= product_right;
								checkops(gecnt + 1);
								memset(&gasm[gecnt], 0, sizeof(gasmtyp));
								gasm[gecnt].r[0].r = gasm[gecnt].r[1].r = p * 8 + KECX;
								gasm[gecnt].r[2].r = gccnt * 8 + KEDX;
								checkops(gccnt + 1);
								// static buf3d[5][3][2];
								//? = buf3d[a];                                 a
								//? = buf3d[a][b];                   int(a)*2 + b
								//? = buf3d[a][b][c];   int(a)*3*2 + int(b)*2 + c
								globval[gccnt] = 1.0; // dimension combined multiplier
								for (l = (~newvar[i].parnum) + fparm - parm + 1; l < (~newvar[i].parnum); l++)
									globval[gccnt] *= ((long *)&newvarnam[newvar[i].proti])[l];
								gccnt++;
								gasm[gecnt].n = 2;
								gasm[gecnt].f = TIMES;
								gecnt++;
								gnext[p] = globi;
							}
							if (fparm > 0)
							{
								// k += p;
								checkops(gecnt + 1);
								memset(&gasm[gecnt], 0, sizeof(gasmtyp));
								gasm[gecnt].r[0].r = gasm[gecnt].r[1].r = k * 8 + KECX;
								gasm[gecnt].r[2].r = p * 8 + KECX;
								gasm[gecnt].n = 2;
								gasm[gecnt].f = PLUS;
								gecnt++;
								gnext[k] = p;
							}

							p = 0;
							if ((st[z + 1] == ' ') && (st[z + 2] == '['))
								z++;
							if (st[z + 1] == '[')
							{
								fparm++;
								oz = z + 2;
								continue;
							}
							z++;
							break;
						}
					}
#endif

					// FIX: combine this with other =,*=,... code?

					if (((st[z] == '=') && (st[z + 1] != '=')) || // =
						((st[z] == '*') && (st[z + 1] == '=')) || // *=
						((st[z] == '/') && (st[z + 1] == '=')) || // /=
						((st[z] == '%') && (st[z + 1] == '=')) || // %=
						((st[z] == '+') && (st[z + 1] == '=')) || // +=
						((st[z] == '-') && (st[z + 1] == '=')) || // -=
						((st[z] == '+') && (st[z + 1] == '+')) || // ++
						((st[z] == '-') && (st[z + 1] == '-')))	  // --
					{
						long l;

						if (newvar[i].parnum >= 0)
						{
							ch = st[z];
							st[z] = 0;
							sprintf(kasm87err, "ERROR: %s can't be used as variable", &st[z - j]);
							st[z] = ch;
							globi = -1;
							return;
						}

						j = z;
						if (st[z] == '=')
							oz = z + 1;
						else
							oz = z + 2;
						checkops(globi + 1);
						gop[globi] = NUL;
						l = globi;
						inquotes = 0;

						for (z = oz, p = 1;; z++)
						{
							if (!st[z])
							{
								strcpy(kasm87err, "ERROR: missing ;");
								globi = -1;
								return;
							}
							if ((st[z] == '\"') && ((!z) || (st[z - 1] != '\\')))
								inquotes ^= 1;
							if (inquotes)
								continue;
							if (st[z] == '(')
							{
								p++;
								continue;
							}
							if (st[z] == ')')
							{
								p--;
								continue;
							}
							if (((st[z] == ';') || (st[z] == ',')) && (p == 1))
								break;
						}
						ch = st[z];
						st[z] = 0;
						parsefunc(&st[oz], breaklab, contlab);
						st[z] = ch;
						if (globi == -1)
							return;
						z++; // skip ';'

						checkops(gecnt + 1);
						memset(&gasm[gecnt], 0, sizeof(gasmtyp));
						checkrxi(numrxi + 1);
						memset(&rxi[numrxi], 0, sizeof(rxi[0]));
						gasm[gecnt].r[0].r = KUNUSED;
						gasm[gecnt].n = 3;
						gasm[gecnt].r[1].r = newvar[i].r;
						gasm[gecnt].r[1].nv = i;
						gasm[gecnt].r[2].r = k * 8 + KECX;
						rxi[numrxi].r = l * 8 + KECX;

						if (st[j] == '=')
						{
							gasm[gecnt].f = POKE;
						}
						else if (st[j] == '*')
						{
							gasm[gecnt].f = POKETIMES;
						}
						else if (st[j] == '/')
						{
							gasm[gecnt].f = POKESLASH;
						}
						else if (st[j] == '%')
						{
							gasm[gecnt].f = POKEPERC;
						}
						else if (st[j] == '+')
						{
							if (st[j + 1] == '=')
							{
								gasm[gecnt].f = POKEPLUS;
							}
							else
							{
								rxi[numrxi].r = gccnt * 8 + KEDX;
								checkops(gccnt + 1);
								globval[gccnt++] = 1.0;
								gasm[gecnt].f = POKEPLUS;
							}
						}
						else if (st[j] == '-')
						{
							if (st[j + 1] == '=')
							{
								gasm[gecnt].f = POKEMINUS;
							}
							else
							{
								rxi[numrxi].r = gccnt * 8 + KEDX;
								checkops(gccnt + 1);
								globval[gccnt++] = 1.0;
								gasm[gecnt].f = POKEMINUS;
							}
						}
						gasm[gecnt].rxi = numrxi++;
						gecnt++;

						goto prebegit;
					}
					else
					{
						checkops(gecnt + 1);
						memset(&gasm[gecnt], 0, sizeof(gasmtyp));
						gasm[gecnt].r[0].r = k * 8 + KECX;
						gasm[gecnt].r[1].r = newvar[i].r;
						gasm[gecnt].r[1].nv = i;
						gasm[gecnt].r[2].r = k * 8 + KECX;
						gasm[gecnt].n = 2;
						gasm[gecnt].f = PEEK;
						gecnt++;
						gnext[k] = globi;
						if (negit < 0)
						{
							checkops(gecnt + 1);
							memset(&gasm[gecnt], 0, sizeof(gasmtyp));
							gasm[gecnt].r[0].r = gasm[gecnt].r[1].r = k * 8 + KECX;
							gasm[gecnt].r[2].r = KUNUSED;
							gasm[gecnt].n = 1;
							gasm[gecnt].f = NEGMOV;
							gecnt++;
						}
					}
					continue;
				}
			}
			else
				arrind = 0;

			if (((st[z] == '=') && (st[z + 1] != '=')) || // =
				((st[z] == '*') && (st[z + 1] == '=')) || // *=
				((st[z] == '/') && (st[z + 1] == '=')) || // /=
				((st[z] == '%') && (st[z + 1] == '=')) || // %=
				((st[z] == '+') && (st[z + 1] == '=')) || // +=
				((st[z] == '-') && (st[z + 1] == '=')) || // -=
				((st[z] == '+') && (st[z + 1] == '+')) || // ++
				((st[z] == '-') && (st[z + 1] == '-')))	  // --
			{
				if (newvar[i].parnum >= 0)
				{
					ch = st[z];
					st[z] = 0;
					sprintf(kasm87err, "ERROR: %s can't be used as variable", &st[z - j]);
					st[z] = ch;
					globi = -1;
					return;
				}

				j = z;
				if (st[z] == '=')
					oz = z + 1;
				else
					oz = z + 2;
				checkops(globi + 1);
				gop[globi] = NUL;
				k = globi;
				inquotes = 0;

				for (z = oz, p = 1;; z++)
				{
					if (!st[z])
					{
						strcpy(kasm87err, "ERROR: missing ;");
						globi = -1;
						return;
					}
					if ((st[z] == '\"') && ((!z) || (st[z - 1] != '\\')))
						inquotes ^= 1;
					if (inquotes)
						continue;
					if (st[z] == '(')
					{
						p++;
						continue;
					}
					if (st[z] == ')')
					{
						p--;
						continue;
					}
					if (((st[z] == ';') || (st[z] == ',')) && (p == 1))
						break;
				}
				ch = st[z];
				st[z] = 0;
				parsefunc(&st[oz], breaklab, contlab);
				st[z] = ch;
				if (globi == -1)
					return;
				z++; // skip ';'

				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = newvar[i].r;
				gasm[gecnt].r[0].q = arrind;
				gasm[gecnt].r[0].nv = i;
				gasm[gecnt].n = 2;
				if (st[j] == '=')
				{
					gasm[gecnt].r[1].r = k * 8 + KECX;
					gasm[gecnt].r[2].r = KUNUSED;
					gasm[gecnt].n = 1;
					gasm[gecnt].f = MOV;
				}
				else if (st[j] == '*')
				{
					gasm[gecnt].r[1] = gasm[gecnt].r[0];
					gasm[gecnt].r[2].r = k * 8 + KECX;
					gasm[gecnt].f = TIMES;
				}
				else if (st[j] == '/')
				{
					gasm[gecnt].r[1] = gasm[gecnt].r[0];
					gasm[gecnt].r[2].r = k * 8 + KECX;
					gasm[gecnt].f = SLASH;
				}
				else if (st[j] == '%')
				{
					gasm[gecnt].r[1] = gasm[gecnt].r[0];
					gasm[gecnt].r[2].r = k * 8 + KECX;
					gasm[gecnt].f = PERC;
				}
				else if (st[j] == '+')
				{
					if (st[j + 1] == '=')
					{
						gasm[gecnt].r[1] = gasm[gecnt].r[0];
						gasm[gecnt].r[2].r = k * 8 + KECX;
						gasm[gecnt].f = PLUS;
					}
					else
					{
						gasm[gecnt].r[1] = gasm[gecnt].r[0];
						gasm[gecnt].r[2].r = gccnt * 8 + KEDX;
						checkops(gccnt + 1);
						globval[gccnt++] = 1.0;
						gasm[gecnt].f = PLUS;
					}
				}
				else if (st[j] == '-')
				{
					if (st[j + 1] == '=')
					{
						gasm[gecnt].r[1] = gasm[gecnt].r[0];
						gasm[gecnt].r[2].r = k * 8 + KECX;
						gasm[gecnt].f = MINUS;
					}
					else
					{
						gasm[gecnt].r[1] = gasm[gecnt].r[0];
						gasm[gecnt].r[2].r = gccnt * 8 + KEDX;
						checkops(gccnt + 1);
						globval[gccnt++] = 1.0;
						gasm[gecnt].f = MINUS;
					}
				}
				gecnt++;

				goto prebegit;
			}
			else if (st[z] == '(')
			{
				if (newvar[i].parnum > 0)
				{
					fmode = USERFUNC;
					newvari = i;
				}
				else
				{
					ch = st[z];
					st[z] = 0;
					sprintf(kasm87err, "ERROR: %s can't be used as function", &st[z - j]);
					st[z] = ch;
					globi = -1;
					return;
				}
			}
			else
			{
				if (newvar[i].parnum > 0)
				{
					ch = st[z];
					st[z] = 0;
					sprintf(kasm87err, "ERROR: %s can't be used as variable", &st[z - j]);
					st[z] = ch;
					globi = -1;
					return;
				}

				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = globi * 8 + KECX;
				gasm[gecnt].r[1].r = newvar[i].r;
				gasm[gecnt].r[1].q = arrind;
				gasm[gecnt].r[1].nv = i;
				gasm[gecnt].r[2].r = KUNUSED;
				gasm[gecnt].n = 1;
				if (negit < 0)
					gasm[gecnt].f = NEGMOV;
				else
					gasm[gecnt].f = MOV;
				gecnt++;
				checkops(globi + 2);
				gnext[globi] = globi + 1;
				globi++;
				gop[globi] = NUL;
			}
			continue; // Name was found; no more processing
		}

		if (st[z] == '\"') // Extended name support (to support filenames)
		{
			for (j = z + 1; st[j]; j++)
				if ((st[j] == '\"') && (st[j - 1] != '\\'))
					break;
			if (!st[j])
				j = z;
			j++;
		}
		else
		{
			for (j = z; isvarchar(st[j]); j++)
				;
		} // Standard C name support
		if (j <= z)
		{
			sprintf(kasm87err, "ERROR: '=' bad dest (%.32s)", &st[z]);
			globi = -1;
			return;
		}
		if ((st[j] == '=') && (st[j + 1] != '=')) // FIXFIX //New variables MUST use assignment operator '='
		{
			// Don't allow new variable to be same name as existing enum..
			for (k = 0, cptr = enumnam; k < enumnum; k++, cptr = &cptr[p + 1])
			{
				for (p = 1; cptr[p]; p++)
					;
				if ((j - z == p) && (!strncmp(&st[z], cptr, p)))
				{
					ch = st[j];
					st[j] = 0;
					sprintf(kasm87err, "ERROR: name conflict: %s", &st[z]);
					st[j] = ch;
					globi = -1;
					return;
				}
			}

			checkvarchars(newvarplc + j - z + 2);
			checkvars(newvarnum + 1);
			newvar[newvarnum].nami = newvarplc;
			// Save new variable name&index to list
			if (st[z] == '\"')
			{
				memcpy(&newvarnam[newvarplc], &st[z + 1], j - z - 2);
				newvarplc += j - z - 2;
			}
			else
			{
				memcpy(&newvarnam[newvarplc], &st[z], j - z);
				newvarplc += j - z;
			}
			newvarnam[newvarplc++] = 0;
			newvar[newvarnum].r = globi * 8 + KECX;
			newvar[newvarnum].maxind = 0;
			newvar[newvarnum].parnum = -1;
			newvar[newvarnum].proti = -1;
			k = getnewvarhash(&newvarnam[newvar[newvarnum].nami]);
			newvar[newvarnum].hashn = newvarhash[k];
			newvarhash[k] = newvarnum;
			newvarnum++;

			oz = j + 1;
			checkops(globi + 1);
			gop[globi] = NUL;
			inquotes = 0;

			for (z = oz, p = 1;; z++)
			{
				if (!st[z])
				{
					strcpy(kasm87err, "ERROR: missing ;");
					globi = -1;
					return;
				}
				if ((st[z] == '\"') && ((!z) || (st[z - 1] != '\\')))
					inquotes ^= 1;
				if (inquotes)
					continue;
				if (st[z] == '(')
				{
					p++;
					continue;
				}
				if (st[z] == ')')
				{
					p--;
					continue;
				}
				if (((st[z] == ';') || (st[z] == ',')) && (p == 1))
					break;
			}
			if (z == oz)
			{
				sprintf(kasm87err, "ERROR: blank function");
				globi = -1;
				return;
			}
			ch = st[z];
			st[z] = 0;
			parsefunc(&st[oz], breaklab, contlab);
			st[z] = ch;
			if (globi == -1)
				return;
			z++; // skip ';'

			goto prebegit;
		}
		else if (st[j] == ':') // New label
		{
			if (j - z <= 0)
			{
				sprintf(kasm87err, "ERROR: : needs label");
				globi = -1;
				return;
			}

			// Save new variable name&index to list
			ch = st[j];
			st[j] = 0;
			for (i = 0, cptr = newlabnam; i < newlabnum; i++, cptr = &cptr[k + 1])
			{
				for (k = 1; cptr[k]; k++)
					;
				if (!strcmp(&st[z], cptr))
					break;
			}
			if (i >= newlabnum)
			{
				checklabchars(newlabplc + j - z + 2);
				checklabs(numlabels + 1);
				strcpy(&newlabnam[newlabplc], &st[z]);
				newlabplc += j - z + 1;
				newlabind[newlabnum++] = numlabels;
				i = numlabels++;
			}
			else
			{
				newlabind[i] &= 0x7fffffff;
				i = newlabind[i];
			} //(&0x7fffffff: label acked)
			st[j] = ch;

			// Insert label at }
			checkops(gecnt + 1);
			memset(&gasm[gecnt], 0, sizeof(gasmtyp));
			gasm[gecnt].f = NUL;
			gasm[gecnt].r[0].r = i + KEIP;
			gecnt++;

			z = j + 1; // skip ':'
		}
		else
		{
			// Check if name is "enum"
			while (isvarchar(st[z]))
				z++;
			for (k = 0, cptr = enumnam; k < enumnum; k++, cptr = &cptr[p + 1])
			{
				for (p = 1; cptr[p]; p++)
					;
				if ((z - oz == p) && (!strncmp(&st[oz], cptr, p)))
				{
					// Parse constants
					checkops(gecnt + 1);
					memset(&gasm[gecnt], 0, sizeof(gasmtyp));
					gasm[gecnt].r[0].r = globi * 8 + KECX;
					gasm[gecnt].r[1].r = gccnt * 8 + KEDX;
					gasm[gecnt].r[2].r = KUNUSED;
					gasm[gecnt].n = 1;
					gasm[gecnt].f = MOV;
					gecnt++;
					checkops(gccnt + 1);
					globval[gccnt++] = enumval[k] * (double)negit;
					checkops(globi + 2);
					gnext[globi] = globi + 1;
					globi++;
					gop[globi] = NUL;
					break;
				}
			}
			if (k < enumnum)
				continue; // It was enum
			z = oz;

			if (st[z] == '\"')
			{
				// Parse const strings (must be used as userfunc parameters)
				checkops(gecnt + 1);
				memset(&gasm[gecnt], 0, sizeof(gasmtyp));
				gasm[gecnt].r[0].r = globi * 8 + KECX;
				gasm[gecnt].r[1].r = gstnum + KSTR;
				gasm[gecnt].r[2].r = KUNUSED;
				gasm[gecnt].n = 1;
				gasm[gecnt].f = MOV;
				gecnt++;

				checkstrings(gstnum + j - z - 1);
#if 0
				memcpy(&gstring[gstnum],&st[z+1],j-z-2);
				gstring[gstnum+j-z-2] = 0;
				gstnum += j-z-1;
#else
				for (k = z + 1; k < j - 1; k++)
				{
					if ((k < j - 2) && (st[k] == '\\') && (st[k + 1] == '\"'))
						continue;
					gstring[gstnum++] = st[k];
				}
				gstring[gstnum++] = 0;
#endif

				checkops(globi + 2);
				gnext[globi] = globi + 1;
				globi++;
				gop[globi] = NUL;
				break;
			}

			if (j > z)
			{
				ch = st[j];
				st[j] = 0;
#if 1
				// FIXFIXFIX: this version is known to work
				sprintf(kasm87err, "ERROR: %s undefined", &st[z]);
#else
				if (!isaddr) //<- FIXFIXFIX: should autogenerate for this too!
					sprintf(kasm87err, "ERROR: %s undefined", &st[z]);
				else
				{
					checkvarchars(newvarplc + j - z + 2);
					checkvars(newvarnum + 1);
					newvar[newvarnum].nami = newvarplc;
					// Save new variable name&index to list
					if (st[z] == '\"')
					{
						memcpy(&newvarnam[newvarplc], &st[z + 1], j - z - 2);
						newvarplc += j - z - 2;
					}
					else
					{
						memcpy(&newvarnam[newvarplc], &st[z], j - z);
						newvarplc += j - z;
					}
					newvarnam[newvarplc++] = 0;
					newvar[newvarnum].r = globi * 8 + KECX;
					newvar[newvarnum].maxind = 0;
					newvar[newvarnum].parnum = -1;
					newvar[newvarnum].proti = -1;
					k = getnewvarhash(&newvarnam[newvar[newvarnum].nami]);
					newvar[newvarnum].hashn = newvarhash[k];
					newvarhash[k] = newvarnum;
					i = newvarnum;
					newvarnum++;

					st[j] = ch;

					checkops(gecnt + 1);
					memset(&gasm[gecnt], 0, sizeof(gasmtyp));
					gasm[gecnt].r[0].r = globi * 8 + KECX;
					gasm[gecnt].r[0].nv = i;
					gasm[gecnt].r[1].r = gccnt * 8 + KEDX;
					gasm[gecnt].n = 1;
					gasm[gecnt].f = MOV;
					checkops(gccnt + 1);
					globval[gccnt++] = 0.0;
					gasm[gecnt].r[2].r = KUNUSED;
					gasm[gecnt].n = 1;
					gasm[gecnt].f = MOV;
					gecnt++;

					checkops(globi + 2);
					gnext[globi] = globi + 1;
					globi++;
					gop[globi] = NUL;

					oz = z; //+1;
					z = j;
					inquotes = 0;
					continue; // Name was generated; no more processing
					// goto prebegit;
				}
#endif
				st[j] = ch;
			}
			else
				sprintf(kasm87err, "ERROR: %c undefined", st[j]);
			globi = -1;
			return;
		}
	}

	if (gop[globi] != NUL)
	{
		strcpy(kasm87err, "ERROR: ");
		tg.f = gop[globi];
		getfuncnam(&tg, &kasm87err[strlen(kasm87err)]);
		strcat(kasm87err, " missing parameter");
		globi = -1;
		return;
	}

#if 1
	// PEMDAS, goes through entire list for every priority... quite wasteful!
	for (i = 0; i < 7; i++)
		for (z = ogi; gnext[z] < globi;)
		{
			p = (long)oprio[gop[gnext[z]]];
			if (i != p)
			{
				z = gnext[z];
				continue;
			}
			checkops(gecnt + 1);
			memset(&gasm[gecnt], 0, sizeof(gasmtyp));
			gasm[gecnt].r[0].r = gasm[gecnt].r[1].r = z * 8 + KECX;
			gasm[gecnt].r[2].r = gnext[z] * 8 + KECX;
			gasm[gecnt].f = gop[gnext[z]];
			gasm[gecnt].n = 2;
			gecnt++;
			gnext[z] = gnext[gnext[z]]; // Remove 2nd param from linked list
		}
#else
	// PEMDAS, faster algo, keeps circling until done
	// for(i=0;i<8;i++) printf("gop[%d] = %d, gnext[%d] = %d\n",i,gop[i],i,gnext[i]);
	// printf("ogi=%d,globi=%d\n",ogi,globi);
	//
	// gop[0] = 0      gnext[0] = 1    ogi = 0, globi = 5
	// gop[1] = TIMES  gnext[1] = 2
	// gop[2] = PLUS   gnext[2] = 3    r0 = X * X
	// gop[3] = TIMES  gnext[3] = 4    r1 = Y * Y
	// gop[4] = LES    gnext[4] = 5    r1 = r1 < 1
	// gop[5] = 0      gnext[5] = 0    r0 = r0 + r1
	//
	//    1 2 1 3  <priority      2 1 3  <priority         2 3  <priority
	//    * + * <  <gop[i]        + * <  <gop[i]           + <  <gop[i]
	//  x*x+y*y<1  <equation    x+y*y<1  <equation       x+y<1  <equation
	//  ^        0->2->3->4->5  ^ ^      0->2->4->5        ^
	//
	// Find leftmost operator satisfying: my_priority <= next_operator_priority
	z = ogi;
	while (gnext[z] != globi)
	{
		// Can't evaluate operator yet if next operator is higher priority...
		if ((gop[gnext[z]] == NUL) || (oprio[gop[gnext[z]]] > oprio[gop[gnext[gnext[z]]]]))
		{
			z = gnext[z];
			continue;
		}

		checkops(gecnt + 1);
		memset(&gasm[gecnt], 0, sizeof(gasmtyp));
		gasm[gecnt].r[0] = gasm[gecnt].r[1] = z * 8 + KECX;
		gasm[gecnt].r[2] = gnext[z] * 8 + KECX;
		gasm[gecnt].f = gop[gnext[z]];
		gasm[gecnt].n = 2;
		gecnt++;
		gnext[z] = gnext[gnext[z]]; // Remove 2nd param from linked list
		z = ogi;
	}
#endif
}

// This function helps find sequences like this which can be safely removed:
//  fld qword ptr [esp+0x28]
//  fstp qword ptr [esp+0x28]
//  ... (qword ptr [esp+0x28] not used again)
//
//  r0 = ? op ?;
//   ? = r0 + ?;
//  (r0 written before read)
static long anyreads1stop, anyreads1stinst;
static long anyreadsbeforewritesrec(long i, rtyp r)
{
	for (; i < gecnt; i++)
	{
		// static char gmybuf[256];
		// getfuncnam(&gasm[i],gmybuf); //For debugging only
		// printf("%d %d %08x (q:%08x) %s\n",firstop,i,r.r,r.q,gmybuf);

		if (!anyreads1stop)
		{
			rtyp *rp;
			long k;
			for (k = gasm[i].n; k > 0; k--)
			{
				if (k < 3)
					rp = &gasm[i].r[k];
				else
					rp = &rxi[gasm[i].rxi + k - 3];
				if (gasmeq(*rp, r))
					return (1);
			}
		}
		else
			anyreads1stop = 0;

		// Don't go past starting instruction (if in a loop)
		if (anyreads1stinst < 0)
			anyreads1stinst &= 0x7fffffff;
		else if (i == anyreads1stinst)
			return (0);

		if ((gasm[i].f == IF0) || (gasm[i].f == IF1) || (gasm[i].f == GOTO))
		{
			long j = lablinum[gasm[i].r[1].r & 0x0fffffff];
			if (j >= 0) // Don't jump to same label again - already processed
			{
				lablinum[gasm[i].r[1].r & 0x0fffffff] |= 0x80000000;
				if (anyreadsbeforewritesrec(j, r))
					return (1);
			}
		}
		if ((gasm[i].f == GOTO) || (gasm[i].f == RETURN))
			break;
		if (gasmeq(gasm[i].r[0], r))
			return (0);
	}
	return (((r.r & 0xf0000000) == KPTR) || ((r.r & 0xf0000000) == KIMM) || ((r.r & 0xf0000000) == KARR) || ((r.r & 0xf0000000) == KGLB) || (((r.r & 0xf0000000) == KEDX) && ((r.r & 0x0fffffff) >= gccnt * 8 + gstnum)));
}
static long anyreadsbeforewrites(long i, rtyp r, long firstop)
{
	long j;
#if 0
		//This block is just an optimization that failed miserably :/
		//See if labels are already set correctly (numlabels < gecnt, so faster)
	long k;
	for(k=numlabels-1;k>=0;k--)
	{
		j = (lablinum[k]&0x7fffffff); if ((unsigned long)j >= gecnt) break;
		if ((gasm[j].f != NUL) || (gasm[j].r[0].r != k)) break;
		lablinum[k] = j;
	}
	if (k >= 0) //Oh well... must check the whole list again
#endif
	for (j = gecnt - 1; j >= 0; j--)
		if (gasm[j].f == NUL)
			lablinum[gasm[j].r[0].r & 0x0fffffff] = j;

	anyreads1stop = firstop;
	anyreads1stinst = (i | 0x80000000);
	return (anyreadsbeforewritesrec(i, r));
}

void kasm87freeall()
{
#if (COMPILE != 0)
	if (patch)
	{
		free(patch);
		patch = 0;
	}
	maxpatch = 0;
#endif
	if (jumpback)
	{
		free(jumpback);
		jumpback = 0;
	}
	maxjumpbacks = 0;
	if (rxi)
	{
		free(rxi);
		rxi = 0;
	}
	maxrxi = 0;
	if (enumnam)
	{
		free(enumnam);
		enumnam = 0;
	}
	maxenumchars = 0;
	if (enumval)
	{
		free(enumval);
		enumval = 0;
	}
	maxenum = 0;
	if (gasm)
	{
		free(gasm);
		gasm = 0;
	}
	if (lablinum)
	{
		free(lablinum);
		lablinum = 0;
	}
	if (jumpat)
	{
		free(jumpat);
		jumpat = 0;
	}
	if (labpat)
	{
		free(labpat);
		labpat = 0;
	}
	if (newlabind)
	{
		free(newlabind);
		newlabind = 0;
	}
	maxlabs = 0;
	if (newlabnam)
	{
		free(newlabnam);
		newlabnam = 0;
	}
	maxlabchars = 0;
	if (newvar)
	{
		free(newvar);
		newvar = 0;
	}
	maxvars = 0;
	if (newvarnam)
	{
		free(newvarnam);
		newvarnam = 0;
	}
	maxvarchars = 0;
	if (gvl)
	{
		free(gvl);
		gvl = 0;
	}
	if (gstring)
	{
		free(gstring);
		gstring = 0;
	}
	maxst = 0;
	if (ginitval)
	{
		free(ginitval);
		ginitval = 0;
	}
	maxinitval = 0;
	if (globval)
	{
		free(globval);
		globval = 0;
	}
	if (gnext)
	{
		free(gnext);
		gnext = 0;
	}
	if (gop)
	{
		free(gop);
		gop = 0;
	}
	maxops = 0;
	if (funcst)
	{
		free(funcst);
		funcst = 0;
	}
	maxfuncst = 0;
	if (texttrans)
	{
		free(texttrans);
		texttrans = 0;
	}
	texttransmal = 0;
}

// mingecnt: hack telling optimizer not to touch gasm[0 .. mingecnt-1]. Set it to 0 for standard behavior.
// duringparse: 0:safe to rename registers, 1:not safe
static long kasmoptimizations(long mingecnt, long duringparse)
{
	rtyp tr, *rp;
	long i, j, k, l, m, got;

	do // Optimizations!
	{
		//{ char debuf[16384]; kasm87_showdebug(1,debuf,sizeof(debuf)); printf("%s\n",debuf); }
		got = 0;
#if 1
		// Remove duplicate constants
		for (j = gccnt - 1; j >= 0; j--)
			for (i = j - 1; i >= 0; i--)
				if (globval[i] == globval[j]) // Rewire: j to i, then rewire: (gccnt-1) to j
				{
					gccnt--;
					got = 1;
					globval[j] = globval[gccnt];
					for (k = gecnt - 1; k >= 0; k--)
						for (l = gasm[k].n; l >= 0; l--)
						{
							if (l < 3)
								rp = &gasm[k].r[l];
							else
								rp = &rxi[gasm[k].rxi + l - 3];
							if (rp->r == j * 8 + KEDX)
								rp->r = i * 8 + KEDX;
							if (rp->r == gccnt * 8 + KEDX)
								rp->r = j * 8 + KEDX;
						}
					break;
				}
#endif
#if 1
		// Remove dead (unused) constants
		for (i = gccnt - 1; i >= 0; i--)
		{
			for (j = gecnt - 1; j >= 0; j--)
			{
				for (k = gasm[j].n; k > 0; k--)
				{
					if (k < 3)
						rp = &gasm[j].r[k];
					else
						rp = &rxi[gasm[j].rxi + k - 3];
					if (rp->r == i * 8 + KEDX)
						break;
				}
				if (k > 0)
					break;
			}
			if (j < 0) // Constant i not used. Rewire: (gccnt-1) to i
			{
				gccnt--;
				got = 1;
				globval[i] = globval[gccnt];
				for (j = gecnt - 1; j >= 0; j--)
					for (k = gasm[j].n; k > 0; k--)
					{
						if (k < 3)
							rp = &gasm[j].r[k];
						else
							rp = &rxi[gasm[j].rxi + k - 3];
						if (rp->r == gccnt * 8 + KEDX)
							rp->r = i * 8 + KEDX;
					}
			}
		}
#endif
#if 1
		// Convert NEGMOV by constant to MOV (and negate constant)
		for (i = gecnt - 1; i >= 0; i--)
			if ((gasm[i].f == NEGMOV) && ((gasm[i].r[1].r & 0xf0000000) == KEDX))
			{
				gasm[i].f = MOV;
				checkops(gccnt + 1);
				globval[gccnt] = -globval[(gasm[i].r[1].r & 0x0fffffff) >> 3];
				gasm[i].r[1].r = gccnt * 8 + KEDX;
				gccnt++;
				got = 1;
			}
#endif
#if 1
		// Convert NEQU0 to MOV if input guaranteed to be 0.0 or 1.0
		for (i = gecnt - 1; i >= 0; i--)
			if ((gasm[i].f == NEQU0) && ((gasm[i].r[1].r & 0xf0000000) == KECX))
				for (j = i - 1; j >= 0; j--)
					if (gasmeq(gasm[j].r[0], gasm[i].r[1]))
					{
						switch (gasm[j].f)
						{
						case LES:
						case LESEQ:
						case MOR:
						case MOREQ:
						case EQU:
						case NEQU:
						case LAND:
						case LOR:
							gasm[i].f = MOV;
							gasm[i].n = 1;
							got = 1;
							break;
						default:
							break;
						}
						if (gasm[i].f == MOV)
							break;
					}
#endif
#if 1
		// FIXFIXFIX: can't remove this without also taking some other block out ??? (ceilflor2.kc crashes)
		// Remove MOV with same src & dest
		for (i = gecnt - 1; i >= mingecnt; i--)
			if ((gasm[i].f == MOV) && (gasmeq(gasm[i].r[0], gasm[i].r[1])))
			{
				gecnt--;
				got = 1; // Delete MOV instruction and re-wire registers at same time)
				for (j = i; j < gecnt; j++)
					gasm[j] = gasm[j + 1]; // Register is overwritten - simply copy rest now
			}
#endif
#if 1
		// Simplify math expressions, such as POW(x,2), TIMES(x,1), etc...
		for (i = gecnt - 1; i >= mingecnt; i--)
		{
			j = ((gasm[i].r[1].r & 0x0fffffff) >> 3);
			k = ((gasm[i].r[2].r & 0x0fffffff) >> 3);
			switch (gasm[i].f)
			{
			case POW:
				if ((gasm[i].r[1].r & 0xf0000000) == KEDX)
				{
					if (globval[j] == 1.0)
					{
						gasm[i].f = MOV;
						gasm[i].n = 1;
						got = 1;
						break;
					}
				}
				else if ((gasm[i].r[2].r & 0xf0000000) == KEDX)
				{
					if (globval[k] == 4.0)
					{
						checkops(gecnt + 1);
						gecnt++;
						for (j = gecnt - 1; j > i; j--)
							gasm[j] = gasm[j - 1];
						gasm[i].f = TIMES;
						gasm[i].r[2] = gasm[i].r[1];
						gasm[i + 1].f = TIMES;
						gasm[i + 1].r[1] = gasm[i + 1].r[2] = gasm[i + 1].r[0];
						got = 1;
						break;
					}
					else if ((globval[k] == 3.0) && (!gasmeq(gasm[i].r[0], gasm[i].r[1]))) // a = pow(b,3), &a != &b
					{
						checkops(gecnt + 1);
						gecnt++;
						for (j = gecnt - 1; j > i; j--)
							gasm[j] = gasm[j - 1];
						gasm[i].f = TIMES;
						gasm[i].r[2] = gasm[i].r[1];
						gasm[i + 1].f = TIMES;
						gasm[i + 1].r[2] = gasm[i + 1].r[0];
						got = 1;
						break;
					}
					else if (globval[k] == 2.0)
					{
						gasm[i].f = TIMES;
						gasm[i].r[2] = gasm[i].r[1];
						got = 1;
						break;
					}
					else if (globval[k] == 1.0)
					{
						gasm[i].f = MOV;
						gasm[i].r[2].r = KUNUSED;
						gasm[i].n = 1;
						got = 1;
						break;
					}
					else if (globval[k] == 0.0)
					{
						gasm[i].f = MOV;
						gasm[i].r[1].r = gccnt * 8 + KEDX;
						gasm[i].r[2].r = KUNUSED;
						gasm[i].n = 1;
						checkops(gccnt + 1);
						globval[gccnt++] = 1.0;
						got = 1;
						break;
					}
					else if (globval[k] == 0.5)
					{
						gasm[i].f = SQRT;
						gasm[i].r[2].r = KUNUSED;
						gasm[i].n = 1;
						got = 1;
						break;
					}
					else if (globval[k] == -1.0)
					{
						gasm[i].f = SLASH;
						gasm[i].r[2] = gasm[i].r[1];
						gasm[i].r[1].r = gccnt * 8 + KEDX;
						checkops(gccnt + 1);
						globval[gccnt++] = 1.0;
						got = 1;
						break;
					}
				}
				break;
			case TIMES:
				if ((gasm[i].r[1].r & 0xf0000000) == KEDX)
				{
					if (globval[j] == -1.0)
					{
						gasm[i].f = NEGMOV;
						gasm[i].r[1] = gasm[i].r[2];
						gasm[i].r[2].r = KUNUSED;
						gasm[i].n = 1;
						got = 1;
						break;
					}
					else if (globval[j] == 0.0)
					{
						gasm[i].f = MOV;
						gasm[i].r[1].r = gccnt * 8 + KEDX;
						gasm[i].r[2].r = KUNUSED;
						gasm[i].n = 1;
						checkops(gccnt + 1);
						globval[gccnt++] = 0.0;
						got = 1;
						break;
					}
					else if (globval[j] == 1.0)
					{
						gasm[i].f = MOV;
						gasm[i].r[1] = gasm[i].r[2];
						gasm[i].r[2].r = KUNUSED;
						gasm[i].n = 1;
						got = 1;
						break;
					}
					else if (globval[j] == 2.0)
					{
						gasm[i].f = PLUS;
						gasm[i].r[1] = gasm[i].r[2];
						got = 1;
						break;
					}
				}
				else if ((gasm[i].r[2].r & 0xf0000000) == KEDX)
				{
					if (globval[k] == -1.0)
					{
						gasm[i].f = NEGMOV;
						gasm[i].r[2].r = KUNUSED;
						gasm[i].n = 1;
						got = 1;
						break;
					}
					else if (globval[k] == 0.0)
					{
						gasm[i].f = MOV;
						gasm[i].r[1].r = gccnt * 8 + KEDX;
						gasm[i].r[2].r = KUNUSED;
						gasm[i].n = 1;
						checkops(gccnt + 1);
						globval[gccnt++] = 0.0;
						got = 1;
						break;
					}
					else if (globval[k] == 1.0)
					{
						gasm[i].f = MOV;
						gasm[i].r[2].r = KUNUSED;
						gasm[i].n = 1;
						got = 1;
						break;
					}
					else if (globval[k] == 2.0)
					{
						gasm[i].f = PLUS;
						gasm[i].r[2] = gasm[i].r[1];
						got = 1;
						break;
					}
				}
				break;
			case SLASH:
				if ((gasm[i].r[2].r & 0xf0000000) == KEDX)
				{
					if (globval[k] == -1.0)
					{
						gasm[i].f = NEGMOV;
						gasm[i].r[2].r = KUNUSED;
						gasm[i].n = 1;
						got = 1;
						break;
					}
					if (globval[k] == 1.0)
					{
						gasm[i].f = MOV;
						gasm[i].r[2].r = KUNUSED;
						gasm[i].n = 1;
						got = 1;
						break;
					}
					if (globval[k] == 0.5)
					{
						gasm[i].f = PLUS;
						gasm[i].r[2] = gasm[i].r[1];
						got = 1;
						break;
					}
					// if (!((*(__int64 *)&globval[k])&0x000fffffffffffff)) //check for exact reciprocal (power of 2)
					{
						gasm[i].f = TIMES;
						checkops(gccnt + 1);
						globval[gccnt] = 1.0 / globval[k];
						gasm[i].r[2].r = gccnt * 8 + KEDX;
						checkops(gccnt + 1);
						gccnt++;
						got = 1;
						break;
					}
				}
				break;
			case PLUS:
				if (((gasm[i].r[2].r & 0xf0000000) == KEDX) && (globval[k] == 0.0))
				{
					gasm[i].f = MOV;
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					got = 1;
					break;
				}
				if (((gasm[i].r[1].r & 0xf0000000) == KEDX) && (globval[j] == 0.0))
				{
					gasm[i].f = MOV;
					gasm[i].r[1] = gasm[i].r[2];
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					got = 1;
					break;
				}
				break;
			case MINUS:
				if (((gasm[i].r[2].r & 0xf0000000) == KEDX) && (globval[k] == 0.0))
				{
					gasm[i].f = MOV;
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					got = 1;
					break;
				}
				if (((gasm[i].r[1].r & 0xf0000000) == KEDX) && (globval[j] == 0.0))
				{
					gasm[i].f = NEGMOV;
					gasm[i].r[1] = gasm[i].r[2];
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					got = 1;
					break;
				}
				if (gasmeq(gasm[i].r[1], gasm[i].r[2]))
				{
					gasm[i].f = MOV;
					gasm[i].r[1].r = gccnt * 8 + KEDX;
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					checkops(gccnt + 1);
					globval[gccnt++] = 0.0;
					got = 1;
					break;
				}
				break;
			case NEQU:
				if (((gasm[i].r[2].r & 0xf0000000) == KEDX) && (globval[k] == 0.0))
				{
					gasm[i].f = NEQU0;
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					got = 1;
					break;
				}
				if (((gasm[i].r[1].r & 0xf0000000) == KEDX) && (globval[j] == 0.0))
				{
					gasm[i].f = NEQU0;
					gasm[i].r[1] = gasm[i].r[2];
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					got = 1;
					break;
				}
				break;
			case LAND:
				if (((gasm[i].r[2].r & 0xf0000000) == KEDX) && (globval[k] == 0.0))
				{
					gasm[i].f = MOV;
					gasm[i].r[1].r = gccnt * 8 + KEDX;
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					checkops(gccnt + 1);
					globval[gccnt++] = 0.0;
					got = 1;
					break;
				}
				if (((gasm[i].r[1].r & 0xf0000000) == KEDX) && (globval[j] == 0.0))
				{
					gasm[i].f = MOV;
					gasm[i].r[1].r = gccnt * 8 + KEDX;
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					checkops(gccnt + 1);
					globval[gccnt++] = 0.0;
					got = 1;
					break;
				}
				if (((gasm[i].r[2].r & 0xf0000000) == KEDX) && (globval[k] != 0.0))
				{
					gasm[i].f = NEQU0;
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					got = 1;
					break;
				}
				if (((gasm[i].r[1].r & 0xf0000000) == KEDX) && (globval[j] != 0.0))
				{
					gasm[i].f = NEQU0;
					gasm[i].r[1] = gasm[i].r[2];
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					got = 1;
					break;
				}
				break;
			case LOR:
				if (((gasm[i].r[2].r & 0xf0000000) == KEDX) && (globval[k] != 0.0))
				{
					gasm[i].f = MOV;
					gasm[i].r[1].r = gccnt * 8 + KEDX;
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					checkops(gccnt + 1);
					globval[gccnt++] = 1.0;
					got = 1;
					break;
				}
				if (((gasm[i].r[1].r & 0xf0000000) == KEDX) && (globval[j] != 0.0))
				{
					gasm[i].f = MOV;
					gasm[i].r[1].r = gccnt * 8 + KEDX;
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					checkops(gccnt + 1);
					globval[gccnt++] = 1.0;
					got = 1;
					break;
				}
				if (((gasm[i].r[2].r & 0xf0000000) == KEDX) && (globval[k] == 0.0))
				{
					gasm[i].f = NEQU0;
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					got = 1;
					break;
				}
				if (((gasm[i].r[1].r & 0xf0000000) == KEDX) && (globval[j] == 0.0))
				{
					gasm[i].f = NEQU0;
					gasm[i].r[1] = gasm[i].r[2];
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					got = 1;
					break;
				}
				break;
			case ATAN2:
				if (((gasm[i].r[2].r & 0xf0000000) == KEDX) && (globval[k] == 1.0))
				{
					gasm[i].f = ATAN;
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					got = 1;
					break;
				}
				break;
			}
		}
#endif
#if 1
		// Evaluate expressions based purely on constants
		for (i = gecnt - 1; i >= mingecnt; i--)
		{
			if ((gasm[i].f == IF0) || (gasm[i].f == IF1))
			{
				if ((gasm[i].r[2].r & 0xf0000000) == KEDX)
				{
					if ((gasm[i].f == IF0) == (globval[(gasm[i].r[2].r & 0x0fffffff) >> 3] == 0.0))
					{
						got = 1;
						gasm[i].f = GOTO;
						gasm[i].r[2].r = KUNUSED;
						gasm[i].n = 1;
					}
					else
					{
						gecnt--;
						got = 1; // Delete instruction [i]
						for (j = i; j < gecnt; j++)
							gasm[j] = gasm[j + 1];
						i++;
					}
				}
				continue;
			}
			if ((gasm[i].f == GOTO) || (gasm[i].f == RETURN))
				continue;
			if ((gasm[i].f < PARAM1) || (gasm[i].f == MOV) || (gasm[i].f == NEGMOV))
				continue;
			if (gasm[i].f < PARAM2)
				gasm[i].r[2].r = KUNUSED;
			if (((gasm[i].r[2].r & 0xf0000000) == KEDX) &&
				(((gasm[i].r[1].r & 0xf0000000) == KIMM) || ((gasm[i].r[1].r & 0xf0000000) == KPTR) || ((gasm[i].r[1].r & 0xf0000000) == KARR) || ((gasm[i].r[1].r & 0xf0000000) == KGLB)))
			{
				double p2;
				if (gasm[i].f == PEEK)
				{
					p2 = *(double *)(((long)globval) + gasm[i].r[2].r - KEDX);
					gasm[i].r[1].q = (long)p2;
					if ((unsigned long)gasm[i].r[1].q >= (unsigned long)newvar[gasm[i].r[1].nv].maxind)
					{
						sprintf(kasm87err, "ERROR: array index out of bounds");
						return (-1);
					}
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					gasm[i].f = MOV;
					got = 1;
					continue;
				}
				if (gasm[i].f == POKE)
				{
					p2 = *(double *)(((long)globval) + gasm[i].r[2].r - KEDX);
					gasm[i].r[0] = gasm[i].r[1];
					gasm[i].r[0].q = (long)p2;
					if ((unsigned long)gasm[i].r[0].q >= (unsigned long)newvar[gasm[i].r[0].nv].maxind)
					{
						sprintf(kasm87err, "ERROR: array index out of bounds");
						return (-1);
					}
					gasm[i].r[1] = rxi[gasm[i].rxi]; // FIX: deallocate spot on rxi?
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					gasm[i].f = MOV;
					got = 1;
					continue;
				}
				if ((gasm[i].f == POKETIMES) || (gasm[i].f == POKESLASH) || (gasm[i].f == POKEPERC) ||
					(gasm[i].f == POKEPLUS) || (gasm[i].f == POKEMINUS))
				{
					p2 = *(double *)(((long)globval) + gasm[i].r[2].r - KEDX);
					gasm[i].r[1].q = (long)p2;
					if ((unsigned long)gasm[i].r[1].q >= (unsigned long)newvar[gasm[i].r[1].nv].maxind)
					{
						sprintf(kasm87err, "ERROR: array index out of bounds");
						return (-1);
					}
					gasm[i].r[0] = gasm[i].r[1];
					gasm[i].r[2] = rxi[gasm[i].rxi]; // FIX: deallocate spot on rxi?
					gasm[i].n = 2;
					switch (gasm[i].f)
					{
					case POKETIMES:
						gasm[i].f = TIMES;
						break;
					case POKESLASH:
						gasm[i].f = SLASH;
						break;
					case POKEPERC:
						gasm[i].f = PERC;
						break;
					case POKEPLUS:
						gasm[i].f = PLUS;
						break;
					case POKEMINUS:
						gasm[i].f = MINUS;
						break;
					}
					got = 1;
					continue;
				}
			}
			if (((gasm[i].r[1].r & 0xf0000000) == KEDX) && ((gasm[i].r[2].r & 0xf0000000) == KEDX))
			{
				double p0, *p1, *p2;
				p1 = (double *)(((long)globval) + gasm[i].r[1].r - KEDX);
				p2 = (double *)(((long)globval) + gasm[i].r[2].r - KEDX);
				j = 0;
				switch (gasm[i].f)
				{
				case NEQU0:
					p0 = ((*p1) != 0.0);
					break;
				case FABS:
					p0 = fabs(*p1);
					break;
				case SGN:
					p0 = ((*p1) > 0.0) - ((*p1) < 0.0);
					break;
				case UNIT:
					p0 = ((*p1) == 0.0) * .5 + ((*p1) > 0.0);
					break;
				case FLOOR:
					p0 = floor(*p1);
					break;
				case CEIL:
					p0 = ceil(*p1);
					break;
				case ROUND0:
				case ROUND0_32:
					if (*p1 >= 0)
						p0 = floor(*p1);
					else
						p0 = -floor(-(*p1));
					break;
				case SIN:
					p0 = sin(*p1);
					break;
				case COS:
					p0 = cos(*p1);
					break;
				case TAN:
					p0 = tan(*p1);
					break;
				case ASIN:
					p0 = asin(*p1);
					break;
				case ACOS:
					p0 = acos(*p1);
					break;
				case ATAN:
					p0 = atan(*p1);
					break;
				case SQRT:
					p0 = sqrt(*p1);
					break;
				case EXP:
					p0 = exp(*p1);
					break;
				case FACT:
					p0 = fact(*p1);
					break;
				case LOG:
					p0 = log(*p1);
					break;
				case TIMES:
					p0 = (*p1) * (*p2);
					break;
				case SLASH:
					p0 = (*p1) / (*p2);
					break;
				case PERC:
					p0 = (*p1) - floor((*p1) / fabs(*p2)) * fabs(*p2);
					break;
				case PLUS:
					p0 = (*p1) + (*p2);
					break;
				case MINUS:
					p0 = (*p1) - (*p2);
					break;
				case POW:
					p0 = pow(*p1, *p2);
					break;
				case MIN:
					if ((*p2) < (*p1))
						p0 = (*p2);
					else
						p0 = (*p1);
					break;
				case MAX:
					if ((*p2) > (*p1))
						p0 = (*p2);
					else
						p0 = (*p1);
					break;
				case FADD:
					j = -1;
					break; // NOTE! The entire purpose of FADD is to NOT optimize it here! Keep this as a placeholder.
				case FMOD:
					p0 = fmod(*p1, *p2);
					break;
				case ATAN2:
					p0 = atan2(*p1, *p2);
					break;
				case LOGB:
					p0 = log(*p1) / log(*p2);
					break;
				case LES:
					p0 = ((*p1) < (*p2));
					break;
				case LESEQ:
					p0 = ((*p1) <= (*p2));
					break;
				case MOR:
					p0 = ((*p1) > (*p2));
					break;
				case MOREQ:
					p0 = ((*p1) >= (*p2));
					break;
				case EQU:
					p0 = ((*p1) == (*p2));
					break;
				case NEQU:
					p0 = ((*p1) != (*p2));
					break;
				case LAND:
					p0 = ((*p1) && (*p2));
					break;
				case LOR:
					p0 = ((*p1) || (*p2));
					break;
				default:
					j = -1;
					break;
				}
				if (!j)
				{
					gasm[i].r[1].r = gccnt * 8 + KEDX;
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					gasm[i].f = MOV;
					checkops(gccnt + 1);
					globval[gccnt++] = p0;
					got = 1;
				}
			}
		}
#endif
#if 0
			//This block is no longer necessary with register renaming, but it speeds things up.
			//I disable it for safety reasons (because code isn't checked as carefully as register renaming)
			//
			//Remove unnecessary MOV instructions, replacing src's in later code with dest of this mov
			// r1 = X;
			// r3 = r1 + r2;   ->  r3 = X + r2;
			// r4 = sqrt(r1);      r4 = sqrt(X);
		for(i=gecnt-2;i>=mingecnt;i--)
			if ((gasm[i].f == MOV) && ((gasm[i].r[0].r&0xf0000000) == KECX))
			{
				k = gasm[i].r[0].r; tr = gasm[i].r[1];

					//When writing a variable in a loop that reads the variable ABOVE the write, it can't be
					//optimized. To be safe, make sure dest of mov (gasm[i].r[0]) isn't a 'named' variable!
				for(j=gnumarg;j<newvarnum;j++)
					if (newvar[j].r == k) break;
				if (j < newvarnum) continue; //finish abort

				m = 0;
				for(j=i+1;j<gecnt;j++) //temp register is used later... abort optimization
				{
						//writes register in other code that may or may not be executed... must abort
					if ((m) && ((gasm[j].r[0].r == k) || (gasm[j].r[1].r == k) || (gasm[j].r[2].r == k))) break;
					if ((gasm[j].f == NUL) || (gasm[j].f == IF0) || (gasm[j].f == IF1) || (gasm[j].f == GOTO) || (gasm[j].f == RETURN)) m = 1;
				}
				if (j < gecnt) continue; //finish abort

					//Make sure source of mov (tr) doesn't get written before last access of (k)
				for(j=i+1;j<gecnt;j++)
					if (gasmeq(gasm[j].r[0],tr))
					{
						for(j++;j<gecnt;j++)
							if ((gasm[j].r[0].r == k) || (gasm[j].r[1].r == k) || (gasm[j].r[2].r == k)) break;
						break;
					}
				if (j < gecnt) continue; //finish abort

				gecnt--; got = 1; //Delete MOV instruction and re-wire registers at same time)
				for(j=i;j<gecnt;j++)
				{
					gasm[j] = gasm[j+1];
					if (gasm[j].r[1].r == k) gasm[j].r[1] = tr;
					if (gasm[j].r[2].r == k) gasm[j].r[2] = tr;
					if (gasm[j].r[0].r == k) { j++; break; }
				}
				for(;j<gecnt;j++) gasm[j] = gasm[j+1]; //Register is overwritten - simply copy rest now
			}
#endif
#if 1
		// Register renaming: (when result of calculation is used only once)
		//
		//          Change:       To:
		//  inst i: r2 = ? +r3 -> r1 = ? +r3   (Later optimizations may remove r2)
		//  inst j: r1 = r2+r4    r1 = r1+r4
		//
		// Special case for MOV instructions:
		//          Change:       To:
		//  inst i: r2 = ?     -> r2 = ?       (Later optimizations may remove r2)
		//  inst j: r1 = r2+r4    r1 = ? +r4
		//
		///            ÚÄÄÄÂÄÄÄÂÄÄÄÂÄÄÄ¿
		///   Rules:   ³ i ³...³ j ³...³ ³
		///ÚÄÄÄÄÄÄÄÄÄÄÄÅÄÄÄÅÄÄÄÅÄÄÄÅÄÄÄ´ ³ 1. i < (r1 access) < j not allowed
		///³ r0 read   ³ û ³ X ³ X ³ û ³ ³ 2. (first r1 access) > j must be write
		///³ r0 write  ³ û ³ X ³ û ³ û ³ ³ 3. (r0 read) at j not allowed
		///³ r1 read   ³ û ³ X ³(X)³>W ³ ³ 4. i < (r0 access) < j not allowed
		///³ r1 write  ³ û ³ X ³ û ³<R ³ ³ 5. (r1 read) at i not allowed if inside possible loop
		///ÀÄÄÄÄÄÄÄÄÄÄÄÁÄÄÄÁÄÄÄÁÄÄÄÁÄÄÄÙ ³
		// 06/23/2004: Above rules rewritten to use anyreadsbeforewrite&now much cleaner!
		for (i = gecnt - 1; i >= mingecnt; i--)
		{
			if ((gasm[i].r[0].r & 0xf0000000) != KECX)
				continue;
			for (j = i + 1; j < gecnt; j++)
			{
				if ((gasm[j].f == NUL) || (gasm[j].f == GOTO))
					break;

				for (k = gasm[j].n; k > 0; k--)
				{
					if (k < 3)
						rp = &gasm[j].r[k];
					else
						rp = &rxi[gasm[j].rxi + k - 3];
					if (gasmeq(gasm[i].r[0], *rp))
						break;
				}

				if (k > 0)
				{
					// Can't rename function parameters that are pointers:
					if ((gasm[j].f == USERFUNC) && (newvarnam[newvar[gasm[j].g].proti + k - 1] <= 'Z'))
						break;

					// Another case:       .f  .r[0]   .r[1]   .r[2]
					// m2 = 0              MOV m2      0       KUNUSED
					// IF !(m2) GOTO l1    IF0 KUNUSED m2      l1
					if (gasm[i].f == MOV)
					{
						for (k = i + 1; k < j; k++) // Make sure no instructions in between write the register
						{
							if (gasmeq(gasm[i].r[1], gasm[k].r[0]))
								break;
							if (gasm[k].f == USERFUNC) // pointer params can write register..
							{
								for (l = gasm[k].n; l > 0; l--)
								{
									m = newvarnam[newvar[gasm[k].g].proti + l - 1];
									if (m > 'Z')
										continue; // not a pointer param
									if (l < 3)
										rp = &gasm[k].r[l];
									else
										rp = &rxi[gasm[k].rxi + l - 3];
									if ((gasm[i].r[1].r == rp->r) && (gasm[i].r[1].q == rp->q))
										break; // pointer matches var
								}
								if (l > 0)
									break;
							}
						}
						if (k < j)
							break;

						// All is good!
						for (k = gasm[j].n; k > 0; k--)
						{
							if (k < 3)
								rp = &gasm[j].r[k];
							else
								rp = &rxi[gasm[j].rxi + k - 3];
							if (gasmeq(gasm[i].r[0], *rp))
								*rp = gasm[i].r[1];
						}

						got = 1;
						break;
					}
					else
					{
						// Don't allow "nop" to set got = 1; (would result in endless loop)
						if (gasmeq(gasm[i].r[0], gasm[j].r[0]))
							break;
						if (gasm[j].r[0].r == KUNUSED)
							break;

						// Don't allow this to happen:
						// m8 = m8 * 3      m4 = m8 * 3
						// m4 = m5 + 1  ->  m4 = m5 + 1
						// m4 = m4 - m8     m4 = m4 - m4
						for (k = i + 1; k < j; k++)
							if (gasmeq(gasm[j].r[0], gasm[k].r[0]))
								break;
						if (k < j)
							break;

						// beg:
						// i: r3 = r2 * r2
						// j: r2 = r3
						//    goto beg
						if (anyreadsbeforewrites(j, gasm[i].r[0], 1))
							break;
						if (anyreadsbeforewrites(i, gasm[j].r[0], 1))
							break;

						// All is good!
						for (k = gasm[j].n; k > 0; k--)
						{
							if (k < 3)
								rp = &gasm[j].r[k];
							else
								rp = &rxi[gasm[j].rxi + k - 3];
							if (gasmeq(gasm[i].r[0], *rp))
								*rp = gasm[j].r[0];
						}
						gasm[i].r[0] = gasm[j].r[0];
						got = 1;

						break;
					}
				}
				if (gasmeq(gasm[i].r[0], gasm[j].r[0]))
					break;
				if (gasm[j].r[0].r == KUNUSED)
					break;
			}
		}
#endif
#if 1
		// Compact unused registers
		if (!duringparse)
		{
			k = KECX;
			for (i = gecnt - 1; i >= mingecnt; i--)
			{
				if ((gasm[i].r[0].r < k) || ((gasm[i].r[0].r & 0xf0000000) != KECX))
					continue;
				for (j = i + 1; j < gecnt; j++)
					if (gasmeq(gasm[i].r[0], gasm[j].r[0]))
						break;
				if (j >= gecnt)
				{
					if (gasm[i].r[0].r != k)
					{ // swap registers: k,gasm[i].r[0]
						got = 1;
						m = gasm[i].r[0].r;
						for (j = gecnt - 1; j >= 0; j--)
							for (l = gasm[j].n; l >= 0; l--)
							{
								if (l < 3)
									rp = &gasm[j].r[l];
								else
									rp = &rxi[gasm[j].rxi + l - 3];
								if (rp->r == k)
									rp->r = m;
								else if (rp->r == m)
									rp->r = k;
							}

						// Make sure variable names match their associated registers (for flow control)
						for (j = gnumarg; j < newvarnum; j++)
						{
							if (newvar[j].r == k)
								newvar[j].r = m;
							else if (newvar[j].r == m)
								newvar[j].r = k;
						}
					}
					k += 8;
				}
			}
		}
#endif
#if 1
		// Remove dead code, for example: { r0 = cos(r1); r0 = i1; } -> { r0 = i1; }
		for (i = gecnt - 2; i >= mingecnt; i--)
		{ // Can't remove labels or jumps
			if ((gasm[i].f == NUL) || (gasm[i].f == USERFUNC) || (gasm[i].r[0].r == KUNUSED))
				continue;
			if (!anyreadsbeforewrites(i + 1, gasm[i].r[0], 0))
			{
				gecnt--;
				got = 1; // Delete instruction [i]
				for (j = i; j < gecnt; j++)
					gasm[j] = gasm[j + 1];
			}
		}
#endif
#if 1
		// Change IF0 to IF1 if:
		//    1.Next line is GOTO, and...
		//    2.Dest of IF's GOTO is exactly 2 lines ahead (this is important!)
		//"if !(m0) goto l1; goto l2;l1:"  ->  "if (m0) goto l2"
		for (i = gecnt - 3; i >= mingecnt; i--)
			if ((gasm[i].f == IF0) &&
				(gasm[i + 1].f == GOTO) &&
				((gasm[i + 2].f == NUL) && (gasmeq(gasm[i].r[1], gasm[i + 2].r[0]))))
			{
				gasm[i].f = IF1;
				gasm[i].r[1] = gasm[i + 1].r[1];
				gecnt--;
				got = 1; // Delete instruction i+1
				for (j = i + 1; j < gecnt; j++)
					gasm[j] = gasm[j + 1];
			}
#endif
#if 1
		// Remove GOTO/IF0/IF1 if its label points to next line
		for (i = gecnt - 2; i >= mingecnt; i--)
			if (((gasm[i].f == GOTO) || (gasm[i].f == IF0) || (gasm[i].f == IF1)) && (gasm[i + 1].f == NUL) && (gasmeq(gasm[i].r[1], gasm[i + 1].r[0])))
			{
				gecnt--;
				got = 1; // Delete instruction i
				for (j = i; j < gecnt; j++)
					gasm[j] = gasm[j + 1];
			}
#endif
#if 1
		// Remove anything after a GOTO or RETURN that isn't a label
		for (i = gecnt - 2; i > mingecnt; i--)
			if (((gasm[i - 1].f == GOTO) || (gasm[i - 1].f == RETURN)) && (gasm[i].f != NUL))
			{
				for (k = i + 1; k < gecnt; k++)
					if (gasm[k].f == NUL)
						break;
				k -= i;

				gecnt -= k;
				got = 1; // Delete instructions {i .. i+k-1}
				for (j = i; j < gecnt; j++)
					gasm[j] = gasm[j + k];
			}
#endif
#if 1
		// Remove unused labels (except for gasm[0] which is used as dummy filler later)
		for (i = gecnt - 1; i > mingecnt; i--)
			if (gasm[i].f == NUL)
			{
				for (j = gecnt - 1; j >= 0; j--)
					if (((gasm[j].f == IF0) || (gasm[j].f == IF1) || (gasm[j].f == GOTO)) &&
						(gasmeq(gasm[j].r[1], gasm[i].r[0])))
						break;
				if (j < 0)
				{
					k = gasm[i].r[0].r;

					gecnt--;
					got = 1; // Delete instruction i (label)
					for (j = i; j < gecnt; j++)
						gasm[j] = gasm[j + 1];

					// Re-wire labels (can't have holes!)
					numlabels--;
					for (j = gecnt - 1; j >= 0; j--)
					{
						if (gasm[j].f == NUL)
							m = 0;
						else
							m = 1;
						if (gasm[j].r[m].r == (signed)KEIP + numlabels)
							gasm[j].r[m].r = k;
					}
				}
			}
#endif
	} while (got);
	return (0);
}

#if (COMPILE == 0)

// kasm87c: similar functionality to kasm87, but pure C code - making it slower and more portable

// ANSI va_arg: supported on all compilers
#include <stdarg.h>

static long gkasm87cptr;
typedef struct
{
	// 0xc7 0x05 [gkasm87cptr] [imm32] ;mov gkasm87cptr, kcd
	// 0xe9 [imm32]                    ;jmp kasm87c()-eip;
	char codestub[16]; // Must be first in structure

	double *globval;
	long gccnt, gstnum, arrnum;
	gasmtyp *gasm;
	long gecnt;
	rtyp *rxi;
	long numrxi;
	evalextyp *gevalext;
	long gevalextnum;
	newvartyp *newvar;
	long newvarnum, gnumarg;
	char *newvarnam;
	long newvarplc;
	long stackdoubs;

	char data[0]; // Must be last in structure
} kcd_t;		  // Kasm87C Data

double kasm87c_run(char *parmdat, kcd_t *kcd)
{
	double *p[17];
	long i, j, k, plst[16];

	if (kcd->gecnt <= 0)
		return (0.0);

	plst[((unsigned long)KECX) >> 28] = ((long)gvlp - KECX);
	plst[((unsigned long)KEDX) >> 28] = ((long)kcd->globval - KEDX);
	plst[((unsigned long)KESP) >> 28] = ((long)parmdat - KESP);
	plst[((unsigned long)KPTR) >> 28] = ((long)parmdat - KPTR);
	plst[((unsigned long)KIMM) >> 28] = ((long)-KIMM);
	plst[((unsigned long)KGLB) >> 28] = ((long)gstatmem - KGLB);

	gvlp += kcd->stackdoubs; // FIX:could do stack overflow check here
	for (i = 0; i < kcd->gecnt; i++)
	{
		//{
		// char mybuf[260]; getfuncnam(&kcd->gasm[i],mybuf); //For debugging only
		// printf("%3d: %10s %08x %08x %08x\n",i,mybuf,kcd->gasm[i].r[0].r,kcd->gasm[i].r[1].r,kcd->gasm[i].r[2].r);
		//}

		for (j = 2; j >= 0; j--)
		{
			p[j] = (double *)(plst[((unsigned long)kcd->gasm[i].r[j].r) >> 28] + (long)kcd->gasm[i].r[j].r);
			if ((kcd->gasm[i].r[j].r & 0xf0000000) == KPTR)
				p[j] = (*(double **)p[j]) + (kcd->gasm[i].r[j].q);
			if ((kcd->gasm[i].r[j].r & 0xf0000000) == KIMM)
				p[j] = (double *)(((long)kcd->gevalext[((long)p[j])].ptr) + kcd->gasm[i].r[j].q * 8);
			if ((kcd->gasm[i].r[j].r & 0xf0000000) == KGLB)
				p[j] = (double *)((gstatmem + ((long)p[j])) + kcd->gasm[i].r[j].q * 8);
			if ((kcd->gasm[i].r[j].r & 0xf0000000) == KEDX)
				p[j] += kcd->gasm[i].r[j].q;
		}
		switch (kcd->gasm[i].f)
		{
		case NUL:
			break;
		case GOTO:
			i = kcd->gasm[i].r[0].r;
			break; // r[1].r is label, r[0].r is gasm index
		case RETURN:
			gvlp -= kcd->stackdoubs;
			return (*p[1]);
		case RND:
			(*p[0]) = ((double)krand()) * (double)oneover2_31;
			break;
		case NRND:
			(*p[0]) = nrnd();
			break;
		case MOV:
			(*p[0]) = (*p[1]);
			break;
		case NEGMOV:
			(*p[0]) = -(*p[1]);
			break;
		case NEQU0:
			(*p[0]) = ((*p[1]) != 0);
			break;
		case IF0:
			if ((*p[2]) == 0)
			{
				i = kcd->gasm[i].r[0].r;
			}
			break; // r[1].r is label, r[0].r is gasm index
		case IF1:
			if ((*p[2]) != 0)
			{
				i = kcd->gasm[i].r[0].r;
			}
			break; // r[1].r is label, r[0].r is gasm index
		case FABS:
			(*p[0]) = fabs(*p[1]);
			break;
		case SGN:
			(*p[0]) = ((*p[1]) > 0) - ((*p[1]) < 0);
			break;
		case UNIT:
			(*p[0]) = ((*p[1]) == 0) * .5 + ((*p[1]) > 0);
			break;
		case FLOOR:
			(*p[0]) = floor(*p[1]);
			break;
		case CEIL:
			(*p[0]) = ceil(*p[1]);
			break;
		case ROUND0:
		case ROUND0_32:
			if (*p[1] >= 0)
				(*p[0]) = floor(*p[1]);
			else
				(*p[0]) = -floor(-(*p[1]));
			break;
		case SIN:
			(*p[0]) = sin(*p[1]);
			break;
		case COS:
			(*p[0]) = cos(*p[1]);
			break;
		case TAN:
			(*p[0]) = tan(*p[1]);
			break;
		case ASIN:
			(*p[0]) = asin(*p[1]);
			break;
		case ACOS:
			(*p[0]) = acos(*p[1]);
			break;
		case ATAN:
			(*p[0]) = atan(*p[1]);
			break;
		case SQRT:
			(*p[0]) = sqrt(*p[1]);
			break;
		case EXP:
			(*p[0]) = exp(*p[1]);
			break;
		case FACT:
			(*p[0]) = fact(*p[1]);
			break;
		case LOG:
			(*p[0]) = log(*p[1]);
			break;
		case TIMES:
			(*p[0]) = (*p[1]) * (*p[2]);
			break;
		case SLASH:
			(*p[0]) = (*p[1]) / (*p[2]);
			break;
		case PERC:
			(*p[0]) = (*p[1]) - floor((*p[1]) / fabs(*p[2])) * fabs(*p[2]);
			break;
		case PLUS: // no break intentional
		case FADD:
			(*p[0]) = (*p[1]) + (*p[2]);
			break;
		case MINUS:
			(*p[0]) = (*p[1]) - (*p[2]);
			break;
		case POW:
			(*p[0]) = pow(*p[1], *p[2]);
			break;
		case MIN:
			if ((*p[2]) < (*p[1]))
				(*p[0]) = (*p[2]);
			else
				(*p[0]) = (*p[1]);
			break;
		case MAX:
			if ((*p[2]) > (*p[1]))
				(*p[0]) = (*p[2]);
			else
				(*p[0]) = (*p[1]);
			break;
		case FMOD:
			(*p[0]) = fmod(*p[1], *p[2]);
			break;
		case ATAN2:
			(*p[0]) = atan2(*p[1], *p[2]);
			break;
		case LOGB:
			(*p[0]) = log(*p[1]) / log(*p[2]);
			break;
		case LES:
			(*p[0]) = ((*p[1]) < (*p[2]));
			break;
		case LESEQ:
			(*p[0]) = ((*p[1]) <= (*p[2]));
			break;
		case MOR:
			(*p[0]) = ((*p[1]) > (*p[2]));
			break;
		case MOREQ:
			(*p[0]) = ((*p[1]) >= (*p[2]));
			break;
		case EQU:
			(*p[0]) = ((*p[1]) == (*p[2]));
			break;
		case NEQU:
			(*p[0]) = ((*p[1]) != (*p[2]));
			break;
		case LAND:
			(*p[0]) = ((*p[1]) && (*p[2]));
			break;
		case LOR:
			(*p[0]) = ((*p[1]) || (*p[2]));
			break;
		case PEEK:
		{
			j = (long)(*p[2]);
			k = kcd->newvar[kcd->gasm[i].r[1].nv].maxind; // quick&dirty bounds check; if 2^x, use "and"
			if ((k) && (!((k - 1) & k)))
				j &= (k - 1);
			else if ((unsigned long)j >= (unsigned long)k)
				j = 0;
			(*p[0]) = p[1][j];
		}
		break;
		case POKE:
		case POKETIMES:
		case POKESLASH:
		case POKEPERC:
		case POKEPLUS:
		case POKEMINUS:
		{
			rtyp *rp = &kcd->rxi[kcd->gasm[i].rxi];
			p[3] = (double *)(plst[((unsigned long)rp->r) >> 28] + (long)rp->r);
			if ((rp->r & 0xf0000000) == KPTR)
				p[3] = (*(double **)p[3]) + (rp->q);
			if ((rp->r & 0xf0000000) == KIMM)
				p[3] = (double *)(((long)kcd->gevalext[((long)p[3])].ptr) + rp->q * 8);
			if ((rp->r & 0xf0000000) == KGLB)
				p[3] = (double *)((gstatmem + ((long)p[3])) + rp->q * 8);

			j = (long)(*p[2]);
			k = kcd->newvar[kcd->gasm[i].r[1].nv].maxind; // quick&dirty bounds check; if 2^x, use "and"
			if ((k) && (!((k - 1) & k)))
				j &= (k - 1);
			else if ((unsigned long)j >= (unsigned long)k)
				j = 0;
			switch (kcd->gasm[i].f)
			{
			case POKE:
				p[1][j] = (*p[3]);
				break;
			case POKETIMES:
				p[1][j] *= (*p[3]);
				break;
			case POKESLASH:
				p[1][j] /= (*p[3]);
				break;
			case POKEPERC:
				p[1][j] -= floor((p[1][j]) / fabs(*p[3])) * fabs(*p[3]);
				break;
			case POKEPLUS:
				p[1][j] += (*p[3]);
				break;
			case POKEMINUS:
				p[1][j] -= (*p[3]);
				break;
			}
		}
		break;
		case USERFUNC:
		{
			char *cptr;
			rtyp *rp;
			double(__cdecl * dafunc)(double, ...);
			if (kcd->gasm[i].n > 16)
			{
				gvlp -= kcd->stackdoubs;
				return (*p[0]);
			} // Display error!
			for (j = kcd->gasm[i].n; j > 2; j--)
			{
				rp = &kcd->rxi[kcd->gasm[i].rxi + j - 3];
				p[j] = (double *)(plst[((unsigned long)rp->r) >> 28] + (long)rp->r);
				if ((rp->r & 0xf0000000) == KPTR)
					p[j] = (*(double **)p[j]) + (rp->q);
				if ((rp->r & 0xf0000000) == KIMM)
					p[j] = (double *)(((long)kcd->gevalext[((long)p[j])].ptr) + rp->q * 8);
				if ((rp->r & 0xf0000000) == KGLB)
					p[j] = (double *)((gstatmem + ((long)p[j])) + rp->q * 8);
			}
			if ((kcd->newvar[kcd->gasm[i].g].r & 0xf0000000) == KIMM)
				dafunc = ((double(__cdecl *)(double, ...))kcd->gevalext[kcd->newvar[kcd->gasm[i].g].r & 0x0fffffff].ptr);
			else
				dafunc = ((double(__cdecl *)(double, ...)) * (long *)(plst[((unsigned long)KESP) >> 28] + kcd->newvar[gasm[i].g].r));

			cptr = &kcd->newvarnam[kcd->newvar[kcd->gasm[i].g].proti];
			switch (kcd->gasm[i].n) // This seems to be the only way to do pure C implementation; it sucks!
			{
			case 1:
				if (!strncmp(cptr, "d", 1))
				{
					(*p[0]) = dafunc(*p[1]);
					break;
				}
				break;
			case 2:
				if (!strncmp(cptr, "dd", 2))
				{
					(*p[0]) = dafunc(*p[1], *p[2]);
					break;
				}
				if (!strncmp(cptr, "dD", 2))
				{
					(*p[0]) = dafunc(*p[1], p[2]);
					break;
				}
				break;
			case 3:
				if (!strncmp(cptr, "ddd", 3))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3]);
					break;
				}
				if (!strncmp(cptr, "ddD", 3))
				{
					(*p[0]) = dafunc(*p[1], *p[2], p[3]);
					break;
				}
				if (!strncmp(cptr, "dDD", 3))
				{
					(*p[0]) = dafunc(*p[1], p[2], p[3]);
					break;
				}
				break;
			case 4:
				if (!strncmp(cptr, "dddd", 4))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4]);
					break;
				}
				if (!strncmp(cptr, "dddD", 4))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], p[4]);
					break;
				}
				if (!strncmp(cptr, "ddDD", 4))
				{
					(*p[0]) = dafunc(*p[1], *p[2], p[3], p[4]);
					break;
				}
				if (!strncmp(cptr, "dDDD", 4))
				{
					(*p[0]) = dafunc(*p[1], p[2], p[3], p[4]);
					break;
				}
				break;
			case 5:
				if (!strncmp(cptr, "ddddd", 5))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5]);
					break;
				}
				if (!strncmp(cptr, "ddddD", 5))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], p[5]);
					break;
				}
				if (!strncmp(cptr, "dddDD", 5))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], p[4], p[5]);
					break;
				}
				if (!strncmp(cptr, "ddDDD", 5))
				{
					(*p[0]) = dafunc(*p[1], *p[2], p[3], p[4], p[5]);
					break;
				}
				if (!strncmp(cptr, "dDDDD", 5))
				{
					(*p[0]) = dafunc(*p[1], p[2], p[3], p[4], p[5]);
					break;
				}
				break;
			case 6:
				if (!strncmp(cptr, "dddddd", 6))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6]);
					break;
				}
				if (!strncmp(cptr, "dddddD", 6))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], p[6]);
					break;
				}
				if (!strncmp(cptr, "ddddDD", 6))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], p[5], p[6]);
					break;
				}
				if (!strncmp(cptr, "dddDDD", 6))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], p[4], p[5], p[6]);
					break;
				}
				if (!strncmp(cptr, "ddDDDD", 6))
				{
					(*p[0]) = dafunc(*p[1], *p[2], p[3], p[4], p[5], p[6]);
					break;
				}
				if (!strncmp(cptr, "dDDDDD", 6))
				{
					(*p[0]) = dafunc(*p[1], p[2], p[3], p[4], p[5], p[6]);
					break;
				}
				break;
			case 7:
				if (!strncmp(cptr, "ddddddd", 7))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6], *p[7]);
					break;
				}
				if (!strncmp(cptr, "ddddddD", 7))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6], p[7]);
					break;
				}
				if (!strncmp(cptr, "dddddDD", 7))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], p[6], p[7]);
					break;
				}
				if (!strncmp(cptr, "ddddDDD", 7))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], p[5], p[6], p[7]);
					break;
				}
				if (!strncmp(cptr, "dddDDDD", 7))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], p[4], p[5], p[6], p[7]);
					break;
				}
				if (!strncmp(cptr, "ddDDDDD", 7))
				{
					(*p[0]) = dafunc(*p[1], *p[2], p[3], p[4], p[5], p[6], p[7]);
					break;
				}
				if (!strncmp(cptr, "dDDDDDD", 7))
				{
					(*p[0]) = dafunc(*p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
					break;
				}
				break;
			case 8:
				if (!strncmp(cptr, "dddddddd", 8))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6], *p[7], *p[8]);
					break;
				}
				if (!strncmp(cptr, "dddddddD", 8))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6], *p[7], p[8]);
					break;
				}
				if (!strncmp(cptr, "ddddddDD", 8))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6], p[7], p[8]);
					break;
				}
				if (!strncmp(cptr, "dddddDDD", 8))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], p[6], p[7], p[8]);
					break;
				}
				if (!strncmp(cptr, "ddddDDDD", 8))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], p[5], p[6], p[7], p[8]);
					break;
				}
				if (!strncmp(cptr, "dddDDDDD", 8))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], p[4], p[5], p[6], p[7], p[8]);
					break;
				}
				if (!strncmp(cptr, "ddDDDDDD", 8))
				{
					(*p[0]) = dafunc(*p[1], *p[2], p[3], p[4], p[5], p[6], p[7], p[8]);
					break;
				}
				if (!strncmp(cptr, "dDDDDDDD", 8))
				{
					(*p[0]) = dafunc(*p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8]);
					break;
				}
				break;
			case 9:
				if (!strncmp(cptr, "ddddddddd", 9))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6], *p[7], *p[8], *p[9]);
					break;
				}
				if (!strncmp(cptr, "ddddddddD", 9))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6], *p[7], *p[8], p[9]);
					break;
				}
				if (!strncmp(cptr, "dddddddDD", 9))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6], *p[7], p[8], p[9]);
					break;
				}
				if (!strncmp(cptr, "ddddddDDD", 9))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6], p[7], p[8], p[9]);
					break;
				}
				if (!strncmp(cptr, "dddddDDDD", 9))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], p[6], p[7], p[8], p[9]);
					break;
				}
				if (!strncmp(cptr, "ddddDDDDD", 9))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], p[5], p[6], p[7], p[8], p[9]);
					break;
				}
				if (!strncmp(cptr, "dddDDDDDD", 9))
				{
					(*p[0]) = dafunc(*p[1], *p[2], *p[3], p[4], p[5], p[6], p[7], p[8], p[9]);
					break;
				}
				if (!strncmp(cptr, "ddDDDDDDD", 9))
				{
					(*p[0]) = dafunc(*p[1], *p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]);
					break;
				}
				if (!strncmp(cptr, "dDDDDDDDD", 9))
				{
					(*p[0]) = dafunc(*p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]);
					break;
				}
				break;
			case 10:
				(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6], *p[7], *p[8], *p[9], *p[10]);
				break;
			case 11:
				(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6], *p[7], *p[8], *p[9], *p[10], *p[11]);
				break;
			case 12:
				(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6], *p[7], *p[8], *p[9], *p[10], *p[11], *p[12]);
				break;
			case 13:
				(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6], *p[7], *p[8], *p[9], *p[10], *p[11], *p[12], *p[13]);
				break;
			case 14:
				(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6], *p[7], *p[8], *p[9], *p[10], *p[11], *p[12], *p[13], *p[14]);
				break;
			case 15:
				(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6], *p[7], *p[8], *p[9], *p[10], *p[11], *p[12], *p[13], *p[14], *p[15]);
				break;
			case 16:
				(*p[0]) = dafunc(*p[1], *p[2], *p[3], *p[4], *p[5], *p[6], *p[7], *p[8], *p[9], *p[10], *p[11], *p[12], *p[13], *p[14], *p[15], *p[16]);
				break;
				// how to do unlimited cases without using switch?
			}
			break;
		}
		}
	}
	gvlp -= kcd->stackdoubs;
	return (*p[0]);
}

double __cdecl kasm87cp(double *first, ...)
{
	va_list marker;
	kcd_t *kcd;
	long i, j;
	char parmdat[sizeof(double) * 16];

	kcd = (kcd_t *)gkasm87cptr;
	*(double **)&parmdat[0] = first;
	va_start(marker, first);
	j = 4;
	for (i = 1; i < kcd->gnumarg; i++)
	{
		if ((newvar[i].parnum < 0) && ((newvar[i].r & 0xf0000000) == KESP))
		{
			*(double *)&parmdat[j] = va_arg(marker, double);
			j += 8;
		} // 8 byte variable
		else
		{
			*(long *)&parmdat[j] = va_arg(marker, long);
			j += 4;
		} // 4 byte variable/function pointer
	}
	va_end(marker); // Keep for compatibility

	return (kasm87c_run(parmdat, kcd));
}

double __cdecl kasm87c(double first, ...)
{
	va_list marker;
	kcd_t *kcd;
	long i, j;
	char parmdat[sizeof(double) * 16];

	kcd = (kcd_t *)gkasm87cptr;
	*(double *)&parmdat[0] = first;
	va_start(marker, first);
	j = 8;
	for (i = 1; i < gnumarg; i++)
	{
		if ((newvar[i].parnum < 0) && ((newvar[i].r & 0xf0000000) == KESP))
		{
			*(double *)&parmdat[j] = va_arg(marker, double);
			j += 8;
		} // 8 byte variable
		else
		{
			*(long *)&parmdat[j] = va_arg(marker, long);
			j += 4;
		} // 4 byte variable/function pointer
	}
	va_end(marker); // Keep for compatibility

	return (kasm87c_run(parmdat, kcd));
}

kcd_t *kasm87c_copyglob2struct(long stackdoubs)
{
	kcd_t *kcd;
	long l;

	l = sizeof(kcd_t);
	l += gccnt * sizeof(globval[0]) + gstnum + arrnum;
	l += gecnt * sizeof(gasm[0]);
	l += numrxi * sizeof(rxi[0]);
	l += gevalextnum * sizeof(gevalext[0]);
	l += newvarnum * sizeof(newvar[0]);
	l += newvarplc * sizeof(newvarnam[0]);

	kcd = (kcd_t *)malloc(l);
	if (!kcd)
		return (0);

	kcd->gccnt = gccnt;
	kcd->gstnum = gstnum;
	kcd->arrnum = arrnum;
	kcd->gecnt = gecnt;
	kcd->numrxi = numrxi;
	kcd->gevalextnum = gevalextnum;
	kcd->newvarnum = newvarnum;
	kcd->gnumarg = gnumarg;
	kcd->newvarplc = newvarplc;

	l = (long)&kcd->data;
	kcd->globval = (double *)l;
	l += gccnt * sizeof(globval[0]) + gstnum + arrnum;
	kcd->gasm = (gasmtyp *)l;
	l += gecnt * sizeof(gasm[0]);
	kcd->rxi = (rtyp *)l;
	l += numrxi * sizeof(rxi[0]);
	kcd->gevalext = (evalextyp *)l;
	l += gevalextnum * sizeof(gevalext[0]);
	kcd->newvar = (newvartyp *)l;
	l += newvarnum * sizeof(newvar[0]);
	kcd->newvarnam = (char *)l;
	l += newvarplc * sizeof(newvarnam[0]);

#if 0
	printf("kcd_t:%d",sizeof(kcd_t));
	if (gccnt|gstnum|arrnum) printf(" + globval:%d"  ,      gccnt*sizeof(globval[0]) + gstnum + arrnum);
	if (gecnt)               printf(" + gasm:%d"     ,      gecnt*sizeof(gasm[0]));
	if (numrxi)              printf(" + rxi:%d"      ,     numrxi*sizeof(rxi[0]));
	if (gevalextnum)         printf(" + gevalext:%d" ,gevalextnum*sizeof(gevalext[0]));
	if (newvarnum)           printf(" + newvar:%d"   ,  newvarnum*sizeof(newvar[0]));
	if (newvarplc)           printf(" + newvarnam:%d",  newvarplc*sizeof(newvarnam[0]));
	printf(" = %d\n",l);
#endif

	if (gccnt | gstnum | arrnum)
		memcpy(kcd->globval, globval, gccnt * sizeof(globval[0]) + gstnum + arrnum);
	if (gecnt)
		memcpy(kcd->gasm, gasm, gecnt * sizeof(gasm[0]));
	if (numrxi)
		memcpy(kcd->rxi, rxi, numrxi * sizeof(rxi[0]));
	if (gevalextnum)
		memcpy(kcd->gevalext, gevalext, gevalextnum * sizeof(gevalext[0])); // need only the ptr's
	if (newvarnum)
		memcpy(kcd->newvar, newvar, newvarnum * sizeof(newvar[0]));
	if (newvarplc)
		memcpy(kcd->newvarnam, newvarnam, newvarplc * sizeof(newvarnam[0]));

	kcd->stackdoubs = stackdoubs;

#if defined(_M_IX86) || defined(__i386__)
	*(short *)kcd->codestub = 0x5c7;
	*(long *)&kcd->codestub[2] = (long)&gkasm87cptr;
	*(long *)&kcd->codestub[6] = (long)kcd;
	kcd->codestub[10] = 0xe9;
	if ((newvar[0].parnum < 0) && ((newvar[0].r & 0xf0000000) == KESP))
		*(long *)&kcd->codestub[11] = ((long)kasm87c) - ((long)&kcd->codestub[15]);
	else
		*(long *)&kcd->codestub[11] = ((long)kasm87cp) - ((long)&kcd->codestub[15]);
#else

	// Increase codestub size to 32?
	// PPC guess:
	// lis r4,imm16hi
	// ori r4,r4,imm16lo
	//?mflr r5
	// addis r4,?r5,ha16(_glob-?)
	// stw r4,lo16(_glob-?)(r2)
	// b (kasm87c - &codestub[20?])

	// FIX: This temp hack allows 1 script in memory to run on a non-x86 platform
	// To fix it: machine code for the native architecture must be written to move and jump (like above)
	gkasm87cptr = (long)kcd;
	if ((newvar[0].parnum < 0) && ((newvar[0].r & 0xf0000000) == KESP))
		return ((void *)kasm87c);
	else
		return ((void *)kasm87cp);
#endif

	return (kcd);
}

#endif

// mode=0: overwrite backward jumps to 0's
// mode=1: restore backward jumps
void kasm87jumpback(void *f, long mode)
{
	long i, jbn;
	jumpback_t *jb;

	jbn = *(long *)(((long)f) - FUNCBYTEOFFS + 0);
	if (jbn <= 0)
		return;
	jb = (jumpback_t *)((*(long *)(((long)f) - FUNCBYTEOFFS + 4)) + ((long)f));
	switch (mode)
	{
	case 0:
		for (i = jbn - 1; i >= 0; i--)
			if (*(long *)(jb[i].addr) < 0)
				*(long *)(jb[i].addr) = 0;
		break;
	case 1:
		for (i = jbn - 1; i >= 0; i--)
			*(long *)(jb[i].addr) = jb[i].val;
		break;
	}
}

void kasm87free(void *f)
{
	return;
	long i;
	if (!f)
		return;
	i = *(long *)(((long)f) - FUNCBYTEOFFS + 8);
	if (i)
		free((void *)i); // free global static block
#if _WIN32
	i = *(long *)(((long)f) - FUNCBYTEOFFS + 4); // i=kasm87leng;
	VirtualProtect((void *)(((long)f) - FUNCBYTEOFFS), i + FUNCBYTEOFFS, 0x04 /*PAGE_READWRITE*/, (unsigned long *)&i);
#endif
	free((void *)(((long)f) - FUNCBYTEOFFS));
}

static void *kasm87comp(char *bakz)
{
	rtyp tr, *rp;
	long i, j, k, l, pcnt, bcnt, maxpcnt, got, jumpatnum, whitespc, regnum, subparms, isaddr, espoff, inquotes;
	long onewvarplc, ibody, ljumpbacknum;
	char *tbuf;

	globi = 0;
	gop[globi] = NUL;
	arrnum = 0;

	gecnt = 0;
	gccnt = 0;
	gstnum = 0;
	ginitvalnum = 0;
	kasm87leng = 0;

	// Insert dummy label at beginning
	checkops(gecnt + 1);
	memset(&gasm[gecnt], 0, sizeof(gasmtyp));
	gasm[gecnt].f = NUL;
	gasm[gecnt].r[0].r = KEIP;
	gecnt = 1;
	numlabels = 1;

	tbuf = (char *)malloc(strlen(bakz) + 1);
	if (!tbuf)
	{
		strcpy(kasm87err, "ERROR: malloc failed");
		return (0);
	}

	// Pre-processor
	ibody = 0;
	l = 0;
	got = 0;
	pcnt = 0;
	bcnt = 0;
	maxpcnt = 0;
	gnumarg = -1;
	whitespc = 0;
	onewvarplc = newvarplc;
	newlabplc = 0;
	newlabnum = 0;
	inquotes = 0;
	for (i = 0; bakz[i]; i++)
	{
		if ((!inquotes) && (bakz[i] == 32) /*|| (bakz[i] == 9)*/)
		{
			whitespc = 1;
			continue;
		} // strip Space/Tab
		if (got)
			continue;

		tbuf[l] = bakz[i];

		if ((tbuf[l] == '\"') && ((!l) || (tbuf[l - 1] != '\\')))
			inquotes ^= 1;
		if (inquotes)
		{
			l++;
			continue;
		}

		if (tbuf[l] == '(')
		{
			pcnt++;
			if (pcnt > maxpcnt)
				maxpcnt = pcnt;
			if ((gnumarg < 0) && (pcnt == 1))
			{
				ibody = l;
				continue;
			} // Note: '(' not stored in tbuf because no l++;
		}
		if (tbuf[l] == ')')
		{
			pcnt--;
			if (pcnt < 0)
				break;
			if ((!pcnt) && (gnumarg < 0))
			{
				tbuf[l] = ',';
				subparms = -2;
				isaddr = 0;
				espoff = 0;
				// Save new variable/function names,indices,parameters to list
				for (k = ibody; k <= l; k++) // verify parnam  //Example param string: "a,b(,&),&c,d(),"
				{							 //                       ibody         l
											 // Save new variable name&index to list
					if (tbuf[k] == '(')
					{
						pcnt++;
						subparms = 0;
						if (pcnt == 1)
						{
							checkvarchars(newvarplc + 1);
							newvarnam[newvarplc++] = 0;
							newvar[newvarnum].proti = newvarplc;
						}
						continue;
					}
					if (tbuf[k] == ')')
					{
						pcnt--;
						if (pcnt == 0)
						{
							checkvarchars(newvarplc + 1);
							switch (isaddr)
							{
							case 0:
								newvarnam[newvarplc] = 'd';
								break; // double
							case 1:
								newvarnam[newvarplc] = 'D';
								break; // double*
							case 2:
								newvarnam[newvarplc] = 'C';
								break; // char* (const string only)
							}
							newvarplc++;
							subparms++;
							isaddr = 0;
							continue;
						}
						continue;
					}
					if (tbuf[k] == '[')
					{
						if (isaddr)
						{
							sprintf(kasm87err, "ERROR: bad syntax, param %d", newvarnum);
							free(tbuf);
							return (0);
						}

						checkvarchars(newvarplc + 1);
						newvarnam[newvarplc++] = 0;
						checkvars(newvarnum + 1);
						newvar[newvarnum].proti = newvarplc;
						newvar[newvarnum].parnum = 0;

						bcnt = parse_dimensions(tbuf, &k, 1);
						if (!bcnt)
						{
							free(tbuf);
							return (0);
						}
						isaddr = 1;
						k--; // without this, continue would skip past comma
						continue;
					}
					if (tbuf[k] == '&')
					{
						if (isaddr)
						{
							sprintf(kasm87err, "ERROR: bad syntax, param %d", newvarnum);
							free(tbuf);
							return (0);
						}
						isaddr = 1;
						continue;
					}
					if (tbuf[k] == '$')
					{
						if (isaddr)
						{
							sprintf(kasm87err, "ERROR: bad syntax, param %d", newvarnum);
							free(tbuf);
							return (0);
						}
						isaddr = 2;
						continue;
					}
					if (tbuf[k] == ',')
					{
						// if (tbuf[k] != ',') { sprintf(kasm87err,"ERROR: bad syntax, param %d",newvarnum); free(tbuf); return(0); }
						if (pcnt == 1)
						{
							checkvarchars(newvarplc + 1);
							switch (isaddr)
							{
							case 0:
								newvarnam[newvarplc] = 'd';
								break; // double
							case 1:
								newvarnam[newvarplc] = 'D';
								break; // double*
							case 2:
								newvarnam[newvarplc] = 'C';
								break; // char* (const string only)
							}
							newvarplc++;
							subparms++;
							isaddr = 0;
							continue;
						}

						if ((k == ibody) && (k == l))
							break; // special case with no arguments
						if (((tbuf[k + 1] == ',') && (k < l)) || (k == ibody) || (k == l - 1))
						{
							sprintf(kasm87err, "ERROR: bad syntax, param %d", newvarnum);
							free(tbuf);
							return (0);
						}

						// if (!isaddr) isaddr = -1;
						if (subparms < 0)
						{
							if (!bcnt)
							{
								checkvarchars(newvarplc + 1);
								newvarnam[newvarplc++] = 0;
								checkvars(newvarnum + 1);
								newvar[newvarnum].proti = -1;
							}
						}
						checkvars(newvarnum + 1);
						newvar[newvarnum].nami = onewvarplc;
						onewvarplc = newvarplc;
						newvar[newvarnum].r = espoff + KESP;
						newvar[newvarnum].maxind = bcnt;
						// Subparms: < 0 for double/array, >= 0 for user function pointers (# is # parms)
						if (subparms < 0)
						{
							if (!isaddr)
							{
								espoff += 8;
							}
							else
							{
								newvar[newvarnum].r = espoff + KPTR;
								espoff += 4;
							}
						}
						else
						{
							if (isaddr)
							{
								sprintf(kasm87err, "ERROR: bad syntax, param %d", newvarnum);
								free(tbuf);
								return (0);
							}
							espoff += 4;
						}
						if ((subparms < 0) && (bcnt))
							newvar[newvarnum].parnum ^= -1;
						else // 1's complement
							newvar[newvarnum].parnum = subparms;
						j = getnewvarhash(&newvarnam[newvar[newvarnum].nami]);
						newvar[newvarnum].hashn = newvarhash[j];
						newvarhash[j] = newvarnum;

						subparms = -2;
						newvarnum++;
						isaddr = 0;
						bcnt = 0;
						continue;
					}
					if (isvarchar(tbuf[k]))
					{
						if (!bcnt)
						{
							checkvarchars(newvarplc + 1);
							newvarnam[newvarplc++] = tbuf[k];
						}
						continue;
					}
					sprintf(kasm87err, "ERROR: bad syntax, param %d", newvarnum);
					free(tbuf);
					return (0);
				}
				gnumarg = newvarnum;
				tbuf[l] = 0;
				ibody = l + 1;
			}
		}
		if (whitespc)
		{
			whitespc = 0;
			if ((l > ibody) && (isvarchar(tbuf[l - 1])) && (isvarchar(tbuf[l])))
			{
				tbuf[l + 1] = tbuf[l];
				tbuf[l] = ' ';
				l++;
			}
		}
		l++;
	}
	tbuf[l++] = 0;
	// if (ibody+1 >= l) { strcpy(kasm87err,"ERROR: no function defined"); free(tbuf); return(0); }
	if (pcnt < 0)
	{
		strcpy(kasm87err, "ERROR: too many )");
		free(tbuf);
		return (0);
	}
	if (pcnt > 0)
	{
		strcpy(kasm87err, "ERROR: not enough )");
		free(tbuf);
		return (0);
	}
	if (maxpcnt > 16)
	{
		strcpy(kasm87err, "ERROR: () nested > 16");
		free(tbuf);
		return (0);
	}

	// Add global library variables&functions to parameter list
	for (i = 0; i < gevalextnum; i++)
	{
		bcnt = 0;
		subparms = -2;
		k = 0;

		// printf("|%s|:",gevalext[i].nam); //useful for debugging

		checkvars(newvarnum + 1);
		newvar[newvarnum].nami = newvarplc;

		if (gevalext[i].nam[k] == '&')
		{
			k++;
			bcnt = 1;
			newvar[newvarnum].proti = -1;
		}
		if (gevalext[i].nam[k] == '$')
			k++; // ignore/invalid

		if (gevalext[i].nam[k] == '\"') // Handle compiled strings for pic(),snd(),getpicsiz(),fil(),etc..
		{
			do
			{
				checkvarchars(newvarplc + 1);
				newvarnam[newvarplc++] = gevalext[i].nam[k++];
			} while ((gevalext[i].nam[k]) && (gevalext[i].nam[k] != '\"'));
		}
		while ((gevalext[i].nam[k]) && (gevalext[i].nam[k] != '(') && (gevalext[i].nam[k] != '[')) // FIX && (gevalext[i].nam[k] != ' '))
		{
			checkvarchars(newvarplc + 1);
			newvarnam[newvarplc++] = gevalext[i].nam[k++];
		}
		if ((newvarplc > newvar[newvarnum].nami) && (newvarnam[newvarplc - 1] == ' '))
			newvarplc--;
		checkvarchars(newvarplc + 1);
		newvarnam[newvarplc++] = 0;

		if (gevalext[i].nam[k] == ' ')
			k++; // Remove whitespace after function name (doesn't work)
		if (gevalext[i].nam[k] == '(')
		{
			newvar[newvarnum].proti = newvarplc;
			subparms = 0;
			do
			{
				checkvarchars(newvarplc + 1);
				subparms++;
				k++;
				while ((isvarchar(gevalext[i].nam[k])) || (gevalext[i].nam[k] == ' '))
					k++; // Don't care about variable name here; skip it
				newvarnam[newvarplc] = 'd';
				if (gevalext[i].nam[k] == '&')
				{
					newvarnam[newvarplc] = 'D';
					k++;
				}
				if (gevalext[i].nam[k] == '$')
				{
					newvarnam[newvarplc] = 'C';
					k++;
				}
				if (gevalext[i].nam[k] == '.')
				{
					newvarnam[newvarplc] = 'e';
					k++;
				}
				newvarplc++;
				while ((isvarchar(gevalext[i].nam[k])) || (gevalext[i].nam[k] == ' '))
					k++; // Don't care about variable name here; skip it
				if (gevalext[i].nam[k] == '[')
				{
					newvarnam[newvarplc - 1] = 'D';
					if (!parse_dimensions(gevalext[i].nam, &k, 0))
					{
						sprintf(kasm87err, "ERROR: %s has bad dimensions", &newvarnam[newvar[newvarnum].nami]);
						free(tbuf);
						return (0);
					}
				}
			} while (gevalext[i].nam[k] == ',');

			// printf("|"); for(j=newvar[newvarnum].proti;j<newvarplc;j++) printf("%c",newvarnam[j]); printf("|"); //useful for debugging
		}
		else if (gevalext[i].nam[k] == '[')
		{
			newvar[newvarnum].proti = newvarplc;
			newvar[newvarnum].parnum = 0;
			bcnt = parse_dimensions(gevalext[i].nam, &k, 1);
			if (!bcnt)
			{
				sprintf(kasm87err, "ERROR: %s has bad dimensions", &newvarnam[newvar[newvarnum].nami]);
				free(tbuf);
				return (0);
			}
			subparms = ~newvar[newvarnum].parnum; // 1's complement
		}

		// printf("|%s|:maxind:%d,parnum:%d\n",&newvarnam[newvar[newvarnum].nami],bcnt,subparms); //useful for debugging
		newvar[newvarnum].r = i + KIMM;
		newvar[newvarnum].maxind = bcnt;
		newvar[newvarnum].parnum = subparms; //<0 for double/arrays:~#dimens, >=0 for function where #parms
		j = getnewvarhash(&newvarnam[newvar[newvarnum].nami]);
		newvar[newvarnum].hashn = newvarhash[j];
		newvarhash[j] = newvarnum;
		newvarnum++;
	}
	gnumglob = newvarnum;

	parsefunc(&tbuf[ibody], -1, -1);
	free(tbuf);
	if (globi == -1)
	{
		gecnt = 0;
		return (0);
	}

	// Make sure all label destinations exist
	for (i = 0, tbuf = newlabnam; i < newlabnum; i++, tbuf = &tbuf[strlen(newlabnam) + 1])
		if (newlabind[i] < 0)
		{
			sprintf(kasm87err, "ERROR: label %s not found", tbuf);
			gecnt = 0;
			return (0);
		}

	// If last instruction is label, add return(0); at end
	if (!gasm[gecnt - 1].f)
	{
		checkops(gecnt + 1);
		memset(&gasm[gecnt], 0, sizeof(gasmtyp));
		gasm[gecnt].r[0].r = KUNUSED;
		gasm[gecnt].r[1].r = gccnt * 8 + KEDX;
		gasm[gecnt].r[2].r = KUNUSED;
		gasm[gecnt].n = 1;
		gasm[gecnt].f = RETURN;
		gecnt++;
		checkops(gccnt + 1);
		globval[gccnt++] = 0.0;
	}

	// Align section after strings to 8-byte boundary
	checkstrings((gstnum + 7) & ~7);
	while (gstnum & 7)
		gstring[gstnum++] = 0;

	if (kasm87optimize)
	{
		if (kasmoptimizations(0, 0) < 0)
		{
			gecnt = 0;
			return (0);
		}
	}

	// Get number of 8-byte memory locations needed for temp storage
	regnum = 0;
	for (i = gecnt - 1; i >= 0; i--)
		for (j = gasm[i].n; j >= 0; j--)
		{
			if (j < 3)
				rp = &gasm[i].r[j];
			else
				rp = &rxi[gasm[i].rxi + j - 3];
			if (((rp->r & 0xf0000000) == KECX) && ((rp->r & 0x0fffffff) > regnum))
				regnum = (rp->r & 0x0fffffff);
			if ((rp->r & 0xf0000000) == KSTR)
				rp->r = (rp->r & 0x0fffffff) + gccnt * 8 + KEDX;
			if ((rp->r & 0xf0000000) == KARR)
				rp->r = (rp->r & 0x0fffffff) + gccnt * 8 + gstnum + KEDX;
		}
	regnum = (regnum >> 3) + 1;

#if (COMPILE == 0)
	// Necessary hack to make jumps in kasm87c faster (Uses gasm[*].r[0] which isn't used for jumps anyway)
	for (i = 0; i < gecnt; i++)
		if ((gasm[i].f == IF0) || (gasm[i].f == IF1) || (gasm[i].f == GOTO))
			for (j = 0; j < gecnt; j++)
				if ((gasm[j].f == NUL) && (gasmeq(gasm[j].r[0], gasm[i].r[1])))
				{
					gasm[i].r[0].r = j;
					break;
				}

	// Hack: append strings&static array variables&data to globval[]
	if (gccnt * 8 + gstnum + arrnum > sizeof(double) * maxops)
		if (!(globval = (double *)realloc(globval, gccnt * 8 + gstnum + arrnum)))
		{
			strcpy(kasm87err, "ERROR: malloc failed");
			gecnt = 0;
			return (0);
		}
	memcpy(&globval[gccnt], gstring, gstnum); // Copy string tables to globval
	// memset(&globval[gccnt+(gstnum>>3)],0,arrnum); //Fill static variables&arrays with 0's
	// Fill static variables&arrays with 0's
	for (i = j = 0; i < arrnum; i += 8, kasm87leng += 8)
	{
		if ((j < ginitvalnum) && (i == ginitval[j].i))
		{
			*(double *)&globval[((gstnum + i) >> 3) + gccnt] = ginitval[j].v;
			j++;
		}
		else
			*(double *)&globval[((gstnum + i) >> 3) + gccnt] = 0;
	}

	if (!gvl)
	{
		gvl = (double *)malloc(65536 * sizeof(double));
		if (!gvl)
		{
			strcpy(kasm87err, "ERROR: malloc failed");
			gecnt = 0;
			return (0);
		}
		gvlp = gvl;
	}

	return ((void *)kasm87c_copyglob2struct(regnum));

	// if ((newvar[0].parnum < 0) && ((newvar[0].r&0xf0000000) == KESP))
	//      return((void *)kasm87c);
	// else return((void *)kasm87cp);
#else

#endif
}

// Finds index to '(' of 1st function. -1 if simple form (no function blocks)
long kasm87_findfirstfuncparen(char *st)
{
	long i, got, bcnt, scnt, inquotes;
	char och;

	// Compact white space, remove comments, handle quotes & char constants
	got = 0;
	inquotes = 0;
	scnt = 0;
	bcnt = 0;
	och = 255;
	for (i = 0; st[i]; i++)
	{
		if (!inquotes)
		{
			if ((st[i] == 32) || (st[i] == 9))
				continue; // strip Space/Tab
			if ((st[i] == 10) || (st[i] == 13))
			{
				if (got == 1)
					got = 0;
				continue;
			} // strip CR,LF
			if ((st[i] == '/') && (st[i + 1] == '/') && (!got))
				got = 1;
			else if ((st[i] == '/') && (st[i + 1] == '*') && (!got))
				got = 2;
			else if ((st[i] == '*') && (st[i + 1] == '/') && (got == 2))
			{
				got = 0;
				i++;
				continue;
			}
			if (got)
				continue;
		}
		if ((st[i] == '\"') && (och != '\\'))
			inquotes ^= 1;
		if (inquotes)
			continue;

		if ((st[i] == '\'') && (st[i + 1] >= 32) && (st[i + 2] == '\''))
			i += 2; // Skip character numbers: ' ', '$', 'a', etc...
		else if (st[i] == '{')
			bcnt++;
		else if (st[i] == '}')
			bcnt--;
		else if (st[i] == '[')
			scnt++;
		else if (st[i] == ']')
			scnt--;
		else if ((st[i] == '(') && (!(bcnt | scnt))) // Found first '(' not inside comment, "", {}, []
		{
			if (isvarchar(och))
				break;
			return (i);
		}
		och = st[i]; // cache previous char not inside comment or "". Don't want white space either.
	}
	return (-1); // Simple script
}

// hello      how    are
// 111111000001111000111
// hello how are
static void setecurs(long i0, long i1)
{
	long i;
	for (i = 0; (i0 > 0) && (i < texttransn); i++)
	{
		if (texttrans[i >> 5] & (1 << i))
			i0--;
	}
	kasm87err0 = i; // WARNING:Uses 32-bit x86 shift trick
	for (i = 0; (i1 > 0) && (i < texttransn); i++)
	{
		if (texttrans[i >> 5] & (1 << i))
			i1--;
	}
	kasm87err1 = i; // WARNING:Uses 32-bit x86 shift trick
}

void *kasm87(char *bakz)
{
	long i, j, k, l, z, oi, got, pcnt, bcnt, scnt, whitespc, funcnt, inquotes, funptr, *lptr;
	long codebytes, databytes, ogevalextnum, globenumcharplc, globenumnum, globnewvarplc, globnewvarnum;
	evalextyp *ogevalext;
	char ch, *tbuf, *tbufmal, *cptr;
	void *v;

	if (!cpuinited)
	{
		cpuinited = 1;
		cputype = getcputype();
		for (i = 0; i < 2048; i++)
		{
			if (i < 1044)
				round0msk[i][0] = 0;
			else if (i >= 1075)
				round0msk[i][0] = -1;
			else
				round0msk[i][0] = -(1 << (1075 - i));

			if (i < 1023)
				round0msk[i][1] = 0;
			else if (i >= 1043)
				round0msk[i][1] = -1;
			else
				round0msk[i][1] = (-(1 << (1043 - i))) | 0xfff00000;
		}
	}

	kasm87err0 = -1;
	kasm87err1 = -1;

	i = strlen(bakz) + 1 + 2;
	texttransn = (((i + 31) >> 5) << 2); //# bytes for texttrans bit buf, aligned to 32-bits
	if (texttransn > texttransmal)
	{
		if (texttrans)
			free((void *)texttrans);
		texttransmal = max(max(texttransn, 1024), texttransmal << 1);
		texttrans = (long *)malloc(texttransmal);
	}
	if (!texttrans)
	{
		strcpy(kasm87err, "ERROR: malloc failed");
		return (0);
	}
	memset(texttrans, 0, texttransn);

	tbufmal = (char *)malloc(i); // 2 more to store "()" if necessary
	if (!tbufmal)
	{
		strcpy(kasm87err, "ERROR: malloc failed");
		return (0);
	}
	tbufmal[0] = '(';
	tbufmal[1] = ')';
	tbuf = tbufmal + 2;

	// Compact white space, remove comments, handle quotes & char constants
	l = 0;
	got = 0;
	whitespc = 0;
	inquotes = 0;
	for (i = 0; bakz[i]; i++)
	{
		if (!inquotes)
		{
			if ((bakz[i] == 32) || (bakz[i] == 9))
			{
				whitespc = 1;
				continue;
			} // strip Space/Tab
			if ((bakz[i] == 10) || (bakz[i] == 13))
			{
				whitespc = 1;
				if (got == 1)
					got = 0;
				continue;
			} // strip CR,LF
			if ((bakz[i] == '/') && (bakz[i + 1] == '/') && (!got))
				got = 1;
			if ((bakz[i] == '/') && (bakz[i + 1] == '*') && (!got))
			{
				whitespc = 1;
				got = 2;
			}
			if ((bakz[i] == '*') && (bakz[i + 1] == '/') && (got == 2))
			{
				got = 0;
				i++;
				continue;
			}
			if (got)
				continue;

			if (whitespc)
			{
				tbuf[l++] = ' ';
				whitespc = 0;
				texttrans[(i - 1) >> 5] |= (1 << (i - 1)); // WARNING:Uses 32-bit x86 shift trick
			}

			// Skip "#opt(..)"
			if ((bakz[i] == '#') && (bakz[i + 1] == 'o') && (bakz[i + 2] == 'p') && (bakz[i + 3] == 't') && (bakz[i + 4] == '('))
			{
				for (i += 5; (bakz[i]) && (bakz[i] != ')'); i++)
					;
				continue;
			}

			ch = bakz[i];
			if ((ch >= 'a') && (ch <= 'z'))
				ch -= 32;
		}
		else
			ch = bakz[i];

		tbuf[l++] = ch;
		texttrans[i >> 5] |= (1 << i); // WARNING:Uses 32-bit x86 shift trick

		if ((ch == '\"') && ((l < 2) || (tbuf[l - 2] != '\\')))
			inquotes ^= 1;
		if (inquotes)
			continue;

		if (ch == '\'') // Convert character numbers: ' ', '$', 'a', etc...
		{
			if ((bakz[i + 1] < 32) || (bakz[i + 2] != '\''))
			{
				sprintf(kasm87err, "ERROR: ' used wrong");
				kasm87err0 = i;
				kasm87err1 = i + 2;
				free(tbufmal);
				return (0);
			}
			j = sprintf(&tbuf[l - 1], "%d", bakz[i + 1]);
			switch (j)
			{
			case 3:
				texttrans[(i + 2) >> 5] |= (1 << (i + 2)); // no break intentinoal //WARNING:Uses 32-bit x86 shift trick
			case 2:
				texttrans[(i + 1) >> 5] |= (1 << (i + 1)); // no break intentinoal //WARNING:Uses 32-bit x86 shift trick
			case 1:
				break;
			}
			l += j - 1;
			i += 2;
			continue;
		}
	}
	tbuf[l] = 0;

	// Split function definitions & global sections. Algo:
	//  * find first '(' not inside "", {}, []
	//  * extract function name by searching backwards like this:  !isvarchar  whitespc  name  whitespc  (
	//  * if char before name isn't ';', '}' or start of buffer, then:
	//          Whole script is treated as single function; add "()" at beginning.
	//       Note: This case only valid when finding the first function. If detected later, report an error.
	//  * else if name is blank, then it becomes the main function: everything before it is treated as global.
	pcnt = 0;
	scnt = 0;
	bcnt = 0;
	funcnt = 0;
	inquotes = 0;
	oi = 0;
	got = 0;
	globi = 0;
	if (!maxfuncst)
	{
		checkfuncst(0);
		if (globi < 0)
		{
			free(tbufmal);
			return (0);
		}
	}
	for (i = 0; i < l; i++)
	{
		ch = tbuf[i];
		if ((ch == '\"') && (!((i) && (tbuf[i - 1] == '\\'))))
			inquotes ^= 1;
		if (inquotes)
			continue;
		if (ch == '{')
		{
			bcnt++;
			continue;
		}
		if (ch == '}')
		{
			bcnt--;
			if (bcnt < 0)
			{
				strcpy(kasm87err, "ERROR: too many }"); /*setecurs(i-1,i+1);*/
				free(tbufmal);
				return (0);
			}
			if (pcnt)
			{
				strcpy(kasm87err, "ERROR: missing )"); /*setecurs(i-1,i+1);*/
				free(tbufmal);
				return (0);
			}
			continue;
		}
		if (ch == '[')
		{
			scnt++;
			continue;
		}
		if (ch == ']')
		{
			scnt--;
			if (scnt < 0)
			{
				strcpy(kasm87err, "ERROR: too many ]"); /*setecurs(i-1,i+1);*/
				free(tbufmal);
				return (0);
			}
			continue;
		}
		if ((ch == '(') && (!bcnt) && (!scnt)) // Found first '(' not inside "", {}, []
		{
			checkfuncst((funcnt + 2) * 4);
			if (globi < 0)
			{ /*setecurs(i-1,i+1);*/
				free(tbufmal);
				return (0);
			}

			// check if it's a function. Extract function name by searching backwards like this:  !isvarchar  whitespc  name  whitespc  (
			k = i;
			if ((k > oi) && (tbuf[k - 1] == ' '))
				k--; // skip whitespc
			for (j = k; (k) && (isvarchar(tbuf[k - 1])); k--)
				; // find string: &tbuf[k<=?<j]

			// insert global section
			if (oi < k - 1)
			{
				funcst[funcnt * 4 + 0] = -1;
				funcst[funcnt * 4 + 1] = 0;
				funcst[funcnt * 4 + 2] = oi;
				funcst[funcnt * 4 + 3] = k;
				funcnt++;
			}

			if (j == k)
			{
				funcst[funcnt * 4 + 0] = i;
			} // no name; ltrim space
			else
			{
				funcst[funcnt * 4 + 0] = k;
				if (!got)
					break; /*Hack for simple cases like "cos(3)+4"*/
			}
			funcst[funcnt * 4 + 1] = i;

			if ((k > oi) && (tbuf[k - 1] == ' '))
				k--; // m = char before string (skipping whitespc)
			if ((k > oi) && (tbuf[k - 1] != ';') && (tbuf[k - 1] != '}'))
			{
				if (got)
				{
					strcpy(kasm87err, "ERROR: global definition missing ; or }"); /*setecurs(i-1,i+1);*/
					free(tbufmal);
					return (0);
				}
				break; // Simple script
			}

			got = 1;

			// Find ')' (end of function parameter list)
			for (pcnt = 1, i++; i < l; i++)
			{
				if (tbuf[i] == '(')
				{
					pcnt++;
					continue;
				}
				if (tbuf[i] == ')')
				{
					pcnt--;
					if (!pcnt)
					{
						funcst[funcnt * 4 + 2] = i + 1;
						break;
					}
				}
			}
			if (i >= l)
			{
				strcpy(kasm87err, "ERROR: function definition missing )"); /*setecurs(i-1,i+1);*/
				free(tbufmal);
				return (0);
			}

			// Find '}' (end of function body)
			i++;
			if (tbuf[i] == 32)
				i++;
			if (tbuf[i] != '{')
			{
				funcst[funcnt * 4 + 3] = l;
				funcnt++;
				oi = l;
				break;
			} // Hack to support a single function without {} around body
			for (bcnt = 1, i++; i < l; i++)
			{
				if (tbuf[i] == '{')
				{
					bcnt++;
					continue;
				}
				if (tbuf[i] == '}')
				{
					bcnt--;
					if (!bcnt)
					{
						funcst[funcnt * 4 + 3] = i + 1;
						oi = i + 1;
						funcnt++;
						break;
					}
				}
			}
			if (i >= l)
			{
				strcpy(kasm87err, "ERROR: function missing } at end"); /*setecurs(k,i);*/
				free(tbufmal);
				return (0);
			}
		}
	}
	// Simple script
	if (!got)
	{
		tbuf -= 2;
		l += 2;
		funcst[0 * 4 + 0] = 0;
		funcst[0 * 4 + 1] = 0;
		funcst[0 * 4 + 2] = 2;
		funcst[0 * 4 + 3] = l;
		funcnt = 1;
	}
	else
	{
		// insert global section
		if (oi < l - 1)
		{
			funcst[funcnt * 4 + 0] = -1;
			funcst[funcnt * 4 + 1] = 0;
			funcst[funcnt * 4 + 2] = oi;
			funcst[funcnt * 4 + 3] = l;
			funcnt++;
		}
	}

	ogevalext = gevalext;
	ogevalextnum = gevalextnum;
	gevalext = (evalextyp *)malloc((gevalextnum + funcnt) * sizeof(evalextyp));
	if (!gevalext)
	{
		strcpy(kasm87err, "ERROR: malloc failed");
		free(tbufmal);
		return (0);
	}

#if (COMPILE != 0)
	patchnum = 0;
#endif

#if 0
		//funcst[?*4+{0...1....2......3}]
		//           |func(x,y){x^2+y}|
		//           |func(x,y)|
		//               |(x,y){x^2+y}|
	for(i=0;i<funcnt;i++)
	{
		if (funcst[i*4+0] < 0)
		{
			ch = tbuf[funcst[i*4+3]]; tbuf[funcst[i*4+3]] = 0; printf("FUNC%d:glob parse sees:|%s|\n",i,&tbuf[funcst[i*4+2]]); tbuf[funcst[i*4+3]] = ch;
		}
		else
		{
			ch = tbuf[funcst[i*4+2]]; tbuf[funcst[i*4+2]] = 0; printf("FUNC%d: gevalext proto:|%s|\n",i,&tbuf[funcst[i*4+0]]); tbuf[funcst[i*4+2]] = ch;
			ch = tbuf[funcst[i*4+3]]; tbuf[funcst[i*4+3]] = 0; printf("FUNC%d:kasm87comp sees:|%s|\n",i,&tbuf[funcst[i*4+1]]); tbuf[funcst[i*4+3]] = ch;
		}
	}
#endif

	globi = 0;
	arrnum = 0;
	if (!maxops)
	{
		if (oprio[0] != 255) // Initialize operator precedence LUT
		{
			memset(oprio, 255, sizeof(oprio));
			oprio[POW] = 0;
			oprio[TIMES] = oprio[SLASH] = oprio[PERC] = 1;
			oprio[PLUS] = oprio[MINUS] = 2;
			oprio[LES] = oprio[LESEQ] = oprio[MOR] = oprio[MOREQ] = 3;
			oprio[EQU] = oprio[NEQU] = 4;
			oprio[LAND] = 5;
			oprio[LOR] = 6;
		}

		checkops(0);
		checkstrings(0);
		checkinitvals(0);
		checkrxi(0);
		checkenum(0);
		checkenumchars(0);
		checkvars(0);
		checkvarchars(0);
		checklabs(0);
		checklabchars(0);
		checkjumpbacks(0);
#if (COMPILE != 0)
		checkpatch(0);
#endif
		if (globi < 0)
		{
			free(gevalext);
			gevalext = ogevalext;
			gevalextnum = ogevalextnum;
			free(tbufmal);
			strcpy(kasm87err, "ERROR: malloc failed");
			return (0);
		}
	}

	// Init for global parse_static() -> parse_dimensions() -> kasmoptimizations()
	gecnt = 0;
	gccnt = 0;
	gstnum = 0;
	ginitvalnum = 0;

	// Init for parse_static()
	memset(newvarhash, -1, sizeof(newvarhash));
	newvarnam[0] = 0;
	newvarplc = 0;
	newvarnum = 0;

	// parse global enum&static declarations
	enumcharplc = 0;
	enumnum = 0;
	for (i = j = 0; i < funcnt; i++)
	{
		if (funcst[i * 4 + 0] < 0)
		{
			ch = tbuf[funcst[i * 4 + 3]];
			tbuf[funcst[i * 4 + 3]] = 0;
			for (z = funcst[i * 4 + 2]; tbuf[z]; z++)
			{
				if ((!strncmp(&tbuf[z], "ENUM", 4)) && (!isvarchar(tbuf[z + 4])))
					z = parse_enum(tbuf, z);
				else if ((!strncmp(&tbuf[z], "STATIC", 6)) && (!isvarchar(tbuf[z + 6])))
					z = parse_static(tbuf, z);
				else
					continue;
				if (globi < 0)
				{
					free(gevalext);
					gevalext = ogevalext;
					gevalextnum = ogevalextnum;
					free(tbufmal);
					return (0);
				}
				if (!tbuf[z])
					break;
			}
			tbuf[funcst[i * 4 + 3]] = ch;
		}
		else
		{
			funcst[j * 4 + 0] = funcst[i * 4 + 0];
			funcst[j * 4 + 1] = funcst[i * 4 + 1];
			funcst[j * 4 + 2] = funcst[i * 4 + 2];
			funcst[j * 4 + 3] = funcst[i * 4 + 3];
			j++;
		}
	}
	funcnt = j;
	globenumnum = enumnum;
	globenumcharplc = enumcharplc;
	memcpy(newvarhash_glob, newvarhash, sizeof(newvarhash));
	globnewvarplc = newvarplc;
	globnewvarnum = newvarnum;
	if (arrnum) // allocate gstatmem block & copy ginitval to it before kasm87comp destroys ginitval
	{
		gstatmem = (long)malloc(arrnum);
		if (!gstatmem)
		{
			free(gevalext);
			gevalext = ogevalext;
			gevalextnum = ogevalextnum;
			free(tbufmal);
			strcpy(kasm87err, "ERROR: malloc failed");
			return (0);
		}

		// Fill gstatmem with assigned values or 0's
		k = (long)gstatmem;
		for (i = j = 0; i < arrnum; i += 8, k += 8)
		{
			if ((j < ginitvalnum) && (i == ginitval[j].i))
			{
				*(double *)k = ginitval[j].v;
				j++;
			}
			else
				*(double *)k = 0;
		}
	}
	else
		gstatmem = 0;
	for (i = 0; i < globnewvarnum; i++)
		if ((newvar[i].r & 0xf0000000) == KARR)
			newvar[i].r = (newvar[i].r & 0x0fffffff) + KGLB;

	jumpbacknum = 0;
	memcpy(gevalext, ogevalext, ogevalextnum * sizeof(evalextyp));
	gevalextnum += funcnt - 1;
	// Main can't be called since it doesn't have a name
	for (i = funcnt - 1; i > 0; i--)
	{
		j = gevalextnum - i;

		ch = tbuf[funcst[i * 4 + 2]];
		tbuf[funcst[i * 4 + 2]] = 0;
		gevalext[j].nam = strdup(&tbuf[funcst[i * 4 + 0]]);
		if (!gevalext[j].nam)
		{
			for (j--; j >= ogevalextnum; j--)
				free(gevalext[j].nam);
			if (gstatmem)
			{
				free((void *)gstatmem);
				gstatmem = 0;
			}
			free(gevalext);
			gevalext = ogevalext;
			gevalextnum = ogevalextnum;
			free(tbufmal);
			strcpy(kasm87err, "ERROR: strdup failed");
			return (0);
		}
		// printf("ext_proto:|%s|\n",gevalext[j].nam);
		tbuf[funcst[i * 4 + 2]] = ch;
	}

	codebytes = databytes = 0;
	for (i = funcnt - 1; i >= 0; i--)
	{
		j = gevalextnum - i;

		enumnum = globenumnum;
		enumcharplc = globenumcharplc;
		memcpy(newvarhash, newvarhash_glob, sizeof(newvarhash));
		newvarplc = globnewvarplc;
		newvarnum = globnewvarnum;

		tbuf[funcst[i * 4 + 3]] = 0;
		// printf("compiling:|%s|\n",&tbuf[funcst[i*4+1]]);
		gevalext[j].ptr = (EVALFUNC)kasm87comp(&tbuf[funcst[i * 4 + 1]]);

#if 1
		{ // DEBUG ONLY!
			char debuf[65536];
			extern char *kdisasm(char *, long, char *, long, long *);
			kasm87_showdebug(1, debuf, sizeof(debuf));
			printf("\n\n%d:\n%s", i, debuf);
			debuf[0] = debuf[sizeof(debuf) - 1] = 0;
			l = 0;
			// kdisasm((char *)compcode,kasm87leng,debuf,sizeof(debuf)-1,&l);
			printf("\n%s", debuf);
		}
#endif
		return gevalext[j].ptr;

		if (!gevalext[j].ptr)
		{
			for (j--; j >= ogevalextnum; j--)
				free((void *)(((long)gevalext[j].ptr) - FUNCBYTEOFFS));
			for (j = gevalextnum - 1; j >= ogevalextnum; j--)
				free(gevalext[j].nam);
			if (gstatmem)
			{
				free((void *)gstatmem);
				gstatmem = 0;
			}
			free(gevalext);
			gevalext = ogevalext;
			gevalextnum = ogevalextnum;
			free(tbufmal);
			return (0);
		}

		funptr = (long)gevalext[j].ptr;
		lptr = (long *)(funptr - FUNCBYTEOFFS);
		codebytes += lptr[1];
		databytes += lptr[3];
	}
	v = (void *)gevalext[gevalextnum].ptr;

#if 1//(COMPILE != 0)

	// merge all functions together
	kasm87leng = codebytes + databytes;
	if (databytes)
		kasm87leng += CODEDATADIST;
	v = malloc(FUNCBYTEOFFS + kasm87leng + jumpbacknum * sizeof(jumpback_t));
	if (!v)
	{
		for (i = 0; i < funcnt; i++)
		{
			j = gevalextnum - i;
			free((void *)(((long)gevalext[j].ptr) - FUNCBYTEOFFS));
		}
		for (j = gevalextnum - 1; j >= ogevalextnum; j--)
			free(gevalext[j].nam);
		if (gstatmem)
		{
			free((void *)gstatmem);
			gstatmem = 0;
		}
		free(gevalext);
		gevalext = ogevalext;
		gevalextnum = ogevalextnum;
		free(tbufmal);
		return (0);
	}
	v = (void *)(((long)v) + FUNCBYTEOFFS);
	cptr = (char *)v;
	l = patchnum - 1;
	k = jumpbacknum - 1;
	for (i = 0; i < funcnt; i++)
	{
		j = gevalextnum - i;
		funptr = (long)gevalext[j].ptr;
		lptr = (long *)(funptr - FUNCBYTEOFFS);
		if (!lptr[1])
			continue;

		// Relocate code
		memcpy(cptr, (void *)funptr, lptr[1]);
		lptr[0] = ((long)cptr); // lptr[0] now holds code pointer so data pointer can be plugged in later

		// Relocate patch pointers
		while ((l >= 0) && (((unsigned long)patch[l].lptr) - ((unsigned long)funptr) < (unsigned long)lptr[1]))
		{
			patch[l].lptr = (long *)(((long)patch[l].lptr) + ((long)cptr) - funptr);
			l--;
		}

		// Relocate jumpback list
		while ((k >= 0) && (((unsigned long)jumpback[k].addr) - ((unsigned long)funptr) < (unsigned long)lptr[1]))
		{
			jumpback[k].addr += ((long)cptr) - funptr;
			k--;
		}

		cptr += lptr[1];
	}
	if (databytes)
	{
		memset(cptr, 0x90, CODEDATADIST);
		cptr += CODEDATADIST;
	}
	for (i = 0; i < funcnt; i++)
	{
		j = gevalextnum - i;
		funptr = (long)gevalext[j].ptr;
		lptr = (long *)(funptr - FUNCBYTEOFFS);
		if (!lptr[3])
			continue;

		// Relocate data
		memcpy(cptr, (void *)(funptr + lptr[2]), lptr[3]);

		// Relocate consts&array (KEDX)
		if (*(unsigned char *)(lptr[0]) == 0xba)
			*(long *)(lptr[0] + 1) = ((long)cptr); // Note: condition should always be true

		cptr += lptr[3];
	}

	for (i = 0; i < funcnt; i++)
	{
		j = gevalextnum - i;
		funptr = (long)gevalext[j].ptr;
		lptr = (long *)(funptr - FUNCBYTEOFFS);
		gevalext[j].ptr = (long *)lptr[0]; // Put the new function pointer on global list
		free((void *)lptr);				   //..and free the original function which was just copied
	}

	for (j = patchnum - 1; j >= 0; j--)
	{
		if ((patch[j].ind & 0xf0000000) == KIMM)
			patch[j].lptr[0] += ((long)gevalext[patch[j].ind & 0x0fffffff].ptr);
		else
			patch[j].lptr[0] += gstatmem + (patch[j].ind & 0x0fffffff);
	}

	for (j = jumpbacknum - 1; j >= 0; j--)
		jumpback[j].val = *(long *)(jumpback[j].addr); // backup original jumpback values
	compcode = (unsigned char *)v;					   // make sure kasm87_showdebug has a valid pointer
#else
	// ??? not implemented .. need to fix .. sorry :/
#endif
	// return 0;
	// Copy jumpback table to script's malloced array so kasm87jumpback() can support multiple scripts
	*(long *)(((long)v) - FUNCBYTEOFFS) = jumpbacknum;
	*(long *)(((long)v) - FUNCBYTEOFFS + 4) = kasm87leng;
	*(long *)(((long)v) - FUNCBYTEOFFS + 8) = gstatmem;
	memcpy((void *)(((long)v) + kasm87leng), jumpback, jumpbacknum * sizeof(jumpback_t));

	for (j = gevalextnum - 1; j >= ogevalextnum; j--)
		free(gevalext[j].nam);
	free(gevalext);
	gevalext = ogevalext;
	gevalextnum = ogevalextnum;
	free(tbufmal);

#if _WIN32
	VirtualProtect((void *)(((long)v) - FUNCBYTEOFFS), kasm87leng + FUNCBYTEOFFS, 0x40 /*PAGE_EXECUTE_READWRITE*/, (unsigned long *)&i);
	// FlushInstructionCache(GetCurrentProcess(),((long)v)-FUNCBYTEOFFS,kasm87leng+FUNCBYTEOFFS);
#endif

	return (v);
}

//------------------------------------------ KASM87 ENDS ------------------------------------------

#ifdef EVALTEST

static __int64 rdtsc64()
{
	return (LL(0));
}

// This function is useful for debugging FP stack overflows/underflows/leaks
void dumpfp()
{
}

static char debuf[16384];

// NOTE: Non-VC compilers only support up to 3 params - else crash.
void testcode(char *st, double *v, long vnum)
{
	// double (__cdecl *fptr)(double, ...);
	EVALFUNC fptr;
	double d;
	__int64 q0, q1, q2;
	long i, j, k;

	printf("%s =\n", st);

	ksrand(17);
	snormstat = 0;

	// fptr = (double (__cdecl *)(double, ...))kasm87(st);
	fptr = (EVALFUNC)kasm87(st);
	if (fptr)
	{
		kasm87_showdebug(3, debuf, sizeof(debuf));
		printf("%s", debuf);
	}
	else
	{
		puts(kasm87err);
		return;
	}
	printf("value:");

	d = fptr(v[0], v[1], v[2]);

	printf(" %.25g", d);
	dumpfp();

	q2 = LL(0x7fffffffffffffff);
	for (j = 16; j; j--)
	{
		q0 = rdtsc64();
		for (i = 256; i; i--)
		{
			fptr(v[0], v[1], v[2]);
		}
		q1 = rdtsc64();
		if (q1 - q0 < q2)
			q2 = q1 - q0;
	}
	printf(" %10" PRINTF64 " cc\n", q2 >> 8);

	kasm87free((void *)fptr);
}

static double pilut[] = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3, 2, 3, 8, 4, 6, 2, 6, 4};
double __cdecl getdigpi(double d)
{
	if ((d < 0) || (d >= (sizeof(pilut) / sizeof(pilut[0]))))
		return (0.0);
	return (pilut[(long)d]);
}

static double goldratlut[] = {1, 6, 1, 8, 0, 3, 3, 9, 8, 8, 7, 4, 9, 8, 9, 5};
double __cdecl getdiggoldrat(double d)
{
	if ((d < 0) || (d >= (sizeof(goldratlut) / sizeof(goldratlut[0]))))
		return (0.0);
	return (goldratlut[(long)d]);
}

double __cdecl getangle360(double x, double y)
{
	double d;
	d = atan2(y, x) * 180 / PI;
	if (d < 0)
		d += 360;
	return (d);
}

double __cdecl func4d(double a, double b, double c, double d) { return (((a * 10 + b) * 10 + c) * 10 + d); }

double __cdecl sndfunc(double x)
{
	return (x);
}

double __cdecl mysrand(double val)
{
	kholdrand = (long)val;
	return (0);
}
double __cdecl printnum(double n)
{
	printf("%g ", n);
	return (0);
}

static double buf8[8], buf11[11], buf3_2[3][2] = {2, 3, 5, 7, 11, 13};

// C += A x B
double __cdecl crossprod(double Ax, double Ay, double Az,
						 double Bx, double By, double Bz,
						 double *Cx, double *Cy, double *Cz)
{
	(*Cx) += Ay * Bz - Az * By;
	(*Cy) += Az * Bx - Ax * Bz;
	(*Cz) += Ax * By - Ay * Bx;
	return (0);
}

double __cdecl printst(double d, char *st)
{
	long i;
	for (i = (long)d; i > 0; i--)
		printf("\n|%s|", st);
	return (0);
}

int main(int argc, char **argv)
{
	double v[256];
	long i, j, nparams;

	memset(v, 0, sizeof(v));
	if (argc >= 2)
	{
		for (i = argc - 2 - 1; i >= 0; i--)
			v[i] = atof(argv[i + 2]);
		testcode(argv[1], v, argc - 2);
		return (0);
	}

	// v[0] = 0.5; v[1] = 0.8; testcode("(x,y)cos(max(x,y)*PI)^2+sin(max(y,x)*PI)^2",v,2); //=1.0!
	// puts("");
	// v[0] = 2.0/3.0; testcode("(x)y=PI/2*x;z=y^2;(z/fact(5)-1/fact(3))*z*y+y",v,1); //~fcos
	// puts("");

	// Simple example #1: Celsius to Fahrenheit converter
	{
		// double (__cdecl *c2f)(double, ...) = (double (__cdecl *)(double, ...))kasm87("(x)(x*(9/5))+32");
		EVALFUNC c2f = (EVALFUNC)kasm87("(x)(x*(9/5))+32");
		if (c2f)
		{
			printf("%gC = %gF\n", 20.0, c2f(20.0));
			kasm87free(c2f);
		}
	}
	// Simple example #2: Hypotenuse calculator
	{
		EVALFUNC hypot = (EVALFUNC)kasm87("(cat,dog)cat*=cat;bozo=dog^2;sqrt(cat+bozo)");
		if (hypot)
		{
			printf("hypot(%g,%g) = %g\n", 3.0, 4.0, hypot(3.0, 4.0));
			kasm87free(hypot);
		}
	}

	// Example #3: Passing user functions by pointer
	{
		EVALFUNC passfunc = (EVALFUNC)kasm87("(x,pifunc())pifunc(x)*10+pifunc(x+1)");
		if (passfunc)
		{
			double d;
			kasm87_showdebug(1, debuf, sizeof(debuf));
			printf("\n%s", debuf);
			for (d = .5; d < 5; d++)
			{
				printf("sillypifunc(%g) = %g\n", d, passfunc(d, getdigpi));
			}
			kasm87free(passfunc);
		}
		else
			puts(kasm87err);
	}
	return 0;

	// Example #4: Passing 2 user functions by pointer
	{
		EVALFUNC passfunc = (EVALFUNC)kasm87("(x,goldfunc(),pifunc())pifunc(x)*1000+goldfunc(x)*100+pifunc(x)*10+goldfunc(x)");
		if (passfunc)
		{
			double d;
			kasm87_showdebug(1, debuf, sizeof(debuf));
			printf("\n%s", debuf);
			for (d = .5; d < 5; d++)
			{
				printf("sillydualfunc(%g) = %g\n", d, passfunc(d, getdiggoldrat, getdigpi));
			}
			kasm87free(passfunc);
		}
		else
			puts(kasm87err);
	}

	// Example #5: Passing user functions with 2 variables by pointer
	{
		EVALFUNC passfunc = (EVALFUNC)kasm87("(x,y,ang2vec(,))ang2vec(x,y)");
		if (passfunc)
		{
			double x, y;
			puts("");
			for (i = 1; i >= -1; i -= 2)
			{
				x = 1;
				y = (double)i;
				printf("dumbanglefunc(%g,%g) = %g (should be %g)\n", x, y, passfunc(x, y, getangle360), getangle360(x, y));
			}
			kasm87free(passfunc);
		}
		else
			puts(kasm87err);
	}

	// Example #6: Passing variables by pointer
	{
		EVALFUNC passfunc = (EVALFUNC)kasm87("(ang,&x,&y)ang*=PI/180;x=cos(ang);y=sin(ang);");
		if (passfunc)
		{
			double a, x, y;
			kasm87_showdebug(1, debuf, sizeof(debuf));
			printf("\n%s", debuf);
			a = 30;
			x = y = -17.0;
			passfunc(a, &x, &y);
			printf("getunitvector(%g) = %g,%g\n", a, x, y);
			printf("        [Should be: %g,%g]\n", cos(a * PI / 180), sin(a * PI / 180));
			kasm87free(passfunc);
		}
		else
			puts(kasm87err);
	}

	// Example #7: More with pointers
	{
		EVALFUNC passfunc = (EVALFUNC)kasm87("(x,y,&r,&g,&b)r=x+y;g=x*y;b=x/y;");
		if (passfunc)
		{
			double x, y, r, g, b;
			kasm87_showdebug(1, debuf, sizeof(debuf));
			printf("\n%s", debuf);
			x = 4;
			y = 3;
			r = g = b = -17.0;
			passfunc(x, y, &r, &g, &b);
			printf("getcol(%g,%g) = %g,%g,%g\n", x, y, r, g, b);
			printf("  [Should be: %g,%g,%g]\n", x + y, x * y, x / y);
			kasm87free(passfunc);
		}
		else
			puts(kasm87err);
	}

	// Example #8: Passing arrays by pointers (indices must be hardcoded)
	{
		EVALFUNCP passfunc = (EVALFUNCP)kasm87("(a[3])a[0]+=a[1];a[1]+=a[2];");
		if (passfunc)
		{
			double a[3];
			kasm87_showdebug(1, debuf, sizeof(debuf));
			printf("\n%s", debuf);
			a[0] = 1;
			a[1] = 2;
			a[2] = 3;
			printf("%g,%g,%g  <-Should be: 1 2 3\n", a[0], a[1], a[2]);
			passfunc(a);
			printf("%g,%g,%g  <-           3 5 3\n", a[0], a[1], a[2]);
			passfunc(a);
			printf("%g,%g,%g  <-           8 8 3\n", a[0], a[1], a[2]);
			kasm87free(passfunc);
		}
		else
			puts(kasm87err);
	}

	// Example #9: Compile with external library
	{
		double d;
		evalextyp myext[] =
			{
				{"LOCVAR", &d},
				{"PILUT[24]", pilut},
				{"GETDIGPI()", getdigpi},
				{"GETANG(,)", getangle360},
				{"FUNC4D(,,,)", func4d},
			};
		kasm87addext(myext, sizeof(myext) / sizeof(myext[0]));
		{
			EVALFUNC libfunc = (EVALFUNC)kasm87("(x)func4d(pilut[0],getdigpi(1),getdigpi(2),pilut[3])+x*10000");
			if (libfunc)
			{
				puts("");
				kasm87_showdebug(1, debuf, sizeof(debuf));
				printf("\n%s", debuf);

				for (i = 0; i < 5; i++)
				{
					d = ((double)(((rand() & 32767) * 10) >> 15));
					printf("%g: = ", d);
					printf("%5g ", libfunc(d));
					printf("\n");
				}

				// puts(""); for(d=0;d<(sizeof(pilut)/sizeof(pilut[0]));d++) printf("%g",libfunc(d));

				kasm87free(libfunc);
			}
			else
				puts(kasm87err);
		}
		kasm87addext(0, 0);
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

	// Example #11: Lookup table
	{
		double d, crc32[256];
		long j, k;
		for (i = 255; i >= 0; i--)
		{
			k = i;
			for (j = 8; j; j--)
				k = ((unsigned long)k >> 1) ^ ((-(k & 1)) & 0xedb88320);
			crc32[i] = (double)k;
		}
		{
			EVALFUNC lutfunc = (EVALFUNC)kasm87("(x,crc32[256])crc32[x]");
			if (lutfunc)
			{
				kasm87_showdebug(1, debuf, sizeof(debuf));
				printf("\n%s", debuf);
				for (i = 0; i < 256; i += 85)
				{
					printf("crc32[%3d]: ", i);
					printf("%11.1f ", lutfunc((double)i, crc32));
					printf("%10d\n", (long)crc32[i]);
				}
				kasm87free(lutfunc);
			}
			else
				puts(kasm87err);
		}
	}

	// Example #12: Write buffer
	{
		double d, sq[8];
		{
			// EVALFUNC lutfunc = (EVALFUNC)kasm87("(sc,buf[8])i=7;do{buf[i]=i*i*sc;i--;}while(i>=0);0");
			EVALFUNC lutfunc = (EVALFUNC)kasm87("(sc,buf[8])for(i=0;i<8;i++)buf[i]=i*i*sc;0");
			if (lutfunc)
			{
				kasm87_showdebug(1, debuf, sizeof(debuf));
				printf("\n%s", debuf);
				memset(sq, 0, sizeof(double) * 8);
				lutfunc(3.0, sq);
				for (i = 0; i < 8; i++)
					printf("%g ", sq[i]);
				printf("\n");
				printf("0 3 12 27 48 75 108 147 <- Should be\n");
				kasm87free(lutfunc);
			}
			else
				puts(kasm87err);
		}
	}

	// Example #13: User static buffer
	{
		double d;
		evalextyp myext[] = {"PRINTNUM()", printnum, "SRAND()", mysrand};
		kasm87addext(myext, sizeof(myext) / sizeof(myext[0]));
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
				// kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
				arrfunc(0.0);
				printf("\n");
				kasm87free(arrfunc);
			}
			else
				puts(kasm87err);
		}
	}

	// Example #14: Test behavior of array index out of bounds
	{
		evalextyp myext[] = {"BUF8[8]", buf8, "BUF11[11]", buf11};
		kasm87addext(myext, sizeof(myext) / sizeof(myext[0]));
		{
			EVALFUNC boundtst = (EVALFUNC)kasm87("(dum) for(i=0;i<13;i++) { buf8[i] = i; buf11[i] = i; } 0");
			if (boundtst)
			{
				printf("\n");
				// kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
				for (i = 0; i < 8; i++)
					buf8[i] = -1.0;
				for (i = 0; i < 11; i++)
					buf11[i] = -1.0;
				boundtst(0.0);
				for (i = 0; i < 8; i++)
					printf("%g,", buf8[i]);
				printf("  ");
				for (i = 0; i < 11; i++)
					printf("%g,", buf11[i]);
				printf("\n");
				printf("8,9,10,11,12,5,6,7,  12,1,2,3,4,5,6,7,8,9,10 <- Should be\n");
				kasm87free(boundtst);
			}
			else
				puts(kasm87err);
		}
		kasm87addext(0, 0);
	}

	// Example #15: External function passing by pointer
	{
		evalextyp myext[] =
			{
				{"CROSSPROD(,,,,,,&,&,&)", crossprod},
			};
		kasm87addext(myext, sizeof(myext) / sizeof(myext[0]));
		{
			EVALFUNCP makevec = (EVALFUNCP)kasm87("(&x,&y,&z)crossprod(y,z,x,z,x,y,&x,&y,&z);x*=10;y*=10;z*=10;");
			if (makevec)
			{
				puts("");
				kasm87_showdebug(1, debuf, sizeof(debuf));
				printf("\n%s", debuf);
				v[0] = 2;
				v[1] = 3;
				v[2] = 5;
				makevec(&v[0], &v[1], &v[2]);
				printf("Should be:[130,40,-140]\n");
				printf("Result is: %g,%g,%g\n", v[0], v[1], v[2]);
				kasm87free(makevec);
			}
			else
				puts(kasm87err);
		}
		kasm87addext(0, 0);
	}

	// Example #16: Eval function calling another Eval function
	{
		EVALFUNC rot2d, doubrot;
		rot2d = (EVALFUNC)kasm87("(a,&x,&y)a*=PI/180;c=cos(a);s=sin(a);ox=x;x=x*c-y*s;y=y*c+ox*s;");
		if (rot2d)
		{
			evalextyp myext[] = {{"ROT2D(,&,&)", rot2d}};
			// kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
			kasm87addext(myext, sizeof(myext) / sizeof(myext[0]));
			doubrot = (EVALFUNC)kasm87("(x,y,&x2,&y2)x2=x;y2=y;rot2d(45,&x2,&y2);x2++;rot2d(-45,&x2,&y2);");
			if (doubrot)
			{
				// kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
				doubrot(1.0, 0.0, &v[0], &v[1]);
				printf("\nShould be:[1.70711,-0.707107]\nResult is: %g,%g\n", v[0], v[1]);
			}
			else
				puts(kasm87err);
		}
		else
			puts(kasm87err);
		kasm87free(doubrot);
		kasm87free(rot2d);
		kasm87addext(0, 0);
	}

	// Example #17: Eval function calling another Eval function
	{
		EVALFUNC cubesum = (EVALFUNC)kasm87("(x,y) { return(cube(x)+cube(y)); } cube(x) { return(x*x*x); }");
		if (cubesum)
		{
			printf("\nShould be:[35]\nResult is: %g\n", cubesum(2.0, 3.0));
		}
		else
			puts(kasm87err);
		kasm87free(cubesum);
		kasm87addext(0, 0);
	}

	// Example #18: Passing string pointer (07/18/2006)
	{
		evalextyp myext[] = {{"PRINTST(,$)", printst}};
		kasm87addext(myext, sizeof(myext) / sizeof(myext[0]));
		{
			EVALFUNC printme = (EVALFUNC)kasm87("(d,$st)printst(d+1,st);printst(d-1,\"[It, works! ( : }\");");
			// kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
			if (printme)
				printme(3, "Hello :)");
			else
				puts(kasm87err);
			kasm87free(printme);
		}
		kasm87addext(0, 0);
	}

	// Example #19: Passing multidimensional array (03/01/2007)
	{
		evalextyp myext[] = {{"BUF[3][2]", buf3_2}};
		kasm87addext(myext, sizeof(myext) / sizeof(myext[0]));
		{
			EVALFUNC multidim = (EVALFUNC)kasm87("(x)buf[3]+buf[1][0]+buf[2][1]+x;");
			// kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n%s",debuf);
			if (multidim)
				printf("\n\nEx.19: Value is:%g\n     [Should be:28.4]", multidim(3.4));
			else
				puts(kasm87err);
			kasm87free(multidim);
		}
		kasm87addext(0, 0);
	}
}

#endif

#if 0
!endif
#endif
