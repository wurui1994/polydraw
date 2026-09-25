
static unsigned char oprio[PARAMEND] = {0};__declspec(align(16)) static long kexptval[4] = {0,0x80000000,0,0};
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
static initval_t *ginitval;
static long ginitvalnum, maxinitval = 0;

static long gecnt, gnumarg = 0, gnumglob = 0;
static double *gvl; //regnum*(recursion depth), stack space used by kasm87c only
static double *gvlp;

static long maxvars = 0, maxvarchars = 0;
static char *newvarnam; //maxvarchars (variable name buffer; strings separated by NULL terminator)
static long newvarhash[256], newvarhash_glob[256];
static newvartyp *newvar;
static long newvarnum, newvarplc; //maxvars

static char *enumnam; static long maxenumchars = 0, enumcharplc; //Enum name list (NULL terminator separators)
static double *enumval; static long maxenum = 0, enumnum;        //Enum value list

static long maxlabs = 0, maxlabchars = 0;
static char *newlabnam; //maxlabchars
static long *newlabind, newlabnum, newlabplc; //maxlabs
static long *labpat, *jumpat, *lablinum, numlabels;
static jumpback_t *jumpback = 0;
static long jumpbacknum = 0, maxjumpbacks = 0;
static gasmtyp *gasm; //maxops

static rtyp *rxi;
static long numrxi, maxrxi = 0;
static patch_t *patch = 0;
static long patchnum = 0, maxpatch = 0;

static void checkfuncst (long i) //note: i is per long, not function
{
	if (i < maxfuncst) return;
	if (!i) { maxfuncst = 256; } else { while (i >= maxfuncst) maxfuncst += (maxfuncst>>2); }
	//printf("maxfuncst=%d\n",maxfuncst);
	if (!(funcst = (long *)realloc(funcst,sizeof(funcst[0])*maxfuncst))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
}

static void checkops (long i)
{
	if (i < maxops) return;
	if (!i) { maxops = 1024; } else { while (i >= maxops) maxops += (maxops>>2); }
	//printf("maxops=%d\n",maxops);
	if (!(      gop = (   long *)realloc(      gop,sizeof(long)   *maxops))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
	if (!(    gnext = (   long *)realloc(    gnext,sizeof(long)   *maxops))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
	if (!(  globval = ( double *)realloc(  globval,sizeof(double) *maxops))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
	if (!(     gasm = (gasmtyp *)realloc(     gasm,sizeof(gasmtyp)*maxops))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
}

static void checkstrings (long i)
{
	if (i < maxst) return;
	if (!i) { maxst = 1024; } else { while (i >= maxst) maxst += (maxst>>2); }
	//printf("maxst=%d\n",maxst);
	if (!(  gstring = (   char *)realloc(  gstring,sizeof(char)   *maxst))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
}

static void checkinitvals (long i)
{
	if (i < maxinitval) return;
	if (!i) { maxinitval = 256; } else { while (i >= maxinitval) maxinitval += (maxinitval>>2); }
	//printf("maxinitval=%d\n",maxinitval);
	if (!(ginitval = (initval_t *)realloc(ginitval,sizeof(initval_t)*maxinitval))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
}

static void checkrxi (long i)
{
	if (i < maxrxi) return;
	if (!i) { maxrxi = 256; } else { while (i >= maxrxi) maxrxi += (maxrxi>>2); }
	//printf("maxrxi=%d\n",maxrxi);
	if (!(rxi = (rtyp *)realloc(rxi,sizeof(rtyp)*maxrxi))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
}

static void checkvarchars (long i)
{
	if (i < maxvarchars) return;
	if (!i) { maxvarchars = 1024; } else { while (i >= maxvarchars) maxvarchars += (maxvarchars>>2); }
	//printf("maxvarchars=%d\n",maxvarchars);
	if (!(newvarnam = (char *)realloc(newvarnam,sizeof(char)*maxvarchars))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
}

static void checkvars (long i)
{
	if (i < maxvars) return;
	if (!i) { maxvars = 256; } else { while (i >= maxvars) maxvars += (maxvars>>2); }
	//printf("maxvars=%d\n",maxvars);
	if (!(newvar = (newvartyp *)realloc(newvar,sizeof(newvartyp)*maxvars))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
}

static void checkenumchars (long i)
{
	if (i < maxenumchars) return;
	if (!i) { maxenumchars = 1024; } else { while (i >= maxenumchars) maxenumchars += (maxenumchars>>2); }
	//printf("maxenumchars=%d\n",maxenumchars);
	if (!(enumnam = (char *)realloc(enumnam,sizeof(char)*maxenumchars))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
}

static void checkenum (long i)
{
	if (i < maxenum) return;
	if (!i) { maxenum = 256; } else { while (i >= maxenum) maxenum += (maxenum>>2); }
	//printf("maxenum=%d\n",maxenum);
	if (!(enumval = (double *)realloc(enumval,sizeof(double)*maxenum))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
}

static void checklabchars (long i)
{
	if (i < maxlabchars) return;
	if (!i) { maxlabchars = 1024; } else { while (i >= maxlabchars) maxlabchars += (maxlabchars>>2); }
	//printf("maxlabchars=%d\n",maxlabchars);
	if (!(newlabnam = (   char *)realloc(newlabnam,sizeof(char)   *maxlabchars))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
}

static void checklabs (long i)
{
	if (i < maxlabs) return;
	if (!i) { maxlabs = 256; } else { while (i >= maxlabs) maxlabs += (maxlabs>>2); }
	//printf("maxlabs=%d\n",maxlabs);
	if (!(newlabind = (   long *)realloc(newlabind,sizeof(long)   *maxlabs))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
	if (!(   labpat = (   long *)realloc(labpat   ,sizeof(long)   *maxlabs))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
	if (!(   jumpat = (   long *)realloc(jumpat   ,sizeof(long)   *maxlabs))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
	if (!( lablinum = (   long *)realloc(lablinum ,sizeof(long)   *maxlabs))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
}

static void checkjumpbacks (long i)
{
	if (i < maxjumpbacks) return;
	if (!i) { maxjumpbacks = 256; } else { while (i >= maxjumpbacks) maxjumpbacks += (maxjumpbacks>>2); }
	//printf("maxjumpbacks=%d\n",maxjumpbacks);
	if (!(jumpback = (jumpback_t *)realloc(jumpback,sizeof(jumpback_t)*maxjumpbacks))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
}static void checkpatch (long i)
{
	if (i < maxpatch) return;
	if (!i) { maxpatch = 256; } else { while (i >= maxpatch) maxpatch += (maxpatch>>2); }
	//printf("maxpatch=%d\n",maxpatch);
	if (!(patch = (patch_t *)realloc(patch,sizeof(patch_t)*maxpatch))) { globi = -1; strcpy(kasm87err,"ERROR: malloc failed"); return; }
}