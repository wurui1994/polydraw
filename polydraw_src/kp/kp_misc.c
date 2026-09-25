

static int kgifrend (const char *kfilebuf, int kfilelength,
	INT_PTR daframeplace, int dabytesperline, int daxres, int dayres,
	int daglobxoffs, int daglobyoffs)
{
	int i, x, y, xsiz, ysiz, yinc, xend, xspan, yspan, currstr, numbitgoal;
	int lzcols, dat, blocklen, bitcnt, xoff, yoff, transcol, backcol, *lptr;
	INT_PTR p;
	char numbits, startnumbits, chunkind, ilacefirst;
	const unsigned char *ptr, *cptr;

	kplib_coltype = 3; kplib_bitdepth = 8; //For PNGOUT

	if ((kfilebuf[0] != 'G') || (kfilebuf[1] != 'I') || (kfilebuf[2] != 'F')) return(-1);
	kplib_paleng = (1<<((kfilebuf[10]&7)+1));
	ptr = (unsigned char *)&kfilebuf[13];
	if (kfilebuf[10]&128) { cptr = ptr; ptr += kplib_paleng*3; }
	transcol = -1;
	while ((chunkind = *ptr++) == '!')
	{      //! 0xf9 leng flags ?? ?? transcol
		if (ptr[0] == 0xf9) { if (ptr[2]&1) transcol = (int)(((unsigned char)ptr[5])); }
		ptr++;
		do { i = *ptr++; ptr += i; } while (i);
	}
	if (chunkind != ',') return(-1);

	xoff = SSWAPIB(*(unsigned short *)&ptr[0]);
	yoff = SSWAPIB(*(unsigned short *)&ptr[2]);
	xspan = SSWAPIB(*(unsigned short *)&ptr[4]);
	yspan = SSWAPIB(*(unsigned short *)&ptr[6]); ptr += 9;
	if (ptr[-1]&64) { yinc = 8; ilacefirst = 1; }
				  else { yinc = 1; ilacefirst = 0; }
	if (ptr[-1]&128)
	{
		kplib_paleng = (1<<((ptr[-1]&7)+1));
		cptr = ptr; ptr += kplib_paleng*3;
	}

	for(i=0;i<kplib_paleng;i++)
		kplib_palcol[i] = LSWAPIB((((int)cptr[i*3])<<16) + (((int)cptr[i*3+1])<<8) + ((int)cptr[i*3+2]) + 0xff000000);
	for(;i<256;i++) kplib_palcol[i] = LSWAPIB(0xff000000);
	if (transcol >= 0) kplib_palcol[transcol] &= LSWAPIB(~0xff000000);

		//Handle GIF files with different logical&image sizes or non-0 offsets (added 05/15/2004)
	xsiz = SSWAPIB(*(unsigned short *)&kfilebuf[6]);
	ysiz = SSWAPIB(*(unsigned short *)&kfilebuf[8]);
	if ((xoff != 0) || (yoff != 0) || (xsiz != xspan) || (ysiz != yspan))
	{
		int xx[4], yy[4];
		if (kfilebuf[10]&128) backcol = kplib_palcol[(unsigned char)kfilebuf[11]]; else backcol = 0;

			//Fill border to backcol
		xx[0] = max(daglobxoffs           ,     0); yy[0] = max(daglobyoffs           ,     0);
		xx[1] = min(daglobxoffs+xoff      ,daxres); yy[1] = min(daglobyoffs+yoff      ,dayres);
		xx[2] = max(daglobxoffs+xoff+xspan,     0); yy[2] = min(daglobyoffs+yoff+yspan,dayres);
		xx[3] = min(daglobxoffs+xsiz      ,daxres); yy[3] = min(daglobyoffs+ysiz      ,dayres);

		lptr = (int *)(yy[0]*dabytesperline+daframeplace);
		for(y=yy[0];y<yy[1];y++,lptr=(int *)(((INT_PTR)lptr)+dabytesperline))
			for(x=xx[0];x<xx[3];x++) lptr[x] = backcol;
		for(;y<yy[2];y++,lptr=(int *)(((INT_PTR)lptr)+dabytesperline))
		{  for(x=xx[0];x<xx[1];x++) lptr[x] = backcol;
			for(x=xx[2];x<xx[3];x++) lptr[x] = backcol;
		}
		for(;y<yy[3];y++,lptr=(int *)(((INT_PTR)lptr)+dabytesperline))
			for(x=xx[0];x<xx[3];x++) lptr[x] = backcol;

		daglobxoffs += xoff; //Offset bitmap image by extra amount
		daglobyoffs += yoff;
	}

	xspan += daglobxoffs;
	yspan += daglobyoffs;  //UGLY HACK
	y = daglobyoffs;
	if ((unsigned int)y < (unsigned int)dayres)
		{ p = y*dabytesperline+daframeplace; x = daglobxoffs; xend = xspan; }
	else
		{ x = daglobxoffs+0x80000000; xend = xspan+0x80000000; }

	lzcols = (1<<(*ptr)); startnumbits = (char)((*ptr)+1); ptr++;
	for(i=lzcols-1;i>=0;i--) { suffix[i] = (char)(prefix[i] = i); }
	currstr = lzcols+2; numbits = startnumbits; numbitgoal = (lzcols<<1);
	blocklen = *ptr++;
	memcpy(filbuffer,ptr,blocklen); ptr += blocklen;
	bitcnt = 0;
	while (1)
	{
		dat = (LSWAPIB(*(int *)&filbuffer[bitcnt>>3])>>(bitcnt&7)) & (numbitgoal-1);
		bitcnt += numbits;
		if ((bitcnt>>3) > blocklen-3)
		{
			*(short *)filbuffer = *(short *)&filbuffer[bitcnt>>3];
			i = blocklen-(bitcnt>>3);
			blocklen = (int)*ptr++;
			memcpy(&filbuffer[i],ptr,blocklen); ptr += blocklen;
			bitcnt &= 7; blocklen += i;
		}
		if (dat == lzcols)
		{
			currstr = lzcols+2; numbits = startnumbits; numbitgoal = (lzcols<<1);
			continue;
		}
		if ((currstr == numbitgoal) && (numbits < 12))
			{ numbits++; numbitgoal <<= 1; }

		prefix[currstr] = dat;
		for(i=0;dat>=lzcols;dat=prefix[dat]) tempstack[i++] = suffix[dat];
		tempstack[i] = (char)prefix[dat];
		suffix[currstr-1] = suffix[currstr] = (char)dat;

		for(;i>=0;i--)
		{
			if ((unsigned int)x < (unsigned int)daxres)
				*(int *)(p+(x<<2)) = kplib_palcol[(int)tempstack[i]];
			x++;
			if (x == xend)
			{
				y += yinc;
				if (y >= yspan)
					switch(yinc)
					{
						case 8: if (!ilacefirst) { y = daglobyoffs+2; yinc = 4; break; }
								  ilacefirst = 0; y = daglobyoffs+4; yinc = 8; break;
						case 4: y = daglobyoffs+1; yinc = 2; break;
						case 2: case 1: return(0);
					}
				if ((unsigned int)y < (unsigned int)dayres)
					{ p = y*dabytesperline+daframeplace; x = daglobxoffs; xend = xspan; }
				else
					{ x = daglobxoffs+0x80000000; xend = xspan+0x80000000; }
			}
		}
		currstr++;
	}
}

