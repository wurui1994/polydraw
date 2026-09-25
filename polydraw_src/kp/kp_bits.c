
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

	//This did not result in a speed-up on P4-3.6Ghz (02/22/2005)
//static int hufgetsym_skipb (int *hitab, int *hbmax, int n, int addit)
//{
//   int v;
//
//   v = bitrev(getbits(n),n)+addit;
//   do { v = (v<<1)+getbits(1)+hbmax[n]-hbmax[n+1]; n++; } while (v >= 0);
//   return(hitab[hbmax[n]+v]);
//}

static void qhufgencode (int *hitab, int *hbmax, int *qhval, unsigned char *qhbit, int numbits)
{
	int i, j, k, n, r;

		//r is the bit reverse of i. Ex: if: i = 1011100111, r = 1110011101
	i = r = 0;
	for(n=1;n<=numbits;n++)
		for(k=hbmax[n-1];k<hbmax[n];k++)
			for(j=i+pow2mask[numbits-n];i<=j;i++)
			{
				r = bitrev(i,numbits);
				qhval[r] = hitab[k];
				qhbit[r] = (char)n;
			}
	for(j=pow2mask[numbits];i<=j;i++)
	{
		r = bitrev(i,numbits);

		//k = 0;
		//for(n=0;n<numbits;n++)
		//   k = (k<<1) + ((r>>n)&1) + hbmax[n]-hbmax[n+1];
		//
		//n = numbits;
		//k = hbmax[n]-r;
		//
		//j = peekbits(LOGQHUFSIZ); i = qhufval[j]; j = qhufbit[j];
		//
		//i = j = 0;
		//do
		//{
		//   i = (i<<1)+getbits(1)+nbuf0[j]-nbuf0[j+1]; j++;
		//} while (i >= 0);
		//i = ibuf0[nbuf0[j]+i];
		//qhval[r] = k;

		qhbit[r] = 0; //n-32;
	}

	//   //hufgetsym_skipb related code:
	//for(k=n=0;n<numbits;n++) k = (k<<1)+hbmax[n]-hbmax[n+1];
	//return(k);
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