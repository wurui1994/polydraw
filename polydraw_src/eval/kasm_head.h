#if 0 //To compile as a stand-alone test program, type "nmake eval.c"
!ifndef COMP
COMP=1
!endif
eval.exe: eval.c; cl eval.c /Ox /G6Fy /Gs /MD /nologo /DEVALTEST /DCOMPILE=$(COMP) /link /opt:nowin98 /nologo kernel32.lib
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
#define max(a,b)  (((a) > (b)) ? (a) : (b))
#endif
#if !defined(min)
#define min(a,b)  (((a) < (b)) ? (a) : (b))
#endif

#if _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h> //for VirtualProtect()
#endif