//===============================  GIF ends ==================================
//==============================  CEL begins =================================

	//   //old .CEL format:
	//short id = 0x9119, xdim, ydim, xoff, yoff, id = 0x0008;
	//int imagebytes, filler[4];
	//char pal6bit[256][3], image[ydim][xdim];
static int kcelrend (const char *buf, int fleng,
	INT_PTR daframeplace, int dabytesperline, int daxres, int dayres,
	int daglobxoffs, int daglobyoffs)
{
	int i, x, y, x0, x1, y0, y1, xsiz, ysiz;
	const char *cptr;

	kplib_coltype = 3; kplib_bitdepth = 8; kplib_paleng = 256; //For PNGOUT

	xsiz = (int)SSWAPIB(*(unsigned short *)&buf[2]); if (xsiz <= 0) return(-1);
	ysiz = (int)SSWAPIB(*(unsigned short *)&buf[4]); if (ysiz <= 0) return(-1);

	cptr = &buf[32];
	for(i=0;i<256;i++)
	{
		kplib_palcol[i] = (((int)cptr[0])<<18) +
								(((int)cptr[1])<<10) +
								(((int)cptr[2])<< 2) + LSWAPIB(0xff000000);
		cptr += 3;
	}

	x0 = daglobyoffs; x1 = xsiz+daglobyoffs;
	y0 = daglobyoffs; y1 = ysiz+daglobyoffs;
	for(y=y0;y<y1;y++)
		for(x=x0;x<x1;x++)
		{
			if (((unsigned int)x < (unsigned int)daxres) && ((unsigned int)y < (unsigned int)dayres))
				*(int *)(y*dabytesperline+x*4+daframeplace) = kplib_palcol[cptr[0]];
			cptr++;
		}
	return(0);
}

