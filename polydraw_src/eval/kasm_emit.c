

//--------------------------------------------------
static unsigned char *compcode = 0;

static char fpustat;
static long putwrite = 0;
static void put1byte (long a) { if (putwrite) compcode[kasm87leng] = (char)a; kasm87leng++; }
static void put2byte (long a) { if (putwrite) *(short *)&compcode[kasm87leng] = (short)a; kasm87leng += 2; }
static void put4byte (long a) { if (putwrite) *(long *)&compcode[kasm87leng] = a; kasm87leng += 4; }

static void putsib (long opcode, long a, rtyp b)
{
	long hbr, lbr;

	if ((b.r&0xf0000000) == KPTR)
	{
		b.r &= 0x0fffffff;
		if ((b.r >= -128) && (b.r < 128)) put4byte((b.r<<24)+0x244c8b); //mov ecx, [esp+imm8]
		else { put2byte(0x8c8b); put1byte(0x24); put4byte(b.r); }   //mov ecx, [esp+imm32]
		b.r = KECX+b.q*8;
	}
	else if ((b.r&0xf0000000) == KEDX) b.r += b.q*8;

	if (!(opcode&0xffffff00)) put1byte(opcode);
								else put2byte(opcode);

	hbr = (((unsigned long)b.r)>>28); lbr = (b.r&0x0fffffff);
	if (hbr == (KFST>>28))
	{
			//d9 c0   fld   st(0)
			//d9 c8   fxch  st(0)
			//dd c0   ffree st(0)
			//dd d0   fst   st(0)
			//dd d8   fstp  st(0)
			// ...
		if (putwrite)
		{
			if ((compcode[kasm87leng-1] != 0xdd) || ((a&0x30) != 0x10)) //Don't do hack for FST or FSTP...
				compcode[kasm87leng-1] -= 0x04; //Nasty hack!!!
		}
		fpustat |= (1<<(lbr>>3));
		put1byte(a+0xc0+(lbr>>3)); return;
	}
	a &= 0x38; //Throw away stack pointer position when using memory access!
	if ((hbr == (KIMM>>28)) || (hbr == (KGLB>>28)))
	{
		put1byte(a+0x05);
#if (COMPILE == 0)
		if (hbr == (KIMM>>28)) put4byte((long)(gevalext[lbr].ptr)+b.q*8); //access static variable (imm32)
								else put4byte(gstatmem + lbr+b.q*8); //access static variable (imm32)
#else
		if (putwrite)
		{
			checkpatch(patchnum+1);
			patch[patchnum].lptr = (long *)&compcode[kasm87leng];
			patch[patchnum].ind = b.r;
			patchnum++;
		}
		put4byte(b.q*8); //access static variable (imm32)
#endif
		return;
	}
	if ((lbr < -128) || (lbr >= 128)) { put1byte(a+0x80+hbr); if (hbr == 4) put1byte(0x24); put4byte(lbr); }
	else if ((lbr) && (hbr != 5))     { put1byte(a+0x40+hbr); if (hbr == 4) put1byte(0x24); put1byte(lbr); }
	else                              { put1byte(a     +hbr); if (hbr == 4) put1byte(0x24);                }
}

