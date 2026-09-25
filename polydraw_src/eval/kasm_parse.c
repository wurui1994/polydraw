
	//Helper function to compare 2 parameters on gasm
static long gasmeq (rtyp g0, rtyp g1)
{
	return((g0.r == g1.r) && (g0.q == g1.q));
}


static long skipparen (char *st, long z)
{
	long p = 0, inquotes = 0;
	for(;1;z++)
	{
		if (!st[z]) return(-1);
		if ((st[z] == '\"') && ((!z) || (st[z-1] != '\\'))) inquotes ^= 1;
		if (inquotes) continue;
		if (st[z] == '(') { p++; continue; }
		if (st[z] == ')') { p--; if (!p) break; }
	}
	return(z+1); //Skip ')'
}

	//findscope(): parses dangling else`s and other weird syntax missing {} properly
	// st: string pointer base
	//  z: string start index
	// z0: index to scope start
	// z1: index to scope end (where to write NULL terminator)
	//returns:index to 1st char of next statement (skips `}` when applicable)
static long findscope (char *st, long z, long *z0, long *z1)
{
	long p, ifcnt, zx0, zx1;

	if (st[z] == '{')
	{
		z++; (*z0) = z; p = 1;
		for(;1;z++)
		{
			if (!st[z]) return(-1);
			if (st[z] == '{') { p++; continue; }
			if (st[z] == '}') { p--; if (!p) break; }
		}
		(*z1) = z;
		z++; //Skip '}'
	}
	else
	{
		(*z0) = z;
		if ((!strncmp(&st[z],"IF",2)) && (!isvarchar(st[z+2])))
		{
			if ((z = skipparen(st,z+2)) < 0) return(-1);
			if ((z = findscope(st,z,&zx0,&zx1)) < 0) return(-1);
			if ((!strncmp(&st[z],"ELSE",4)) && (!isvarchar(st[z+4])))
			{
				z += 4; if (st[z] == ' ') z++;
				if ((z = findscope(st,z,&zx0,&zx1)) < 0) return(-1);
			}
		}
		else if ((!strncmp(&st[z],"DO",2)) && (!isvarchar(st[z+2])))
		{
			if ((z = findscope(st,z+2,&zx0,&zx1)) < 0) return(-1);
			if ((!strncmp(&st[z],"WHILE",5)) && (!isvarchar(st[z+5])))
			{
				if ((z = skipparen(st,z+5)) < 0) return(-1);
				if (st[z] == ';') z++;
			} else return(-1);
		}
		else if ((!strncmp(&st[z],"WHILE",5)) && (!isvarchar(st[z+5])))
		{
			if ((z = skipparen(st,z+5)) < 0) return(-1);
			if ((z = findscope(st,z,&zx0,&zx1)) < 0) return(-1);
		}
		else if ((!strncmp(&st[z],"FOR",3)) && (!isvarchar(st[z+3])))
		{
			if ((z = skipparen(st,z+3)) < 0) return(-1);
			if ((z = findscope(st,z,&zx0,&zx1)) < 0) return(-1);
		}
		else
		{
			for(;1;z++)
			{
				if (!st[z]) return(-1);
				if (st[z] == ';') { z++; break; }
			}
		}
		(*z1) = z;
	}
	return(z);
}

void kasm87addext (evalextyp *daeet, long n) { gevalext = daeet; gevalextnum = n; }

	//NOTE: newvar must be written and 0-terminated first before calling this!
	//Writes each dimension to newvar[newvarnum]; returns total array size (error=0), and new z in daz
static void parsefunc (char *, long, long);
static long kasmoptimizations (long, long);
static long parse_dimensions (char *st, long *daz, long writenewvar)
{
	long i, j, k, p, z, arrind = 1;
	char *cptr;

	z = j = (*daz);
	if (st[j] == ' ') j++;
	while (st[j] == '[')
	{
		j++;
		for(z=j;st[z] != ']';z++)
			if (!st[z]) { strcpy(kasm87err,"ERROR: missing ]"); return(0); }

#if 0
		i = -1; //First see if array index is an "enum" style name...
		for(k=0,cptr=enumnam;k<enumnum;k++,cptr=&cptr[p+1])
		{
			for(p=1;cptr[p];p++);
			if ((z-j == p) && (!strncmp(&st[j],cptr,p)))
				{ i = (long)enumval[k]; break; }
		}
		if (i < 0) { i = strtol(&st[j],(char **)&z,10); z -= (long)st; } //If no, get number
#else
			 //This version handles expressions.. left old case for nostalgia?
		{
		long oglobi, ogecnt;
		char *nst;
		oglobi = globi; ogecnt = gecnt;

		nst = (char *)malloc(z-j+1); if (!nst) { strcpy(kasm87err,"ERROR: malloc failed"); return(0); }
		memcpy(nst,&st[j],z-j); nst[z-j] = 0; parsefunc(nst,-1,-1); free(nst);
		if (globi == -1) return(0);

		kasmoptimizations(ogecnt,1);

		if ((gecnt-ogecnt != 1) || (gasm[ogecnt].f != MOV) || ((gasm[ogecnt].r[1].r&0xf0000000) != KEDX))
			{ st[z] = 0; sprintf(kasm87err,"ERROR: could not simplify dimension (%s) to constant",&st[j]); st[z] = ']'; return(0); }
		i = ((long)ceil(globval[(gasm[ogecnt].r[1].r&0x0fffffff)>>3]));

		globi = oglobi; gecnt = ogecnt; gnext[oglobi] = 0x7fffffff;
		}
#endif
		if (i <= 0) { strcpy(kasm87err,"ERROR: invalid dimensions"); return(0); }
		z++; //skip ']'

		if (writenewvar)
		{
			checkvarchars(newvarplc+4);
			*(long *)&newvarnam[newvarplc] = i; newvarplc += 4; //i is size of array dimension
			newvar[newvarnum].parnum++;
		}

		arrind *= i;

		if (st[z] == ' ') z++;
		j = z;
	}
	(*daz) = z;
	return(arrind);
}

static long parse_enum (char *st, long z)
{
	double d;
	long k, p;

	z += 4;
	if (st[z] == ' ') z++;
	if (st[z] == '{')
	{
		d = 0.0; k = 0; z++; p = 1;
		for(;st[z];z++)
		{
			if (st[z] == ' ') z++;
			if (st[z] == '=')
			{
				z++;
#if 0
				d = strtod(&st[z],(char **)&z); z -= (long)st;
#else
					 //This version handles expressions.. left old case for nostalgia?
				{
				long oglobi, ogecnt, np;
				char *nst;
				oglobi = globi; ogecnt = gecnt;

				k = z;
				np = 0;
				do
				{
					if (!st[z]) { strcpy(kasm87err,"ERROR: static init missing , or ;"); return(-1); }
					if (st[z] == '{') { np++; }
					if (st[z] == '}') { np--; if (np < 0) break; }
					if (((st[z] == ';') || (st[z] == ',')) && (np <= 0)) break;
					z++;
				} while (1);

				nst = (char *)malloc(z-k+1); if (!nst) { strcpy(kasm87err,"ERROR: malloc failed"); return(-1); }
				memcpy(nst,&st[k],z-k); nst[z-k] = 0; parsefunc(nst,-1,-1); free(nst);
				if (globi == -1) return(-1);

				kasmoptimizations(ogecnt,1);

				if ((gecnt-ogecnt != 1) || (gasm[ogecnt].f != MOV) || ((gasm[ogecnt].r[1].r&0xf0000000) != KEDX))
					{ st[z] = 0; sprintf(kasm87err,"ERROR: could not simplify enum init (%s) to constant",&st[k]); st[z] = ']'; return(-1); }
				d = globval[(gasm[ogecnt].r[1].r&0x0fffffff)>>3];

				globi = oglobi; gecnt = ogecnt; gnext[oglobi] = 0x7fffffff;
				}
#endif
				k = 1;
			}
			if (st[z] == ' ') z++;
			if ((st[z] == ',') || (st[z] == '}'))
			{
				checkenumchars(enumcharplc+1); enumnam[enumcharplc++] = 0;
				checkenum(enumnum+1); enumval[enumnum++] = d; d++;
				k = 0;
			}
			else
			{
				if (k) { strcpy(kasm87err,"ERROR: ENUM syntax incorrect"); globi = -1; return(-1); }
				checkenumchars(enumcharplc+1); enumnam[enumcharplc++] = st[z];
			}
			if (st[z] == '{') { p++; continue; }
			if (st[z] == '}') { p--; if (!p) break; }
		}
		if (st[z] != '}') { strcpy(kasm87err,"ERROR: missing }"); globi = -1; return(-1); }
		z++; //Skip '}'
	}

	return(z);
}

	//writes: newvar,newvarhash,newvarnam,newvarnum,newvarplc
	//writes: ginitval,ginitvalnum