//===============================  CEL ends ==================================
//=============================  TARGA begins ================================

static int ktgarend (const char *header, int fleng,
	INT_PTR daframeplace, int dabytesperline, int daxres, int dayres,
	int daglobxoffs, int daglobyoffs)
{
	int i, x, y, pi, xi, yi, x0, x1, y0, y1, xsiz, ysiz, rlestat, colbyte, pixbyte;
	INT_PTR p;
	const unsigned char *fptr, *cptr, *nptr;

		//Ugly and unreliable identification for .TGA!
	if ((fleng < 19) || (header[1]&0xfe)) return(-1);
	if ((header[2] >= 12) || (!((1<<header[2])&0xe0e))) return(-1);
	if ((header[16]&7) || (header[16] == 0) || (header[16] > 32)) return(-1);
	if (header[17]&0xc0) return(-1);

	fptr = (unsigned char *)&header[header[0]+18];
	xsiz = (int)SSWAPIB(*(unsigned short *)&header[12]); if (xsiz <= 0) return(-1);
	ysiz = (int)SSWAPIB(*(unsigned short *)&header[14]); if (ysiz <= 0) return(-1);
	colbyte = ((((int)header[16])+7)>>3);

	if (header[1] == 1)
	{
		pixbyte = ((((int)header[7])+7)>>3);
		cptr = &fptr[-SSWAPIB(*(unsigned short *)&header[3])*pixbyte];
		fptr += SSWAPIB(*(unsigned short *)&header[5])*pixbyte;
	} else pixbyte = colbyte;

	switch(pixbyte) //For PNGOUT
	{
		case 1: kplib_coltype = 0; kplib_bitdepth = 8; kplib_palcol[0] = LSWAPIB(0xff000000);
				  for(i=1;i<256;i++) kplib_palcol[i] = kplib_palcol[i-1]+LSWAPIB(0x10101); break;
		case 2: case 3: kplib_coltype = 2; break;
		case 4: kplib_coltype = 6; break;
	}

	if (!(header[17]&16)) { x0 = 0;      x1 = xsiz; xi = 1; }
						  else { x0 = xsiz-1; x1 = -1;   xi =-1; }
	if (header[17]&32) { y0 = 0;      y1 = ysiz; yi = 1; pi = dabytesperline; }
					  else { y0 = ysiz-1; y1 = -1;   yi =-1; pi =-dabytesperline; }
	x0 += daglobxoffs; y0 += daglobyoffs;
	x1 += daglobxoffs; y1 += daglobyoffs;
	if (header[2] < 8) rlestat = -2; else rlestat = -1;

	p = y0*dabytesperline+daframeplace;
	for(y=y0;y!=y1;y+=yi,p+=pi)
		for(x=x0;x!=x1;x+=xi)
		{
			if (rlestat < 128)
			{
				if ((rlestat&127) == 127) { rlestat = (int)fptr[0]; fptr++; }
				if (header[1] == 1)
				{
					if (colbyte == 1) i = fptr[0];
									 else i = (int)SSWAPIB(*(unsigned short *)&fptr[0]);
					nptr = &cptr[i*pixbyte];
				} else nptr = fptr;

				switch(pixbyte)
				{
					case 1: i = kplib_palcol[(int)nptr[0]]; break;
					case 2: i = (int)SSWAPIB(*(unsigned short *)&nptr[0]);
						i = LSWAPIB(((i&0x7c00)<<9) + ((i&0x03e0)<<6) + ((i&0x001f)<<3) + 0xff000000);
						break;
					case 3: i = (*(int *)&nptr[0]) | LSWAPIB(0xff000000); break;
					case 4: i = (*(int *)&nptr[0]); break;
				}
				fptr += colbyte;
			}
			if (rlestat >= 0) rlestat--;

			if (((unsigned int)x < (unsigned int)daxres) && ((unsigned int)y < (unsigned int)dayres))
				*(int *)(x*4+p) = i;
		}
	return(0);
}

