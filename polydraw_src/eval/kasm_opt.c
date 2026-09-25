
static long anyreadsbeforewritesrec (long i, rtyp r)
{
	for(;i<gecnt;i++)
	{
		//static char gmybuf[256];
		//getfuncnam(&gasm[i],gmybuf); //For debugging only
		//printf("%d %d %08x (q:%08x) %s\n",firstop,i,r.r,r.q,gmybuf);

		if (!anyreads1stop)
		{
			rtyp *rp;
			long k;
			for(k=gasm[i].n;k>0;k--)
			{
				if (k < 3) rp = &gasm[i].r[k]; else rp = &rxi[gasm[i].rxi+k-3];
				if (gasmeq(*rp,r)) return(1);
			}
		} else anyreads1stop = 0;

			//Don't go past starting instruction (if in a loop)
		if (anyreads1stinst < 0) anyreads1stinst &= 0x7fffffff; else if (i == anyreads1stinst) return(0);

		if ((gasm[i].f == IF0) || (gasm[i].f == IF1) || (gasm[i].f == GOTO))
		{
			long j = lablinum[gasm[i].r[1].r&0x0fffffff];
			if (j >= 0) //Don't jump to same label again - already processed
			{
				lablinum[gasm[i].r[1].r&0x0fffffff] |= 0x80000000;
				if (anyreadsbeforewritesrec(j,r)) return(1);
			}
		}
		if ((gasm[i].f == GOTO) || (gasm[i].f == RETURN)) break;
		if (gasmeq(gasm[i].r[0],r)) return(0);
	}
	return(((r.r&0xf0000000) == KPTR) || ((r.r&0xf0000000) == KIMM) || ((r.r&0xf0000000) == KARR) || ((r.r&0xf0000000) == KGLB)
		|| (((r.r&0xf0000000) == KEDX) && ((r.r&0x0fffffff) >= gccnt*8+gstnum)) );
}
static long anyreadsbeforewrites (long i, rtyp r, long firstop)
{
	long j;
#if 0
		//This block is just an optimization that failed miserably :/
		//See if labels are already set correctly (numlabels < gecnt, so faster)
	long k;
	for(k=numlabels-1;k>=0;k--)
	{
		j = (lablinum[k]&0x7fffffff); if ((unsigned long)j >= gecnt) break;
		if ((gasm[j].f != NUL) || (gasm[j].r[0].r != k)) break;
		lablinum[k] = j;
	}
	if (k >= 0) //Oh well... must check the whole list again
#endif
	for(j=gecnt-1;j>=0;j--)
		if (gasm[j].f == NUL)
			lablinum[gasm[j].r[0].r&0x0fffffff] = j;

	anyreads1stop = firstop;
	anyreads1stinst = (i|0x80000000);
	return(anyreadsbeforewritesrec(i,r));
}

static void put1stfld (long i, rtyp r)
{
	if ((gasm[i-1].f) && (gasmeq(gasm[i-1].r[0],r)))
	{
		long j = (putlen(gasm[i-1].r[0])+1);
#if (COMPILE != 0)
		if ((patchnum > 0) && (patch[patchnum-1].lptr >= (long *)&compcode[kasm87leng-j]) &&
				  (putwrite) && (patch[patchnum-1].lptr <= (long *)&compcode[kasm87leng-4])) patchnum--;
#endif
		kasm87leng -= j;
			//Replace fld...fstp to same location with fst
		if (gasmeq(gasm[i].r[1],gasm[i].r[2]) || (anyreadsbeforewrites(i,r,1)))
			putsib(0xdd,0x11,r); //fst qword ptr [?]
	}
	else
		putsib(0xdd,0x00,r); //fld qword ptr [?]
}

void kasm87freeall ()
{
#if (COMPILE != 0)
	if (patch)     { free(patch);     patch     = 0; } maxpatch = 0;
#endif
	if (jumpback)  { free(jumpback);  jumpback  = 0; } maxjumpbacks = 0;
	if (rxi)       { free(rxi);       rxi       = 0; } maxrxi = 0;
	if (enumnam)   { free(enumnam);   enumnam   = 0; } maxenumchars = 0;
	if (enumval)   { free(enumval);   enumval   = 0; } maxenum = 0;
	if (gasm)      { free(gasm);      gasm      = 0; }
	if (lablinum)  { free(lablinum);  lablinum  = 0; }
	if (jumpat)    { free(jumpat);    jumpat    = 0; }
	if (labpat)    { free(labpat);    labpat    = 0; }
	if (newlabind) { free(newlabind); newlabind = 0; } maxlabs = 0;
	if (newlabnam) { free(newlabnam); newlabnam = 0; } maxlabchars = 0;
	if (newvar)    { free(newvar);    newvar    = 0; } maxvars = 0;
	if (newvarnam) { free(newvarnam); newvarnam = 0; } maxvarchars = 0;
	if (gvl)       { free(gvl);       gvl       = 0; }
	if (gstring)   { free(gstring);   gstring   = 0; } maxst = 0;
	if (ginitval)  { free(ginitval);  ginitval  = 0; } maxinitval = 0;
	if (globval)   { free(globval);   globval   = 0; }
	if (gnext)     { free(gnext);     gnext     = 0; }
	if (gop)       { free(gop);       gop       = 0; } maxops = 0;
	if (funcst)    { free(funcst);    funcst    = 0; } maxfuncst = 0;
	if (texttrans) { free(texttrans); texttrans = 0; } texttransmal = 0;
}

	//mingecnt: hack telling optimizer not to touch gasm[0 .. mingecnt-1]. Set it to 0 for standard behavior.
	//duringparse: 0:safe to rename registers, 1:not safe
