

static unsigned char fakebuf[8], *nfilptr;
static int nbitpos;
static void suckbitsnextblock ()
{
	int n;

	if (!zipfilmode)
	{
		if (!nfilptr)
		{     //|===|===|crc|lng|typ|===|===|
				//        \  fakebuf: /
				//          |===|===|
				//----x     O---x     O--------
			nbitpos = LSWAPIL(*(int *)&filptr[8]);
			nfilptr = (unsigned char *)&filptr[nbitpos+12];
			*(int *)&fakebuf[0] = *(int *)&filptr[0]; //Copy last dword of IDAT chunk
			if (*(int *)&filptr[12] == LSWAPIB(0x54414449)) //Copy 1st dword of next IDAT chunk
				*(int *)&fakebuf[4] = *(int *)&filptr[16];
			filptr = &fakebuf[4]; bitpos -= 32;
		}
		else
		{
			filptr = nfilptr; nfilptr = 0;
			bitpos -= ((nbitpos-4)<<3);
		}
		//if (n_from_suckbits < 4) will it crash?
	}
	else
	{
			//NOTE: should only read bytes inside compsize, not 64K!!! :/
		*(int *)&olinbuf[0] = *(int *)&olinbuf[sizeof(olinbuf)-4];
		n = min((unsigned)(kzfs.compleng-kzfs.comptell),sizeof(olinbuf)-4);
		fread(&olinbuf[4],n,1,kzfs.fil);
		kzfs.comptell += n;
		bitpos -= ((sizeof(olinbuf)-4)<<3);
	}
}

static _inline int peekbits (int n) { return((LSWAPIB(*(int *)&filptr[bitpos>>3])>>(bitpos&7))&pow2mask[n]); }
static _inline void suckbits (int n) { bitpos += n; if (bitpos >= 0) suckbitsnextblock(); }
static _inline int getbits (int n) { int i = peekbits(n); suckbits(n); return(i); }

static int hufgetsym (int *hitab, int *hbmax)
{
	int v, n;

	v = n = 0;
	do { v = (v<<1)+getbits(1)+hbmax[n]-hbmax[n+1]; n++; } while (v >= 0);
	return(hitab[hbmax[n]+v]);
}

	//inbuf[inum] : Bit length of each symbol
	//inum        : Number of indices
	//hitab[inum] : Indices from size-ordered list to original symbol
	//hbmax[0-31] : Highest index (+1) of n-bit symbol
static void hufgencode (int *inbuf, int inum, int *hitab, int *hbmax)
{
	int i, tbuf[31];

	for(i=30;i;i--) tbuf[i] = 0;
	for(i=inum-1;i>=0;i--) tbuf[inbuf[i]]++;
	tbuf[0] = hbmax[0] = 0; //Hack to remove symbols of length 0?
	for(i=0;i<31;i++) hbmax[i+1] = hbmax[i]+tbuf[i];
	for(i=0;i<inum;i++) if (inbuf[i]) hitab[hbmax[inbuf[i]]++] = i;
}

static void huffgetval (int index, int curbits, int num, int *daval, int *dabits)
{
	int b, v, pow2, *hmax;

	hmax = &hufmaxatbit[index][0];
	pow2 = pow2long[curbits-1];
	if (num&pow2) v = 1; else v = 0;
	for(b=1;b<=16;b++)
	{
		if (v < hmax[b])
		{
			*dabits = b;
			*daval = huftable[index][hufvalatbit[index][b]+v];
			return;
		}
		pow2 >>= 1; v <<= 1;
		if (num&pow2) v++;
	}
	*dabits = 16; *daval = 0;
}