static long parse_static (char *st, long z)
{
	long i, j, k, p, arrind;
	char ch, *cptr;

	z += 7;
	do
	{
		if (st[z] == ' ') z++;
		if ((st[z] >= '0') && (st[z] <= '9'))
			{ strcpy(kasm87err,"ERROR: bad static init syntax"); globi = -1; return(-1); }

		cptr = &st[z];
		for(j=z;isvarchar(st[j]);j++);
		if (j == z) { strcpy(kasm87err,"ERROR: static missing variable"); globi = -1; return(-1); }

		ch = st[j]; st[j] = 0;
		for(i=newvarhash[getnewvarhash(cptr)];i>=0;i=newvar[i].hashn)
			if (!strcmp(cptr,&newvarnam[newvar[i].nami]))
				{ sprintf(kasm87err,"ERROR: %s already defined",cptr); st[j] = ch; globi = -1; return(-1); }
		st[j] = ch;


		checkvarchars(newvarplc+j-z+2);
		checkvars(newvarnum+1);
		newvar[newvarnum].nami = newvarplc;
			//Save new variable name&index to list
		//if (st[z] == '\"') { memcpy(&newvarnam[newvarplc],&st[z+1],j-z-2); newvarplc += j-z-2; }
		//              else
						  { memcpy(&newvarnam[newvarplc],&st[z  ],j-z  ); newvarplc += j-z  ; }
		newvarnam[newvarplc++] = 0;

		newvar[newvarnum].proti = newvarplc;
		newvar[newvarnum].parnum = 0;

		z = j; arrind = parse_dimensions(st,&z,1);
		if (!arrind) { globi = -1; return(-1); }

		if (st[z] == ' ') z++;
		if (st[z] == '=') //Parse static initializers
		{
			z++; j = 0; p = 0;
			do
			{
				while ((st[z] == '{') || (st[z] == ' ')) { p += (st[z] == '{'); z++; }

				checkinitvals(ginitvalnum+1);
				ginitval[ginitvalnum].i = arrnum+j;

				k = z;
#if 0
				if ((st[z] == '0') && (st[z+1] == 'X')) //Handle HEX numbers
				{
					ginitval[ginitvalnum].v = strtoul(&st[z],(char **)&z,0);
					if (ginitval[ginitvalnum].v >= 2147483648.0) ginitval[ginitvalnum].v -= 4294967296.0;
				} else { ginitval[ginitvalnum].v = strtod(&st[z],(char **)&z); }
				z -= (long)st;
#else
					 //This version handles expressions.. left old case for nostalgia?
				{
				long oglobi, ogecnt, np;
				char *nst;
				oglobi = globi; ogecnt = gecnt;

				np = 0;
				do
				{
					if (!st[z]) { strcpy(kasm87err,"ERROR: static init missing , or ;"); return(-1); }
					if (st[z] == '{') { np++; }
					if (st[z] == '}') { np--; if (np < 0) break; }
					if (((st[z] == ';') || (st[z] == ',')) && (np <= 0)) break;
					z++;
				} while (1);

				nst = (char *)malloc(z-k+1); if (!nst) { strcpy(kasm87err,"ERROR: malloc failed"); return(-1); }
				memcpy(nst,&st[k],z-k); nst[z-k] = 0; parsefunc(nst,-1,-1); free(nst);
				if (globi == -1) return(-1);

				kasmoptimizations(ogecnt,1);

				if ((gecnt-ogecnt != 1) || (gasm[ogecnt].f != MOV) || ((gasm[ogecnt].r[1].r&0xf0000000) != KEDX))
					{ st[z] = 0; sprintf(kasm87err,"ERROR: could not simplify static init (%s) to constant",&st[k]); st[z] = ']'; return(-1); }
				ginitval[ginitvalnum].v = globval[(gasm[ogecnt].r[1].r&0x0fffffff)>>3];

				globi = oglobi; gecnt = ogecnt; gnext[oglobi] = 0x7fffffff;
				}
#endif
				if ((j >= (arrind<<3)) && (z > k))
					{ sprintf(kasm87err,"ERROR: too many initializers for %s",&newvarnam[newvar[newvarnum].nami]); globi = -1; return(-1); }
				if (ginitval[ginitvalnum].v != 0.0) ginitvalnum++;
				j += 8;

				while ((st[z] == '}') || (st[z] == ' ')) { p -= (st[z] == '}'); z++; if (!p) break; }
				if (!p) break;

				if (st[z] != ',') { strcpy(kasm87err,"ERROR: bad static init syntax"); globi = -1; return(-1); }
				z++;
			} while (1);
		}

		newvar[newvarnum].r = arrnum+KARR; arrnum += (arrind<<3);
		newvar[newvarnum].maxind = arrind;
		newvar[newvarnum].parnum ^= -1; //1's complement
		k = getnewvarhash(&newvarnam[newvar[newvarnum].nami]);
		newvar[newvarnum].hashn = newvarhash[k]; newvarhash[k] = newvarnum;
		newvarnum++;

		if (st[z] == ' ') z++;
		if ((st[z] != ';') && (st[z] != ',')) { strcpy(kasm87err,"ERROR: bad static init syntax"); globi = -1; return(-1); }
		z++; //skip ';' or ','
	} while (st[z-1] == ',');
	return(z);
}

