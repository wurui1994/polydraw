

static long isvarchar (unsigned char ch)
{
	static const long isvarcharbuf[8] = {0,0x03ff0000,0x87fffffe,0x07fffffe,0,0,0,0};
#if defined(_M_IX86) || defined(__i386__)
	return(isvarcharbuf[ch>>5]&(1<<ch)); //WARNING: Shift auto-modulo`d by 32 trick only works on Intel CPUs!
#elif 1
	return(isvarcharbuf[ch>>5]&(1<<(ch&31)));
#endif
	//return(((ch >= '0') && (ch <= '9')) || ((ch >= 'A') && (ch <= 'Z')) || (ch == '_') || ((ch >= 'a') && (ch <= 'z')));
}

static long getnewvarhash (const char *st)
{
	long i, hashind;
	char ch;

	for(i=0,hashind=0;st[i];i++)
	{
		ch = st[i]; if ((ch >= 'a') && (ch <= 'z')) ch -= 32;
		hashind = ch - hashind*3;
	}
	return(hashind&(sizeof(newvarhash)/sizeof(newvarhash[0])-1));
}