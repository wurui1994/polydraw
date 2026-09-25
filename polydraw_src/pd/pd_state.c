static int shad[3][SHADMAX], shadn[3] = {0,0,0}, geo2blocki[SHADMAX];
#define PROGMAX 256
static int shadprog[PROGMAX], shadprogn = 0, gcurshader = 0;
typedef struct { int v, g, f, ishw; } shadprogi_t;
static shadprogi_t shadprogi[PROGMAX]; //remember linkages

static OSVERSIONINFO osvi;
static int supporttimerquery = 1;
static GLint queries[1];

static char *prognam = "PolyDraw";
static int oxres = 0, oyres = 0, xres, yres, ActiveApp = 1, shkeystatus = 0;
	//Omni benchmark (2026-09-25): "/bench:N" runs N frames (first 30 warm up and are not
	//timed), appends "script<TAB>fps<TAB>ms_per_frame" to polydraw_bench.txt, then quits.
	//Stock polydraw only shows fps in the title bar, which cannot be batched.
static int gbenchn = 0, gbenchi = 0; static __int64 gbenchq0 = 0;
static int gshaderstuck = 0, gshadercrashed = 0;
static double gfov, dbstatus = 0.0, dkeystatus[256] = {0}