static long putlen (rtyp b)
{
	long r, lng;

	lng = 0;
	if ((b.r&0xf0000000) == KPTR)
	{
		b.r &= 0x0fffffff;
		if ((b.r >= -128) && (b.r < 128)) lng = 4; else lng = 7;
		b.r = KECX+b.q*8;
	}
	else if ((b.r&0xf0000000) == KEDX) b.r += b.q*8;

	r = (((unsigned long)b.r)>>28); b.r &= 0x0fffffff;
	if (r == (KFST>>28)) return(lng+1);
	if ((r == (KIMM>>28)) || (r == (KGLB>>28))) return(lng+5);
	if ((b.r < -128) || (b.r >= 128)) return(lng+(r==4)+5);
	else if ((b.r) && (r != 5))       return(lng+(r==4)+2);
	else                              return(lng+(r==4)+1);
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

static void *kasm87comp (char *bakz)
{
	rtyp tr, *rp;
	long i, j, k, l, pcnt, bcnt, maxpcnt, got, jumpatnum, whitespc, regnum, subparms, isaddr, espoff, inquotes;
	long onewvarplc, ibody, ljumpbacknum;
	char *tbuf;

	globi = 0; gop[globi] = NUL; arrnum = 0;

	gecnt = 0; gccnt = 0; gstnum = 0; ginitvalnum = 0; kasm87leng = 0;

		//Insert dummy label at beginning
	checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
	gasm[gecnt].f = NUL; gasm[gecnt].r[0].r = KEIP; gecnt = 1;
	numlabels = 1;

	tbuf = (char *)malloc(strlen(bakz)+1);
	if (!tbuf) { strcpy(kasm87err,"ERROR: malloc failed"); return(0); }

		//Pre-processor
	ibody = 0; l = 0; got = 0; pcnt = 0; bcnt = 0; maxpcnt = 0; gnumarg = -1; whitespc = 0; onewvarplc = newvarplc;
	newlabplc = 0; newlabnum = 0;
	inquotes = 0;
	for(i=0;bakz[i];i++)
	{
		if ((!inquotes) && (bakz[i] == 32) /*|| (bakz[i] == 9)*/) { whitespc = 1; continue; } //strip Space/Tab
		if (got) continue;

		tbuf[l] = bakz[i];

		if ((tbuf[l] == '\"') && ((!l) || (tbuf[l-1] != '\\'))) inquotes ^= 1;
		if (inquotes) { l++; continue; }

		if (tbuf[l] == '(')
		{
			pcnt++; if (pcnt > maxpcnt) maxpcnt = pcnt;
			if ((gnumarg < 0) && (pcnt == 1)) { ibody = l; continue; } //Note: '(' not stored in tbuf because no l++;
		}
		if (tbuf[l] == ')')
		{
			pcnt--; if (pcnt < 0) break;
			if ((!pcnt) && (gnumarg < 0))
			{
				tbuf[l] = ','; subparms = -2; isaddr = 0; espoff = 0;
					//Save new variable/function names,indices,parameters to list
				for(k=ibody;k<=l;k++) //verify parnam  //Example param string: "a,b(,&),&c,d(),"
				{                                      //                       ibody         l
						//Save new variable name&index to list
					if (tbuf[k] == '(')
					{
						pcnt++; subparms = 0;
						if (pcnt == 1)
						{
							checkvarchars(newvarplc+1); newvarnam[newvarplc++] = 0;
							newvar[newvarnum].proti = newvarplc;
						}
						continue;
					}
					if (tbuf[k] == ')')
					{
						pcnt--;
						if (pcnt == 0)
						{
							checkvarchars(newvarplc+1);
							switch(isaddr)
							{
								case 0: newvarnam[newvarplc] = 'd'; break; //double
								case 1: newvarnam[newvarplc] = 'D'; break; //double*
								case 2: newvarnam[newvarplc] = 'C'; break; //char* (const string only)
							}
							newvarplc++; subparms++; isaddr = 0; continue;
						}
						continue;
					}
					if (tbuf[k] == '[')
					{
						if (isaddr) { sprintf(kasm87err,"ERROR: bad syntax, param %d",newvarnum); free(tbuf); return(0); }

						checkvarchars(newvarplc+1); newvarnam[newvarplc++] = 0;
						checkvars(newvarnum+1);
						newvar[newvarnum].proti = newvarplc;
						newvar[newvarnum].parnum = 0;

						bcnt = parse_dimensions(tbuf,&k,1);
						if (!bcnt) { free(tbuf); return(0); }
						isaddr = 1;
						k--; //without this, continue would skip past comma
						continue;
					}
					if (tbuf[k] == '&')
					{
						if (isaddr) { sprintf(kasm87err,"ERROR: bad syntax, param %d",newvarnum); free(tbuf); return(0); }
						isaddr = 1; continue;
					}
					if (tbuf[k] == '$')
					{
						if (isaddr) { sprintf(kasm87err,"ERROR: bad syntax, param %d",newvarnum); free(tbuf); return(0); }
						isaddr = 2; continue;
					}
					if (tbuf[k] == ',')
					{
						//if (tbuf[k] != ',') { sprintf(kasm87err,"ERROR: bad syntax, param %d",newvarnum); free(tbuf); return(0); }
						if (pcnt == 1)
						{
							checkvarchars(newvarplc+1);
							switch(isaddr)
							{
								case 0: newvarnam[newvarplc] = 'd'; break; //double
								case 1: newvarnam[newvarplc] = 'D'; break; //double*
								case 2: newvarnam[newvarplc] = 'C'; break; //char* (const string only)
							}
							newvarplc++; subparms++; isaddr = 0; continue;
						}

						if ((k == ibody) && (k == l)) break; //special case with no arguments
						if (((tbuf[k+1] == ',') && (k < l)) || (k == ibody) || (k == l-1))
							{ sprintf(kasm87err,"ERROR: bad syntax, param %d",newvarnum); free(tbuf); return(0); }

						//if (!isaddr) isaddr = -1;
						if (subparms < 0)
						{
							if (!bcnt)
							{
								checkvarchars(newvarplc+1); newvarnam[newvarplc++] = 0;
								checkvars(newvarnum+1); newvar[newvarnum].proti = -1;
							}
						}
						checkvars(newvarnum+1);
						newvar[newvarnum].nami = onewvarplc; onewvarplc = newvarplc;
						newvar[newvarnum].r = espoff+KESP;
						newvar[newvarnum].maxind = bcnt;
							//Subparms: < 0 for double/array, >= 0 for user function pointers (# is # parms)
						if (subparms < 0)
						{
							if (!isaddr) { espoff += 8; }
							else { newvar[newvarnum].r = espoff+KPTR; espoff += 4; }
						}
						else
						{
							if (isaddr) { sprintf(kasm87err,"ERROR: bad syntax, param %d",newvarnum); free(tbuf); return(0); }
							espoff += 4;
						}
						if ((subparms < 0) && (bcnt)) newvar[newvarnum].parnum ^= -1; else //1's complement
						newvar[newvarnum].parnum = subparms;
						j = getnewvarhash(&newvarnam[newvar[newvarnum].nami]);
						newvar[newvarnum].hashn = newvarhash[j]; newvarhash[j] = newvarnum;

						subparms = -2;
						newvarnum++;
						isaddr = 0; bcnt = 0;
						continue;
					}
					if (isvarchar(tbuf[k]))
						{ if (!bcnt) { checkvarchars(newvarplc+1); newvarnam[newvarplc++] = tbuf[k]; } continue; }
					sprintf(kasm87err,"ERROR: bad syntax, param %d",newvarnum); free(tbuf); return(0);
				}
				gnumarg = newvarnum;
				tbuf[l] = 0; ibody = l+1;
			}
		}
		if (whitespc)
		{
			whitespc = 0;
			if ((l > ibody) && (isvarchar(tbuf[l-1])) && (isvarchar(tbuf[l])))
				{ tbuf[l+1] = tbuf[l]; tbuf[l] = ' '; l++; }
		}
		l++;
	}
	tbuf[l++] = 0;
	//if (ibody+1 >= l) { strcpy(kasm87err,"ERROR: no function defined"); free(tbuf); return(0); }
	if (pcnt < 0) { strcpy(kasm87err,"ERROR: too many )"); free(tbuf); return(0); }
	if (pcnt > 0) { strcpy(kasm87err,"ERROR: not enough )"); free(tbuf); return(0); }
	if (maxpcnt > 16) { strcpy(kasm87err,"ERROR: () nested > 16"); free(tbuf); return(0); }

		//Add global library variables&functions to parameter list
	for(i=0;i<gevalextnum;i++)
	{
		bcnt = 0; subparms = -2; k = 0;

		//printf("|%s|:",gevalext[i].nam); //useful for debugging

		checkvars(newvarnum+1);
		newvar[newvarnum].nami = newvarplc;

		if (gevalext[i].nam[k] == '&') { k++; bcnt = 1; newvar[newvarnum].proti = -1; }
		if (gevalext[i].nam[k] == '$') k++; //ignore/invalid

		if (gevalext[i].nam[k] == '\"') //Handle compiled strings for pic(),snd(),getpicsiz(),fil(),etc..
		{
			do
			{
				checkvarchars(newvarplc+1); newvarnam[newvarplc++] = gevalext[i].nam[k++];
			} while ((gevalext[i].nam[k]) && (gevalext[i].nam[k] != '\"'));
		}
		while ((gevalext[i].nam[k]) && (gevalext[i].nam[k] != '(') && (gevalext[i].nam[k] != '[')) //FIX && (gevalext[i].nam[k] != ' '))
			{ checkvarchars(newvarplc+1); newvarnam[newvarplc++] = gevalext[i].nam[k++]; }
		if ((newvarplc > newvar[newvarnum].nami) && (newvarnam[newvarplc-1] == ' ')) newvarplc--;
		checkvarchars(newvarplc+1); newvarnam[newvarplc++] = 0;

		if (gevalext[i].nam[k] == ' ') k++; //Remove whitespace after function name (doesn't work)
		if (gevalext[i].nam[k] == '(')
		{
			newvar[newvarnum].proti = newvarplc; subparms = 0;
			do
			{
				checkvarchars(newvarplc+1); subparms++; k++;
				while ((isvarchar(gevalext[i].nam[k])) || (gevalext[i].nam[k] == ' ')) k++; //Don't care about variable name here; skip it
				newvarnam[newvarplc] = 'd';
				if (gevalext[i].nam[k] == '&') { newvarnam[newvarplc] = 'D'; k++; }
				if (gevalext[i].nam[k] == '$') { newvarnam[newvarplc] = 'C'; k++; }
				if (gevalext[i].nam[k] == '.') { newvarnam[newvarplc] = 'e'; k++; }
				newvarplc++;
				while ((isvarchar(gevalext[i].nam[k])) || (gevalext[i].nam[k] == ' ')) k++; //Don't care about variable name here; skip it
				if (gevalext[i].nam[k] == '[')
				{
					newvarnam[newvarplc-1] = 'D';
					if (!parse_dimensions(gevalext[i].nam,&k,0))
						{ sprintf(kasm87err,"ERROR: %s has bad dimensions",&newvarnam[newvar[newvarnum].nami]); free(tbuf); return(0); }
				}
			} while (gevalext[i].nam[k] == ',');

			//printf("|"); for(j=newvar[newvarnum].proti;j<newvarplc;j++) printf("%c",newvarnam[j]); printf("|"); //useful for debugging
		}
		else if (gevalext[i].nam[k] == '[')
		{
			newvar[newvarnum].proti = newvarplc;
			newvar[newvarnum].parnum = 0;
			bcnt = parse_dimensions(gevalext[i].nam,&k,1);
			if (!bcnt) { sprintf(kasm87err,"ERROR: %s has bad dimensions",&newvarnam[newvar[newvarnum].nami]); free(tbuf); return(0); }
			subparms = ~newvar[newvarnum].parnum; //1's complement
		}

		//printf("|%s|:maxind:%d,parnum:%d\n",&newvarnam[newvar[newvarnum].nami],bcnt,subparms); //useful for debugging
		newvar[newvarnum].r = i+KIMM;
		newvar[newvarnum].maxind = bcnt;
		newvar[newvarnum].parnum = subparms; //<0 for double/arrays:~#dimens, >=0 for function where #parms
		j = getnewvarhash(&newvarnam[newvar[newvarnum].nami]);
		newvar[newvarnum].hashn = newvarhash[j]; newvarhash[j] = newvarnum;
		newvarnum++;
	}
	gnumglob = newvarnum;

	parsefunc(&tbuf[ibody],-1,-1); free(tbuf);
	if (globi == -1) { gecnt = 0; return(0); }

		//Make sure all label destinations exist
	for(i=0,tbuf=newlabnam;i<newlabnum;i++,tbuf=&tbuf[strlen(newlabnam)+1])
		if (newlabind[i] < 0)
			{ sprintf(kasm87err,"ERROR: label %s not found",tbuf); gecnt = 0; return(0); }

		//If last instruction is label, add return(0); at end
	if (!gasm[gecnt-1].f)
	{
		checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
		gasm[gecnt].r[0].r = KUNUSED;
		gasm[gecnt].r[1].r = gccnt*8+KEDX;
		gasm[gecnt].r[2].r = KUNUSED;
		gasm[gecnt].n = 1;
		gasm[gecnt].f = RETURN; gecnt++;
		checkops(gccnt+1); globval[gccnt++] = 0.0;
	}

		//Align section after strings to 8-byte boundary
	checkstrings((gstnum+7)&~7);
	while (gstnum&7) gstring[gstnum++] = 0;

	if (kasm87optimize)
	{
		if (kasmoptimizations(0,0) < 0) { gecnt = 0; return(0); }
	}

		//Get number of 8-byte memory locations needed for temp storage
	regnum = 0;
	for(i=gecnt-1;i>=0;i--)
		for(j=gasm[i].n;j>=0;j--)
		{
			if (j < 3) rp = &gasm[i].r[j]; else rp = &rxi[gasm[i].rxi+j-3];
			if (((rp->r&0xf0000000) == KECX) && ((rp->r&0x0fffffff) > regnum)) regnum = (rp->r&0x0fffffff);
			if ((rp->r&0xf0000000) == KSTR) rp->r = (rp->r&0x0fffffff)+gccnt*8+KEDX;
			if ((rp->r&0xf0000000) == KARR) rp->r = (rp->r&0x0fffffff)+gccnt*8+gstnum+KEDX;
		}
	regnum = (regnum>>3)+1;

#if (COMPILE == 0)
		//Necessary hack to make jumps in kasm87c faster (Uses gasm[*].r[0] which isn't used for jumps anyway)
	for(i=0;i<gecnt;i++)
		if ((gasm[i].f == IF0) || (gasm[i].f == IF1) || (gasm[i].f == GOTO))
			for(j=0;j<gecnt;j++)
				if ((gasm[j].f == NUL) && (gasmeq(gasm[j].r[0],gasm[i].r[1])))
					{ gasm[i].r[0].r = j; break; }

		//Hack: append strings&static array variables&data to globval[]
	if (gccnt*8+gstnum+arrnum > sizeof(double)*maxops)
		if (!(globval = (double *)realloc(globval,gccnt*8+gstnum+arrnum)))
			{ strcpy(kasm87err,"ERROR: malloc failed"); gecnt = 0; return(0); }
	memcpy(&globval[gccnt],gstring,gstnum); //Copy string tables to globval
	//memset(&globval[gccnt+(gstnum>>3)],0,arrnum); //Fill static variables&arrays with 0's
		//Fill static variables&arrays with 0's
	for(i=j=0;i<arrnum;i+=8,kasm87leng+=8)
	{
		if ((j < ginitvalnum) && (i == ginitval[j].i))
			{ *(double *)&globval[((gstnum+i)>>3)+gccnt] = ginitval[j].v; j++; }
		else *(double *)&globval[((gstnum+i)>>3)+gccnt] = 0;
	}

	if (!gvl)
	{
		gvl = (double *)malloc(65536*sizeof(double));
		if (!gvl) { strcpy(kasm87err,"ERROR: malloc failed"); gecnt = 0; return(0); }
		gvlp = gvl;
	}

	return((void *)kasm87c_copyglob2struct(regnum));

	//if ((newvar[0].parnum < 0) && ((newvar[0].r&0xf0000000) == KESP))
	//     return((void *)kasm87c);
	//else return((void *)kasm87cp);
#else

	if (regnum > 4) { memnum = regnum-4; regnum = 4; } else memnum = 0;
		//0-4 temps: memnum = 0
		//  5 temps: memnum = 1
		//  6 temps: memnum = 2, etc...

		//Use up to 4 registers supported on the x87 stack
		//Replace all [ecx] with [esp], adjusting offsets by +4 or +(memnum<<3)+8
		//
		//Ex. for: memnum=0,myfunc(x,y) | //Ex. for: memnum=2,myfunc(x,y)
		//------------------------------+---------------------------------
		//  [esp+ 0]: return address    |  [esp+ 0]: tempdat1
		//->[esp+ 4]: param 1 (x)       |  [esp+ 8]: tempdat2
		//  [esp+12]: param 2 (y)       |  [esp+16]: (filler)
		//                              |  [esp+20]: return address
		//                              |->[esp+24]: param 1 (x)
		//                              |  [esp+32]: param 2 (y)
	if (!memnum) espoff = 4; else espoff = (memnum<<3)+8; //Offset ESP (passed params) by temp register size
	for(i=gecnt-1;i>=0;i--)
		for(j=gasm[i].n;j>=0;j--)
		{
			if (j < 3) rp = &gasm[i].r[j]; else rp = &rxi[gasm[i].rxi+j-3];

				  if ((rp->r&0xf0000000) == KESP) rp->r += espoff;
			else if ((rp->r&0xf0000000) == KPTR) rp->r += espoff;
			else if ((rp->r&0xf0000000) == KECX)
			{
				if ((rp->r&0x0fffffff) < (4<<3)) rp->r += (KFST-KECX);
													 else rp->r += (KESP-KECX)-(4<<3);
			}
		}

	for(i=0;i<newvarnum;i++)
		if (((newvar[i].r&0xf0000000) == KESP) || ((newvar[i].r&0xf0000000) == KPTR))
			newvar[i].r += espoff;
#if 0
	{ //Show registers associated with variable names (for debug only)
	for(i=0;i<newvarnum;i++)
	{
		printf("%s: ",&newvarnam[newvar[i].nami]);
		if (i < gnumarg) printf("i%d\n",i); else printf("r%d\n",newvar[i].r);
	} printf("\n");
	}
#endif

	ljumpbacknum = 0;
	for(putwrite=0;putwrite<2;putwrite++)
	{
		kasm87leng = 0; jumpatnum = 0; fpustat = 0;

			//mov edx, ? (to be filled with globval pointer later)
		if ((gccnt) || (gstnum) || (arrnum)) { put1byte(0xba); put4byte(0); }

		if (memnum)
		{
			if ((memnum<<3)+4 <= 128) { put2byte(0xc483); put1byte(-((memnum<<3)+4)); } //add esp, -((memnum<<3)+4)
										else { put2byte(0xc481); put4byte(-((memnum<<3)+4)); } //add esp, -((memnum<<3)+4)
		}
		for(j=regnum;j>0;j--) put2byte(0xeed9); //fldz

		for(i=1;i<gecnt;i++)
		{
			switch(gasm[i].f)
			{
				case NUL:
					checklabs((gasm[i].r[0].r&0x0fffffff)+1);
					labpat[gasm[i].r[0].r&0x0fffffff] = kasm87leng;
					break;
				case GOTO:
					//put1byte(0xeb); put1byte(0);     //jmp short ?
					put1byte(0xe9); put4byte(gasm[i].r[1].r&0x0fffffff); //jmp ?
					checklabs(jumpatnum+1); jumpat[jumpatnum++] = kasm87leng;
					if (putwrite) { checkjumpbacks(jumpbacknum+1); jumpback[jumpbacknum++].addr = (long)&compcode[kasm87leng-4]; } else ljumpbacknum++;
					break;
				case RETURN:
					put1stfld(i,gasm[i].r[1]);            //fld qword ptr [?]

						//Ensure stack is empty (except for return value which is st(0))
						//WARNING: fcompp trick only works properly if BOTH regs are known to be valid (not empty)
					//for(j=regnum;j>0;j--) put2byte(0xd9dd); //fstp st(1)
					for(j=regnum;j>0;j--) /*if (fpustat&(1<<(j-1)))*/ put2byte(0xc0dd+(j<<8)); //ffree st(j) (ffree faster on P4: less dependency?)

					if (memnum)
					{
						if ((memnum<<3)+4 <= 128) { put2byte(0xec83); put1byte(-((memnum<<3)+4)); } //sub esp, -((memnum<<3)+4)
													else { put2byte(0xec81); put4byte(-((memnum<<3)+4)); } //sub esp, -((memnum<<3)+4)
					}
					put1byte(0xc3); //ret

					break;
				case MOV: //gvl[gasm[i].r[0]] = globval[gasm[i].r[1]];
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					break;
				case NEGMOV: //gvl[gasm[i].r[0]] = -globval[gasm[i].r[1]];
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					put2byte(0xe0d9);                      //fchs
					break;
				case NEQU0: //gvl[gasm[i].r[0]] = gvl[gasm[i].r[1]] != 0
					if (((gasm[i].r[1].r&0xf0000000) == KFST) || ((gasm[i].r[1].r&0xf0000000) == KPTR) || ((gasm[i].r[1].r&0xf0000000) == KIMM) || ((gasm[i].r[1].r&0xf0000000) == KGLB))
					{
						put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
						if (!(cputype&(1<<15))) //no CMOV
						{
							put2byte(0xe4d9);                      //ftst
							put2byte(0xe0df);                      //fnstsw ax
							put2byte(0xd8dd);                      //fstp st(0)
							put2byte(0xe480); put1byte(0x40);      //and ah, 0x40
							put1byte(0x75); put1byte(4);           //jnz short ?
						}
						else
						{
							put2byte(0xeed9);                      //fldz
							put2byte(0xf1df);                      //fcomip st(1) ;Requires >=PPRO
							put2byte(0xd8dd);                      //fstp st(0)
							put1byte(0x74); put1byte(4);           //jz short ?
						}
					}
					else
					{
						putsib(0x8b,0x00,gasm[i].r[1]);        //mov eax, dword ptr [?]
						gasm[i].r[1].r += 4;
						putsib(0x0b,0x00,gasm[i].r[1]);        //or eax, dword ptr [?+4]
						gasm[i].r[1].r -= 4;
						put1byte(0x74); put1byte(4);         //jz short ?
					}
					put2byte(0xe8d9);                      //fld1
					put1byte(0xeb); put1byte(2);           //jmp short endit
					put2byte(0xeed9);                      //skip: fldz
					break;
				case IF0:
					if (((gasm[i].r[2].r&0xf0000000) == KFST) || ((gasm[i].r[2].r&0xf0000000) == KPTR) || ((gasm[i].r[2].r&0xf0000000) == KIMM) || ((gasm[i].r[2].r&0xf0000000) == KGLB))
					{
						put1stfld(i,gasm[i].r[2]);             //fld qword ptr [?]
						if (!(cputype&(1<<15))) //no CMOV
						{
							put2byte(0xe4d9);                      //ftst
							put2byte(0xe0df);                      //fnstsw ax
							put2byte(0xd8dd);                      //fstp st(0)
							put2byte(0xe480); put1byte(0x40);      //and ah, 0x40
							put2byte(0x850f); put4byte(gasm[i].r[1].r&0x0fffffff); //jnz ?
						}
						else
						{
							put2byte(0xeed9);                      //fldz
							put2byte(0xf1df);                      //fcomip st(1) ;Requires >=PPRO
							put2byte(0xd8dd);                      //fstp st(0)
							put2byte(0x840f); put4byte(gasm[i].r[1].r&0x0fffffff); //jz ?
						}
					}
					else
					{
						putsib(0x8b,0x00,gasm[i].r[2]);        //mov eax, dword ptr [?]
						gasm[i].r[2].r += 4;
						putsib(0x0b,0x00,gasm[i].r[2]);        //or eax, dword ptr [?+4]
						gasm[i].r[2].r -= 4;
						put2byte(0x840f); put4byte(gasm[i].r[1].r&0x0fffffff); //jz ?
					}
					checklabs(jumpatnum+1); jumpat[jumpatnum++] = kasm87leng;
					if (putwrite) { checkjumpbacks(jumpbacknum+1); jumpback[jumpbacknum++].addr = (long)&compcode[kasm87leng-4]; } else ljumpbacknum++;
					break;
				case IF1:
					if (((gasm[i].r[2].r&0xf0000000) == KFST) || ((gasm[i].r[2].r&0xf0000000) == KPTR) || ((gasm[i].r[2].r&0xf0000000) == KIMM) || ((gasm[i].r[2].r&0xf0000000) == KGLB))
					{
						put1stfld(i,gasm[i].r[2]);             //fld qword ptr [?]
						if (!(cputype&(1<<15))) //no CMOV
						{
							put2byte(0xe4d9);                      //ftst
							put2byte(0xe0df);                      //fnstsw ax
							put2byte(0xd8dd);                      //fstp st(0)
							put2byte(0xe480); put1byte(0x40);      //and ah, 0x40
							put2byte(0x840f); put4byte(gasm[i].r[1].r&0x0fffffff); //jz ?
						}
						else
						{
							put2byte(0xeed9);                      //fldz
							put2byte(0xf1df);                      //fcomip st(1) ;Requires >=PPRO
							put2byte(0xd8dd);                      //fstp st(0)
							put2byte(0x850f); put4byte(gasm[i].r[1].r&0x0fffffff); //jnz ?
						}
					}
					else
					{
						putsib(0x8b,0x00,gasm[i].r[2]);        //mov eax, dword ptr [?]
						gasm[i].r[2].r += 4;
						putsib(0x0b,0x00,gasm[i].r[2]);        //or eax, dword ptr [?+4]
						gasm[i].r[2].r -= 4;
						put2byte(0x850f); put4byte(gasm[i].r[1].r&0x0fffffff); //jnz ?
					}
					checklabs(jumpatnum+1); jumpat[jumpatnum++] = kasm87leng;
					if (putwrite) { checkjumpbacks(jumpbacknum+1); jumpback[jumpbacknum++].addr = (long)&compcode[kasm87leng-4]; } else ljumpbacknum++;
					break;

				case RND: //gvl[gasm[i].r[0]] = ((double)krand())/2^31; break;
						//31-bit RND
					put1byte(0xa1); put4byte((long)&kholdrand); //mov eax, kholdrand
					put2byte(0xc069); put4byte(214013*2);  //imul eax, 214013*2
					put1byte(0x05); put4byte(2531011*2);   //add eax, 2531011*2
					put2byte(0xe8d1);                      //shr eax, 1
					put1byte(0xa3); put4byte((long)&kholdrand); //mov kholdrand, eax
					put2byte(0x05db); put4byte((long)&kholdrand); //fild dword ptr [kholdrand]
					put2byte(0x0dd8); put4byte((long)&oneover2_31); //fmul dword ptr [oneover2_31]
					break;
				case NRND: //gvl[gasm[i].r[0]] = nrnd(); break;
					put1byte(0x52);                        //push edx

					put1byte(0xb8); put4byte((long)nrnd);  //mov eax, offset nrnd //use this to allow code dup
					put2byte(0xd0ff);                      //call eax
					//put1byte(0xe8); put4byte(((long)nrnd)-((long)&compcode[kasm87leng+4])); //call nrnd (relative)

					put1byte(0x5a);                        //pop edx
					break;
				case POW: //gvl[gasm[i].r[0]] = pow(gvl[gasm[i].r[1]],gvl[gasm[i].r[2]]); break; //POW: x ^ y = 2^(LOG2(x)*y)
						//inline: y=-1 y=-.5  y=0  y=.5  y=1    kpow: y=-1 y=-.5 y=0  y=.5  y=1
						// x=- 1  4816x 4816 4816x 4816 4816x   x=- 1  488  488  528  488  488   //number is cc
						// x=-.5  4816x 4816 4816x 4816 4816x   x=-.5  424  381  528  381  424   //x means bad result!
						// x=  0  4684  4684 4816x 2936 2936    x=  0   97   97  104   92   92
						// x= .5   372   357  472   357  372    x= .5  408  393  492  393  408
						// x=  1   444   444  472   444  444    x=  1  468  468  492  468  468
					put1stfld(i,gasm[i].r[2]);             //fld qword ptr [?]
					putsib(0xdd,0x01,gasm[i].r[1]);        //fld qword ptr [?]
#if 0
						//inline: returns incorrect values and very slow for x <= 0!
					put2byte(0xf1d9);                      //fyl2x (st1 *= log2(st0), pop st)
					put1byte(0xb8); put4byte(((long)kexptval)+8); //mov eax, offset kexptval[8]
					put2byte(0x10db);                      //fist dword ptr [eax]
					put2byte(0x20da);                      //fisub dword ptr [eax]
					put2byte(0x0081); put4byte(0x3fff);    //add dword ptr [eax], 0x3fff
					put2byte(0xf0d9);                      //f2xm1
					put2byte(0x05d8); put4byte((long)&posone); //fadd dword ptr [posone]
					put2byte(0x68db); put1byte(0xf8);      //fld tbyte ptr [eax-8]
					put2byte(0xc9de);                      //fmulp st(1), st(0)
#else
						//kpow: returns correct results, but a bit slower for x > 0 (more overhead)
					put2byte(0xec83); put1byte(16);        //sub esp, 16
					put1byte(0xdd); put2byte(0x241c);      //fstp qword ptr [esp]
					put4byte(0x08245cdd);                  //fstp qword ptr [esp+8]

					put1byte(0xb8); put4byte((long)kpow);  //mov eax, offset kpow //use this to allow code dup
					put2byte(0xd0ff);                      //call eax
					//put1byte(0xe8); put4byte(((long)kpow)-((long)&compcode[kasm87leng+4])); //call kpow

					put2byte(0xc483); put1byte(16);        //add esp, 16
#endif
					break;
				case TIMES: //gvl[gasm[i].r[0]] = gvl[gasm[i].r[1]]*gvl[gasm[i].r[2]]; break;
					if (gasmeq(gasm[i].r[1],gasm[i].r[2]))
					{
						put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
						put2byte(0xc8dc);                      //fmul st, st(0)
						//tr.r = KFST; putsib(0xdc,0x08,tr);   //fmul st(0)
					}
					else
					{
						if (gasmeq(gasm[i-1].r[0],gasm[i].r[1])) j = 1; else j = 2;
						put1stfld(i,gasm[i].r[j]);             //fld qword ptr [?]
						putsib(0xdc,0x09,gasm[i].r[3-j]);      //fmul qword ptr [?]
					}
					break;
				case SLASH: //gvl[gasm[i].r[0]] = gvl[gasm[i].r[1]]/gvl[gasm[i].r[2]]; break;
					if (gasmeq(gasm[i-1].r[0],gasm[i].r[2]))
					{
						put1stfld(i,gasm[i].r[2]);             //fld qword ptr [?]
						putsib(0xdc,0x39,gasm[i].r[1]);        //fdivr qword ptr [?]
					}
					else
					{
						put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
						putsib(0xdc,0x31,gasm[i].r[2]);        //fdiv qword ptr [?]
					}
					break;
				case PERC: //gvl[gasm[i].r[0]] = gvl[gasm[i].r[1]]-floor(gvl[gasm[i].r[1]]/gvl[gasm[i].r[2]])*gvl[gasm[i].r[2]]; break;
					putsib(0xdd,0x00,gasm[i].r[2]);        //fld qword ptr [?]
					putsib(0xdd,0x01,gasm[i].r[1]);        //fld qword ptr [?]
					put2byte(0xf8d9);                      //fprem
					put2byte(0xe4d9);                      //ftst
					put2byte(0xe0df);                      //fnstsw ax
					put2byte(0xe480); put1byte(0x01);      //and ah, 0x01
					put1byte(0x74); put1byte(6);           //jz short skip
					put2byte(0xc1d9);                      //fld st(1)
					put2byte(0xe1d9);                      //fabs
					put2byte(0xc1de);                      //faddp st(1), st
					put2byte(0xd9dd);                      //skip: fstp st(1)
					break;
				case PLUS: //gvl[gasm[i].r[0]] = gvl[gasm[i].r[1]]+gvl[gasm[i].r[2]]; break;
				case FADD: //no break intentional
					if (gasmeq(gasm[i].r[1],gasm[i].r[2]))
					{
						put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
						put2byte(0xc0dc);                      //fadd st, st(0)
						//tr.r = KFST; putsib(0xdc,0x00,tr);   //fadd st(0)
					}
					else
					{
						if (gasmeq(gasm[i-1].r[0],gasm[i].r[1])) j = 1; else j = 2;
						put1stfld(i,gasm[i].r[j]);             //fld qword ptr [?]
						putsib(0xdc,0x01,gasm[i].r[3-j]);      //fadd qword ptr [?]
					}
					break;
				case MINUS: //gvl[gasm[i].r[0]] = gvl[gasm[i].r[1]]-gvl[gasm[i].r[2]]; break;
					if (gasmeq(gasm[i-1].r[0],gasm[i].r[2]))
					{
						put1stfld(i,gasm[i].r[2]);             //fld qword ptr [?]
						putsib(0xdc,0x29,gasm[i].r[1]);        //fsubr qword ptr [?]
					}
					else
					{
						put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
						putsib(0xdc,0x21,gasm[i].r[2]);        //fsub qword ptr [?]
					}
					break;
				case FABS: //gvl[gasm[i].r[0]] = fabs(gvl[gasm[i].r[1]]);
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					put2byte(0xe1d9);                      //fabs st, st(0)
					break;
				case SGN: //if (gvl[gasm[i].r[1]] < 0) gvl[gasm[i].r[0]] =-1.0; else if (gvl[gasm[i].r[1]] > 0) gvl[gasm[i].r[0]] = 1.0; else gvl[gasm[i].r[0]] = 0.0; break;
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					put2byte(0xe4d9);                      //ftst
					put2byte(0xe0df);                      //fnstsw ax
					put2byte(0xd8dd);                      //fstp st(0)
					put4byte(0x0100a966);                  //test ax, 0x0100
					put1byte(0x74); put1byte(8);           //jz short skip1
					put2byte(0x05d9); put4byte((long)&negone); //fld dword ptr [negone]
					put1byte(0xeb); put1byte(12);          //jmp short endit
					put4byte(0x4000a966);                  //skip1: test ax, 0x4000
					put1byte(0x74); put1byte(4);           //jz short skip2
					put2byte(0xeed9);                      //fldz
					put1byte(0xeb); put1byte(2);           //jmp short endit
					put2byte(0xe8d9);                      //skip2: fld1
					break;
				case UNIT: //if (gvl[gasm[i].r[1]] < 0) gvl[gasm[i].r[0]] = 0.0; else if (gvl[gasm[i].r[1]] > 0) gvl[gasm[i].r[0]] = 1.0; else gvl[gasm[i].r[0]] = 0.5; break;
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					put2byte(0xe4d9);                      //ftst
					put2byte(0xe0df);                      //fnstsw ax
					put2byte(0xd8dd);                      //fstp st(0)
					put4byte(0x0100a966);                  //test ax, 0x0100
					put1byte(0x74); put1byte(4);           //jz short skip1
					put2byte(0xeed9);                      //fldz
					put1byte(0xeb); put1byte(16);          //jmp short endit
					put4byte(0x4000a966);                  //skip1: test ax, 0x4000
					put1byte(0x74); put1byte(8);           //jz short skip2
					put2byte(0x05d9); put4byte((long)&pointfive); //fld dword ptr [pointfive]
					put1byte(0xeb); put1byte(2);           //jmp short endit
					put2byte(0xe8d9);                      //skip2: fld1
					break;
				case FLOOR: //gvl[gasm[i].r[0]] = floor(gvl[gasm[i].r[1]]); break;
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					put2byte(0xec83); put1byte(8);         //sub esp, 8
					tr.r = KESP;
					putsib(0xd99b,0x38,tr);                //fstcw word ptr [esp]
					putsib(0x8b66,0x00,tr);                //mov ax, word ptr [esp]
					put4byte(0xf0ff2566);                  //and ax, 0xf0ff
					put4byte(0x07000d66);                  //or ax, 0x0700
					putsib(0x8766,0x00,tr);                //xchg word ptr [esp], ax
					putsib(0xd9,0x28,tr);                  //fldcw word ptr [esp]
					putsib(0xdf,0x38,tr);                  //fistp qword ptr [esp]
					putsib(0xdf,0x28,tr);                  //fild qword ptr [esp]
					putsib(0x8966,0x00,tr);                //mov word ptr [esp], ax
					putsib(0xd9,0x28,tr);                  //fldcw word ptr [esp]
					put2byte(0xc483); put1byte(8);         //add esp, 8
					break;
				case CEIL: //gvl[gasm[i].r[0]] = ceil(gvl[gasm[i].r[1]]); break;
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					put2byte(0xe0d9);                      //fchs
					put2byte(0xec83); put1byte(8);         //sub esp, 8
					tr.r = KESP;
					putsib(0xd99b,0x38,tr);                //fstcw word ptr [esp]
					putsib(0x8b66,0x00,tr);                //mov ax, word ptr [esp]
					put4byte(0xf0ff2566);                  //and ax, 0xf0ff
					put4byte(0x07000d66);                  //or ax, 0x0700
					putsib(0x8766,0x00,tr);                //xchg word ptr [esp], ax
					putsib(0xd9,0x28,tr);                  //fldcw word ptr [esp]
					putsib(0xdf,0x38,tr);                  //fistp qword ptr [esp]
					putsib(0xdf,0x28,tr);                  //fild qword ptr [esp]
					putsib(0x8966,0x00,tr);                //mov word ptr [esp], ax
					putsib(0xd9,0x28,tr);                  //fldcw word ptr [esp]
					put2byte(0xc483); put1byte(8);         //add esp, 8
					put2byte(0xe0d9);                      //fchs
					break;
				case ROUND0: case ROUND0_32: //gvl[gasm[i].r[0]] = round0(gvl[gasm[i].r[1]]); break;
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					if (!(cputype&(1<<27))) //no SSE3
					{
						put2byte(0xec83); put1byte(8);         //sub esp, 8
						put2byte(0x1cdd); put1byte(0x24);      //fstp qword ptr [esp]
						put4byte(0x0424448b);                  //mov eax, dword ptr [esp+4]
						put1byte(0x52);                        //push edx
						put2byte(0xd08b);                      //mov edx, eax
						put1byte(0x25); put4byte(0x7ff00000);  //and eax, 0x7ff00000
						put2byte(0xe8c1); put1byte(17);        //shr eax, 17
						put2byte(0x9023); put4byte(((long)round0msk)+4); //and edx, dword ptr round0msk[eax+4]
						put4byte(0x08245489);                  //mov dword ptr [esp+8], edx
						put4byte(0x0424548b);                  //mov edx, dword ptr [esp+4]
						put2byte(0x9023); put4byte((long)round0msk); //and edx, dword ptr round0msk[eax]
						put4byte(0x04245489);                  //mov dword ptr [esp+4], edx
						put1byte(0x5a);                        //pop edx
						put2byte(0x04dd); put1byte(0x24);      //fld qword ptr [esp]
						put2byte(0xc483); put1byte(8);         //add esp, 8
					}
					else
					{
						if (gasm[i].f == ROUND0_32) //array indices only need 32-bit precision
						{
							put2byte(0xec83); put1byte(4);         //sub esp, 4
							put2byte(0x0cdb); put1byte(0x24);      //fisttp dword ptr [esp]
							put2byte(0x04db); put1byte(0x24);      //fild qword ptr [esp]
							put2byte(0xc483); put1byte(4);         //add esp, 4
						}
						else
						{
							put2byte(0xec83); put1byte(8);         //sub esp, 8
							put2byte(0x0cdd); put1byte(0x24);      //fisttp qword ptr [esp]
							put2byte(0x2cdf); put1byte(0x24);      //fild qword ptr [esp]
							put2byte(0xc483); put1byte(8);         //add esp, 8
						}
					}
					break;

				case MIN: //if (gvl[gasm[i].r[2]] < gvl[gasm[i].r[1]]) gvl[gasm[i].r[0]] = gvl[gasm[i].r[2]]; else gvl[gasm[i].r[0]] = gvl[gasm[i].r[1]]; break;
					putsib(0xdd,0x00,gasm[i].r[1]);        //fld qword ptr [?]
					putsib(0xdc,0x19,gasm[i].r[2]);        //fcomp qword ptr [?]
					put2byte(0xe0df);                      //fnstsw ax
					put2byte(0xe480); put1byte(0x41);      //and ah, 0x41
					put1byte(0x75); put1byte(putlen(gasm[i].r[2])+3); //jnz short skip
					putsib(0xdd,0x00,gasm[i].r[2]);        //fld qword ptr [?]
					put1byte(0xeb); put1byte(putlen(gasm[i].r[1])+1); //jmp short endit
					putsib(0xdd,0x00,gasm[i].r[1]);        //fld qword ptr [?]
					break;
				case MAX: //if (gvl[gasm[i].r[2]] > gvl[gasm[i].r[1]]) gvl[gasm[i].r[0]] = gvl[gasm[i].r[2]]; else gvl[gasm[i].r[0]] = gvl[gasm[i].r[1]]; break;
					putsib(0xdd,0x00,gasm[i].r[1]);        //fld qword ptr [?]
					putsib(0xdc,0x19,gasm[i].r[2]);        //fcomp qword ptr [?]
					put2byte(0xe0df);                      //fnstsw ax
					put2byte(0xe480); put1byte(0x41);      //and ah, 0x41
					put1byte(0x74); put1byte(putlen(gasm[i].r[2])+3); //jz short skip
					putsib(0xdd,0x00,gasm[i].r[2]);        //fld qword ptr [?]
					put1byte(0xeb); put1byte(putlen(gasm[i].r[1])+1); //jmp short endit
					putsib(0xdd,0x00,gasm[i].r[1]);        //fld qword ptr [?]
					break;
				case FMOD: //gvl[gasm[i].r[0]] = fmod(gvl[gasm[i].r[1]],gvl[gasm[i].r[2]]); break;
					put1stfld(i,gasm[i].r[2]);             //fld qword ptr [?]
					putsib(0xdd,0x01,gasm[i].r[1]);        //fld qword ptr [?]
					put2byte(0xf8d9);                      //fprem
					put2byte(0xd9dd);                      //skip: fstp st(1)
					break;
				case SIN: //gvl[gasm[i].r[0]] = sin(gvl[gasm[i].r[1]]); break;
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					put2byte(0xfed9);                      //fsin
					break;
				case COS: //gvl[gasm[i].r[0]] = cos(gvl[gasm[i].r[1]]); break;
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					put2byte(0xffd9);                      //fcos
					break;
				case TAN: //gvl[gasm[i].r[0]] = tan(gvl[gasm[i].r[1]]); break;
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					put2byte(0xf2d9);                      //fptan
					put2byte(0xd8dd);                      //fstp st(0) //discard the 1.0
					break;
				case ASIN: //gvl[gasm[i].r[0]] = acos(gvl[gasm[i].r[1]]); break;
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					put2byte(0xe8d9);                      //fld1
					put2byte(0xc1d9);                      //fld st(1)
					put2byte(0xcad8);                      //fmul st, st(2)
					put2byte(0xe9de);                      //fsubp (st0 = st1-st0)
					put2byte(0xfad9);                      //fsqrt
					put2byte(0xf3d9);                      //fpatan
					break;
				case ACOS: //gvl[gasm[i].r[0]] = asin(gvl[gasm[i].r[1]]); break;
					put2byte(0xe8d9);                      //fld1
					putsib(0xdd,0x01,gasm[i].r[1]);        //fld qword ptr [?]
					put2byte(0xc8dc);                      //fmul st, st
					put2byte(0xe9de);                      //fsubp (st0 = st1-st0)
					put2byte(0xfad9);                      //fsqrt
					putsib(0xdd,0x01,gasm[i].r[1]);        //fld qword ptr [?]
					put2byte(0xf3d9);                      //fpatan
					break;
				case ATAN: //gvl[gasm[i].r[0]] = atan(gvl[gasm[i].r[1]]); break;
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					put2byte(0xe8d9);                      //fld1
					put2byte(0xf3d9);                      //fpatan
					break;
				case ATAN2: //gvl[gasm[i].r[0]] = atan2(gvl[gasm[i].r[1]],gvl[gasm[i].r[2]]); break;
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					putsib(0xdd,0x01,gasm[i].r[2]);        //fld qword ptr [?]
					put2byte(0xf3d9);                      //fpatan
					break;
				case SQRT: //gvl[gasm[i].r[0]] = sqrt(gvl[gasm[i].r[1]]); break;
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					put2byte(0xfad9);                      //fsqrt
					break;
				case EXP: //gvl[gasm[i].r[0]] = exp(gvl[gasm[i].r[1]]); break;
					put2byte(0xead9);                      //fldl2e
					putsib(0xdc,0x09,gasm[i].r[1]);        //fmul qword ptr [?]
#if 0
						//This code is not thread-safe
					put1byte(0xb8); put4byte(((long)kexptval)+8); //mov eax, offset kexptval[8]
					put2byte(0x10db);                      //fist dword ptr [eax]
					put2byte(0x20da);                      //fisub dword ptr [eax]
					put2byte(0x0081); put4byte(0x3fff);    //add dword ptr [eax], 0x3fff
					put2byte(0xf0d9);                      //f2xm1
					put2byte(0x05d8); put4byte((long)&posone); //fadd dword ptr [posone]
					put2byte(0x68db); put1byte(0xf8);      //fld tbyte ptr [eax-8]
					put2byte(0xc9de);                      //fmulp st(1), st(0)
#else
						//Use this for Multithreaded code (~80cc slower than unsafe)
					put2byte(0xec83); put1byte(8);         //sub esp, 8
					put2byte(0x14db); put1byte(0x24);      //fist dword ptr [esp]
					put2byte(0x24da); put1byte(0x24);      //fisub dword ptr [esp]
					put2byte(0x0481); put1byte(0x24); put4byte(0x3fff); //add dword ptr [esp], 0x3fff
					put2byte(0xf0d9);                      //f2xm1
					put2byte(0x05d8); put4byte((long)&posone); //fadd dword ptr [posone]
					put1byte(0x68); put4byte(0x80000000);  //push 0x80000000
					put2byte(0x006a);                      //push 0
					put2byte(0x2cdb); put1byte(0x24);      //fld tbyte ptr [esp]
					put2byte(0xc9de);                      //fmulp st(1), st(0)
					put2byte(0xc483); put1byte(16);        //add esp, 16
#endif
					break;
				case FACT: //gvl[gasm[i].r[0]] = fact(gvl[gasm[i].r[1]]); break;
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					put2byte(0xec83); put1byte(8);         //sub esp, 8
					put1byte(0xdd); put2byte(0x241c);      //fstp qword ptr [esp]

					put1byte(0xb8); put4byte((long)fact);  //mov eax, offset fact //use this to allow code dup
					put2byte(0xd0ff);                      //call eax
					//put1byte(0xe8); put4byte(((long)fact)-((long)&compcode[kasm87leng+4])); //call fact

					put2byte(0xc483); put1byte(0x08);      //add esp, 8
					break;
				case LOG: //gvl[gasm[i].r[0]] = log(gvl[gasm[i].r[1]]); break;
					put2byte(0xedd9);                      //fldln2 (log(2)/log(10))
					putsib(0xdd,0x01,gasm[i].r[1]);        //fld qword ptr [?]
					put2byte(0xf1d9);                      //fyl2x (st1 *= log2(st0), pop st)
					break;
				case LOGB: //gvl[gasm[i].r[0]] = log(gvl[gasm[i].r[1]])/log(gvl[gasm[i].r[2]]); break;
					put2byte(0xe8d9);                      //fld1
					putsib(0xdd,0x01,gasm[i].r[1]);        //fld qword ptr [?]
					put2byte(0xf1d9);                      //fyl2x (st1 *= log2(st0), pop st)
					put2byte(0xe8d9);                      //fld1
					putsib(0xdd,0x02,gasm[i].r[2]);        //fld qword ptr [?]
					put2byte(0xf1d9);                      //fyl2x (st1 *= log2(st0), pop st)
					put2byte(0xf9de);                      //fdivp
					break;
				case PEEK: //gvl[gasm[i].r[0]] = *(double *)(((long)&gvl[gasm[i].r[1]])+gvl[gasm[i].r[2]]*8); break;
					put1stfld(i,gasm[i].r[2]);             //fld qword ptr [?]
					if (cputype&(1<<27)) //SSE3
					{
						put2byte(0xec83); put1byte(4);         //sub esp, 4
						put2byte(0x0cdb); put1byte(0x24);      //fisttp dword ptr [esp]
						put1byte(0x58);                        //pop eax
					}
					else
					{
							//Round ninf (floor). Algo lifted from Agner Fog's optimizing_assembly.pdf
						put2byte(0xec83); put1byte(8);         //sub esp, 8
						put2byte(0x14db); put1byte(0x24);      //fist dword ptr [esp]   (rounded value)
						put2byte(0x24da); put1byte(0x24);      //fisub dword ptr [esp]  (sub rounded value)
						put4byte(0x04245cd9);                  //fstp dword ptr [esp+4] (diff)
						put1byte(0x58);                        //pop eax                (rounded value)
						put1byte(0x59);                        //pop ecx                (diff (float))
						put2byte(0xc181); put4byte(0x7fffffff);//add ecx, 0x7fffffff    (set CF if diff < -0)
						put2byte(0xd883); put1byte(0);         //sbb eax, 0             (dec if x-round(x) < -0)
					}

						//quick&dirty bounds check
					j = newvar[gasm[i].r[1].nv].maxind;
					if ((j) && (!((j-1)&j))) //if 2^x, use "and"
					{
						if (j <= 128) { put2byte(0xe083); put1byte(j-1); } //and eax, imm8
									else { put1byte(0x25);   put4byte(j-1); } //and eax, imm32
					}
					else
					{
						put1byte(0x3d); put4byte(j);        //cmp eax, j
						put2byte(0x0272);                   //jc short skipzeroit
						put2byte(0xc033);                   //xor eax, eax
					}

					if (((gasm[i].r[1].r&0xf0000000) == KIMM) || ((gasm[i].r[1].r&0xf0000000) == KGLB))
					{
						put2byte(0x04dd); put1byte(0xc5);      //fld qword ptr [eax*8+imm32]
						tr = gasm[i].r[1];
						if (putwrite)
						{
							checkpatch(patchnum+1);
							patch[patchnum].lptr = (long *)&compcode[kasm87leng];
							patch[patchnum].ind = tr.r;
							patchnum++;
						}
						put4byte(tr.q*8);
					}
					else
					{
						putsib(0x8d,0x08,gasm[i].r[1]);        //lea ecx, [?]
							//Warning: do not put any putsib calls between lea&fld because it can modify ecx
						put2byte(0x04dd); put1byte(0xc1);      //fld qword ptr [eax*8+ecx]
					}
					break;
				case LES: //gvl[gasm[i].r[0]] = gvl[gasm[i].r[1]] < gvl[gasm[i].r[2]];
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					putsib(0xdc,0x19,gasm[i].r[2]);        //fcomp qword ptr [?]
					put2byte(0xe0df);                      //fnstsw ax
					put2byte(0xe480); put1byte(0x01);      //and ah, 0x01
					put1byte(0x74); put1byte(4);           //jz short skip
					put2byte(0xe8d9);                      //fld1
					put1byte(0xeb); put1byte(2);           //jmp short endit
					put2byte(0xeed9);                      //fldz
					break;
				case LESEQ: //gvl[gasm[i].r[0]] = gvl[gasm[i].r[1]] <= gvl[gasm[i].r[2]];
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					putsib(0xdc,0x19,gasm[i].r[2]);        //fcomp qword ptr [?]
					put2byte(0xe0df);                      //fnstsw ax
					put2byte(0xe480); put1byte(0x41);      //and ah, 0x41
					put1byte(0x74); put1byte(4);           //jz short skip
					put2byte(0xe8d9);                      //fld1
					put1byte(0xeb); put1byte(2);           //jmp short endit
					put2byte(0xeed9);                      //fldz
					break;
				case MOR: //gvl[gasm[i].r[0]] = gvl[gasm[i].r[1]] > gvl[gasm[i].r[2]];
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					putsib(0xdc,0x19,gasm[i].r[2]);        //fcomp qword ptr [?]
					put2byte(0xe0df);                      //fnstsw ax
					put2byte(0xe480); put1byte(0x41);      //and ah, 0x41
					put1byte(0x75); put1byte(4);           //jnz short skip
					put2byte(0xe8d9);                      //fld1
					put1byte(0xeb); put1byte(2);           //jmp short endit
					put2byte(0xeed9);                      //fldz
					break;
				case MOREQ: //gvl[gasm[i].r[0]] = gvl[gasm[i].r[1]] >= gvl[gasm[i].r[2]];
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					putsib(0xdc,0x19,gasm[i].r[2]);        //fcomp qword ptr [?]
					put2byte(0xe0df);                      //fnstsw ax
					put2byte(0xe480); put1byte(0x01);      //and ah, 0x01
					put1byte(0x75); put1byte(4);           //jnz short skip
					put2byte(0xe8d9);                      //fld1
					put1byte(0xeb); put1byte(2);           //jmp short endit
					put2byte(0xeed9);                      //fldz
					break;
				case EQU: //gvl[gasm[i].r[0]] = gvl[gasm[i].r[1]] == gvl[gasm[i].r[2]];
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					putsib(0xdc,0x19,gasm[i].r[2]);        //fcomp qword ptr [?]
					put2byte(0xe0df);                      //fnstsw ax
					put2byte(0xe480); put1byte(0x40);      //and ah, 0x40
					put1byte(0x74); put1byte(4);           //jz short skip
					put2byte(0xe8d9);                      //fld1
					put1byte(0xeb); put1byte(2);           //jmp short endit
					put2byte(0xeed9);                      //fldz
					break;
				case NEQU: //gvl[gasm[i].r[0]] = gvl[gasm[i].r[1]] != gvl[gasm[i].r[2]];
					put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
					putsib(0xdc,0x19,gasm[i].r[2]);        //fcomp qword ptr [?]
					put2byte(0xe0df);                      //fnstsw ax
					put2byte(0xe480); put1byte(0x40);      //and ah, 0x40
					put1byte(0x75); put1byte(4);           //jnz short skip
					put2byte(0xe8d9);                      //fld1
					put1byte(0xeb); put1byte(2);           //jmp short endit
					put2byte(0xeed9);                      //fldz
					break;
				case LAND: //gvl[gasm[i].r[0]] = gvl[gasm[i].r[1]] && gvl[gasm[i].r[2]];
					if (((gasm[i].r[1].r&0xf0000000) == KFST) ||
						 ((gasm[i].r[1].r&0xf0000000) == KPTR) ||
						 ((gasm[i].r[1].r&0xf0000000) == KIMM) ||
						 ((gasm[i].r[2].r&0xf0000000) == KFST) ||
						 ((gasm[i].r[2].r&0xf0000000) == KPTR) ||
						 ((gasm[i].r[2].r&0xf0000000) == KIMM) ||
						 ((gasm[i].r[2].r&0xf0000000) == KGLB))
					{
						put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
						put2byte(0xe4d9);                      //ftst
						put2byte(0xe0df);                      //fnstsw ax
						put2byte(0xd8dd);                      //fstp st(0)
						put2byte(0xe480); put1byte(0x40);      //and ah, 0x40
						put1byte(0x75); put1byte(putlen(gasm[i].r[2])+16); //jnz short ?
						putsib(0xdd,0x00,gasm[i].r[2]);        //fld qword ptr [?]
						put2byte(0xe4d9);                      //ftst
						put2byte(0xe0df);                      //fnstsw ax
						put2byte(0xd8dd);                      //fstp st(0)
						put2byte(0xe480); put1byte(0x40);      //and ah, 0x40
						put1byte(0x75); put1byte(4);           //jnz short ?
					}
					else
					{
						putsib(0x8b,0x00,gasm[i].r[1]);        //mov eax, dword ptr [?]
						gasm[i].r[1].r += 4;
						putsib(0x0b,0x00,gasm[i].r[1]);        //or eax, dword ptr [?+4]
						gasm[i].r[1].r -= 4;
						tr = gasm[i].r[2]; tr.r += 4;
						put1byte(0x74); put1byte(putlen(gasm[i].r[2])+putlen(tr)+8); //jz short skip
						putsib(0x8b,0x00,gasm[i].r[2]);        //mov eax, dword ptr [?]
						putsib(0x0b,0x00,tr);                  //or eax, dword ptr [?+4]
						put1byte(0x74); put1byte(4);           //jz short skip
					}
					put2byte(0xe8d9);                      //fld1
					put1byte(0xeb); put1byte(2);           //jmp short endit
					put2byte(0xeed9);                      //skip: fldz
					break;
				case LOR: //gvl[gasm[i].r[0]] = gvl[gasm[i].r[1]] || gvl[gasm[i].r[2]];
					if (((gasm[i].r[1].r&0xf0000000) == KFST) ||
						 ((gasm[i].r[1].r&0xf0000000) == KPTR) ||
						 ((gasm[i].r[1].r&0xf0000000) == KIMM) ||
						 ((gasm[i].r[2].r&0xf0000000) == KFST) ||
						 ((gasm[i].r[2].r&0xf0000000) == KPTR) ||
						 ((gasm[i].r[2].r&0xf0000000) == KIMM) ||
						 ((gasm[i].r[2].r&0xf0000000) == KGLB))
					{
						put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]
						put2byte(0xe4d9);                      //ftst
						put2byte(0xe0df);                      //fnstsw ax
						put2byte(0xd8dd);                      //fstp st(0)
						put2byte(0xe480); put1byte(0x40);      //and ah, 0x40
						put1byte(0x74); put1byte(putlen(gasm[i].r[2])+16); //jz short ?
						putsib(0xdd,0x00,gasm[i].r[2]);        //fld qword ptr [?]
						put2byte(0xe4d9);                      //ftst
						put2byte(0xe0df);                      //fnstsw ax
						put2byte(0xd8dd);                      //fstp st(0)
						put2byte(0xe480); put1byte(0x40);      //and ah, 0x40
						put1byte(0x74); put1byte(4);           //jz short ?
					}
					else
					{
						putsib(0x8b,0x00,gasm[i].r[1]);        //mov eax, dword ptr [?]
						tr = gasm[i].r[1]; tr.r += 4;
						putsib(0x0b,0x00,tr);                  //or eax, dword ptr [?+4]
						putsib(0x0b,0x00,gasm[i].r[2]);        //or eax, dword ptr [?]
						tr = gasm[i].r[2]; tr.r += 4;
						putsib(0x0b,0x00,tr);                  //or eax, dword ptr [?+4]
						put1byte(0x75); put1byte(4);           //jnz short skip
					}
					put2byte(0xeed9);                      //fldz
					put1byte(0xeb); put1byte(2);           //jmp short endit
					put2byte(0xe8d9);                      //skip: fld1
					break;
				case POKE: //*(double *)(((long)&gvl[gasm[i].r[1]])+gvl[gasm[i].r[2]]*8) ?= gvl[gasm[i].r[3]]; break;
				case POKETIMES: case POKESLASH: case POKEPERC: case POKEPLUS: case POKEMINUS:
					put1stfld(i,gasm[i].r[2]);             //fld qword ptr [?]
					if (cputype&(1<<27)) //SSE3
					{
						put2byte(0xec83); put1byte(4);         //sub esp, 4
						put2byte(0x0cdb); put1byte(0x24);      //fisttp dword ptr [esp]
						put1byte(0x58);                        //pop eax
					}
					else
					{
							//Round ninf (floor). Algo lifted from Agner Fog's optimizing_assembly.pdf
						put2byte(0xec83); put1byte(8);         //sub esp, 8
						put2byte(0x14db); put1byte(0x24);      //fist dword ptr [esp]   (rounded value)
						put2byte(0x24da); put1byte(0x24);      //fisub dword ptr [esp]  (sub rounded value)
						put4byte(0x04245cd9);                  //fstp dword ptr [esp+4] (diff)
						put1byte(0x58);                        //pop eax                (rounded value)
						put1byte(0x59);                        //pop ecx                (diff (float))
						put2byte(0xc181); put4byte(0x7fffffff);//add ecx, 0x7fffffff    (set CF if diff < -0)
						put2byte(0xd883); put1byte(0);         //sbb eax, 0             (dec if x-round(x) < -0)
					}

					putsib(0xdd,0x00,rxi[gasm[i].rxi]);    //fld qword ptr gasm[i].r[?3?] (3rd input parameter)

						//quick&dirty bounds check
					j = newvar[gasm[i].r[1].nv].maxind;
					if ((j) && (!((j-1)&j))) //if 2^x, use "and"
					{
						if (j <= 128) { put2byte(0xe083); put1byte(j-1); } //and eax, imm8
									else { put1byte(0x25);   put4byte(j-1); } //and eax, imm32
					}
					else
					{
						put1byte(0x3d); put4byte(j);        //cmp eax, j
						put2byte(0x0272);                   //jc short skipzeroit
						put2byte(0xc033);                   //xor eax, eax
					}

					if (((gasm[i].r[1].r&0xf0000000) == KIMM) || ((gasm[i].r[1].r&0xf0000000) == KGLB))
					{
						if (gasm[i].f == POKEPERC)
						{
							put2byte(0x04dd); put1byte(0xc5);      //fld qword ptr [eax*8+imm32]
							tr = gasm[i].r[1];
							if (putwrite)
							{
								checkpatch(patchnum+1);
								patch[patchnum].lptr = (long *)&compcode[kasm87leng];
								patch[patchnum].ind = tr.r;
								patchnum++;
							}
							put4byte(tr.q*8);
							put2byte(0xf8d9);                      //fprem
							put2byte(0xe4d9);                      //ftst
							put1byte(0x50);                        //push eax
							put2byte(0xe0df);                      //fnstsw ax
							put2byte(0xe480); put1byte(0x01);      //and ah, 0x01
							put1byte(0x58);                        //pop eax
							put1byte(0x74); put1byte(6);           //jz short skip
							put2byte(0xc1d9);                      //fld st(1)
							put2byte(0xe1d9);                      //fabs
							put2byte(0xc1de);                      //faddp st(1), st
							put2byte(0xd9dd);                      //skip: fstp st(1)
						}
						else if (gasm[i].f != POKE)
						{
							switch(gasm[i].f)
							{
								case POKETIMES: put2byte(0x0cdc); put1byte(0xc5); break; //fmul qword ptr [eax*8+imm32]
								case POKESLASH: put2byte(0x3cdc); put1byte(0xc5); break; //fdivr qword ptr [eax*8+imm32]
								case POKEPLUS:  put2byte(0x04dc); put1byte(0xc5); break; //fadd qword ptr [eax*8+imm32]
								case POKEMINUS: put2byte(0x2cdc); put1byte(0xc5); break; //fsubr qword ptr [eax*8+imm32]
							}
							tr = gasm[i].r[1];
							if (putwrite)
							{
								checkpatch(patchnum+1);
								patch[patchnum].lptr = (long *)&compcode[kasm87leng];
								patch[patchnum].ind = tr.r;
								patchnum++;
							}
							put4byte(tr.q*8);
						}
						put2byte(0x1cdd); put1byte(0xc5);      //fstp qword ptr [eax*8+imm32]
						tr = gasm[i].r[1];
						if (putwrite)
						{
							checkpatch(patchnum+1);
							patch[patchnum].lptr = (long *)&compcode[kasm87leng];
							patch[patchnum].ind = tr.r;
							//patch[patchnum].ind = -1;
							//if ((tr.r&0xf0000000) == KIMM) patch[patchnum].ptr = gevalext[tr.r&0x0fffffff].ptr;
							//                          else patch[patchnum].ptr = (void *)(gstatmem + (tr.r&0x0fffffff));
							patchnum++;
						}
						put4byte(tr.q*8);
					}
					else
					{
						putsib(0x8d,0x08,gasm[i].r[1]);        //lea ecx, [?]
							//Warning: do not put any putsib calls between lea&fld because it can modify ecx
						if (gasm[i].f == POKEPERC)
						{
							put2byte(0x04dd); put1byte(0xc1);      //fld qword ptr [eax*8+ecx]

							put2byte(0xf8d9);                      //fprem
							put2byte(0xe4d9);                      //ftst
							put1byte(0x50);                        //push eax
							put2byte(0xe0df);                      //fnstsw ax
							put2byte(0xe480); put1byte(0x01);      //and ah, 0x01
							put1byte(0x58);                        //pop eax
							put1byte(0x74); put1byte(6);           //jz short skip
							put2byte(0xc1d9);                      //fld st(1)
							put2byte(0xe1d9);                      //fabs
							put2byte(0xc1de);                      //faddp st(1), st
							put2byte(0xd9dd);                      //skip: fstp st(1)
						}
						else if (gasm[i].f != POKE)
						{
							switch(gasm[i].f)
							{
								case POKETIMES: put2byte(0x0cdc); put1byte(0xc1); break; //fmul qword ptr [eax*8+ecx]
								case POKESLASH: put2byte(0x3cdc); put1byte(0xc1); break; //fdivr qword ptr [eax*8+ecx]
								case POKEPLUS:  put2byte(0x04dc); put1byte(0xc1); break; //fadd qword ptr [eax*8+ecx]
								case POKEMINUS: put2byte(0x2cdc); put1byte(0xc1); break; //fsubr qword ptr [eax*8+ecx]
							}
						}
						put2byte(0x1cdd); put1byte(0xc1);      //fstp qword ptr [eax*8+ecx]
					}
					break;
				case USERFUNC:
					//put1stfld(i,gasm[i].r[1]);             //fld qword ptr [?]

					tbuf = &newvarnam[newvar[gasm[i].g].proti];
					for(j=k=0;j<gasm[i].n;j++)
					{
						if (tbuf[j] == 'e') { k += (gasm[i].n-j)*8; break; }
						if (tbuf[j] >= 'a') k += 8; else k += 4;
					}

					put1byte(0x52);                        //push edx
					l = -((regnum<<3)+k);                  //add esp, i
					if (l >= -128) { put2byte(0xc483); put1byte(l); } else { put2byte(0xc481); put4byte(l); }

					got = 0;
					for(j=0,l=0;j<gasm[i].n;j++)
					{
						if (j < 2) tr = gasm[i].r[j+1]; else tr = rxi[gasm[i].rxi+j-2];
						if (((tr.r&0xf0000000) == KESP) || ((tr.r&0xf0000000) == KPTR))
							tr.r += (regnum<<3)+k+4; //hack for "add esp,i"&pushes

						if ((!got) && (tbuf[j] == 'e')) got = 1;
						if ((got) || (tbuf[j] >= 'a')) //'d'
						{
							putsib(0xdd,0x00,tr);            //fld qword ptr [?]
							put1byte(0xdd);                  //fstp qword ptr [esp+l]
							if      (!l)      { put2byte(0x241c); }
							else if (l < 128) { put2byte(0x245c); put1byte(l); }
							else              { put2byte(0x249c); put4byte(l); }
							l += 8;
						}
						else //'C','D'
						{
							if ((tr.r&0xf0000000) == KEDX)
							{
								if ((tr.r&0x0fffffff) < gccnt*8)
									{ strcpy(kasm87err,"ERROR: pointer to constant"); return(0); }
									//Hack to protect EVALDRAW (passing filename string as a pointer in evalextyp)
								else if ((tbuf[j] == 'D') && ((tr.r&0x0fffffff) < gccnt*8+gstnum))
									{ strcpy(kasm87err,"ERROR: bad string"); return(0); }
							}
							if ((tr.r&0xf0000000) == KFST) tr.r = (tr.r&0x0fffffff)+k+KESP;
							putsib(0x8d,0x00,tr);            //lea eax, qword ptr [?]
							put1byte(0x89);                  //mov dword ptr [esp+l], eax
							if      (!l)      { put2byte(0x2404); }
							else if (l < 128) { put2byte(0x2444); put1byte(l); }
							else              { put2byte(0x2484); put4byte(l); }
							l += 4;
						}
						//printf("gasm[%2d].r[%d],%c,.r=%x+%2d,.q=%d,.nv=%d\n",i,j,tbuf[j],((unsigned)tr.r)>>28,tr.r&0x0fffffff,tr.q,tr.nv);
					}

						//push FP stack to ESP stack
					for(j=0;j<regnum;j++)
					{
						l = (j<<3)+k; put1byte(0xdd); //fstp qword ptr [esp+l]
						if (l < 128) { put2byte(0x245c); put1byte(l); } else { put2byte(0x249c); put4byte(l); }
					}

					if ((newvar[gasm[i].g].r&0xf0000000) == KESP)
					{
						tr.r = newvar[gasm[i].g].r + (regnum<<3)+k+4;
						putsib(0x8b,0x00,tr);               //mov eax, [esp+?] //load passed user function address
					}
					else
					{
						put1byte(0xb8);
						if (putwrite)
						{
							checkpatch(patchnum+1);
							patch[patchnum].lptr = (long *)&compcode[kasm87leng];
							patch[patchnum].ind = (newvar[gasm[i].g].r&0x0fffffff) + KIMM;
							patchnum++;
						}
						put4byte(0); //mov eax, ? //load static user function address
					}
					put2byte(0xd0ff);                      //call eax

						//pop FP stack from ESP stack
					for(j=regnum-1;j>=0;j--)
					{
						l = (j<<3)+k; put1byte(0xdd); //fld qword ptr [esp+l]
						if (l < 128) { put2byte(0x2444); put1byte(l); } else { put2byte(0x2484); put4byte(l); }
					}
					if (regnum) //move return value to top of stack (damn this is annoying! :/)
					{
						put2byte(0xc0d9+(regnum<<8));       //fld st(regnum)
						put2byte(0xc0dd+((regnum+1)<<8));   //ffree st(regnum+1)
					}

					l = -((regnum<<3)+k);                  //sub esp, l
					if (l >= -128) { put2byte(0xec83); put1byte(l); } else { put2byte(0xec81); put4byte(l); }

					put1byte(0x5a);                        //pop edx
					break;
			}
			if ((gasm[i].f != NUL) && (gasm[i].r[0].r != KUNUSED))
				putsib(0xdd,0x19,gasm[i].r[0]);       //fstp qword ptr [?]
		}

			//NOTE:Can't skip return if last line is GOTO: would cause jumpback hack to roll into UD1!
		if (gasm[gecnt-1].f != RETURN)
		{
			if (((gasm[gecnt-1].r[0].r&0xf0000000) != KIMM) &&
				 ((gasm[gecnt-1].r[0].r&0xf0000000) != KGLB) &&
				 ((gasm[gecnt-1].r[0].r&0xf0000000) != KEDX) &&
				 ((gasm[gecnt-1].r[0].r&0xf0000000) != KPTR) && (gasm[gecnt-1].r[0].r != KUNUSED) &&
				  (gasm[gecnt-1].f != NUL))
			{
				long j = (putlen(gasm[gecnt-1].r[0])+1);
				if ((patchnum > 0) && (patch[patchnum-1].lptr >= (long *)&compcode[kasm87leng-j]) &&
						  (putwrite) && (patch[patchnum-1].lptr <= (long *)&compcode[kasm87leng-4])) patchnum--;
				kasm87leng -= j; //remove previous "fstp qword ptr [?]"
			}
			else
				put2byte(0xeed9); //fldz (add fake return value if last line isn't expression)

				//Ensure stack is empty (except for return value which is st(0))
				//WARNING: fcompp trick only works properly if BOTH regs are known to be valid (not empty)
			//for(j=regnum;j>0;j--) put2byte(0xd9dd); //fstp st(1)
			for(j=regnum;j>0;j--) /*if (fpustat&(1<<(j-1)))*/ put2byte(0xc0dd+(j<<8)); //ffree st(j) (ffree faster on P4: less dependency?)

			if (memnum)
			{
				if ((memnum<<3)+4 <= 128) { put2byte(0xec83); put1byte(-((memnum<<3)+4)); } //sub esp, -((memnum<<3)+4)
											else { put2byte(0xec81); put4byte(-((memnum<<3)+4)); } //sub esp, -((memnum<<3)+4)
			}
			put1byte(0xc3);   //ret
		}
		put2byte(0x0b0f); //UD1 (undefined opcode): Prevents fetcher from crashing or getting very slow!
		while (kasm87leng&15) put1byte(0x90); //ALIGN 16 for code blocks

		if (putwrite)
		{
				//Code block:
			*(long *)&compcode[ 0-FUNCBYTEOFFS] = 0;
			*(long *)&compcode[ 4-FUNCBYTEOFFS] = kasm87leng;

				//Data block:
			*(long *)&compcode[ 8-FUNCBYTEOFFS] = kasm87leng;
			*(long *)&compcode[12-FUNCBYTEOFFS] = gccnt*8 + gstnum + arrnum+(arrnum&7);
		}

		if ((gccnt) || (gstnum) || (arrnum)) //transplant constant table to after code
		{
			for(i=0;i<gccnt;i++) { if (putwrite) *(double *)&compcode[kasm87leng] = globval[i]; kasm87leng += 8; }

				//Copy string tables
			for(i=0;i<gstnum;i++) put1byte(gstring[i]);

				//Fill static variables&arrays with assigned values or 0's
			if (!putwrite) kasm87leng += arrnum;
			else
			{
				for(i=j=0;i<arrnum;i+=8,kasm87leng+=8)
				{
					if ((j < ginitvalnum) && (i == ginitval[j].i))
						{ *(double *)&compcode[kasm87leng] = ginitval[j].v; j++; }
					else *(double *)&compcode[kasm87leng] = 0;
				}
				for(i=arrnum&7;i>0;i--) put1byte(0); //Align 8 for DATA blocks (shouldn't happen unless I implement non-double types)
			}
		}

		if (!putwrite)
		{
				//Note: jumpback table is copied after returning to kasm87()
				//Reserve the space in case it's a single function script
			compcode = (unsigned char *)malloc(FUNCBYTEOFFS + kasm87leng + ljumpbacknum*sizeof(jumpback_t));
			if (!compcode) { strcpy(kasm87err,"ERROR: malloc failed"); return(0); }
			compcode += FUNCBYTEOFFS;
		}
		else
		{
				//Fix jump table addresses
			for(i=0;i<jumpatnum;i++)
				(*(long *)&compcode[jumpat[i]-4]) = labpat[*(long *)&compcode[jumpat[i]-4]]-jumpat[i];
		}
	}

	return(compcode);
#endif
}


	//hello      how    are
	//111111000001111000111
	//hello how are
