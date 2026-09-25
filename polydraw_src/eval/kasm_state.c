
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
static unsigned char oprio[PARAMEND] = {0};
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
__declspec(align(16)) static long kexptval[4] = {0,0x80000000,0,0};
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

static const long pinf = 0x7f800000, ninf = 0xff800000, pind = 0x7fc00000, nind = 0xffc00000;
static const float posone = 1.f, negone = -1.f, pointfive = .5f, oneover2_31 = 1.f/2147483648.f;
//static const float threeup51 = 6755399441055744;

//--------------------------------------------------
static long *funcst = 0; //for initial parsing of functions/global sections
static long maxfuncst = 0;

static long gstatmem = 0; //pointer to global static buffer

static evalextyp *gevalext = 0;
static long gevalextnum = 0;

	//kasm87 parsing temp variables
static long maxops = 0, arrnum;
static long *gop, *gnext, globi, memnum; //maxops

static double *globval; //maxops
static long gccnt;

static char *gstring; //maxst
static long gstnum, maxst = 0;

typedef struct { long i; double v; } initval_t;
static initval_t *ginitval;
static long ginitvalnum, maxinitval = 0;

static long gecnt, gnumarg = 0, gnumglob = 0;
static double *gvl; //regnum*(recursion depth), stack space used by kasm87c only
static double *gvlp;

static long maxvars = 0, maxvarchars = 0;
static char *newvarnam; //maxvarchars (variable name buffer; strings separated by NULL terminator)
static long newvarhash[256], newvarhash_glob[256];
typedef struct
{
	long r;      //pointer to register family & offset
	long maxind; //Maximum index for arrays (0 if not an array)
	long parnum; //>=0: # parameters for user functions. <0: not a function; # = 1's complement of # dimensions
	long proti;  //For funcs/arrays: newvarnam index. FuncProto:{d=double,D=double*}, ArrayDims:{(~parnum)*4}
	long nami;   //index to start of variable/function`s name string in newvarnam
	long hashn;  //hash index for variable/function name (for faster string finding & function overloading)
} newvartyp;
static newvartyp *newvar;
static long newvarnum, newvarplc; //maxvars

static char *enumnam; static long maxenumchars = 0, enumcharplc; //Enum name list (NULL terminator separators)
static double *enumval; static long maxenum = 0, enumnum;        //Enum value list

static long maxlabs = 0, maxlabchars = 0;
static char *newlabnam; //maxlabchars
static long *newlabind, newlabnum, newlabplc; //maxlabs
static long *labpat, *jumpat, *lablinum, numlabels; //maxlabs

typedef struct { long addr, val; } jumpback_t;
static jumpback_t *jumpback = 0;
static long jumpbacknum = 0, maxjumpbacks = 0;

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
static gasmtyp *gasm; //maxops

static rtyp *rxi;
static long numrxi, maxrxi = 0;

#if (COMPILE != 0)
#define FUNCBYTEOFFS 16 //Should be multiple of 16 for alignment speed. Pointer to jumpback table.
#define CODEDATADIST 1024 //Number of bytes separate code and data blocks

	//if (?.ind >= 0) ?.ptr = gevalext[?.ind].ptr (must look up later for user function pointers)
typedef struct { long *lptr; long ind; } patch_t;
static patch_t *patch = 0;
static long patchnum = 0, maxpatch = 0;
#else
#define FUNCBYTEOFFS 0
#endif

static long round0msk[2048][2];
//--------------------------------------------------
static long cputype = 0, cpuinited = 0;