//==============================  TARGA ends =================================
//==============================  BMP begins =================================
	//TODO: handle BI_RLE8 and BI_RLE4 (compression types 1&2 respectively)
	//                        ÚÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ¿
	//                        ³  0(2): "BM"   ³
	// ÚÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ¿³ 10(4): rastoff³ ÚÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ¿
	// ³headsiz=12 (OS/2 1.x)³³ 14(4): headsiz³ ³ All new formats: ³
	//ÚÁÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÁÁÄÄÄÄÄÄÄÄÄÄÄÄÄÂÄÁÄÁÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÁÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ¿
	//³ 18(2): xsiz                         ³ 18(4): xsiz                                  ³
	//³ 20(2): ysiz                         ³ 22(4): ysiz                                  ³
	//³ 22(2): planes (always 1)            ³ 26(2): planes (always 1)                     ³
	//³ 24(2): cdim (1,4,8,24)              ³ 28(2): cdim (1,4,8,16,24,32)                 ³
	//³ if (cdim < 16)                      ³ 30(4): compression (0,1,2,3!?,4)             ³
	//³    26(rastoff-14-headsiz): pal(bgr) ³ 34(4): (bitmap data size+3)&3                ³
	//³                                     ³ 46(4): N colors (0=2^cdim)                   ³
	//³                                     ³ if (cdim < 16)                               ³
	//³                                     ³    14+headsiz(rastoff-14-headsiz): pal(bgr0) ³
	//ÀÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÂÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÁÄÄÄÄÄÄÄÄÄÂÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÙ
	//                      ³ rastoff(?): bitmap data ³
	//                      ÀÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÙ
static int kbmprend (const char *buf, int fleng,
	INT_PTR daframeplace, int dabytesperline, int daxres, int dayres,
	int daglobxoffs, int daglobyoffs)
{
	int i, j, x, y, x0, x1, y0, y1, rastoff, headsiz, xsiz, ysiz, cdim, comp, cptrinc, *lptr;
	const char *cptr;

	headsiz = *(int *)&buf[14];
	if (headsiz == LSWAPIB(12)) //OS/2 1.x (old format)
	{
		if (*(short *)(&buf[22]) != SSWAPIB(1)) return(-1);
		xsiz = (int)SSWAPIB(*(unsigned short *)&buf[18]);
		ysiz = (int)SSWAPIB(*(unsigned short *)&buf[20]);
		cdim = (int)SSWAPIB(*(unsigned short *)&buf[24]);
		comp = 0;
	}
	else //All newer formats...
	{
		if (*(short *)(&buf[26]) != SSWAPIB(1)) return(-1);
		xsiz = LSWAPIB(*(int *)&buf[18]);
		ysiz = LSWAPIB(*(int *)&buf[22]);
		cdim = (int)SSWAPIB(*(unsigned short *)&buf[28]);
		comp = LSWAPIB(*(int *)&buf[30]);
	}
	if ((xsiz <= 0) || (!ysiz)) return(-1);
		//cdim must be: (1,4,8,16,24,32)
	if (((unsigned int)(cdim-1) >= (unsigned int)32) || (!((1<<cdim)&0x1010113))) return(-1);
	if ((comp != 0) && (comp != 3)) return(-1);

	rastoff = LSWAPIB(*(int *)&buf[10]);

	if (cdim < 16)
	{
		if (cdim == 2) { kplib_palcol[0] = 0xffffffff; kplib_palcol[1] = LSWAPIB(0xff000000); }
		if (headsiz == LSWAPIB(12)) j = 3; else j = 4;
		for(i=0,cptr=&buf[headsiz+14];cptr<&buf[rastoff];i++,cptr+=j)
			kplib_palcol[i] = ((*(int *)&cptr[0])|LSWAPIB(0xff000000));
		kplib_coltype = 3; kplib_bitdepth = (signed char)cdim; kplib_paleng = i; //For PNGOUT
	}
	else if (!(cdim&15))
	{
		kplib_coltype = 2;
		switch(cdim)
		{
			case 16: kplib_palcol[0] = 10; kplib_palcol[1] = 5; kplib_palcol[2] = 0; kplib_palcol[3] = 5; kplib_palcol[4] = 5; kplib_palcol[5] = 5; break;
			case 32: kplib_palcol[0] = 16; kplib_palcol[1] = 8; kplib_palcol[2] = 0; kplib_palcol[3] = 8; kplib_palcol[4] = 8; kplib_palcol[5] = 8; break;
		}
		if (comp == 3) //BI_BITFIELD (RGB masks)
		{
			for(i=0;i<3;i++)
			{
				j = *(int *)&buf[headsiz+(i<<2)+14];
				for(kplib_palcol[i]=0;kplib_palcol[i]<32;kplib_palcol[i]++)
				{
					if (j&1) break;
					j = (((unsigned int)j)>>1);
				}
				for(kplib_palcol[i+3]=0;kplib_palcol[i+3]<32;kplib_palcol[i+3]++)
				{
					if (!(j&1)) break;
					j = (((unsigned int)j)>>1);
				}
			}
		}
		kplib_palcol[0] = 24-(kplib_palcol[0]+kplib_palcol[3]);
		kplib_palcol[1] = 16-(kplib_palcol[1]+kplib_palcol[4]);
		kplib_palcol[2] =  8-(kplib_palcol[2]+kplib_palcol[5]);
		kplib_palcol[3] = ((-1<<(24-kplib_palcol[3]))&0x00ff0000);
		kplib_palcol[4] = ((-1<<(16-kplib_palcol[4]))&0x0000ff00);
		kplib_palcol[5] = ((-1<<( 8-kplib_palcol[5]))&0x000000ff);
	}

	cptrinc = (((xsiz*cdim+31)>>3)&~3); cptr = &buf[rastoff];
	if (ysiz < 0) { ysiz = -ysiz; } else { cptr = &cptr[(ysiz-1)*cptrinc]; cptrinc = -cptrinc; }

	x0 = daglobxoffs; x1 = xsiz+daglobxoffs;
	y0 = daglobyoffs; y1 = ysiz+daglobyoffs;
	if ((x0 >= daxres) || (x1 <= 0) || (y0 >= dayres) || (y1 <= 0)) return(0);
	if (x0 < 0) x0 = 0;
	if (x1 > daxres) x1 = daxres;
	for(y=y0;y<y1;y++,cptr=&cptr[cptrinc])
	{
		if ((unsigned int)y >= (unsigned int)dayres) continue;
		lptr = (int *)(y*dabytesperline-(daglobyoffs<<2)+daframeplace);
		switch(cdim)
		{
			case  1: for(x=x0;x<x1;x++) lptr[x] = kplib_palcol[(int)((cptr[x>>3]>>((x&7)^7))&1)]; break;
			case  4: for(x=x0;x<x1;x++) lptr[x] = kplib_palcol[(int)((cptr[x>>1]>>(((x&1)^1)<<2))&15)]; break;
			case  8: for(x=x0;x<x1;x++) lptr[x] = kplib_palcol[(int)(cptr[x])]; break;
			case 16: for(x=x0;x<x1;x++)
						{
							i = ((int)(*(short *)&cptr[x<<1]));
							lptr[x] = (_lrotl(i,kplib_palcol[0])&kplib_palcol[3]) +
										 (_lrotl(i,kplib_palcol[1])&kplib_palcol[4]) +
										 (_lrotl(i,kplib_palcol[2])&kplib_palcol[5]) + LSWAPIB(0xff000000);
						} break;
			case 24: for(x=x0;x<x1;x++) lptr[x] = ((*(int *)&cptr[x*3])|LSWAPIB(0xff000000)); break;
			case 32: for(x=x0;x<x1;x++)
						{
							i = (*(int *)&cptr[x<<2]);
							lptr[x] = (_lrotl(i,kplib_palcol[0])&kplib_palcol[3]) +
										 (_lrotl(i,kplib_palcol[1])&kplib_palcol[4]) +
										 (_lrotl(i,kplib_palcol[2])&kplib_palcol[5]) + LSWAPIB(0xff000000);
						} break;
		}

	}
	return(0);
}
//===============================  BMP ends ==================================
//==============================  PCX begins =================================
	//Note: currently only supports 8 and 24 bit PCX