static void parsefunc (char *st, long breaklab, long contlab)
{
	rtyp *rp;
	gasmtyp tg;
	long i, j, k, z, oz, p, fmode, newvari, fparm, ogi, parm, negit, arrind, inquotes, isaddr;
	char ch, ch2, *cptr;

	//printf("|%s|\n",st); //Enable to debug expression splitting

	if (!st[0]) st = "0";
	z = 0; fmode = NOP; newvari = -1; isaddr = 0;
prebegit:;
	ogi = globi;
	checkops(ogi+1); gnext[ogi] = 0x7fffffff; //write ending val to gnext for null expressions (Ex:GOTO)
begit:;
	if (!z)
	{
		if ((st[z] == '-') || (st[z] == '+')) //Hack for first '-' or '+' to fix priority of "-x^2"
		{
				//Insert '0' before '-' or '+' if sign is 1st char in expression
			checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
				gasm[gecnt].r[0].r = globi*8+KECX; gasm[gecnt].r[1].r = gccnt*8+KEDX; gasm[gecnt].r[2].r = KUNUSED; gasm[gecnt].n = 1; gasm[gecnt].f = MOV; gecnt++;
			checkops(gccnt+1); globval[gccnt++] = 0.0;
			checkops(globi+2); gnext[globi] = globi+1; globi++; gop[globi] = NUL;
		}
		negit = 1;
	}
	else
	{
			//Hack to make "2^(+3)" not translate as: "2+3"
			//handle unary '-' operator. 0:nothing special, 1:negate next node!
		for(negit=1;(st[z] == '+') || (st[z] == '-');z++) if (st[z] == '-') negit ^= ((+1)^(-1));
	}

	while (st[z])
	{
		oz = z;
		switch(st[z])
		{
			case '(':
				p = 1; z++; oz = z; parm = 1; i = globi; inquotes = 0;
				for(;st[z];z++)
				{
					if ((st[z] == '\"') && (st[z-1] != '\\')) inquotes ^= 1;
					if (inquotes) continue;
					if (st[z] == '(') { p++; continue; }
					if (st[z] == ')') { p--; if (!p) break; }
					if ((st[z] == ',') && (p == 1))
					{
						if (z == oz)
						{
								//Insert '0' if blank param
							checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
								gasm[gecnt].r[0].r = globi*8+KECX; gasm[gecnt].r[1].r = gccnt*8+KEDX; gasm[gecnt].r[2].r = KUNUSED; gasm[gecnt].n = 1; gasm[gecnt].f = MOV; gecnt++;
							checkops(gccnt+1); globval[gccnt++] = 0.0;
							checkops(globi+2); gnext[globi] = globi+1; globi++; gop[globi] = NUL;
						}
						else
						{
							ch = st[z]; st[z] = 0; parsefunc(&st[oz],breaklab,contlab); st[z] = ch; if (globi == -1) return;
						}
						parm++; oz = z+1;
					}
				}
				if (z == oz)
				{
						//Insert '0' if blank param
					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
						gasm[gecnt].r[0].r = globi*8+KECX; gasm[gecnt].r[1].r = gccnt*8+KEDX; gasm[gecnt].r[2].r = KUNUSED; gasm[gecnt].n = 1; gasm[gecnt].f = MOV; gecnt++;
					checkops(gccnt+1); globval[gccnt++] = 0.0;
					checkops(globi+2); gnext[globi] = globi+1; globi++; gop[globi] = NUL;
				}
				else
				{
					ch = st[z]; st[z] = 0; parsefunc(&st[oz],breaklab,contlab); st[z] = ch; if (globi == -1) return;
				}
				if (st[z] != ')') { strcpy(kasm87err,"ERROR: missing )"); globi = -1; return; }
				z++; //Skip ')'

				if ((fmode == LOG) && (parm == 2)) fmode = LOGB; //Choose function based on parm #

				fparm = -1;
					  if                       (fmode < PARAM1)  ;
				else if ((fmode >= PARAM1) && (fmode < PARAM2)) fparm = 1;
				else if ((fmode >= PARAM2) && (fmode < PARAM3)) fparm = 2;
				else if ((fmode >= PARAM3) && (fmode != USERFUNC)) ;
				else if (newvari >= 0)                          fparm = newvar[newvari].parnum;

					//Hack to support function overloading :)
				tg.g = newvari;
				if ((fmode == USERFUNC) && (newvari >= 0) && (parm != fparm))
				{
					if (newvarnam[newvar[newvari].proti+fparm-1] == 'e') fparm = parm;
					else
					{
						cptr = &newvarnam[newvar[newvari].nami];
						while ((parm != fparm) || (stricmp(cptr,&newvarnam[newvar[newvari].nami])))
						{
							newvari = newvar[newvari].hashn;
							if (newvari < 0) { fparm = -1; break; } //function with same # parms not found :/
							fparm = newvar[newvari].parnum;
						}
					}
				}
				if (parm != fparm)
				{
					strcpy(kasm87err,"ERROR: ");
					tg.f = fmode; getfuncnam(&tg,&kasm87err[strlen(kasm87err)]);
					sprintf(&kasm87err[strlen(kasm87err)]," doesn't take %d param",parm);
					if (parm != 1) strcat(kasm87err,"s");
					globi = -1; return;
				}

				if (fmode == USERFUNC)
				{
					long skip4ellipses = 0;
					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].r[0].r = i*8+KECX;
					if (fparm > 2) { checkrxi(numrxi+fparm-2); memset(&rxi[numrxi],0,sizeof(rxi[0])*(fparm-2)); }
					for(j=i,k=0;k<fparm;j=gnext[j],k++)
					{
						if (k < 2) rp = &gasm[gecnt].r[k+1]; else rp = &rxi[numrxi+k-2];
						if ((unsigned)j > (unsigned)globi)
						{
							strcpy(kasm87err,"ERROR: ");
							tg.f = fmode; getfuncnam(&tg,&kasm87err[strlen(kasm87err)]);
							sprintf(&kasm87err[strlen(kasm87err)]," param %d: pointer invalid",k+1);
							globi = -1; return;
						}
						rp->r = j*8+KECX;
						if (newvarnam[newvar[newvari].proti+k] == 'e') skip4ellipses = 1;
						if (skip4ellipses) continue;
						if (newvarnam[newvar[newvari].proti+k] >= 'a') continue;
						for(p=gecnt-1;p>=0;p--) //pointer params (double* or char*) must not be an expression
							if (gasm[p].r[0].r == rp->r)
							{
								if (gasm[p].f != MOV)
								{
									strcpy(kasm87err,"ERROR: ");
									tg.f = fmode; getfuncnam(&tg,&kasm87err[strlen(kasm87err)]);
									sprintf(&kasm87err[strlen(kasm87err)]," param %d: pointer invalid",k+1);
									globi = -1; return;
								}
								//if ((gasm[p].r[1].r&0xf0000000) == KEDX) break; //FIXFIXFIX
								(*rp) = gasm[p].r[1];
								break;
							}
					}
					if (fparm > 2) { gasm[gecnt].rxi = numrxi; numrxi += fparm-2; }
					gasm[gecnt].f = fmode;
					if (newvari >= 0) gasm[gecnt].g = newvari; else gasm[gecnt].g = 0;
					gasm[gecnt].n = fparm;
					gecnt++;
				}
				else if (fmode != NOP)
				{
					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].r[0].r = gasm[gecnt].r[1].r = i*8+KECX;
					if (fmode < PARAM2) { gasm[gecnt].n = 1; gasm[gecnt].r[2].r = KUNUSED;         }
										else { gasm[gecnt].n = 2; gasm[gecnt].r[2].r = gnext[i]*8+KECX; }
					gasm[gecnt].f = fmode;
					if (newvari >= 0) gasm[gecnt].g = newvari; else gasm[gecnt].g = 0;
					gecnt++;
				}

				gnext[i] = globi;
				if (negit < 0)
				{
					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].r[0].r = gasm[gecnt].r[1].r = i*8+KECX; gasm[gecnt].r[2].r = KUNUSED; gasm[gecnt].n = 1; gasm[gecnt].f = NEGMOV; gecnt++;
				}
				fmode = NOP;
				break;

				//Parse operators
			case '=': if (st[z+1] == '=') { gop[globi] = EQU; z += 2; goto begit; } break;
			case '!': if (st[z+1] == '=') { gop[globi] = NEQU; z += 2; goto begit; } break;
			case '&': if (st[z+1] == '&') { gop[globi] = LAND; z += 2; goto begit; }
						 if (isvarchar(st[z+1])) isaddr = 1; z++; break;
			case '|': if (st[z+1] == '|') { gop[globi] = LOR; z += 2; goto begit; } break;
			case '^': gop[globi] = POW; z++; goto begit;
			case '*': gop[globi] = TIMES; z++; goto begit;
			case '/': gop[globi] = SLASH; z++; goto begit;
			case '%': gop[globi] = PERC; z++; goto begit;
			case '+': gop[globi] = PLUS; z++; goto begit;
			case '-': gop[globi] = MINUS; z++; goto begit;
			case '<': if (st[z+1] == '=') { gop[globi] = LESEQ; z += 2; } else { gop[globi] = LES; z++; } goto begit;
			case '>': if (st[z+1] == '=') { gop[globi] = MOREQ; z += 2; } else { gop[globi] = MOR; z++; } goto begit;

				//Parse constants
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9': case '.':
				checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].r[0].r = globi*8+KECX; gasm[gecnt].r[1].r = gccnt*8+KEDX; gasm[gecnt].r[2].r = KUNUSED; gasm[gecnt].n = 1; gasm[gecnt].f = MOV; gecnt++;
				checkops(gccnt+1);
					if ((st[z] == '0') && (st[z+1] == 'X')) //Handle HEX numbers
					{
						globval[gccnt] = strtoul(&st[z],(char **)&z,0);
						if (globval[gccnt] >= 2147483648.0) globval[gccnt] -= 4294967296.0;
					}
					else
						globval[gccnt] = strtod(&st[z],(char **)&z)*(double)negit;
					gccnt++; z -= (long)st;
				checkops(globi+2); gnext[globi] = globi+1; globi++; gop[globi] = NUL;
				break;

				//Parse functions&statements
			case 'A': if (!strncmp(&st[z],"ACOS(",5))  { fmode = ACOS;  z += 4; }
				  else if (!strncmp(&st[z],"ASIN(",5))  { fmode = ASIN;  z += 4; }
				  else if (!strncmp(&st[z],"ATAN2(",6)) { fmode = ATAN2; z += 5; }
				  else if (!strncmp(&st[z],"ATAN(",5))  { fmode = ATAN;  z += 4; }
				  else if (!strncmp(&st[z],"ATN(",4))   { fmode = ATAN;  z += 3; }
				  else if (!strncmp(&st[z],"ABS(",4))   { fmode = FABS;  z += 3; } break;
			case 'B': if (!strncmp(&st[z],"BREAK;",6))
				{
					z += 6;
					if (breaklab < 0) { sprintf(kasm87err,"ERROR: BREAK not allowed outside loop"); globi = -1; return; }

					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].f = GOTO;
					gasm[gecnt].r[0].r = KUNUSED;
					gasm[gecnt].r[1].r = breaklab+KEIP;
					gasm[gecnt].n = 1;
					gecnt++;
				} break;
			case 'C': if (!strncmp(&st[z],"COS(",4))   { fmode = COS;   z += 3; }
				  else if (!strncmp(&st[z],"CEIL(",5))  { fmode = CEIL;  z += 4; }
				  else if (!strncmp(&st[z],"CONTINUE;",9))
				{
					z += 9;
					if (contlab < 0) { sprintf(kasm87err,"ERROR: CONTINUE not allowed outside loop"); globi = -1; return; }

					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].f = GOTO;
					gasm[gecnt].r[0].r = KUNUSED;
					gasm[gecnt].r[1].r = contlab+KEIP;
					gasm[gecnt].n = 1;
					gecnt++;
				} break;
			case 'D': if ((!strncmp(&st[z],"DO",2)) && (!isvarchar(st[z+2])))
				{
					z += 2;
						//l1: (fmode)
						//   <code_true>
						//l2: (fmode+1)
						//   if (expression) goto l1
						//l3: (fmode+2)

						//Reserve label #
					checklabs(numlabels+3);
					fmode = numlabels; numlabels += 3; //fmode used for unrelated temp here

						//Insert label
					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].f = NUL; gasm[gecnt].r[0].r = fmode+KEIP; gecnt++;

					if ((p = findscope(st,z,&oz,&z)) < 0)
						{ strcpy(kasm87err,"ERROR: DO needs statement"); globi = -1; return; }
					ch = st[z]; st[z] = 0; parsefunc(&st[oz],fmode+2,fmode+1); st[z] = ch; if (globi == -1) return;
					z = p;

						//Insert label
					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].f = NUL; gasm[gecnt].r[0].r = fmode+1+KEIP; gecnt++;

					if ((strncmp(&st[z],"WHILE",5)) || (isvarchar(st[z+5])) || (!st[z+5]))
						{ strcpy(kasm87err,"ERROR: DO needs WHILE"); globi = -1; return; }
					z += 5;

					p = 1; z++; oz = z; i = globi; inquotes = 0;
					for(;st[z];z++)
					{
						if ((st[z] == '\"') && ((!z) || (st[z-1] != '\\'))) inquotes ^= 1;
						if (inquotes) continue;
						if (st[z] == '(') { p++; continue; }
						if (st[z] == ')') { p--; if (!p) break; }
						if ((st[z] == ',') && (p == 1))
							{ sprintf(kasm87err,"ERROR: DO takes 1 param"); globi = -1; return; }
					}
					ch = st[z]; st[z] = 0; parsefunc(&st[oz],breaklab,contlab); st[z] = ch; if (globi == -1) return;
					if (st[z] != ')') { strcpy(kasm87err,"ERROR: missing )"); globi = -1; return; }
					z++; //Skip ')'

					if (st[z] != ';') { strcpy(kasm87err,"ERROR: missing ; after WHILE"); globi = -1; return; }
					z++;

					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].r[0].r = KUNUSED;
					gasm[gecnt].r[1].r = fmode+KEIP;
					gasm[gecnt].r[2].r = i*8+KECX;
					gasm[gecnt].n = 2;
					gasm[gecnt].f = IF1; gecnt++;
					gnext[i] = globi;

						//Insert label
					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].f = NUL; gasm[gecnt].r[0].r = fmode+2+KEIP; gecnt++;

					fmode = NOP;
				} break;
			case 'E': if (!strncmp(&st[z],"EXP(",4))   { fmode = EXP;   z += 3; }
				  else if ((!strncmp(&st[z],"ENUM",4)) && (!isvarchar(st[z+4]))) { z = parse_enum(st,z); if (globi < 0) return; }
				break;
			case 'F': if (!strncmp(&st[z],"FABS(",5))  { fmode = FABS;  z += 4; }
				  else if (!strncmp(&st[z],"FACT(",5))  { fmode = FACT;  z += 4; }
				  else if (!strncmp(&st[z],"FADD(",5))  { fmode = FADD;  z += 4; }
				  else if (!strncmp(&st[z],"FLOOR(",6)) { fmode = FLOOR; z += 5; }
				  else if (!strncmp(&st[z],"FMOD(",5))  { fmode = FMOD;  z += 4; }
				  else if (!strncmp(&st[z],"FOR(",4))
				{
					z += 4;

						 //Do Initial Condition of (;;)
					p = 1; oz = z; inquotes = 0;
					for(;st[z];z++)
					{
						if ((st[z] == '\"') && ((!z) || (st[z-1] != '\\'))) inquotes ^= 1;
						if (inquotes) continue;
						if (st[z] == '(') { p++; continue; }
						if (st[z] == ')') { p--; if (!p) { strcpy(kasm87err,"ERROR: FOR needs 3 fields"); globi = -1; return; } continue; }
						if ((st[z] == ';') && (p == 1)) break;
					}
					if (st[z] != ';') { strcpy(kasm87err,"ERROR: FOR missing ;"); globi = -1; return; }
					z++; //Skip ';'
					ch = st[z]; st[z] = 0; parsefunc(&st[oz],breaklab,contlab); st[z] = ch; if (globi == -1) return;


						//   Init;
						//l1: (fmode)
						//   if !(expression) goto l3
						//   <code_true>
						//l2: (fmode+1)
						//   Iterate;
						//   goto l1
						//l3: (fmode+2)

						//Reserve label #
					checklabs(numlabels+3);
					fmode = numlabels; numlabels += 3; //fmode used for unrelated temp here
						//Insert label
					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].f = NUL; gasm[gecnt].r[0].r = fmode+KEIP; gecnt++;

					p = 1; oz = z; i = globi; inquotes = 0;
					for(;st[z];z++)
					{
						if ((st[z] == '\"') && ((!z) || (st[z-1] != '\\'))) inquotes ^= 1;
						if (inquotes) continue;
						if (st[z] == '(') { p++; continue; }
						if (st[z] == ')') { p--; if (!p) { strcpy(kasm87err,"ERROR: FOR needs 3 fields"); globi = -1; return; } continue; }
						if ((st[z] == ';') && (p == 1)) break;
					}
					ch = st[z]; st[z] = 0; parsefunc(&st[oz],breaklab,contlab); st[z] = ch; if (globi == -1) return;
					if (st[z] != ';') { strcpy(kasm87err,"ERROR: FOR missing ;"); globi = -1; return; }
					z++; //Skip ';'

					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].r[0].r = KUNUSED;
					gasm[gecnt].r[1].r = fmode+2+KEIP;
					gasm[gecnt].r[2].r = i*8+KECX;
					gasm[gecnt].n = 2;
					gasm[gecnt].f = IF0; gecnt++;
					gnext[i] = globi;

						//Save 3rd param of (;;) as j&k for Iteration (which is done after {})
					j = z; p = 1; inquotes = 0;
					for(;st[z];z++)
					{
						if ((st[z] == '\"') && ((!z) || (st[z-1] != '\\'))) inquotes ^= 1;
						if (inquotes) continue;
						if (st[z] == '(') { p++; continue; }
						if (st[z] == ')') { p--; if (!p) break; }
					}
					if (st[z] != ')') { strcpy(kasm87err,"ERROR: FOR missing )"); globi = -1; return; }
					z++; //Skip ')'
					k = z;

					if ((p = findscope(st,z,&oz,&z)) < 0)
						{ strcpy(kasm87err,"ERROR: FOR needs statement"); globi = -1; return; }
					ch = st[z]; st[z] = 0; parsefunc(&st[oz],fmode+2,fmode+1); st[z] = ch; if (globi == -1) return;
					z = p;

						//Insert label
					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].f = NUL; gasm[gecnt].r[0].r = fmode+1+KEIP; gecnt++;

						//Do Iteration of (;;)
					ch2 = st[k-1]; st[k-1] = ';';
					ch = st[k]; st[k] = 0; parsefunc(&st[j],breaklab,contlab); st[k] = ch;
					st[k-1] = ch2;
					if (globi == -1) return;

					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].r[0].r = KUNUSED;
					gasm[gecnt].r[1].r = fmode+KEIP; gasm[gecnt].n = 1; gasm[gecnt].f = GOTO; gecnt++;

						//Insert label
					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].f = NUL; gasm[gecnt].r[0].r = fmode+2+KEIP; gecnt++;

					fmode = NOP;
				} break;
			case 'G': if (!strncmp(&st[z],"GOTO ",5))
				{
					z += 5;
					for(j=z;isvarchar(st[j]);j++);
					if (j-z <= 0) { sprintf(kasm87err,"ERROR: GOTO needs label"); globi = -1; return; }

					ch = st[j]; st[j] = 0;
					for(i=0,cptr=newlabnam;i<newlabnum;i++,cptr=&cptr[k+1])
					{
						for(k=1;cptr[k];k++);
						if (!strcmp(&st[z],cptr)) break;
					}
					if (i >= newlabnum)
					{
						checklabchars(newlabplc+j-z+2);
						checklabs(numlabels+1);
						strcpy(&newlabnam[newlabplc],&st[z]); newlabplc += j-z+1;
						newlabind[newlabnum++] = (numlabels|0x80000000); i = numlabels++;
					} else i = (newlabind[i]&0x7fffffff);
					st[j] = ch;

					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].r[0].r = KUNUSED;
					gasm[gecnt].r[1].r = i+KEIP; gasm[gecnt].f = GOTO; gasm[gecnt].n = 1; gecnt++;

					z = j+1; //skip label
				} break;
			case 'I': if (!strncmp(&st[z],"INT(",4))   { fmode = ROUND0; z += 3; }
				  else if ((!strncmp(&st[z],"IF",2)) && (!isvarchar(st[z+2])) && (st[z+2]))
				{
					z += 2;

					checklabs(numlabels+1);
					fmode = numlabels; numlabels++; //fmode used for unrelated temp here

					p = 1; z++; oz = z; i = globi; inquotes = 0;
					for(;st[z];z++)
					{
						if ((st[z] == '\"') && ((!z) || (st[z-1] != '\\'))) inquotes ^= 1;
						if (inquotes) continue;
						if (st[z] == '(') { p++; continue; }
						if (st[z] == ')') { p--; if (!p) break; }
						if (((st[z] == ',') && (p == 1)) || (st[z] == ';'))
							{ strcpy(kasm87err,"ERROR: IF condition invalid"); globi = -1; return; }
					}
					ch = st[z]; st[z] = 0; parsefunc(&st[oz],breaklab,contlab); st[z] = ch; if (globi == -1) return;
					if (st[z] != ')') { strcpy(kasm87err,"ERROR: IF missing )"); globi = -1; return; }
					z++; //Skip ')'

					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].r[0].r = KUNUSED;
					gasm[gecnt].r[1].r = fmode+KEIP;
					gasm[gecnt].r[2].r = i*8+KECX;
					gasm[gecnt].n = 2;
					gasm[gecnt].f = IF0; gecnt++;
					gnext[i] = globi;

					i = globi;
					if ((p = findscope(st,z,&oz,&z)) < 0)
						{ strcpy(kasm87err,"ERROR: IF needs statement"); globi = -1; return; }
					ch = st[z]; st[z] = 0; parsefunc(&st[oz],breaklab,contlab); st[z] = ch; if (globi == -1) return;
					z = p;

					if ((!strncmp(&st[z],"ELSE",4)) && (!isvarchar(st[z+4])))
					{
						z += 4;
							//if !(expression) goto l1
							//   <code_true>
							//   goto l2
							//l1:
							//   <code_false>
							//l2:

						checklabs(numlabels+2);
							//Insert goto so true case skips 'else' part
						checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
						gasm[gecnt].r[0].r = KUNUSED;
						gasm[gecnt].r[1].r = numlabels+KEIP; gasm[gecnt].f = GOTO; gasm[gecnt].n = 1; numlabels++;
						gecnt++;
							//Insert label to begin else part
						checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
						gasm[gecnt].f = NUL; gasm[gecnt].r[0].r = fmode+KEIP; gecnt++;
						fmode = numlabels-1;

						i = globi;

							//Some cases to verify code with:
							//"(x)if(x<0)r=0;else if(x<1) r=x; else r=1;  r"
							//"(x)if(x<0)r=0;else{if(x<1) r=x; else r=1;} r"
							//"(x)if(x<0)r=0;else{if(x<1){r=x;}else{r=1;}}r"
							//"(x)if(x<0)r=0;else if(x<1) r=x; else{r=1;} r"
						if (st[z] == ' ') z++;
						oz = z; p = 0;
						for(;;z++)
						{
							if (!st[z]) { strcpy(kasm87err,"ERROR: ELSE needs statement"); globi = -1; return; }
							if (st[z] == '{') { p++; continue; }
							if (st[z] == '}') p--;
							if (((st[z] == '}') || (st[z] == ';')) && (p <= 0))
							{
								z++;
								if ((!strncmp(&st[z],"ELSE",4)) && (!isvarchar(st[z+4]))) { z += 4-1; continue; }
								break;
							}
						}

						if (st[oz] == '{') { oz++; z--; }
						ch = st[z]; st[z] = 0; parsefunc(&st[oz],breaklab,contlab); st[z] = ch; if (globi == -1) return;
						if (st[z] == '}') z++;
					}

						//Insert label at }
					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].f = NUL; gasm[gecnt].r[0].r = fmode+KEIP; gecnt++;

					fmode = NOP;
				} break;
			case 'L': if (!strncmp(&st[z],"LOG(",4))   { fmode = LOG;   z += 3; } break;
			case 'M': if (!strncmp(&st[z],"MIN(",4))   { fmode = MIN;   z += 3; }
				  else if (!strncmp(&st[z],"MAX(",4))   { fmode = MAX;   z += 3; } break;
			case 'N': if ((!strncmp(&st[z],"NRND",4)) && (!isvarchar(st[z+4])))
				  {
					  z += 4;
					  checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
						  gasm[gecnt].r[0].r = globi*8+KECX; gasm[gecnt].r[1].r = gasm[gecnt].r[2].r = KUNUSED; gasm[gecnt].f = NRND; gecnt++;
					  checkops(globi+2); gnext[globi] = globi+1; globi++; gop[globi] = NUL;
				  } break;
			case 'P': if (!strncmp(&st[z],"POW(",4))   { fmode = POW;   z += 3; }
				  else if ((!strncmp(&st[z],"PI",2)) && (!isvarchar(st[z+2])))
				  {
					  z += 2;
					  checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
						  gasm[gecnt].r[0].r = globi*8+KECX; gasm[gecnt].r[1].r = gccnt*8+KEDX; gasm[gecnt].r[2].r = KUNUSED; gasm[gecnt].n = 1; gasm[gecnt].f = MOV; gecnt++;
					  checkops(gccnt+1); globval[gccnt++] = PI*(double)negit;
					  checkops(globi+2); gnext[globi] = globi+1; globi++; gop[globi] = NUL;
				  } break;
			case 'R': if ((!strncmp(&st[z],"RND",3)) && (!isvarchar(st[z+3])))
				{
					z += 3;
					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
						gasm[gecnt].r[0].r = globi*8+KECX; gasm[gecnt].r[1].r = gasm[gecnt].r[2].r = KUNUSED; gasm[gecnt].f = RND; gecnt++;
					checkops(globi+2); gnext[globi] = globi+1; globi++; gop[globi] = NUL;
				}
				else if ((!strncmp(&st[z],"RETURN",6)) && (!isvarchar(st[z+6])))
				{
					z += 6;
					if (st[z] == ' ') z++;

					p = 0; oz = z; i = globi; inquotes = 0;
					for(;st[z];z++)
					{
						if ((st[z] == '\"') && ((!z) || (st[z-1] != '\\'))) inquotes ^= 1;
						if (inquotes) continue;
						if (st[z] == '(') { p++; continue; }
						if (st[z] == ')') { p--; continue; }
						if ((p == 0) && (st[z] == ';')) break;
						if ((p == 1) && (st[z] == ','))
							{ strcpy(kasm87err,"ERROR: RETURN ',' invalid syntax"); globi = -1; return; }
					}
					ch = st[z]; st[z] = 0; parsefunc(&st[oz],-1,-1); st[z] = ch; if (globi == -1) return;
					if (st[z] != ';') { strcpy(kasm87err,"ERROR: RETURN missing ;"); globi = -1; return; }
					z++; //Skip ';'

					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].r[0].r = KUNUSED;
					gasm[gecnt].r[1].r = i*8+KECX;
					gasm[gecnt].r[2].r = KUNUSED;
					gasm[gecnt].n = 1;
					gasm[gecnt].f = RETURN; gecnt++;

				} break;
			case 'S': if (!strncmp(&st[z],"SQRT(",5))  { fmode = SQRT;  z += 4; }
				//else if (!strncmp(&st[z],"SQR(",4))   { fmode = SQRT;  z += 3; }
				  else if (!strncmp(&st[z],"SIN(",4))   { fmode = SIN;   z += 3; }
				  else if (!strncmp(&st[z],"SGN(",4))   { fmode = SGN;   z += 3; }
				  else if ((!strncmp(&st[z],"STATIC",6)) && (!isvarchar(st[z+6]))) { z = parse_static(st,z); if (globi < 0) return; goto prebegit; }
				  break;
			case 'T': if (!strncmp(&st[z],"TAN(",4))   { fmode = TAN;   z += 3; } break;
			case 'U': if (!strncmp(&st[z],"UNIT(",5))  { fmode = UNIT;  z += 4; } break;
			case 'W': if ((!strncmp(&st[z],"WHILE",5)) && (!isvarchar(st[z+5])) && (st[z+5]))
				{
					z += 5;
						//l1: (fmode)
						//   if !(expression) goto l2
						//   <code_true>
						//   goto l1
						//l2: (fmode+1)

						//Reserve label #
					checklabs(numlabels+2);
					fmode = numlabels; numlabels += 2; //fmode used for unrelated temp here
						//Insert label
					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].f = NUL; gasm[gecnt].r[0].r = fmode+KEIP; gecnt++;

					p = 1; z++; oz = z; i = globi; inquotes = 0;
					for(;st[z];z++)
					{
						if ((st[z] == '\"') && ((!z) || (st[z-1] != '\\'))) inquotes ^= 1;
						if (inquotes) continue;
						if (st[z] == '(') { p++; continue; }
						if (st[z] == ')') { p--; if (!p) break; }
						if ((st[z] == ',') && (p == 1))
							{ strcpy(kasm87err,"ERROR: WHILE takes 1 param"); globi = -1; return; }
					}
					ch = st[z]; st[z] = 0; parsefunc(&st[oz],breaklab,contlab); st[z] = ch; if (globi == -1) return;
					if (st[z] != ')') { strcpy(kasm87err,"ERROR: WHILE missing )"); globi = -1; return; }
					z++; //Skip ')'

					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].r[0].r = KUNUSED;
					gasm[gecnt].r[1].r = fmode+1+KEIP;
					gasm[gecnt].r[2].r = i*8+KECX;
					gasm[gecnt].n = 2;
					gasm[gecnt].f = IF0; gecnt++;
					gnext[i] = globi;

					if ((p = findscope(st,z,&oz,&z)) < 0)
						{ strcpy(kasm87err,"ERROR: WHILE needs statement"); globi = -1; return; }
					ch = st[z]; st[z] = 0; parsefunc(&st[oz],fmode+1,fmode); st[z] = ch; if (globi == -1) return;
					z = p;

					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].r[0].r = KUNUSED;
					gasm[gecnt].r[1].r = fmode+KEIP; gasm[gecnt].n = 1; gasm[gecnt].f = GOTO; gecnt++;

						//Insert label
					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].f = NUL; gasm[gecnt].r[0].r = fmode+1+KEIP; gecnt++;

					fmode = NOP;
				} break;
			case '{':
				if ((p = findscope(st,z,&oz,&z)) < 0) { strcpy(kasm87err,"ERROR: missing }"); globi = -1; return; }
				ch = st[z]; st[z] = 0; parsefunc(&st[oz],breaklab,contlab); st[z] = ch; if (globi == -1) return;
				z = p; fmode = NOP; break;
			case ';': case ',': z++; fmode = NOP; break;
			case ' ': z++; break;
			default: break;
		}
		if (oz != z) continue; //Token found: no more processing

			//Undefined token... see if it's a variable/function name
		cptr = &st[z];
		if (st[z] == '\"') //Support filenames
			  { for(z++;(st[z]) && ((st[z] != '\"') || (st[z-1] == '\\'));z++); if (st[z]) z++; }
		else { while (isvarchar(st[z])) z++; }
		p = z; j = z-oz;
		ch = st[p]; st[p] = 0;
		for(i=newvarhash[getnewvarhash(cptr)];i>=0;i=newvar[i].hashn)
		{
			//printf("|%s| vs. |%s|\n",cptr,&newvarnam[newvar[i].nami]); //nice for debugging
			if (!strcmp(cptr,&newvarnam[newvar[i].nami])) break;
		}
		st[p] = ch;

		if (i < 0) z = oz;
		else
		{
			if (st[z] == '[')
			{
				char *cptr2;

				p = 1; z++; oz = z; parm = 1; //pal[c[x][y][z]]
				for(;st[z];z++)
				{
					if (st[z] == '[') { p++; continue; }
					if (st[z] == ']')
					{
						p--; if (p) continue;
						if ((st[z+1] == ' ') && (st[z+2] == '[')) z++;
						if (st[z+1] == '[') { parm++; continue; }
						break;
					}
				}
				if (!st[z]) { sprintf(kasm87err,"ERROR: %s missing ]",&newvarnam[newvar[i].nami]); globi = -1; return; }
				if (z == oz) { sprintf(kasm87err,"ERROR: Blank param in %s[]",&newvarnam[newvar[i].nami]); globi = -1; return; }
				if (parm > (~newvar[i].parnum)) { sprintf(kasm87err,"ERROR: Array (%s) too many dimensions",&newvarnam[newvar[i].nami]); globi = -1; return; }

				if (parm == 1)
				{
						//Doing enum/strtol check here is merely an optimization... (not necessary)
					arrind = -1; //First see if array index is an "enum" style name...
					for(k=0,cptr2=enumnam;k<enumnum;k++,cptr2=&cptr2[p+1])
					{
						for(p=1;cptr2[p];p++);
						if ((z-oz == p) && (!strncmp(&st[oz],cptr2,p)))
							{ arrind = (long)enumval[k]; p = z; break; }
					}
					if (arrind < 0) { arrind = strtol(&st[oz],(char **)&p,10); p -= (long)st; }
				}
				if (p == z)
				{
					if ((unsigned long)arrind >= (unsigned long)newvar[i].maxind)
						{ sprintf(kasm87err,"ERROR: array index out of bounds (%s)",&newvarnam[newvar[i].nami]); globi = -1; return; }
					z++;
				}
				else //Array parameter is not enum, constant, or < 2 dimens; treat index as expression
				{
					k = globi;
#if 0
					ch = st[z]; st[z] = 0; parsefunc(&st[oz],breaklab,contlab); st[z] = ch; if (globi == -1) return;
					if (st[z] != ']') { sprintf(kasm87err,"ERROR: array (%s) missing ]",&newvarnam[newvar[i].nami]); globi = -1; return; }
					z++; //Skip ']'
#else
					p = 1; fparm = 0; //multidimensional arrays
					for(z=oz;st[z];z++)
					{
						if (st[z] == '[') { p++; continue; }
						if (st[z] == ']')
						{
							p--; if (p) continue;
							p = globi;

							ch = st[z]; st[z] = 0; parsefunc(&st[oz],breaklab,contlab); st[z] = ch;
							if (globi == -1) return;

							if (fparm < parm-1)
							{
								long l; //if (parm > 2)

									//p = int(p);
								checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
								gasm[gecnt].r[0].r = gasm[gecnt].r[1].r = p*8+KECX;
								gasm[gecnt].r[2].r = KUNUSED; gasm[gecnt].n = 1;
								gasm[gecnt].f = ROUND0_32; gecnt++; //(array indices only need 32-bit precision)
								gnext[p] = globi;

									//p *= product_right;
								checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
								gasm[gecnt].r[0].r = gasm[gecnt].r[1].r = p*8+KECX;
								gasm[gecnt].r[2].r = gccnt*8+KEDX; checkops(gccnt+1);
									//static buf3d[5][3][2];
									//? = buf3d[a];                                 a
									//? = buf3d[a][b];                   int(a)*2 + b
									//? = buf3d[a][b][c];   int(a)*3*2 + int(b)*2 + c
								globval[gccnt] = 1.0; //dimension combined multiplier
								for(l=(~newvar[i].parnum)+fparm-parm+1;l<(~newvar[i].parnum);l++)
									globval[gccnt] *= ((long *)&newvarnam[newvar[i].proti])[l];
								gccnt++;
								gasm[gecnt].n = 2;
								gasm[gecnt].f = TIMES; gecnt++;
								gnext[p] = globi;
							}
							if (fparm > 0)
							{
									//k += p;
								checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
								gasm[gecnt].r[0].r = gasm[gecnt].r[1].r = k*8+KECX;
								gasm[gecnt].r[2].r = p*8+KECX; gasm[gecnt].n = 2;
								gasm[gecnt].f = PLUS; gecnt++;
								gnext[k] = p;
							}

							p = 0;
							if ((st[z+1] == ' ') && (st[z+2] == '[')) z++;
							if (st[z+1] == '[') { fparm++; oz = z+2; continue; }
							z++; break;
						}
					}
#endif

						//FIX: combine this with other =,*=,... code?

					if (((st[z] == '=') && (st[z+1] != '=')) || // =
						 ((st[z] == '*') && (st[z+1] == '=')) || // *=
						 ((st[z] == '/') && (st[z+1] == '=')) || // /=
						 ((st[z] == '%') && (st[z+1] == '=')) || // %=
						 ((st[z] == '+') && (st[z+1] == '=')) || // +=
						 ((st[z] == '-') && (st[z+1] == '=')) || // -=
						 ((st[z] == '+') && (st[z+1] == '+')) || // ++
						 ((st[z] == '-') && (st[z+1] == '-')))   // --
					{
						long l;

						if (newvar[i].parnum >= 0)
						{
							ch = st[z]; st[z] = 0; sprintf(kasm87err,"ERROR: %s can't be used as variable",&st[z-j]); st[z] = ch;
							globi = -1; return;
						}

						j = z;
						if (st[z] == '=') oz = z+1; else oz = z+2;
						checkops(globi+1); gop[globi] = NUL; l = globi; inquotes = 0;

						for(z=oz,p=1;;z++)
						{
							if (!st[z]) { strcpy(kasm87err,"ERROR: missing ;"); globi = -1; return; }
							if ((st[z] == '\"') && ((!z) || (st[z-1] != '\\'))) inquotes ^= 1;
							if (inquotes) continue;
							if (st[z] == '(') { p++; continue; }
							if (st[z] == ')') { p--; continue; }
							if (((st[z] == ';') || (st[z] == ',')) && (p == 1)) break;
						}
						ch = st[z]; st[z] = 0; parsefunc(&st[oz],breaklab,contlab); st[z] = ch; if (globi == -1) return;
						z++; //skip ';'

						checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
						checkrxi(numrxi+1); memset(&rxi[numrxi],0,sizeof(rxi[0]));
						gasm[gecnt].r[0].r = KUNUSED; gasm[gecnt].n = 3;
						gasm[gecnt].r[1].r = newvar[i].r; gasm[gecnt].r[1].nv = i;
						gasm[gecnt].r[2].r = k*8+KECX;
						rxi[numrxi].r = l*8+KECX;

							  if (st[j] == '=') { gasm[gecnt].f = POKE; }
						else if (st[j] == '*') { gasm[gecnt].f = POKETIMES; }
						else if (st[j] == '/') { gasm[gecnt].f = POKESLASH; }
						else if (st[j] == '%') { gasm[gecnt].f = POKEPERC; }
						else if (st[j] == '+')
						{
							if (st[j+1] == '=') { gasm[gecnt].f = POKEPLUS; }
												else { rxi[numrxi].r = gccnt*8+KEDX; checkops(gccnt+1); globval[gccnt++] = 1.0; gasm[gecnt].f = POKEPLUS; }
						}
						else if (st[j] == '-')
						{
							if (st[j+1] == '=') { gasm[gecnt].f = POKEMINUS; }
												else { rxi[numrxi].r = gccnt*8+KEDX; checkops(gccnt+1); globval[gccnt++] = 1.0; gasm[gecnt].f = POKEMINUS; }
						}
						gasm[gecnt].rxi = numrxi++; gecnt++;

						goto prebegit;
					}
					else
					{
						checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
						gasm[gecnt].r[0].r = k*8+KECX;
						gasm[gecnt].r[1].r = newvar[i].r; gasm[gecnt].r[1].nv = i;
						gasm[gecnt].r[2].r = k*8+KECX; gasm[gecnt].n = 2;
						gasm[gecnt].f = PEEK; gecnt++;
						gnext[k] = globi;
						if (negit < 0)
						{
							checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
							gasm[gecnt].r[0].r = gasm[gecnt].r[1].r = k*8+KECX; gasm[gecnt].r[2].r = KUNUSED; gasm[gecnt].n = 1; gasm[gecnt].f = NEGMOV; gecnt++;
						}
					}
					continue;
				}
			}
			else arrind = 0;

			if (((st[z] == '=') && (st[z+1] != '=')) || // =
				 ((st[z] == '*') && (st[z+1] == '=')) || // *=
				 ((st[z] == '/') && (st[z+1] == '=')) || // /=
				 ((st[z] == '%') && (st[z+1] == '=')) || // %=
				 ((st[z] == '+') && (st[z+1] == '=')) || // +=
				 ((st[z] == '-') && (st[z+1] == '=')) || // -=
				 ((st[z] == '+') && (st[z+1] == '+')) || // ++
				 ((st[z] == '-') && (st[z+1] == '-')))   // --
			{
				if (newvar[i].parnum >= 0)
				{
					ch = st[z]; st[z] = 0; sprintf(kasm87err,"ERROR: %s can't be used as variable",&st[z-j]); st[z] = ch;
					globi = -1; return;
				}

				j = z;
				if (st[z] == '=') oz = z+1; else oz = z+2;
				checkops(globi+1); gop[globi] = NUL; k = globi; inquotes = 0;

				for(z=oz,p=1;;z++)
				{
					if (!st[z]) { strcpy(kasm87err,"ERROR: missing ;"); globi = -1; return; }
					if ((st[z] == '\"') && ((!z) || (st[z-1] != '\\'))) inquotes ^= 1;
					if (inquotes) continue;
					if (st[z] == '(') { p++; continue; }
					if (st[z] == ')') { p--; continue; }
					if (((st[z] == ';') || (st[z] == ',')) && (p == 1)) break;
				}
				ch = st[z]; st[z] = 0; parsefunc(&st[oz],breaklab,contlab); st[z] = ch; if (globi == -1) return;
				z++; //skip ';'

				checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
				gasm[gecnt].r[0].r = newvar[i].r; gasm[gecnt].r[0].q = arrind; gasm[gecnt].r[0].nv = i;
				gasm[gecnt].n = 2;
					  if (st[j] == '=') { gasm[gecnt].r[1].r = k*8+KECX; gasm[gecnt].r[2].r = KUNUSED; gasm[gecnt].n = 1; gasm[gecnt].f = MOV; }
				else if (st[j] == '*') { gasm[gecnt].r[1] = gasm[gecnt].r[0]; gasm[gecnt].r[2].r = k*8+KECX; gasm[gecnt].f = TIMES; }
				else if (st[j] == '/') { gasm[gecnt].r[1] = gasm[gecnt].r[0]; gasm[gecnt].r[2].r = k*8+KECX; gasm[gecnt].f = SLASH; }
				else if (st[j] == '%') { gasm[gecnt].r[1] = gasm[gecnt].r[0]; gasm[gecnt].r[2].r = k*8+KECX; gasm[gecnt].f = PERC; }
				else if (st[j] == '+')
				{
					if (st[j+1] == '=') { gasm[gecnt].r[1] = gasm[gecnt].r[0]; gasm[gecnt].r[2].r = k*8+KECX; gasm[gecnt].f = PLUS; }
										else { gasm[gecnt].r[1] = gasm[gecnt].r[0]; gasm[gecnt].r[2].r = gccnt*8+KEDX; checkops(gccnt+1); globval[gccnt++] = 1.0; gasm[gecnt].f = PLUS; }
				}
				else if (st[j] == '-')
				{
					if (st[j+1] == '=') { gasm[gecnt].r[1] = gasm[gecnt].r[0]; gasm[gecnt].r[2].r = k*8+KECX; gasm[gecnt].f = MINUS; }
										else { gasm[gecnt].r[1] = gasm[gecnt].r[0]; gasm[gecnt].r[2].r = gccnt*8+KEDX; checkops(gccnt+1); globval[gccnt++] = 1.0; gasm[gecnt].f = MINUS; }
				}
				gecnt++;

				goto prebegit;
			}
			else if (st[z] == '(')
			{
				if (newvar[i].parnum > 0)
					{ fmode = USERFUNC; newvari = i; }
				else
				{
					ch = st[z]; st[z] = 0; sprintf(kasm87err,"ERROR: %s can't be used as function",&st[z-j]); st[z] = ch;
					globi = -1; return;
				}
			}
			else
			{
				if (newvar[i].parnum > 0)
				{
					ch = st[z]; st[z] = 0; sprintf(kasm87err,"ERROR: %s can't be used as variable",&st[z-j]); st[z] = ch;
					globi = -1; return;
				}

				checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
				gasm[gecnt].r[0].r = globi*8+KECX;
				gasm[gecnt].r[1].r = newvar[i].r; gasm[gecnt].r[1].q = arrind; gasm[gecnt].r[1].nv = i;
				gasm[gecnt].r[2].r = KUNUSED;
				gasm[gecnt].n = 1;
				if (negit < 0) gasm[gecnt].f = NEGMOV; else gasm[gecnt].f = MOV; gecnt++;
				checkops(globi+2); gnext[globi] = globi+1; globi++; gop[globi] = NUL;
			}
			continue; //Name was found; no more processing
		}

		if (st[z] == '\"') //Extended name support (to support filenames)
			{ for(j=z+1;st[j];j++) if ((st[j] == '\"') && (st[j-1] != '\\')) break; if (!st[j]) j = z; j++; }
		else
			{ for(j=z;isvarchar(st[j]);j++); } //Standard C name support
		if (j <= z) { sprintf(kasm87err,"ERROR: '=' bad dest (%.32s)",&st[z]); globi = -1; return; }
		if ((st[j] == '=') && (st[j+1] != '=')) //FIXFIX //New variables MUST use assignment operator '='
		{
				//Don't allow new variable to be same name as existing enum..
			for(k=0,cptr=enumnam;k<enumnum;k++,cptr=&cptr[p+1])
			{
				for(p=1;cptr[p];p++);
				if ((j-z == p) && (!strncmp(&st[z],cptr,p)))
				{
					ch = st[j]; st[j] = 0; sprintf(kasm87err,"ERROR: name conflict: %s",&st[z]); st[j] = ch;
					globi = -1; return;
				}
			}

			checkvarchars(newvarplc+j-z+2);
			checkvars(newvarnum+1);
			newvar[newvarnum].nami = newvarplc;
				//Save new variable name&index to list
			if (st[z] == '\"') { memcpy(&newvarnam[newvarplc],&st[z+1],j-z-2); newvarplc += j-z-2; }
			else               { memcpy(&newvarnam[newvarplc],&st[z  ],j-z  ); newvarplc += j-z  ; }
			newvarnam[newvarplc++] = 0;
			newvar[newvarnum].r = globi*8+KECX;
			newvar[newvarnum].maxind = 0;
			newvar[newvarnum].parnum = -1;
			newvar[newvarnum].proti = -1;
			k = getnewvarhash(&newvarnam[newvar[newvarnum].nami]);
			newvar[newvarnum].hashn = newvarhash[k]; newvarhash[k] = newvarnum;
			newvarnum++;

			oz = j+1; checkops(globi+1); gop[globi] = NUL; inquotes = 0;

			for(z=oz,p=1;;z++)
			{
				if (!st[z]) { strcpy(kasm87err,"ERROR: missing ;"); globi = -1; return; }
				if ((st[z] == '\"') && ((!z) || (st[z-1] != '\\'))) inquotes ^= 1;
				if (inquotes) continue;
				if (st[z] == '(') { p++; continue; }
				if (st[z] == ')') { p--; continue; }
				if (((st[z] == ';') || (st[z] == ',')) && (p == 1)) break;
			}
			if (z == oz) { sprintf(kasm87err,"ERROR: blank function"); globi = -1; return; }
			ch = st[z]; st[z] = 0; parsefunc(&st[oz],breaklab,contlab); st[z] = ch; if (globi == -1) return;
			z++; //skip ';'

			goto prebegit;
		}
		else if (st[j] == ':') //New label
		{
			if (j-z <= 0) { sprintf(kasm87err,"ERROR: : needs label"); globi = -1; return; }

				//Save new variable name&index to list
			ch = st[j]; st[j] = 0;
			for(i=0,cptr=newlabnam;i<newlabnum;i++,cptr=&cptr[k+1])
			{
				for(k=1;cptr[k];k++);
				if (!strcmp(&st[z],cptr)) break;
			}
			if (i >= newlabnum)
			{
				checklabchars(newlabplc+j-z+2);
				checklabs(numlabels+1);
				strcpy(&newlabnam[newlabplc],&st[z]); newlabplc += j-z+1;
				newlabind[newlabnum++] = numlabels; i = numlabels++;
			} else { newlabind[i] &= 0x7fffffff; i = newlabind[i]; } //(&0x7fffffff: label acked)
			st[j] = ch;

				//Insert label at }
			checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
			gasm[gecnt].f = NUL; gasm[gecnt].r[0].r = i+KEIP; gecnt++;

			z = j+1; //skip ':'
		}
		else
		{
				//Check if name is "enum"
			while (isvarchar(st[z])) z++;
			for(k=0,cptr=enumnam;k<enumnum;k++,cptr=&cptr[p+1])
			{
				for(p=1;cptr[p];p++);
				if ((z-oz == p) && (!strncmp(&st[oz],cptr,p)))
				{
						//Parse constants
					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
						gasm[gecnt].r[0].r = globi*8+KECX; gasm[gecnt].r[1].r = gccnt*8+KEDX; gasm[gecnt].r[2].r = KUNUSED; gasm[gecnt].n = 1; gasm[gecnt].f = MOV; gecnt++;
					checkops(gccnt+1); globval[gccnt++] = enumval[k]*(double)negit;
					checkops(globi+2); gnext[globi] = globi+1; globi++; gop[globi] = NUL;
					break;
				}
			}
			if (k < enumnum) continue; //It was enum
			z = oz;

			if (st[z] == '\"')
			{
					//Parse const strings (must be used as userfunc parameters)
				checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].r[0].r = globi*8+KECX; gasm[gecnt].r[1].r = gstnum+KSTR; gasm[gecnt].r[2].r = KUNUSED; gasm[gecnt].n = 1; gasm[gecnt].f = MOV; gecnt++;

				checkstrings(gstnum+j-z-1);
#if 0
				memcpy(&gstring[gstnum],&st[z+1],j-z-2);
				gstring[gstnum+j-z-2] = 0;
				gstnum += j-z-1;
#else
				for(k=z+1;k<j-1;k++)
				{
					if ((k < j-2) && (st[k] == '\\') && (st[k+1] == '\"')) continue;
					gstring[gstnum++] = st[k];
				}
				gstring[gstnum++] = 0;
#endif

				checkops(globi+2); gnext[globi] = globi+1; globi++; gop[globi] = NUL;
				break;
			}

			if (j > z)
			{
				ch = st[j]; st[j] = 0;
#if 1
					//FIXFIXFIX: this version is known to work
				sprintf(kasm87err,"ERROR: %s undefined",&st[z]);
#else
				if (!isaddr) //<- FIXFIXFIX: should autogenerate for this too!
					sprintf(kasm87err,"ERROR: %s undefined",&st[z]);
				else
				{
					checkvarchars(newvarplc+j-z+2);
					checkvars(newvarnum+1);
					newvar[newvarnum].nami = newvarplc;
						//Save new variable name&index to list
					if (st[z] == '\"') { memcpy(&newvarnam[newvarplc],&st[z+1],j-z-2); newvarplc += j-z-2; }
					else               { memcpy(&newvarnam[newvarplc],&st[z  ],j-z  ); newvarplc += j-z  ; }
					newvarnam[newvarplc++] = 0;
					newvar[newvarnum].r = globi*8+KECX;
					newvar[newvarnum].maxind = 0;
					newvar[newvarnum].parnum = -1;
					newvar[newvarnum].proti = -1;
					k = getnewvarhash(&newvarnam[newvar[newvarnum].nami]);
					newvar[newvarnum].hashn = newvarhash[k]; newvarhash[k] = newvarnum;
					i = newvarnum;
					newvarnum++;
					
					st[j] = ch;

					checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
					gasm[gecnt].r[0].r = globi*8+KECX; gasm[gecnt].r[0].nv = i;
					gasm[gecnt].r[1].r = gccnt*8+KEDX; gasm[gecnt].n = 1; gasm[gecnt].f = MOV; checkops(gccnt+1); globval[gccnt++] = 0.0;
					gasm[gecnt].r[2].r = KUNUSED;
					gasm[gecnt].n = 1;
					gasm[gecnt].f = MOV; gecnt++;
					
					checkops(globi+2); gnext[globi] = globi+1; globi++; gop[globi] = NUL;
					
					 oz = z; //+1;
					z = j;
					inquotes = 0; continue; //Name was generated; no more processing
					//goto prebegit;
				}
#endif
				st[j] = ch;
			}
			else
				sprintf(kasm87err,"ERROR: %c undefined",st[j]);
			globi = -1; return;
		}
	}

	if (gop[globi] != NUL)
	{
		strcpy(kasm87err,"ERROR: ");
		tg.f = gop[globi]; getfuncnam(&tg,&kasm87err[strlen(kasm87err)]);
		strcat(kasm87err," missing parameter");
		globi = -1; return;
	}

