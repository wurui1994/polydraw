

//===============================  DDS ends ==================================
//=================== External picture interface begins ======================

int kpgetdim (const char *buf, int leng, int *xsiz, int *ysiz)
{
	int *lptr;
	const unsigned char *cptr;
	unsigned char *ubuf = (unsigned char *)buf;

	(*xsiz) = (*ysiz) = 0; if (leng < 16) return(KPLIB_NONE);
	if ((ubuf[0] == 0x89) && (ubuf[1] == 0x50)) //.PNG
	{
		lptr = (int *)buf;
		if ((lptr[0] != LSWAPIB(0x474e5089)) || (lptr[1] != LSWAPIB(0x0a1a0a0d))) return(KPLIB_NONE);
		lptr = &lptr[2];
		while (((UINT_PTR)lptr-(UINT_PTR)buf) < (UINT_PTR)(leng-16))
		{
			if (lptr[1] == LSWAPIB(0x52444849)) //IHDR
				{ (*xsiz) = LSWAPIL(lptr[2]); (*ysiz) = LSWAPIL(lptr[3]); return(KPLIB_PNG); }
			lptr = (int *)((INT_PTR)lptr + LSWAPIL(lptr[0]) + 12);
		}
		return(KPLIB_NONE);
	}
	else if ((ubuf[0] == 0xff) && (ubuf[1] == 0xd8)) //.JPG
	{
		cptr = (unsigned char *)&buf[2];
		while (((UINT_PTR)cptr-(UINT_PTR)buf) < (UINT_PTR)(leng-8))
		{
			if ((cptr[0] != 0xff) || (cptr[1] == 0xff)) { cptr++; continue; }
			if ((unsigned int)(cptr[1]-0xc0) < 3)
			{
				(*ysiz) = SSWAPIL(*(unsigned short *)&cptr[5]);
				(*xsiz) = SSWAPIL(*(unsigned short *)&cptr[7]);
				return(KPLIB_JPG);
			}
			cptr = &cptr[SSWAPIL(*(unsigned short *)&cptr[2])+2];
		}
		return(KPLIB_NONE);
	}
	else if ((ubuf[0] == 'G') && (ubuf[1] == 'I') && (ubuf[2] == 'F')) //.GIF
	{
		(*xsiz) = (int)SSWAPIB(*(unsigned short *)&buf[6]);
		(*ysiz) = (int)SSWAPIB(*(unsigned short *)&buf[8]);
		return(KPLIB_GIF);
	}
	else if ((ubuf[0] == 0x19) && (ubuf[1] == 0x91) && (ubuf[10] == 8) && (ubuf[11] == 0)) //old .CEL/.PIC
	{
		(*xsiz) = (int)SSWAPIB(*(unsigned short *)&buf[2]);
		(*ysiz) = (int)SSWAPIB(*(unsigned short *)&buf[4]);
		return(KPLIB_CEL);
	}
	else if ((ubuf[0] == 'B') && (ubuf[1] == 'M')) //.BMP
	{
		if (*(int *)(&buf[14]) == LSWAPIB(12)) //OS/2 1.x (old format)
		{
			if (*(short *)(&buf[22]) != SSWAPIB(1)) return(KPLIB_NONE);
			(*xsiz) = (int)SSWAPIB(*(unsigned short *)&buf[18]);
			(*ysiz) = (int)SSWAPIB(*(unsigned short *)&buf[20]);
		}
		else //All newer formats...
		{
			if (*(short *)(&buf[26]) != SSWAPIB(1)) return(KPLIB_NONE);
			(*xsiz) = LSWAPIB(*(int *)&buf[18]);
			(*ysiz) = LSWAPIB(*(int *)&buf[22]);
		}
		return(KPLIB_BMP);
	}
	else if (*(int *)ubuf == LSWAPIB(0x0801050a)) //.PCX
	{
		(*xsiz) = SSWAPIB(*(short *)&buf[ 8])-SSWAPIB(*(short *)&buf[4])+1;
		(*ysiz) = SSWAPIB(*(short *)&buf[10])-SSWAPIB(*(short *)&buf[6])+1;
		return(KPLIB_PCX);
	}
	else if ((*(int *)ubuf == LSWAPIB(0x20534444)) && (*(int *)&ubuf[4] == LSWAPIB(124))) //.DDS
	{
		(*xsiz) = LSWAPIB(*(int *)&buf[16]);
		(*ysiz) = LSWAPIB(*(int *)&buf[12]);
		return(KPLIB_DDS);
	}
	else
	{     //Unreliable .TGA identification - this MUST be final case!
		if ((leng >= 19) && (!(ubuf[1]&0xfe)))
			if ((ubuf[2] < 12) && ((1<<ubuf[2])&0xe0e))
				if ((!(ubuf[16]&7)) && (ubuf[16] != 0) && (ubuf[16] <= 32))
					if (!(buf[17]&0xc0))
					{
						(*xsiz) = (int)SSWAPIB(*(unsigned short *)&buf[12]);
						(*ysiz) = (int)SSWAPIB(*(unsigned short *)&buf[14]);
						return(KPLIB_TGA);
					}
	}
	return(KPLIB_NONE);
}