static int kpcxrend (const char *buf, int fleng,
	INT_PTR daframeplace, int dabytesperline, int daxres, int dayres,
	int daglobxoffs, int daglobyoffs)
{
	int i, j, x, y, nplanes, x0, x1, y0, y1, bpl, xsiz, ysiz;
	INT_PTR p;
	unsigned char c, *cptr;

	if (*(int *)buf != LSWAPIB(0x0801050a)) return(-1);
	xsiz = SSWAPIB(*(short *)&buf[ 8])-SSWAPIB(*(short *)&buf[4])+1; if (xsiz <= 0) return(-1);
	ysiz = SSWAPIB(*(short *)&buf[10])-SSWAPIB(*(short *)&buf[6])+1; if (ysiz <= 0) return(-1);
		//buf[3]: bpp/plane:{1,2,4,8}
	nplanes = buf[65]; //nplanes*bpl bytes per scanline; always be decoding break at the end of scan line
	bpl = SSWAPIB(*(short *)&buf[66]); //#bytes per scanline. Must be EVEN. May have unused data.
	if (nplanes == 1)
	{
		//if (buf[fleng-769] != 12) return(-1); //Some PCX are buggy!
		cptr = (unsigned char *)&buf[fleng-768];
		for(i=0;i<256;i++)
		{
			kplib_palcol[i] = (((int)cptr[0])<<16) +
									(((int)cptr[1])<< 8) +
									(((int)cptr[2])    ) + LSWAPIB(0xff000000);
			cptr += 3;
		}
		kplib_coltype = 3; kplib_bitdepth = 8; kplib_paleng = 256; //For PNGOUT
	}
	else if (nplanes == 3)
	{
		kplib_coltype = 2;

			//Make sure background is opaque (since 24-bit PCX renderer doesn't do it)
		x0 = max(daglobxoffs,0); x1 = min(xsiz+daglobxoffs,daxres);
		y0 = max(daglobyoffs,0); y1 = min(ysiz+daglobyoffs,dayres);
		p = y0*dabytesperline + daframeplace+3;
		for(y=y0;y<y1;y++,p+=dabytesperline)
			for(x=x0;x<x1;x++) *(char *)((x<<2)+p) = 255;
	}

	x = x0 = daglobxoffs; x1 = xsiz+daglobxoffs;
	y = y0 = daglobyoffs; y1 = ysiz+daglobyoffs;
	cptr = (unsigned char *)&buf[128];
	p = y*dabytesperline+daframeplace;

	if (bpl > xsiz) { daxres = min(daxres,x1); x1 += bpl-xsiz; }

	j = nplanes-1; daxres <<= 2; x0 <<= 2; x1 <<= 2; x <<= 2; x += j;
	if (nplanes == 1) //8-bit PCX
	{
		do
		{
			c = *cptr++; if (c < 192) i = 1; else { i = (c&63); c = *cptr++; }
			j = kplib_palcol[(int)c];
			for(;i;i--)
			{
				if ((unsigned int)y < (unsigned int)dayres)
					if ((unsigned int)x < (unsigned int)daxres) *(int *)(x+p) = j;
				x += 4; if (x >= x1) { x = x0; y++; p += dabytesperline; }
			}
		} while (y < y1);
	}
	else if (nplanes == 3) //24-bit PCX
	{
		do
		{
			c = *cptr++; if (c < 192) i = 1; else { i = (c&63); c = *cptr++; }
			for(;i;i--)
			{
				if ((unsigned int)y < (unsigned int)dayres)
					if ((unsigned int)x < (unsigned int)daxres) *(char *)(x+p) = c;
				x += 4; if (x >= x1) { j--; if (j < 0) { j = 3-1; y++; p += dabytesperline; } x = x0+j; }
			}
		} while (y < y1);
	}

	return(0);
}