#if 1
		//PEMDAS, goes through entire list for every priority... quite wasteful!
	for(i=0;i<7;i++)
		for(z=ogi;gnext[z]<globi;)
		{
			p = (long)oprio[gop[gnext[z]]]; if (i != p) { z = gnext[z]; continue; }
			checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
			gasm[gecnt].r[0].r = gasm[gecnt].r[1].r = z*8+KECX;
			gasm[gecnt].r[2].r = gnext[z]*8+KECX; gasm[gecnt].f = gop[gnext[z]];
			gasm[gecnt].n = 2; gecnt++;
			gnext[z] = gnext[gnext[z]]; //Remove 2nd param from linked list
		}
#else
		//PEMDAS, faster algo, keeps circling until done
		//for(i=0;i<8;i++) printf("gop[%d] = %d, gnext[%d] = %d\n",i,gop[i],i,gnext[i]);
		//printf("ogi=%d,globi=%d\n",ogi,globi);
		//
		//gop[0] = 0      gnext[0] = 1    ogi = 0, globi = 5
		//gop[1] = TIMES  gnext[1] = 2
		//gop[2] = PLUS   gnext[2] = 3    r0 = X * X
		//gop[3] = TIMES  gnext[3] = 4    r1 = Y * Y
		//gop[4] = LES    gnext[4] = 5    r1 = r1 < 1
		//gop[5] = 0      gnext[5] = 0    r0 = r0 + r1
		//
		//   1 2 1 3  <priority      2 1 3  <priority         2 3  <priority
		//   * + * <  <gop[i]        + * <  <gop[i]           + <  <gop[i]
		// x*x+y*y<1  <equation    x+y*y<1  <equation       x+y<1  <equation
		// ^        0->2->3->4->5  ^ ^      0->2->4->5        ^
		//
		//Find leftmost operator satisfying: my_priority <= next_operator_priority
	z = ogi;
	while (gnext[z] != globi)
	{
			//Can't evaluate operator yet if next operator is higher priority...
		if ((gop[gnext[z]] == NUL) || (oprio[gop[gnext[z]]] > oprio[gop[gnext[gnext[z]]]]))
			{ z = gnext[z]; continue; }

		checkops(gecnt+1); memset(&gasm[gecnt],0,sizeof(gasmtyp));
		gasm[gecnt].r[0] = gasm[gecnt].r[1] = z*8+KECX;
		gasm[gecnt].r[2] = gnext[z]*8+KECX; gasm[gecnt].f = gop[gnext[z]];
		gasm[gecnt].n = 2; gecnt++;
		gnext[z] = gnext[gnext[z]]; //Remove 2nd param from linked list
		z = ogi;
	}
#endif
}

static char fpustat;
static long putwrite = 0;