static long kasmoptimizations (long mingecnt, long duringparse)
{
	rtyp tr, *rp;
	long i, j, k, l, m, got;

	do //Optimizations!
	{
		//{ char debuf[16384]; kasm87_showdebug(1,debuf,sizeof(debuf)); printf("%s\n",debuf); }
		got = 0;
#if 1
			//Remove duplicate constants
		for(j=gccnt-1;j>=0;j--)
			for(i=j-1;i>=0;i--)
				if (globval[i] == globval[j]) //Rewire: j to i, then rewire: (gccnt-1) to j
				{
					gccnt--; got = 1;
					globval[j] = globval[gccnt];
					for(k=gecnt-1;k>=0;k--)
						for(l=gasm[k].n;l>=0;l--)
						{
							if (l < 3) rp = &gasm[k].r[l]; else rp = &rxi[gasm[k].rxi+l-3];
							if (rp->r ==     j*8+KEDX) rp->r = i*8+KEDX;
							if (rp->r == gccnt*8+KEDX) rp->r = j*8+KEDX;
						}
					break;
				}
#endif
#if 1
			//Remove dead (unused) constants
		for(i=gccnt-1;i>=0;i--)
		{
			for(j=gecnt-1;j>=0;j--)
			{
				for(k=gasm[j].n;k>0;k--)
				{
					if (k < 3) rp = &gasm[j].r[k]; else rp = &rxi[gasm[j].rxi+k-3];
					if (rp->r == i*8+KEDX) break;
				}
				if (k > 0) break;
			}
			if (j < 0) //Constant i not used. Rewire: (gccnt-1) to i
			{
				gccnt--; got = 1;
				globval[i] = globval[gccnt];
				for(j=gecnt-1;j>=0;j--)
					for(k=gasm[j].n;k>0;k--)
					{
						if (k < 3) rp = &gasm[j].r[k]; else rp = &rxi[gasm[j].rxi+k-3];
						if (rp->r == gccnt*8+KEDX) rp->r = i*8+KEDX;
					}
			}
		}
#endif
#if 1
			//Convert NEGMOV by constant to MOV (and negate constant)
		for(i=gecnt-1;i>=0;i--)
			if ((gasm[i].f == NEGMOV) && ((gasm[i].r[1].r&0xf0000000) == KEDX))
			{
				gasm[i].f = MOV;
				checkops(gccnt+1);
				globval[gccnt] = -globval[(gasm[i].r[1].r&0x0fffffff)>>3];
				gasm[i].r[1].r = gccnt*8+KEDX;
				gccnt++;
				got = 1;
			}
#endif
#if 1
			//Convert NEQU0 to MOV if input guaranteed to be 0.0 or 1.0
		for(i=gecnt-1;i>=0;i--)
			if ((gasm[i].f == NEQU0) && ((gasm[i].r[1].r&0xf0000000) == KECX))
				for(j=i-1;j>=0;j--)
					if (gasmeq(gasm[j].r[0],gasm[i].r[1]))
					{
						switch(gasm[j].f)
						{
							case LES: case LESEQ: case MOR: case MOREQ: case EQU: case NEQU: case LAND: case LOR:
								gasm[i].f = MOV; gasm[i].n = 1; got = 1; break;
							default: break;
						}
						if (gasm[i].f == MOV) break;
					}
#endif
#if 1
			//FIXFIXFIX: can't remove this without also taking some other block out ??? (ceilflor2.kc crashes)
			//Remove MOV with same src & dest
		for(i=gecnt-1;i>=mingecnt;i--)
			if ((gasm[i].f == MOV) && (gasmeq(gasm[i].r[0],gasm[i].r[1])))
			{
				gecnt--; got = 1; //Delete MOV instruction and re-wire registers at same time)
				for(j=i;j<gecnt;j++) gasm[j] = gasm[j+1]; //Register is overwritten - simply copy rest now
			}
#endif
#if 1
			//Simplify math expressions, such as POW(x,2), TIMES(x,1), etc...
		for(i=gecnt-1;i>=mingecnt;i--)
		{
			j = ((gasm[i].r[1].r&0x0fffffff)>>3);
			k = ((gasm[i].r[2].r&0x0fffffff)>>3);
			switch(gasm[i].f)
			{
				case POW:
					if ((gasm[i].r[1].r&0xf0000000) == KEDX)
					{
						if (globval[j] == 1.0) { gasm[i].f = MOV; gasm[i].n = 1; got = 1; break; }
					}
					else if ((gasm[i].r[2].r&0xf0000000) == KEDX)
					{
						if (globval[k] == 4.0)
						{
							checkops(gecnt+1); gecnt++; for(j=gecnt-1;j>i;j--) gasm[j] = gasm[j-1];
							gasm[i  ].f = TIMES; gasm[i].r[2] = gasm[i].r[1];
							gasm[i+1].f = TIMES; gasm[i+1].r[1] = gasm[i+1].r[2] = gasm[i+1].r[0];
							got = 1; break;
						}
						else if ((globval[k] == 3.0) && (!gasmeq(gasm[i].r[0],gasm[i].r[1]))) //a = pow(b,3), &a != &b
						{
							checkops(gecnt+1); gecnt++; for(j=gecnt-1;j>i;j--) gasm[j] = gasm[j-1];
							gasm[i  ].f = TIMES; gasm[i].r[2] = gasm[i].r[1];
							gasm[i+1].f = TIMES; gasm[i+1].r[2] = gasm[i+1].r[0];
							got = 1; break;
						}
						else if (globval[k] == 2.0) { gasm[i].f = TIMES; gasm[i].r[2] = gasm[i].r[1]; got = 1; break; }
						else if (globval[k] == 1.0) { gasm[i].f = MOV;   gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
						else if (globval[k] == 0.0) { gasm[i].f = MOV;   gasm[i].r[1].r = gccnt*8+KEDX; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; checkops(gccnt+1); globval[gccnt++] = 1.0; got = 1; break; }
						else if (globval[k] == 0.5) { gasm[i].f = SQRT;  gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
						else if (globval[k] ==-1.0) { gasm[i].f = SLASH; gasm[i].r[2] = gasm[i].r[1]; gasm[i].r[1].r = gccnt*8+KEDX; checkops(gccnt+1); globval[gccnt++] = 1.0; got = 1; break; }
					}
					break;
				case TIMES:
					if ((gasm[i].r[1].r&0xf0000000) == KEDX)
					{
							  if (globval[j] ==-1.0) { gasm[i].f = NEGMOV; gasm[i].r[1] = gasm[i].r[2]; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
						else if (globval[j] == 0.0) { gasm[i].f = MOV; gasm[i].r[1].r = gccnt*8+KEDX; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; checkops(gccnt+1); globval[gccnt++] = 0.0; got = 1; break; }
						else if (globval[j] == 1.0) { gasm[i].f = MOV; gasm[i].r[1] = gasm[i].r[2]; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
						else if (globval[j] == 2.0) { gasm[i].f = PLUS; gasm[i].r[1] = gasm[i].r[2]; got = 1; break; }
					}
					else if ((gasm[i].r[2].r&0xf0000000) == KEDX)
					{
							  if (globval[k] ==-1.0) { gasm[i].f = NEGMOV; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
						else if (globval[k] == 0.0) { gasm[i].f = MOV; gasm[i].r[1].r = gccnt*8+KEDX; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; checkops(gccnt+1); globval[gccnt++] = 0.0; got = 1; break; }
						else if (globval[k] == 1.0) { gasm[i].f = MOV; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
						else if (globval[k] == 2.0) { gasm[i].f = PLUS; gasm[i].r[2] = gasm[i].r[1]; got = 1; break; }
					}
					break;
				case SLASH:
					if ((gasm[i].r[2].r&0xf0000000) == KEDX)
					{
						if (globval[k] ==-1.0) { gasm[i].f = NEGMOV; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
						if (globval[k] == 1.0) { gasm[i].f = MOV; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
						if (globval[k] == 0.5) { gasm[i].f = PLUS; gasm[i].r[2] = gasm[i].r[1]; got = 1; break; }
						//if (!((*(__int64 *)&globval[k])&0x000fffffffffffff)) //check for exact reciprocal (power of 2)
							{ gasm[i].f = TIMES; checkops(gccnt+1); globval[gccnt] = 1.0 / globval[k]; gasm[i].r[2].r = gccnt*8+KEDX; checkops(gccnt+1); gccnt++; got = 1; break; }
					}
					break;
				case PLUS:
					if (((gasm[i].r[2].r&0xf0000000) == KEDX) && (globval[k] == 0.0)) { gasm[i].f = MOV; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
					if (((gasm[i].r[1].r&0xf0000000) == KEDX) && (globval[j] == 0.0)) { gasm[i].f = MOV; gasm[i].r[1] = gasm[i].r[2]; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
					break;
				case MINUS:
					if (((gasm[i].r[2].r&0xf0000000) == KEDX) && (globval[k] == 0.0)) { gasm[i].f = MOV; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
					if (((gasm[i].r[1].r&0xf0000000) == KEDX) && (globval[j] == 0.0)) { gasm[i].f = NEGMOV; gasm[i].r[1] = gasm[i].r[2]; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
					if (gasmeq(gasm[i].r[1],gasm[i].r[2])) { gasm[i].f = MOV; gasm[i].r[1].r = gccnt*8+KEDX; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; checkops(gccnt+1); globval[gccnt++] = 0.0; got = 1; break; }
					break;
				case NEQU:
					if (((gasm[i].r[2].r&0xf0000000) == KEDX) && (globval[k] == 0.0)) { gasm[i].f = NEQU0; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
					if (((gasm[i].r[1].r&0xf0000000) == KEDX) && (globval[j] == 0.0)) { gasm[i].f = NEQU0; gasm[i].r[1] = gasm[i].r[2]; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
					break;
				case LAND:
					if (((gasm[i].r[2].r&0xf0000000) == KEDX) && (globval[k] == 0.0)) { gasm[i].f = MOV; gasm[i].r[1].r = gccnt*8+KEDX; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; checkops(gccnt+1); globval[gccnt++] = 0.0; got = 1; break; }
					if (((gasm[i].r[1].r&0xf0000000) == KEDX) && (globval[j] == 0.0)) { gasm[i].f = MOV; gasm[i].r[1].r = gccnt*8+KEDX; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; checkops(gccnt+1); globval[gccnt++] = 0.0; got = 1; break; }
					if (((gasm[i].r[2].r&0xf0000000) == KEDX) && (globval[k] != 0.0)) { gasm[i].f = NEQU0; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
					if (((gasm[i].r[1].r&0xf0000000) == KEDX) && (globval[j] != 0.0)) { gasm[i].f = NEQU0; gasm[i].r[1] = gasm[i].r[2]; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
					break;
				case LOR:
					if (((gasm[i].r[2].r&0xf0000000) == KEDX) && (globval[k] != 0.0)) { gasm[i].f = MOV; gasm[i].r[1].r = gccnt*8+KEDX; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; checkops(gccnt+1); globval[gccnt++] = 1.0; got = 1; break; }
					if (((gasm[i].r[1].r&0xf0000000) == KEDX) && (globval[j] != 0.0)) { gasm[i].f = MOV; gasm[i].r[1].r = gccnt*8+KEDX; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; checkops(gccnt+1); globval[gccnt++] = 1.0; got = 1; break; }
					if (((gasm[i].r[2].r&0xf0000000) == KEDX) && (globval[k] == 0.0)) { gasm[i].f = NEQU0; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
					if (((gasm[i].r[1].r&0xf0000000) == KEDX) && (globval[j] == 0.0)) { gasm[i].f = NEQU0; gasm[i].r[1] = gasm[i].r[2]; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
					break;
				case ATAN2:
					if (((gasm[i].r[2].r&0xf0000000) == KEDX) && (globval[k] == 1.0)) { gasm[i].f = ATAN; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1; got = 1; break; }
					break;
			}
		}
#endif
#if 1
			//Evaluate expressions based purely on constants
		for(i=gecnt-1;i>=mingecnt;i--)
		{
			if ((gasm[i].f == IF0) || (gasm[i].f == IF1))
			{
				if ((gasm[i].r[2].r&0xf0000000) == KEDX)
				{
					if ((gasm[i].f == IF0) == (globval[(gasm[i].r[2].r&0x0fffffff)>>3] == 0.0))
					{
						got = 1; gasm[i].f = GOTO; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1;
					}
					else
					{
						gecnt--; got = 1; //Delete instruction [i]
						for(j=i;j<gecnt;j++) gasm[j] = gasm[j+1];
						i++;
					}
				}
				continue;
			}
			if ((gasm[i].f == GOTO) || (gasm[i].f == RETURN)) continue;
			if ((gasm[i].f < PARAM1) || (gasm[i].f == MOV) || (gasm[i].f == NEGMOV)) continue;
			if (gasm[i].f < PARAM2) gasm[i].r[2].r = KUNUSED;
			if (((gasm[i].r[2].r&0xf0000000) == KEDX) &&
				 (((gasm[i].r[1].r&0xf0000000) == KIMM) || ((gasm[i].r[1].r&0xf0000000) == KPTR) || ((gasm[i].r[1].r&0xf0000000) == KARR) || ((gasm[i].r[1].r&0xf0000000) == KGLB)))
			{
				double p2;
				if (gasm[i].f == PEEK)
				{
					p2 = *(double *)(((long)globval)+gasm[i].r[2].r-KEDX);
					gasm[i].r[1].q = (long)p2;
					if ((unsigned long)gasm[i].r[1].q >= (unsigned long)newvar[gasm[i].r[1].nv].maxind)
						{ sprintf(kasm87err,"ERROR: array index out of bounds"); return(-1); }
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					gasm[i].f = MOV; got = 1;
					continue;
				}
				if (gasm[i].f == POKE)
				{
					p2 = *(double *)(((long)globval)+gasm[i].r[2].r-KEDX);
					gasm[i].r[0] = gasm[i].r[1];
					gasm[i].r[0].q = (long)p2;
					if ((unsigned long)gasm[i].r[0].q >= (unsigned long)newvar[gasm[i].r[0].nv].maxind)
						{ sprintf(kasm87err,"ERROR: array index out of bounds"); return(-1); }
					gasm[i].r[1] = rxi[gasm[i].rxi]; //FIX: deallocate spot on rxi?
					gasm[i].r[2].r = KUNUSED;
					gasm[i].n = 1;
					gasm[i].f = MOV; got = 1;
					continue;
				}
				if ((gasm[i].f == POKETIMES) || (gasm[i].f == POKESLASH) || (gasm[i].f == POKEPERC) ||
					 (gasm[i].f == POKEPLUS) || (gasm[i].f == POKEMINUS))
				{
					p2 = *(double *)(((long)globval)+gasm[i].r[2].r-KEDX);
					gasm[i].r[1].q = (long)p2;
					if ((unsigned long)gasm[i].r[1].q >= (unsigned long)newvar[gasm[i].r[1].nv].maxind)
						{ sprintf(kasm87err,"ERROR: array index out of bounds"); return(-1); }
					gasm[i].r[0] = gasm[i].r[1];
					gasm[i].r[2] = rxi[gasm[i].rxi]; //FIX: deallocate spot on rxi?
					gasm[i].n = 2;
					switch(gasm[i].f)
					{
						case POKETIMES: gasm[i].f = TIMES; break;
						case POKESLASH: gasm[i].f = SLASH; break;
						case POKEPERC:  gasm[i].f = PERC;  break;
						case POKEPLUS:  gasm[i].f = PLUS;  break;
						case POKEMINUS: gasm[i].f = MINUS; break;
					}
					got = 1;
					continue;
				}
			}
			if (((gasm[i].r[1].r&0xf0000000) == KEDX) && ((gasm[i].r[2].r&0xf0000000) == KEDX))
			{
				double p0, *p1, *p2;
				p1 = (double *)(((long)globval)+gasm[i].r[1].r-KEDX);
				p2 = (double *)(((long)globval)+gasm[i].r[2].r-KEDX);
				j = 0;
				switch(gasm[i].f)
				{
					case NEQU0: p0 = ((*p1) != 0.0); break;
					case FABS:  p0 = fabs(*p1); break;
					case SGN:   p0 = ((*p1) > 0.0) - ((*p1) < 0.0); break;
					case UNIT:  p0 = ((*p1) == 0.0)*.5 + ((*p1) > 0.0); break;
					case FLOOR: p0 = floor(*p1); break;
					case CEIL:  p0 = ceil(*p1); break;
					case ROUND0: case ROUND0_32: if (*p1 >= 0) p0 = floor(*p1); else p0 = -floor(-(*p1)); break;
					case SIN:   p0 = sin(*p1); break;
					case COS:   p0 = cos(*p1); break;
					case TAN:   p0 = tan(*p1); break;
					case ASIN:  p0 = asin(*p1); break;
					case ACOS:  p0 = acos(*p1); break;
					case ATAN:  p0 = atan(*p1); break;
					case SQRT:  p0 = sqrt(*p1); break;
					case EXP:   p0 = exp(*p1); break;
					case FACT:  p0 = fact(*p1); break;
					case LOG:   p0 = log(*p1); break;
					case TIMES: p0 = (*p1)*(*p2); break;
					case SLASH: p0 = (*p1)/(*p2); break;
					case PERC:  p0 = (*p1)-floor((*p1)/fabs(*p2))*fabs(*p2); break;
					case PLUS:  p0 = (*p1)+(*p2); break;
					case MINUS: p0 = (*p1)-(*p2); break;
					case POW:   p0 = pow(*p1,*p2); break;
					case MIN:   if ((*p2) < (*p1)) p0 = (*p2); else p0 = (*p1); break;
					case MAX:   if ((*p2) > (*p1)) p0 = (*p2); else p0 = (*p1); break;
					case FADD:  j = -1; break; //NOTE! The entire purpose of FADD is to NOT optimize it here! Keep this as a placeholder.
					case FMOD:  p0 = fmod(*p1,*p2); break;
					case ATAN2: p0 = atan2(*p1,*p2); break;
					case LOGB:  p0 = log(*p1)/log(*p2); break;
					case LES:   p0 = ((*p1) <  (*p2)); break;
					case LESEQ: p0 = ((*p1) <= (*p2)); break;
					case MOR:   p0 = ((*p1) >  (*p2)); break;
					case MOREQ: p0 = ((*p1) >= (*p2)); break;
					case EQU:   p0 = ((*p1) == (*p2)); break;
					case NEQU:  p0 = ((*p1) != (*p2)); break;
					case LAND:  p0 = ((*p1) && (*p2)); break;
					case LOR:   p0 = ((*p1) || (*p2)); break;
					default: j = -1; break;
				}
				if (!j)
				{
					gasm[i].r[1].r = gccnt*8+KEDX; gasm[i].r[2].r = KUNUSED; gasm[i].n = 1;
					gasm[i].f = MOV; checkops(gccnt+1); globval[gccnt++] = p0; got = 1;
				}
			}
		}
#endif
#if 0
			//This block is no longer necessary with register renaming, but it speeds things up.
			//I disable it for safety reasons (because code isn't checked as carefully as register renaming)
			//
			//Remove unnecessary MOV instructions, replacing src's in later code with dest of this mov
			// r1 = X;
			// r3 = r1 + r2;   ->  r3 = X + r2;
			// r4 = sqrt(r1);      r4 = sqrt(X);
		for(i=gecnt-2;i>=mingecnt;i--)
			if ((gasm[i].f == MOV) && ((gasm[i].r[0].r&0xf0000000) == KECX))
			{
				k = gasm[i].r[0].r; tr = gasm[i].r[1];

					//When writing a variable in a loop that reads the variable ABOVE the write, it can't be
					//optimized. To be safe, make sure dest of mov (gasm[i].r[0]) isn't a 'named' variable!
				for(j=gnumarg;j<newvarnum;j++)
					if (newvar[j].r == k) break;
				if (j < newvarnum) continue; //finish abort

				m = 0;
				for(j=i+1;j<gecnt;j++) //temp register is used later... abort optimization
				{
						//writes register in other code that may or may not be executed... must abort
					if ((m) && ((gasm[j].r[0].r == k) || (gasm[j].r[1].r == k) || (gasm[j].r[2].r == k))) break;
					if ((gasm[j].f == NUL) || (gasm[j].f == IF0) || (gasm[j].f == IF1) || (gasm[j].f == GOTO) || (gasm[j].f == RETURN)) m = 1;
				}
				if (j < gecnt) continue; //finish abort

					//Make sure source of mov (tr) doesn't get written before last access of (k)
				for(j=i+1;j<gecnt;j++)
					if (gasmeq(gasm[j].r[0],tr))
					{
						for(j++;j<gecnt;j++)
							if ((gasm[j].r[0].r == k) || (gasm[j].r[1].r == k) || (gasm[j].r[2].r == k)) break;
						break;
					}
				if (j < gecnt) continue; //finish abort

				gecnt--; got = 1; //Delete MOV instruction and re-wire registers at same time)
				for(j=i;j<gecnt;j++)
				{
					gasm[j] = gasm[j+1];
					if (gasm[j].r[1].r == k) gasm[j].r[1] = tr;
					if (gasm[j].r[2].r == k) gasm[j].r[2] = tr;
					if (gasm[j].r[0].r == k) { j++; break; }
				}
				for(;j<gecnt;j++) gasm[j] = gasm[j+1]; //Register is overwritten - simply copy rest now
			}
#endif
#if 1
			//Register renaming: (when result of calculation is used only once)
			//
			//         Change:       To:
			// inst i: r2 = ? +r3 -> r1 = ? +r3   (Later optimizations may remove r2)
			// inst j: r1 = r2+r4    r1 = r1+r4
			//
			//Special case for MOV instructions:
			//         Change:       To:
			// inst i: r2 = ?     -> r2 = ?       (Later optimizations may remove r2)
			// inst j: r1 = r2+r4    r1 = ? +r4
			//
			///            ÚÄÄÄÂÄÄÄÂÄÄÄÂÄÄÄ¿
			///   Rules:   ³ i ³...³ j ³...³ ³
			///ÚÄÄÄÄÄÄÄÄÄÄÄÅÄÄÄÅÄÄÄÅÄÄÄÅÄÄÄ´ ³ 1. i < (r1 access) < j not allowed
			///³ r0 read   ³ û ³ X ³ X ³ û ³ ³ 2. (first r1 access) > j must be write
			///³ r0 write  ³ û ³ X ³ û ³ û ³ ³ 3. (r0 read) at j not allowed
			///³ r1 read   ³ û ³ X ³(X)³>W ³ ³ 4. i < (r0 access) < j not allowed
			///³ r1 write  ³ û ³ X ³ û ³<R ³ ³ 5. (r1 read) at i not allowed if inside possible loop
			///ÀÄÄÄÄÄÄÄÄÄÄÄÁÄÄÄÁÄÄÄÁÄÄÄÁÄÄÄÙ ³
			//06/23/2004: Above rules rewritten to use anyreadsbeforewrite&now much cleaner!
		for(i=gecnt-1;i>=mingecnt;i--)
		{
			if ((gasm[i].r[0].r&0xf0000000) != KECX) continue;
			for(j=i+1;j<gecnt;j++)
			{
				if ((gasm[j].f == NUL) || (gasm[j].f == GOTO)) break;

				for(k=gasm[j].n;k>0;k--)
				{
					if (k < 3) rp = &gasm[j].r[k]; else rp = &rxi[gasm[j].rxi+k-3];
					if (gasmeq(gasm[i].r[0],*rp)) break;
				}

				if (k > 0)
				{
						//Can't rename function parameters that are pointers:
					if ((gasm[j].f == USERFUNC) && (newvarnam[newvar[gasm[j].g].proti+k-1] <= 'Z')) break;

						//Another case:       .f  .r[0]   .r[1]   .r[2]
						//m2 = 0              MOV m2      0       KUNUSED
						//IF !(m2) GOTO l1    IF0 KUNUSED m2      l1
					if (gasm[i].f == MOV)
					{
						for(k=i+1;k<j;k++) //Make sure no instructions in between write the register
						{
							if (gasmeq(gasm[i].r[1],gasm[k].r[0])) break;
							if (gasm[k].f == USERFUNC) //pointer params can write register..
							{
								for(l=gasm[k].n;l>0;l--)
								{
									m = newvarnam[newvar[gasm[k].g].proti+l-1]; if (m > 'Z') continue; //not a pointer param
									if (l < 3) rp = &gasm[k].r[l]; else rp = &rxi[gasm[k].rxi+l-3];
									if ((gasm[i].r[1].r == rp->r) && (gasm[i].r[1].q == rp->q)) break; //pointer matches var
								}
								if (l > 0) break;
							}
						}
						if (k < j) break;

							//All is good!
						for(k=gasm[j].n;k>0;k--)
						{
							if (k < 3) rp = &gasm[j].r[k]; else rp = &rxi[gasm[j].rxi+k-3];
							if (gasmeq(gasm[i].r[0],*rp)) *rp = gasm[i].r[1];
						}

						got = 1;
						break;
					}
					else
					{
							//Don't allow "nop" to set got = 1; (would result in endless loop)
						if (gasmeq(gasm[i].r[0],gasm[j].r[0])) break;
						if (gasm[j].r[0].r == KUNUSED) break;

							//Don't allow this to happen:
							//m8 = m8 * 3      m4 = m8 * 3
							//m4 = m5 + 1  ->  m4 = m5 + 1
							//m4 = m4 - m8     m4 = m4 - m4
						for(k=i+1;k<j;k++)
							if (gasmeq(gasm[j].r[0],gasm[k].r[0])) break;
						if (k < j) break;

							//beg:
							//i: r3 = r2 * r2
							//j: r2 = r3
							//   goto beg
						if (anyreadsbeforewrites(j,gasm[i].r[0],1)) break;
						if (anyreadsbeforewrites(i,gasm[j].r[0],1)) break;

							//All is good!
						for(k=gasm[j].n;k>0;k--)
						{
							if (k < 3) rp = &gasm[j].r[k]; else rp = &rxi[gasm[j].rxi+k-3];
							if (gasmeq(gasm[i].r[0],*rp)) *rp = gasm[j].r[0];
						}
						gasm[i].r[0] = gasm[j].r[0];
						got = 1;

						break;
					}
				}
				if (gasmeq(gasm[i].r[0],gasm[j].r[0])) break;
				if (gasm[j].r[0].r == KUNUSED) break;
			}
		}
#endif
#if 1
			//Compact unused registers
		if (!duringparse)
		{
			k = KECX;
			for(i=gecnt-1;i>=mingecnt;i--)
			{
				if ((gasm[i].r[0].r < k) || ((gasm[i].r[0].r&0xf0000000) != KECX)) continue;
				for(j=i+1;j<gecnt;j++) if (gasmeq(gasm[i].r[0],gasm[j].r[0])) break;
				if (j >= gecnt)
				{
					if (gasm[i].r[0].r != k)
					{     //swap registers: k,gasm[i].r[0]
						got = 1; m = gasm[i].r[0].r;
						for(j=gecnt-1;j>=0;j--)
							for(l=gasm[j].n;l>=0;l--)
							{
								if (l < 3) rp = &gasm[j].r[l]; else rp = &rxi[gasm[j].rxi+l-3];
								if (rp->r == k) rp->r = m; else if (rp->r == m) rp->r = k;
							}

							//Make sure variable names match their associated registers (for flow control)
						for(j=gnumarg;j<newvarnum;j++)
							{ if (newvar[j].r == k) newvar[j].r = m; else if (newvar[j].r == m) newvar[j].r = k; }
					}
					k += 8;
				}
			}
		}
#endif
#if 1
			//Remove dead code, for example: { r0 = cos(r1); r0 = i1; } -> { r0 = i1; }
		for(i=gecnt-2;i>=mingecnt;i--)
		{     //Can't remove labels or jumps
			if ((gasm[i].f == NUL) || (gasm[i].f == USERFUNC) || (gasm[i].r[0].r == KUNUSED)) continue;
			if (!anyreadsbeforewrites(i+1,gasm[i].r[0],0))
			{
				gecnt--; got = 1; //Delete instruction [i]
				for(j=i;j<gecnt;j++) gasm[j] = gasm[j+1];
			}
		}
#endif
#if 1
			//Change IF0 to IF1 if:
			//   1.Next line is GOTO, and...
			//   2.Dest of IF's GOTO is exactly 2 lines ahead (this is important!)
			//"if !(m0) goto l1; goto l2;l1:"  ->  "if (m0) goto l2"
		for(i=gecnt-3;i>=mingecnt;i--)
			if ((gasm[i].f == IF0) &&
				 (gasm[i+1].f == GOTO) &&
				 ((gasm[i+2].f == NUL) && (gasmeq(gasm[i].r[1],gasm[i+2].r[0]))))
			{
				gasm[i].f = IF1; gasm[i].r[1] = gasm[i+1].r[1];
				gecnt--; got = 1; //Delete instruction i+1
				for(j=i+1;j<gecnt;j++) gasm[j] = gasm[j+1];
			}
#endif
#if 1
			//Remove GOTO/IF0/IF1 if its label points to next line
		for(i=gecnt-2;i>=mingecnt;i--)
			if (((gasm[i].f == GOTO) || (gasm[i].f == IF0) || (gasm[i].f == IF1)) && (gasm[i+1].f == NUL) && (gasmeq(gasm[i].r[1],gasm[i+1].r[0])))
			{
				gecnt--; got = 1; //Delete instruction i
				for(j=i;j<gecnt;j++) gasm[j] = gasm[j+1];
			}
#endif
#if 1
			//Remove anything after a GOTO or RETURN that isn't a label
		for(i=gecnt-2;i>mingecnt;i--)
			if (((gasm[i-1].f == GOTO) || (gasm[i-1].f == RETURN)) && (gasm[i].f != NUL))
			{
				for(k=i+1;k<gecnt;k++) if (gasm[k].f == NUL) break;
				k -= i;

				gecnt -= k; got = 1; //Delete instructions {i .. i+k-1}
				for(j=i;j<gecnt;j++) gasm[j] = gasm[j+k];
			}
#endif
#if 1
			//Remove unused labels (except for gasm[0] which is used as dummy filler later)
		for(i=gecnt-1;i>mingecnt;i--)
			if (gasm[i].f == NUL)
			{
				for(j=gecnt-1;j>=0;j--)
					if (((gasm[j].f == IF0) || (gasm[j].f == IF1) || (gasm[j].f == GOTO)) &&
						  (gasmeq(gasm[j].r[1],gasm[i].r[0]))) break;
				if (j < 0)
				{
					k = gasm[i].r[0].r;

					gecnt--; got = 1; //Delete instruction i (label)
					for(j=i;j<gecnt;j++) gasm[j] = gasm[j+1];

						//Re-wire labels (can't have holes!)
					numlabels--;
					for(j=gecnt-1;j>=0;j--)
					{
						if (gasm[j].f == NUL) m = 0; else m = 1;
						if (gasm[j].r[m].r == (signed)KEIP+numlabels) gasm[j].r[m].r = k;
					}
				}
			}
#endif
	} while (got);
	return(0);
}

#if (COMPILE == 0)

	//kasm87c: similar functionality to kasm87, but pure C code - making it slower and more portable

	//ANSI va_arg: supported on all compilers
#include <stdarg.h>
