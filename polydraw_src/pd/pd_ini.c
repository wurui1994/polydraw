

//--------------------------------------------------------------------------------------------------
static char gexefullpath[MAX_PATH] = "", gexedironly[MAX_PATH] = "", ginifilnam[MAX_PATH] = "";

typedef struct
{
	int rendcorn, fullscreen, clearbuffer, timeout, fontheight, fontwidth, compctrlent, sepchar;
	char fontname[256];
} popt_t;
static popt_t popts, opopts;

static void loadini (void)
{
	char tbuf[512];

	popts.rendcorn    =   0;
	popts.fullscreen  =   0;
    popts.clearbuffer =   1;
	popts.timeout     = 250;
	popts.fontheight  = -13;
	popts.fontwidth   =   0;
	popts.compctrlent =   0;
	popts.sepchar     = '-';
	strcpy(popts.fontname,"Courier");

	popts.rendcorn    = min(max(        GetPrivateProfileInt("POLYDRAW","rendcorn"   ,popts.rendcorn   ,ginifilnam),    0),   4);
	popts.fullscreen  = min(max(        GetPrivateProfileInt("POLYDRAW","fullscreen" ,popts.fullscreen ,ginifilnam),    0),   1);
	popts.clearbuffer = min(max(        GetPrivateProfileInt("POLYDRAW","clearbuffer",popts.clearbuffer,ginifilnam),    0),   1);
	popts.timeout     = min(max(        GetPrivateProfileInt("POLYDRAW","timeout"    ,popts.timeout    ,ginifilnam),    0),5000);
	popts.fontheight  = min(max((signed)GetPrivateProfileInt("POLYDRAW","fontheight" ,popts.fontheight ,ginifilnam),-1000),1000);
	popts.fontwidth   = min(max((signed)GetPrivateProfileInt("POLYDRAW","fontwidth"  ,popts.fontwidth  ,ginifilnam),-1000),1000);
	popts.compctrlent = min(max(        GetPrivateProfileInt("POLYDRAW","compctrlent",popts.compctrlent,ginifilnam),    0),   1);
	popts.sepchar     = min(max(        GetPrivateProfileInt("POLYDRAW","sepchar"    ,popts.sepchar    ,ginifilnam),    0), 255);
	GetPrivateProfileString("POLYDRAW","fontname",popts.fontname,popts.fontname,sizeof(popts.fontname),ginifilnam);

	memcpy(&opopts,&popts,sizeof(opopts));
}

static void saveini (void)
{
	char tbuf[512];

	if (!memcmp(&opopts,&popts,sizeof(opopts))) return;
	sprintf(tbuf,"%d",popts.rendcorn   ); WritePrivateProfileString("POLYDRAW","rendcorn"   ,tbuf,ginifilnam);
	sprintf(tbuf,"%d",popts.fullscreen ); WritePrivateProfileString("POLYDRAW","fullscreen" ,tbuf,ginifilnam);
	sprintf(tbuf,"%d",popts.clearbuffer); WritePrivateProfileString("POLYDRAW","clearbuffer",tbuf,ginifilnam);
	sprintf(tbuf,"%d",popts.timeout    ); WritePrivateProfileString("POLYDRAW","timeout"    ,tbuf,ginifilnam);
	sprintf(tbuf,"%d",popts.fontheight ); WritePrivateProfileString("POLYDRAW","fontheight" ,tbuf,ginifilnam);
	sprintf(tbuf,"%d",popts.fontwidth  ); WritePrivateProfileString("POLYDRAW","fontwidth"  ,tbuf,ginifilnam);
	sprintf(tbuf,"%d",popts.compctrlent); WritePrivateProfileString("POLYDRAW","compctrlent",tbuf,ginifilnam);
	sprintf(tbuf,"%d",popts.sepchar    ); WritePrivateProfileString("POLYDRAW","sepchar"    ,tbuf,ginifilnam);
	sprintf(tbuf,"%s",popts.fontname   ); WritePrivateProfileString("POLYDRAW","fontname"   ,tbuf,ginifilnam);
}