static void setecurs (long i0, long i1)
{
	long i;
	for(i=0;(i0 > 0) && (i < texttransn);i++) { if (texttrans[i>>5]&(1<<i)) i0--; } kasm87err0 = i; //WARNING:Uses 32-bit x86 shift trick
	for(i=0;(i1 > 0) && (i < texttransn);i++) { if (texttrans[i>>5]&(1<<i)) i1--; } kasm87err1 = i; //WARNING:Uses 32-bit x86 shift trick
}

void *kasm87 (char *bakz)
{
	long i, j, k, l, z, oi, got, pcnt, bcnt, scnt, whitespc, funcnt, inquotes, funptr, *lptr;
	long codebytes, databytes, ogevalextnum, globenumcharplc, globenumnum, globnewvarplc, globnewvarnum;
	evalextyp *ogevalext;
	char ch, *tbuf, *tbufmal, *cptr;
	void *v;

	if (!cpuinited)
	{
		cpuinited = 1; cputype = getcputype();
		for(i=0;i<2048;i++)
		{
				  if (i < 1044) round0msk[i][0] = 0;
			else if (i >=1075) round0msk[i][0] = -1;
			else               round0msk[i][0] = -(1<<(1075-i));

				  if (i < 1023) round0msk[i][1] = 0;
			else if (i >=1043) round0msk[i][1] = -1;
			else               round0msk[i][1] = (-(1<<(1043-i)))|0xfff00000;
		}
	}

	kasm87err0 = -1; kasm87err1 = -1;

	i = strlen(bakz)+1+2;
	texttransn = (((i+31)>>5)<<2); //# bytes for texttrans bit buf, aligned to 32-bits
	if (texttransn > texttransmal)
	{
		if (texttrans) free((void *)texttrans);
		texttransmal = max(max(texttransn,1024),texttransmal<<1);
		texttrans = (long *)malloc(texttransmal);
	}
	if (!texttrans) { strcpy(kasm87err,"ERROR: malloc failed"); return(0); }
	memset(texttrans,0,texttransn);

	tbufmal = (char *)malloc(i); //2 more to store "()" if necessary
	if (!tbufmal) { strcpy(kasm87err,"ERROR: malloc failed"); return(0); }
	tbufmal[0] = '('; tbufmal[1] = ')'; tbuf = tbufmal+2;

		//Compact white space, remove comments, handle quotes & char constants
	l = 0; got = 0; whitespc = 0; inquotes = 0;
	for(i=0;bakz[i];i++)
	{
		if (!inquotes)
		{
			if ((bakz[i] == 32) || (bakz[i] == 9)) { whitespc = 1; continue; } //strip Space/Tab
			if ((bakz[i] == 10) || (bakz[i] == 13)) { whitespc = 1; if (got == 1) got = 0; continue; } //strip CR,LF
			if ((bakz[i] == '/') && (bakz[i+1] == '/') && (!got)) got = 1;
			if ((bakz[i] == '/') && (bakz[i+1] == '*') && (!got)) { whitespc = 1; got = 2; }
			if ((bakz[i] == '*') && (bakz[i+1] == '/') && (got == 2)) { got = 0; i++; continue; }
			if (got) continue;

			if (whitespc)
			{
				tbuf[l++] = ' '; whitespc = 0;
				texttrans[(i-1)>>5] |= (1<<(i-1)); //WARNING:Uses 32-bit x86 shift trick
			}

				//Skip "#opt(..)"
			if ((bakz[i] == '#') && (bakz[i+1] == 'o') && (bakz[i+2] == 'p') && (bakz[i+3] == 't') && (bakz[i+4] == '('))
				{ for(i+=5;(bakz[i]) && (bakz[i] != ')');i++); continue; }

			ch = bakz[i]; if ((ch >= 'a') && (ch <= 'z')) ch -= 32;
		} else ch = bakz[i];

		tbuf[l++] = ch;
		texttrans[i>>5] |= (1<<i); //WARNING:Uses 32-bit x86 shift trick

		if ((ch == '\"') && ((l < 2) || (tbuf[l-2] != '\\'))) inquotes ^= 1;
		if (inquotes) continue;

		if (ch == '\'') //Convert character numbers: ' ', '$', 'a', etc...
		{
			if ((bakz[i+1] < 32) || (bakz[i+2] != '\'')) { sprintf(kasm87err,"ERROR: ' used wrong"); kasm87err0 = i; kasm87err1 = i+2; free(tbufmal); return(0); }
			j = sprintf(&tbuf[l-1],"%d",bakz[i+1]);
			switch (j)
			{
				case 3: texttrans[(i+2)>>5] |= (1<<(i+2)); //no break intentinoal //WARNING:Uses 32-bit x86 shift trick
				case 2: texttrans[(i+1)>>5] |= (1<<(i+1)); //no break intentinoal //WARNING:Uses 32-bit x86 shift trick
				case 1: break;
			}
			l += j-1; i += 2; continue;
		}
	}
	tbuf[l] = 0;

		//Split function definitions & global sections. Algo:
		// * find first '(' not inside "", {}, []
		// * extract function name by searching backwards like this:  !isvarchar  whitespc  name  whitespc  (
		// * if char before name isn't ';', '}' or start of buffer, then:
		//         Whole script is treated as single function; add "()" at beginning.
		//      Note: This case only valid when finding the first function. If detected later, report an error.
		// * else if name is blank, then it becomes the main function: everything before it is treated as global.
	pcnt = 0; scnt = 0; bcnt = 0; funcnt = 0; inquotes = 0; oi = 0; got = 0;
	globi = 0; if (!maxfuncst) { checkfuncst(0); if (globi < 0) { free(tbufmal); return(0); } }
	for(i=0;i<l;i++)
	{
		ch = tbuf[i];
		if ((ch == '\"') && (!((i) && (tbuf[i-1] == '\\')))) inquotes ^= 1;
		if (inquotes) continue;
		if (ch == '{') { bcnt++; continue; }
		if (ch == '}') { bcnt--; if (bcnt < 0) { strcpy(kasm87err,"ERROR: too many }"); /*setecurs(i-1,i+1);*/ free(tbufmal); return(0); }
										 if (pcnt    ) { strcpy(kasm87err,"ERROR: missing )");  /*setecurs(i-1,i+1);*/ free(tbufmal); return(0); }
							  continue; }
		if (ch == '[') { scnt++; continue; }
		if (ch == ']') { scnt--; if (scnt < 0) { strcpy(kasm87err,"ERROR: too many ]"); /*setecurs(i-1,i+1);*/ free(tbufmal); return(0); }
							  continue; }
		if ((ch == '(') && (!bcnt) && (!scnt)) //Found first '(' not inside "", {}, []
		{
			checkfuncst((funcnt+2)*4); if (globi < 0) { /*setecurs(i-1,i+1);*/ free(tbufmal); return(0); }

				//check if it's a function. Extract function name by searching backwards like this:  !isvarchar  whitespc  name  whitespc  (
			k = i;
			if ((k > oi) && (tbuf[k-1] == ' ')) k--; //skip whitespc
			for(j=k;(k) && (isvarchar(tbuf[k-1]));k--); //find string: &tbuf[k<=?<j]

				//insert global section
			if (oi < k-1) { funcst[funcnt*4+0] = -1; funcst[funcnt*4+1] = 0; funcst[funcnt*4+2] = oi; funcst[funcnt*4+3] = k; funcnt++; }

			if (j == k) { funcst[funcnt*4+0] = i; } //no name; ltrim space
					 else { funcst[funcnt*4+0] = k; if (!got) break;/*Hack for simple cases like "cos(3)+4"*/ }
			funcst[funcnt*4+1] = i;

			if ((k > oi) && (tbuf[k-1] == ' ')) k--; //m = char before string (skipping whitespc)
			if ((k > oi) && (tbuf[k-1] != ';') && (tbuf[k-1] != '}'))
			{
				if (got) { strcpy(kasm87err,"ERROR: global definition missing ; or }"); /*setecurs(i-1,i+1);*/ free(tbufmal); return(0); }
				break; //Simple script
			}

			got = 1;

				//Find ')' (end of function parameter list)
			for(pcnt=1,i++;i<l;i++)
			{
				if (tbuf[i] == '(') { pcnt++; continue; }
				if (tbuf[i] == ')') { pcnt--; if (!pcnt) { funcst[funcnt*4+2] = i+1; break; } }
			}
			if (i >= l) { strcpy(kasm87err,"ERROR: function definition missing )"); /*setecurs(i-1,i+1);*/ free(tbufmal); return(0); }

				//Find '}' (end of function body)
			i++; if (tbuf[i] == 32) i++;
			if (tbuf[i] != '{') { funcst[funcnt*4+3] = l; funcnt++; oi = l; break; } //Hack to support a single function without {} around body
			for(bcnt=1,i++;i<l;i++)
			{
				if (tbuf[i] == '{') { bcnt++; continue; }
				if (tbuf[i] == '}') { bcnt--; if (!bcnt) { funcst[funcnt*4+3] = i+1; oi = i+1; funcnt++; break; } }
			}
			if (i >= l) { strcpy(kasm87err,"ERROR: function missing } at end"); /*setecurs(k,i);*/ free(tbufmal); return(0); }
		}
	}
		//Simple script
	if (!got) { tbuf -= 2; l += 2; funcst[0*4+0] = 0; funcst[0*4+1] = 0; funcst[0*4+2] = 2; funcst[0*4+3] = l; funcnt = 1; }
	else
	{
			//insert global section
		if (oi < l-1) { funcst[funcnt*4+0] = -1; funcst[funcnt*4+1] = 0; funcst[funcnt*4+2] = oi; funcst[funcnt*4+3] = l; funcnt++; }
	}

	ogevalext = gevalext; ogevalextnum = gevalextnum;
	gevalext = (evalextyp *)malloc((gevalextnum+funcnt)*sizeof(evalextyp));
	if (!gevalext) { strcpy(kasm87err,"ERROR: malloc failed"); free(tbufmal); return(0); }

#if (COMPILE != 0)
	patchnum = 0;
#endif

#if 0
		//funcst[?*4+{0...1....2......3}]
		//           |func(x,y){x^2+y}|
		//           |func(x,y)|
		//               |(x,y){x^2+y}|
	for(i=0;i<funcnt;i++)
	{
		if (funcst[i*4+0] < 0)
		{
			ch = tbuf[funcst[i*4+3]]; tbuf[funcst[i*4+3]] = 0; printf("FUNC%d:glob parse sees:|%s|\n",i,&tbuf[funcst[i*4+2]]); tbuf[funcst[i*4+3]] = ch;
		}
		else
		{
			ch = tbuf[funcst[i*4+2]]; tbuf[funcst[i*4+2]] = 0; printf("FUNC%d: gevalext proto:|%s|\n",i,&tbuf[funcst[i*4+0]]); tbuf[funcst[i*4+2]] = ch;
			ch = tbuf[funcst[i*4+3]]; tbuf[funcst[i*4+3]] = 0; printf("FUNC%d:kasm87comp sees:|%s|\n",i,&tbuf[funcst[i*4+1]]); tbuf[funcst[i*4+3]] = ch;
		}
	}
#endif


	globi = 0; arrnum = 0;
	if (!maxops)
	{
		if (oprio[0] != 255) //Initialize operator precedence LUT
		{
			memset(oprio,255,sizeof(oprio));
			oprio[POW] = 0;
			oprio[TIMES] = oprio[SLASH] = oprio[PERC] = 1;
			oprio[PLUS] = oprio[MINUS] = 2;
			oprio[LES] = oprio[LESEQ] = oprio[MOR] = oprio[MOREQ] = 3;
			oprio[EQU] = oprio[NEQU] = 4;
			oprio[LAND] = 5;
			oprio[LOR] = 6;
		}

		checkops(0); checkstrings(0); checkinitvals(0); checkrxi(0);
		checkenum(0); checkenumchars(0);
		checkvars(0); checkvarchars(0);
		checklabs(0); checklabchars(0);
		checkjumpbacks(0);
#if (COMPILE != 0)
		checkpatch(0);
#endif
		if (globi < 0)
		{
			free(gevalext); gevalext = ogevalext; gevalextnum = ogevalextnum; free(tbufmal);
			strcpy(kasm87err,"ERROR: malloc failed"); return(0);
		}
	}

		//Init for global parse_static() -> parse_dimensions() -> kasmoptimizations()
	gecnt = 0; gccnt = 0; gstnum = 0; ginitvalnum = 0;

		//Init for parse_static()
	memset(newvarhash,-1,sizeof(newvarhash));
	newvarnam[0] = 0; newvarplc = 0; newvarnum = 0;

		//parse global enum&static declarations
	enumcharplc = 0; enumnum = 0;
	for(i=j=0;i<funcnt;i++)
	{
		if (funcst[i*4+0] < 0)
		{
			ch = tbuf[funcst[i*4+3]]; tbuf[funcst[i*4+3]] = 0;
			for(z=funcst[i*4+2];tbuf[z];z++)
			{
					  if ((!strncmp(&tbuf[z],"ENUM"  ,4)) && (!isvarchar(tbuf[z+4]))) z = parse_enum(tbuf,z);
				else if ((!strncmp(&tbuf[z],"STATIC",6)) && (!isvarchar(tbuf[z+6]))) z = parse_static(tbuf,z);
				else continue;
				if (globi < 0) { free(gevalext); gevalext = ogevalext; gevalextnum = ogevalextnum; free(tbufmal); return(0); }
				if (!tbuf[z]) break;
			}
			tbuf[funcst[i*4+3]] = ch;
		}
		else
		{
			funcst[j*4+0] = funcst[i*4+0];
			funcst[j*4+1] = funcst[i*4+1];
			funcst[j*4+2] = funcst[i*4+2];
			funcst[j*4+3] = funcst[i*4+3];
			j++;
		}
	}
	funcnt = j;
	globenumnum = enumnum; globenumcharplc = enumcharplc;
	memcpy(newvarhash_glob,newvarhash,sizeof(newvarhash));
	globnewvarplc = newvarplc; globnewvarnum = newvarnum;
	if (arrnum) //allocate gstatmem block & copy ginitval to it before kasm87comp destroys ginitval
	{
		gstatmem = (long)malloc(arrnum);
		if (!gstatmem)
		{
			free(gevalext); gevalext = ogevalext; gevalextnum = ogevalextnum; free(tbufmal);
			strcpy(kasm87err,"ERROR: malloc failed"); return(0);
		}

			//Fill gstatmem with assigned values or 0's
		k = (long)gstatmem;
		for(i=j=0;i<arrnum;i+=8,k+=8)
		{
			if ((j < ginitvalnum) && (i == ginitval[j].i))
				{ *(double *)k = ginitval[j].v; j++; }
			else *(double *)k = 0;
		}
	} else gstatmem = 0;
	for(i=0;i<globnewvarnum;i++)
		if ((newvar[i].r&0xf0000000) == KARR)
			newvar[i].r = (newvar[i].r&0x0fffffff)+KGLB;


	jumpbacknum = 0;
	memcpy(gevalext,ogevalext,ogevalextnum*sizeof(evalextyp));
	gevalextnum += funcnt-1;
		//Main can't be called since it doesn't have a name
	for(i=funcnt-1;i>0;i--)
	{
		j = gevalextnum-i;

		ch = tbuf[funcst[i*4+2]]; tbuf[funcst[i*4+2]] = 0;
		gevalext[j].nam = strdup(&tbuf[funcst[i*4+0]]);
		if (!gevalext[j].nam)
		{
			for(j--;j>=ogevalextnum;j--) free(gevalext[j].nam);
			if (gstatmem) { free((void *)gstatmem); gstatmem = 0; }
			free(gevalext); gevalext = ogevalext; gevalextnum = ogevalextnum;
			free(tbufmal); strcpy(kasm87err,"ERROR: strdup failed"); return(0);
		}
		//printf("ext_proto:|%s|\n",gevalext[j].nam);
		tbuf[funcst[i*4+2]] = ch;
	}

	codebytes = databytes = 0;
	for(i=funcnt-1;i>=0;i--)
	{
		j = gevalextnum-i;

		enumnum = globenumnum; enumcharplc = globenumcharplc;
		memcpy(newvarhash,newvarhash_glob,sizeof(newvarhash));
		newvarplc = globnewvarplc; newvarnum = globnewvarnum;

		tbuf[funcst[i*4+3]] = 0;
		//printf("compiling:|%s|\n",&tbuf[funcst[i*4+1]]);
		gevalext[j].ptr = (EVALFUNC)kasm87comp(&tbuf[funcst[i*4+1]]);

#if 0
		{ //DEBUG ONLY!
			char debuf[65536];
			extern char *kdisasm (char *, long, char *, long, long *);
			kasm87_showdebug(1,debuf,sizeof(debuf)); printf("\n\n%d:\n%s",i,debuf);
			debuf[0] = debuf[sizeof(debuf)-1] = 0;
			l = 0; kdisasm((char *)compcode,kasm87leng,debuf,sizeof(debuf)-1,&l);
			printf("\n%s",debuf);
		}
#endif

		if (!gevalext[j].ptr)
		{
			for(j--;j>=ogevalextnum;j--) free((void *)(((long)gevalext[j].ptr)-FUNCBYTEOFFS));
			for(j=gevalextnum-1;j>=ogevalextnum;j--) free(gevalext[j].nam);
			if (gstatmem) { free((void *)gstatmem); gstatmem = 0; }
			free(gevalext); gevalext = ogevalext; gevalextnum = ogevalextnum;
			free(tbufmal); return(0);
		}

		funptr = (long)gevalext[j].ptr; lptr = (long *)(funptr-FUNCBYTEOFFS);
		codebytes += lptr[1];
		databytes += lptr[3];
	}
	v = (void *)gevalext[gevalextnum].ptr;

#if (COMPILE != 0)

		//merge all functions together
	kasm87leng = codebytes + databytes; if (databytes) kasm87leng += CODEDATADIST;
	v = malloc(FUNCBYTEOFFS + kasm87leng + jumpbacknum*sizeof(jumpback_t));
	if (!v)
	{
		for(i=0;i<funcnt;i++) { j = gevalextnum-i; free((void *)(((long)gevalext[j].ptr)-FUNCBYTEOFFS)); }
		for(j=gevalextnum-1;j>=ogevalextnum;j--) free(gevalext[j].nam);
		if (gstatmem) { free((void *)gstatmem); gstatmem = 0; }
		free(gevalext); gevalext = ogevalext; gevalextnum = ogevalextnum;
		free(tbufmal); return(0);
	}
	v = (void *)(((long)v)+FUNCBYTEOFFS);
	cptr = (char *)v; l = patchnum-1; k = jumpbacknum-1;
	for(i=0;i<funcnt;i++)
	{
		j = gevalextnum-i; funptr = (long)gevalext[j].ptr; lptr = (long *)(funptr-FUNCBYTEOFFS); if (!lptr[1]) continue;

			//Relocate code
		memcpy(cptr,(void *)funptr,lptr[1]);
		lptr[0] = ((long)cptr); //lptr[0] now holds code pointer so data pointer can be plugged in later

			//Relocate patch pointers
		while ((l >= 0) && (((unsigned long)patch[l].lptr)-((unsigned long)funptr) < (unsigned long)lptr[1]))
			{ patch[l].lptr = (long *)(((long)patch[l].lptr) + ((long)cptr) - funptr); l--; }

			//Relocate jumpback list
		while ((k >= 0) && (((unsigned long)jumpback[k].addr)-((unsigned long)funptr) < (unsigned long)lptr[1]))
			{ jumpback[k].addr += ((long)cptr) - funptr; k--; }

		cptr += lptr[1];
	}
	if (databytes) { memset(cptr,0x90,CODEDATADIST); cptr += CODEDATADIST; }
	for(i=0;i<funcnt;i++)
	{
		j = gevalextnum-i; funptr = (long)gevalext[j].ptr; lptr = (long *)(funptr-FUNCBYTEOFFS); if (!lptr[3]) continue;

			//Relocate data
		memcpy(cptr,(void *)(funptr+lptr[2]),lptr[3]);

			//Relocate consts&array (KEDX)
		if (*(unsigned char *)(lptr[0]) == 0xba) *(long *)(lptr[0]+1) = ((long)cptr); //Note: condition should always be true

		cptr += lptr[3];
	}

	for(i=0;i<funcnt;i++)
	{
		j = gevalextnum-i; funptr = (long)gevalext[j].ptr; lptr = (long *)(funptr-FUNCBYTEOFFS);
		gevalext[j].ptr = (long *)lptr[0]; //Put the new function pointer on global list
		free((void *)lptr);                //..and free the original function which was just copied
	}

	for(j=patchnum-1;j>=0;j--)
	{
		if ((patch[j].ind&0xf0000000) == KIMM) patch[j].lptr[0] += ((long)gevalext[patch[j].ind&0x0fffffff].ptr);
													 else patch[j].lptr[0] += gstatmem + (patch[j].ind&0x0fffffff);
	}

	for(j=jumpbacknum-1;j>=0;j--) jumpback[j].val = *(long *)(jumpback[j].addr); //backup original jumpback values
	compcode = (unsigned char *)v; //make sure kasm87_showdebug has a valid pointer
#else
	??? not implemented .. need to fix .. sorry :/
#endif

		//Copy jumpback table to script's malloced array so kasm87jumpback() can support multiple scripts
	*(long *)(((long)v)-FUNCBYTEOFFS) = jumpbacknum;
	*(long *)(((long)v)-FUNCBYTEOFFS+4) = kasm87leng;
	*(long *)(((long)v)-FUNCBYTEOFFS+8) = gstatmem;
	memcpy((void *)(((long)v)+kasm87leng),jumpback,jumpbacknum*sizeof(jumpback_t));

	for(j=gevalextnum-1;j>=ogevalextnum;j--) free(gevalext[j].nam);
	free(gevalext); gevalext = ogevalext; gevalextnum = ogevalextnum;
	free(tbufmal);

#if _WIN32
	VirtualProtect((void *)(((long)v)-FUNCBYTEOFFS),kasm87leng+FUNCBYTEOFFS,0x40/*PAGE_EXECUTE_READWRITE*/,(unsigned long *)&i);
	//FlushInstructionCache(GetCurrentProcess(),((long)v)-FUNCBYTEOFFS,kasm87leng+FUNCBYTEOFFS);
#endif

	return(v);
}