

//--------------------------------------------------------------------------------------------------

#define MAXZIPS 256 //FIXME: should be dynamic allocation!
static char zipnam[MAXZIPS][MAX_PATH+4];
static long numzips = 0;
double mykzaddstack (char *filnam)
{
	long i;

	for(i=numzips-1;i>=0;i--) if (!stricmp(zipnam[i],filnam)) return(0.0);
	i = strlen(filnam);
	if ((numzips >= MAXZIPS) || (i >= MAX_PATH+4)) return(-1.0);
	memcpy(&zipnam[numzips],filnam,i+1); numzips++;
	kzaddstack(filnam);
	return(0.0);
}

extern void ksrand (int);
double mysrand (double val) { ksrand((int)val); return(0.0); }