int kprender (const char *buf, int leng, INT_PTR frameptr, int bpl,
					int xdim, int ydim, int xoff, int yoff)
{
	unsigned char *ubuf = (unsigned char *)buf;

	kplib_paleng = 0; kplib_bakcol = 0; kplib_numhufblocks = 0; kplib_zlibcompflags = 0;

	if ((ubuf[0] == 0x89) && (ubuf[1] == 0x50)) //.PNG
		return(kpngrend(buf,leng,frameptr,bpl,xdim,ydim,xoff,yoff));

	if ((ubuf[0] == 0xff) && (ubuf[1] == 0xd8)) //.JPG
		return(kpegrend(buf,leng,frameptr,bpl,xdim,ydim,xoff,yoff));

	if ((ubuf[0] == 'G') && (ubuf[1] == 'I') && (ubuf[2] == 'F')) //.GIF
		return(kgifrend(buf,leng,frameptr,bpl,xdim,ydim,xoff,yoff));

	if ((ubuf[0] == 0x19) && (ubuf[1] == 0x91) && (ubuf[10] == 8) && (ubuf[11] == 0)) //old .CEL/.PIC
		return(kcelrend(buf,leng,frameptr,bpl,xdim,ydim,xoff,yoff));

	if ((ubuf[0] == 'B') && (ubuf[1] == 'M')) //.BMP
		return(kbmprend(buf,leng,frameptr,bpl,xdim,ydim,xoff,yoff));

	if (*(int *)ubuf == LSWAPIB(0x0801050a)) //.PCX
		return(kpcxrend(buf,leng,frameptr,bpl,xdim,ydim,xoff,yoff));

	if ((*(int *)ubuf == LSWAPIB(0x20534444)) && (*(int *)&ubuf[4] == LSWAPIB(124))) //.DDS
		return(kddsrend(buf,leng,frameptr,bpl,xdim,ydim,xoff,yoff));

		//Unreliable .TGA identification - this MUST be final case!
	if ((leng >= 19) && (!(ubuf[1]&0xfe)))
		if ((ubuf[2] < 12) && ((1<<ubuf[2])&0xe0e))
			if ((!(ubuf[16]&7)) && (ubuf[16] != 0) && (ubuf[16] <= 32))
				if (!(ubuf[17]&0xc0))
					return(ktgarend(buf,leng,frameptr,bpl,xdim,ydim,xoff,yoff));

	return(-1);
}

//==================== External picture interface ends =======================

	//Brute-force case-insensitive, slash-insensitive, * and ? wildcard matcher
	//Given: string i and string j. string j can have wildcards
	//Returns: 1:matches, 0:doesn't match
static int wildmatch (const char *i, const char *j)
{
	const char *k;
	char c0, c1;

	if (!*j) return(1);
	do
	{
		if (*j == '*')
		{
			for(k=i,j++;*k;k++) if (wildmatch(k,j)) return(1);
			continue;
		}
		if (!*i) return(0);
		if (*j == '?') { i++; j++; continue; }
		c0 = *i; if ((c0 >= 'a') && (c0 <= 'z')) c0 -= 32;
		c1 = *j; if ((c1 >= 'a') && (c1 <= 'z')) c1 -= 32;
		if (c0 == '/') c0 = '\\';
		if (c1 == '/') c1 = '\\';
		if (c0 != c1) return(0);
		i++; j++;
	} while (*j);
	return(!*i);
}

	//Same as: stricmp(st0,st1) except: '/' == '\'
static int filnamcmp (const char *st0, const char *st1)
{
	int i;
	char ch0, ch1;

	for(i=0;st0[i];i++)
	{
		ch0 = st0[i]; if ((ch0 >= 'a') && (ch0 <= 'z')) ch0 -= 32;
		ch1 = st1[i]; if ((ch1 >= 'a') && (ch1 <= 'z')) ch1 -= 32;
		if (ch0 == '/') ch0 = '\\';
		if (ch1 == '/') ch1 = '\\';
		if (ch0 != ch1) return(-1);
	}
	if (!st1[i]) return(0);
	return(-1);
}

//===================== ZIP decompression code begins ========================

	//format: (used by kzaddstack/kzopen to cache file name&start info)
	//[char zipnam[?]\0]
	//[next hashindex/-1][next index/-1][zipnam index][fileoffs][fileleng][iscomp][char filnam[?]\0]
	//[next hashindex/-1][next index/-1][zipnam index][fileoffs][fileleng][iscomp][char filnam[?]\0]
	//...
	//[char zipnam[?]\0]
	//[next hashindex/-1][next index/-1][zipnam index][fileoffs][fileleng][iscomp][char filnam[?]\0]
	//[next hashindex/-1][next index/-1][zipnam index][fileoffs][fileleng][iscomp][char filnam[?]\0]
	//...
#define KZHASHINITSIZE 8192
static char *kzhashbuf = 0;
static int kzhashead[256], kzhashpos, kzlastfnam, kzhashsiz, kzdirnamhead = -1;