//===============================  PCX ends ==================================
//==============================  DDS begins =================================

	//Note:currently supports: DXT1,DXT2,DXT3,DXT4,DXT5,A8R8G8B8
static int kddsrend (const char *buf, int leng,
	INT_PTR frameptr, int bpl, int xdim, int ydim, int xoff, int yoff)
{
	int x, y, z, xx, yy, xsiz, ysiz, dxt, al[2], ai, j, k, v, c0, c1, stride;
	INT_PTR p;
	unsigned int lut[256], r[4], g[4], b[4], a[8], rr, gg, bb;
	unsigned char *uptr, *wptr;

	xsiz = LSWAPIB(*(int *)&buf[16]);
	ysiz = LSWAPIB(*(int *)&buf[12]);
	if ((*(int *)&buf[80])&LSWAPIB(64)) //Uncompressed supports only A8R8G8B8 for now
	{
		if ((*(int *)&buf[88]) != LSWAPIB(32)) return(-1);
		if ((*(int *)&buf[92]) != LSWAPIB(0x00ff0000)) return(-1);
		if ((*(int *)&buf[96]) != LSWAPIB(0x0000ff00)) return(-1);
		if ((*(int *)&buf[100]) != LSWAPIB(0x000000ff)) return(-1);
		if ((*(int *)&buf[104]) != LSWAPIB(0xff000000)) return(-1);
		buf += 128;

		p = yoff*bpl + (xoff<<2) + frameptr; xx = (xsiz<<2);
		if (xoff < 0) { p -= (xoff<<2); buf -= (xoff<<2); xsiz += xoff; }
		xsiz = (min(xsiz,xdim-xoff)<<2); ysiz = min(ysiz,ydim);
		for(y=0;y<ysiz;y++,p+=bpl,buf+=xx)
		{
			if ((unsigned int)(y+yoff) >= (unsigned int)ydim) continue;
			memcpy((void *)p,(void *)buf,xsiz);
		}
		return(0);
	}
	if (!((*(int *)&buf[80])&LSWAPIB(4))) return(-1); //FOURCC invalid
	dxt = buf[87]-'0';
	if ((buf[84] != 'D') || (buf[85] != 'X') || (buf[86] != 'T') || (dxt < 1) || (dxt > 5)) return(-1);
	buf += 128;

	if (!(dxt&1))
	{
		for(z=256-1;z>0;z--) lut[z] = (255<<16)/z;
		lut[0] = (1<<16);
	}
	if (dxt == 1) stride = (xsiz<<1); else stride = (xsiz<<2);

	for(y=0;y<ysiz;y+=4,buf+=stride)
		for(x=0;x<xsiz;x+=4)
		{
			if (dxt == 1) uptr = (unsigned char *)(((INT_PTR)buf)+(x<<1));
						else uptr = (unsigned char *)(((INT_PTR)buf)+(x<<2)+8);
			c0 = SSWAPIB(*(unsigned short *)&uptr[0]);
			r[0] = ((c0>>8)&0xf8); g[0] = ((c0>>3)&0xfc); b[0] = ((c0<<3)&0xfc); a[0] = 255;
			c1 = SSWAPIB(*(unsigned short *)&uptr[2]);
			r[1] = ((c1>>8)&0xf8); g[1] = ((c1>>3)&0xfc); b[1] = ((c1<<3)&0xfc); a[1] = 255;
			if ((c0 > c1) || (dxt != 1))
			{
				r[2] = (((r[0]*2 + r[1] + 1)*(65536/3))>>16);
				g[2] = (((g[0]*2 + g[1] + 1)*(65536/3))>>16);
				b[2] = (((b[0]*2 + b[1] + 1)*(65536/3))>>16); a[2] = 255;
				r[3] = (((r[0] + r[1]*2 + 1)*(65536/3))>>16);
				g[3] = (((g[0] + g[1]*2 + 1)*(65536/3))>>16);
				b[3] = (((b[0] + b[1]*2 + 1)*(65536/3))>>16); a[3] = 255;
			}
			else
			{
				r[2] = (r[0] + r[1])>>1;
				g[2] = (g[0] + g[1])>>1;
				b[2] = (b[0] + b[1])>>1; a[2] = 255;
				r[3] = g[3] = b[3] = a[3] = 0; //Transparent
			}
			v = LSWAPIB(*(int *)&uptr[4]);
			if (dxt >= 4)
			{
				a[0] = uptr[-8]; a[1] = uptr[-7]; k = a[1]-a[0];
				if (k < 0)
				{
					z = a[0]*6 + a[1] + 3;
					for(j=2;j<8;j++) { a[j] = ((z*(65536/7))>>16); z += k; }
				}
				else
				{
					z = a[0]*4 + a[1] + 2;
					for(j=2;j<6;j++) { a[j] = ((z*(65536/5))>>16); z += k; }
					a[6] = 0; a[7] = 255;
				}
				al[0] = LSWAPIB(*(int *)&uptr[-6]);
				al[1] = LSWAPIB(*(int *)&uptr[-3]);
			}
			wptr = (unsigned char *)((y+yoff)*bpl + ((x+xoff)<<2) + frameptr);
			ai = 0;
			for(yy=0;yy<4;yy++,wptr+=bpl)
			{
				if ((unsigned int)(y+yy+yoff) >= (unsigned int)ydim) { ai += 4; continue; }
				for(xx=0;xx<4;xx++,ai++)
				{
					if ((unsigned int)(x+xx+xoff) >= (unsigned int)xdim) continue;

					j = ((v>>(ai<<1))&3);
					switch(dxt)
					{
						case 1: z = a[j]; break;
						case 2: case 3: z = (( uptr[(ai>>1)-8] >> ((xx&1)<<2) )&15)*17; break;
						case 4: case 5: z = a[( al[yy>>1] >> ((ai&7)*3) )&7]; break;
					}
					rr = r[j]; gg = g[j]; bb = b[j];
					if (!(dxt&1))
					{
						bb = min((bb*lut[z])>>16,255);
						gg = min((gg*lut[z])>>16,255);
						rr = min((rr*lut[z])>>16,255);
					}
					wptr[(xx<<2)+0] = (unsigned char)bb;
					wptr[(xx<<2)+1] = (unsigned char)gg;
					wptr[(xx<<2)+2] = (unsigned char)rr;
					wptr[(xx<<2)+3] = (unsigned char)z;
				}
			}
		}
	return(0);
}