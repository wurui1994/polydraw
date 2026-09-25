
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
#define max(a,b)  (((a) > (b)) ? (a) : (b))
#endif
#if !defined(min)
#define min(a,b)  (((a) < (b)) ? (a) : (b))
#endif

#if _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h> //for VirtualProtect()
#endif

//----------------------------------------- KASM87 BEGINS -----------------------------------------

#ifndef COMPILE
	//if `COMPILE` is not specified in the makefile, choose the fastest supported option
#if defined(_M_IX86) || defined(__i386__)
#define COMPILE 1 //True compile (Windows/Linux)
#else
#define COMPILE 0 //Virtual machine (PowerPC)
#endif
#endif

enum
{
	PARAM0=0,NUL=PARAM0,GOTO,RETURN, RND,NRND,
	PARAM1,  NOP=PARAM1,MOV,NEGMOV,NEQU0, IF0,IF1,
				FABS,SGN,UNIT,FLOOR,CEIL,ROUND0,ROUND0_32,SIN,COS,TAN,ASIN,ACOS,ATAN,SQRT,EXP,FACT,LOG,
	PARAM2,  TIMES=PARAM2,SLASH,PERC,PLUS,MINUS,LES,LESEQ,MOR,MOREQ,EQU,NEQU,LAND,LOR,
				POW,MIN,MAX,FADD,FMOD,ATAN2,LOGB,PEEK,
	PARAM3,  POKE,POKETIMES,POKESLASH,POKEPERC,POKEPLUS,POKEMINUS,
				USERFUNC,
	PARAMEND
};
#define KEAX 0x00000000 // -
#define KECX 0x10000000 //Local variable (moved to KFST/KESP for compiled)
#define KEDX 0x20000000 //Constants (doubles/strings/arrays)
#define KEBX 0x30000000 // -
#define KESP 0x40000000 //Function parameter
#define KEBP 0x50000000 // -
#define KESI 0x60000000 // -
#define KEDI 0x70000000 // -
#define KEIP 0x80000000 //Jump location for GOTO/USERFUNC/IF*
#define KFST 0x90000000 //Floating point stack (lowest 4 local variables)
#define KPTR 0xa0000000 //Pointer to function parameter (addressed by ESP)
#define KIMM 0xb0000000 //Immediate address from evalextyp[?].ptr
#define KSTR 0xc0000000 //String table (moved to end of KEDX for compiled)
#define KARR 0xd0000000 //Array table (moved to end of KEDX for compiled)
#define KGLB 0xe0000000 //Global static (behaves similar to KARR&KIMM, but separate list)
#define KUNUSED (KEDX+1)        //Make parameter act like constant (best for optimization) and not match anything

	//min/max values for exp: -745.13321910194116528 (-log(2)*(1024+51)) and 709.78271289338396 (log(2)*1024)
#define PI 3.14159265358979323
#ifdef _MSC_VER

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
#define lnglng(x) x ## ll
#define stricmp strcasecmp
#endif


typedef struct { long i; double v; } initval_t;
typedef struct
{
	long r;      //pointer to register family & offset
	long maxind; //Maximum index for arrays (0 if not an array)
	long parnum; //>=0: # parameters for user functions. <0: not a function; # = 1's complement of # dimensions
	long proti;  //For funcs/arrays: newvarnam index. FuncProto:{d=double,D=double*}, ArrayDims:{(~parnum)*4}
	long nami;   //index to start of variable/function`s name string in newvarnam
	long hashn;  //hash index for variable/function name (for faster string finding & function overloading)
} newvartyp; //maxlabs

typedef struct { long addr, val; } jumpback_t;

#define MAXPARMS (1+2) //Output + #Inputs (for > 2 inputs, use rxi)
typedef struct
{
	long r; //register family (EAX,ECX,EDX,ESP,etc...) in highest 4 bits, and offset in lower 28 bits
	long q; //additional info (array index, which user function)
	long nv; //newvar index
} rtyp;
typedef struct
{
	long f;           //function enum index
	long g;           //additional info for function
	long n;           //Number of inputs
	rtyp r[MAXPARMS]; //register description
	long rxi;         //Register eXtra Index
} gasmtyp;

#if (COMPILE != 0)
#define FUNCBYTEOFFS 16 //Should be multiple of 16 for alignment speed. Pointer to jumpback table.
#define CODEDATADIST 1024 //Number of bytes separate code and data blocks

	//if (?.ind >= 0) ?.ptr = gevalext[?.ind].ptr (must look up later for user function pointers)
typedef struct { long *lptr; long ind; } patch_t;
#else
#define FUNCBYTEOFFS 0
#endif


#ifdef _MSC_VER

#else

#endif

#ifndef _MSC_VER

#else

#endif


#ifndef _MSC_VER

#else

#endif

	//Ken`s replacement for pow...
#ifndef _MSC_VER

#else

#endif


#if (COMPILE != 0)

#endif


#if (COMPILE == 0)

	//kasm87c: similar functionality to kasm87, but pure C code - making it slower and more portable

	//ANSI va_arg: supported on all compilers
#include <stdarg.h>


#endif
