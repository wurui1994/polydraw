

//--------------------------------------------------------------------------------------------------
static void kputs (const char *st, int addcr)
{
	static char buf[8192];
	static int bufleng = 0, obufleng;
	int i, j, stleng, iminmod;

	if (!st) return;
	stleng = 2; for(i=0;st[i];i++) if (st[i] == '\n') stleng++; //calculate processed string length
	stleng += i; if (stleng >= sizeof(buf)-1) return;

		//Remove lines at the top if necessary
	j = 0; iminmod = bufleng; obufleng = bufleng;
	while (bufleng-j+stleng >= sizeof(buf)-1) { for(;j<bufleng;j++) if (buf[j] == '\n') { j++; break; } }
	if (j) { bufleng -= j; memmove(&buf[0],&buf[j],bufleng+1); iminmod = 0; }

	for(j=0;st[j];j++)
	{
		if (st[j] == '\r')
		{
			while ((bufleng > 0) && (buf[bufleng-1] != '\n')) bufleng--;
			if (iminmod) iminmod = bufleng;
			continue;
		}
		if (st[j] == '\n') { buf[bufleng] = '\r'; bufleng++; }
		buf[bufleng] = st[j]; bufleng++;
	}
	if (addcr)
	{
		buf[bufleng] = '\r'; bufleng++;
		buf[bufleng] = '\n'; bufleng++;
	}
	buf[bufleng] = 0;

	if (!iminmod)
	{
		SendMessage(hWndCons,WM_SETTEXT,0,(long)buf); //SetWindowText(hWndCons,buf);
		SendMessage(hWndCons,EM_LINESCROLL,0,0x7fffffff);
	}
	else
	{
		SendMessage(hWndCons,EM_SETSEL,iminmod,obufleng);
		SendMessage(hWndCons,EM_REPLACESEL,0,(LPARAM)&buf[iminmod]);
	}
}