

//--------------------------------------------------------------------------------------------------
static HANDLE safecallhand = 0, safecallevent[2] = {0,0};
static volatile int safecall_kill = 0;
static volatile double saferetdouble;
static double (__cdecl *quickfunc)(void);
static unsigned __stdcall eval_highlight_safethread (void *_)
{
	while (1)
	{
		WaitForSingleObject(safecallevent[0],INFINITE);
		if (safecall_kill) break;
		saferetdouble = quickfunc();
		SetEvent(safecallevent[1]);
	}
	return(0);
}

static int eval_highlight (char *ptr, int leng)
{
	double d;
	int i;
	char *quickbuf, tbuf[256];

	quickbuf = (char *)_alloca(leng+3); if (!quickbuf) return(0);

	quickbuf[0] = '('; quickbuf[1] = ')';
	memcpy(&quickbuf[2],ptr,leng);
	quickbuf[leng+2] = 0;
	quickfunc = (double (__cdecl *)(void))kasm87(quickbuf);
	if (!quickfunc) { kputs(kasm87err,1); return(0); }

	if (!safecallhand)
	{
		unsigned win98requiresme;
		for(i=0;i<2;i++) safecallevent[i] = CreateEvent(0,0,0,0);
		safecall_kill = 0;
		safecallhand = (HANDLE)_beginthreadex(0,1048576,eval_highlight_safethread,0,0,&win98requiresme);
	}

	SetEvent(safecallevent[0]);
	if (WaitForSingleObject(safecallevent[1],1000) == WAIT_TIMEOUT)
	{
		kasm87jumpback(quickfunc,0);
		WaitForSingleObject(safecallevent[1],INFINITE);
		kasm87jumpback(quickfunc,1);
		kasm87free((void *)quickfunc);
		kputs("Ctrl+'=' timeout!",1);
		return(0);
	}
	kasm87free((void *)quickfunc);
	_snprintf(tbuf,sizeof(tbuf),"%.20g",saferetdouble);
	kputs(tbuf,1);
	return(1);
}

static eval_highlight_kill (void)
{
	safecall_kill = 1; SetEvent(safecallevent[0]);
	WaitForSingleObject(safecallhand,1000);
}