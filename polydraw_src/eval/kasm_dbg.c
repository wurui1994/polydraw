
static void getfuncnam (gasmtyp *g, char *st)
{
	switch(g->f)
	{
		case NUL:   strcpy(st,"NUL"); break;
		case GOTO:  strcpy(st,"GOTO"); break;
		case RETURN:strcpy(st,"RETURN"); break;
		case RND:   strcpy(st,"RND"); break;
		case NRND:  strcpy(st,"NRND"); break;
		case NOP:   strcpy(st,"NOP"); break;
		case MOV:   strcpy(st,"MOV"); break;
		case NEGMOV:strcpy(st,"NEGMOV"); break;
		case NEQU0: strcpy(st,"NEQU0"); break;
		case IF0:   strcpy(st,"IF0"); break;
		case IF1:   strcpy(st,"IF1"); break;
		case FABS:  strcpy(st,"FABS"); break;
		case SGN:   strcpy(st,"SGN"); break;
		case UNIT:  strcpy(st,"UNIT"); break;
		case FLOOR: strcpy(st,"FLOOR"); break;
		case ROUND0: case ROUND0_32: strcpy(st,"ROUND0"); break;
		case CEIL:  strcpy(st,"CEIL"); break;
		case SIN:   strcpy(st,"SIN"); break;
		case COS:   strcpy(st,"COS"); break;
		case TAN:   strcpy(st,"TAN"); break;
		case ASIN:  strcpy(st,"ASIN"); break;
		case ACOS:  strcpy(st,"ACOS"); break;
		case ATAN:  strcpy(st,"ATAN"); break;
		case SQRT:  strcpy(st,"SQRT"); break;
		case EXP:   strcpy(st,"EXP"); break;
		case FACT:  strcpy(st,"FACT"); break;
		case LOG:   strcpy(st,"LOG"); break;
		case TIMES: strcpy(st,"*"); break;
		case SLASH: strcpy(st,"/"); break;
		case PERC:  strcpy(st,"%%"); break;
		case PLUS:  strcpy(st,"+"); break;
		case MINUS: strcpy(st,"-"); break;
		case LES:   strcpy(st,"<"); break;
		case LESEQ: strcpy(st,"<="); break;
		case MOR:   strcpy(st,">"); break;
		case MOREQ: strcpy(st,">="); break;
		case EQU:   strcpy(st,"=="); break;
		case NEQU:  strcpy(st,"!="); break;
		case LAND:  strcpy(st,"&&"); break;
		case LOR:   strcpy(st,"||"); break;

		case POW:   strcpy(st,"POW"); break;
		case MIN:   strcpy(st,"MIN"); break;
		case MAX:   strcpy(st,"MAX"); break;
		case FADD:  strcpy(st,"FADD"); break;
		case FMOD:  strcpy(st,"FMOD"); break;
		case ATAN2: strcpy(st,"ATAN2"); break;
		case LOGB:  strcpy(st,"LOGB"); break;
		case PEEK:  strcpy(st,"PEEK"); break;

		case POKE:  strcpy(st,"POKE"); break;
		case POKETIMES:strcpy(st,"POKETIMES"); break;
		case POKESLASH:strcpy(st,"POKESLASH"); break;
		case POKEPERC: strcpy(st,"POKEPERC"); break;
		case POKEPLUS: strcpy(st,"POKEPLUS"); break;
		case POKEMINUS:strcpy(st,"POKEMINUS"); break;
		case USERFUNC: if (g->g >= 0) strcpy(st,&newvarnam[newvar[g->g].nami]); else strcpy(st,"USERFUNC"); break;

		default: st[0] = 0; break;
	}
}

static void getvarnam (rtyp reg, char *st)
{
	long l, m;

	if (reg.r == KUNUSED) { strcpy(st,"?"); return; }
	switch(((unsigned long)reg.r)>>28)
	{
		case (KECX>>28): sprintf(st,"m%d",(reg.r&0x0fffffff)>>3); break;
		case (KSTR>>28): reg.r = (reg.r&0x0fffffff)+gccnt*8+KEDX;        goto getvarnam_casekedx;
		case (KARR>>28): reg.r = (reg.r&0x0fffffff)+gccnt*8+gstnum+KEDX; goto getvarnam_casekedx;
		case (KEDX>>28):
getvarnam_casekedx:
			l = (reg.r&0x0fffffff);
				  if (l < (gccnt<<3)       ) { sprintf(st,"%g",globval[l>>3]); }
			else if (l < (gccnt<<3)+gstnum) { sprintf(st,"\"%s\"",&gstring[l-(gccnt<<3)]); }
			else
			{
				for(m=newvarnum-1;m>=0;m--)
					if (newvar[m].r == l-(gccnt<<3)-gstnum+(signed)KARR)
					{
						strcpy(st,&newvarnam[newvar[m].nami]);
						sprintf(&st[strlen(st)],"[%d]",reg.q);
						break;
					}
				if (m < 0) sprintf(st,"??0x%08x??",reg.r);
			}
			break;
		case (KESP>>28): case (KPTR>>28):
			if (((reg.r&0x0fffffff)>>3) < memnum)
			{
				sprintf(st,"m%d",(reg.r&0x0fffffff)>>3);
				break;
			} //No break intentional
		case (KIMM>>28): case (KGLB>>28):
			for(m=0;m<gnumglob;m++)
				if (newvar[m].r == reg.r)
				{
					strcpy(st,&newvarnam[newvar[m].nami]);
					if ((newvar[m].maxind) || ((((unsigned long)reg.r)>>28) == (KPTR>>28)))
						sprintf(&st[strlen(st)],"[%d]",reg.q);
					break;
				}
			if (m >= gnumglob) sprintf(st,"??0x%08x??",reg.r);
			break;
		case (KEIP>>28): sprintf(st,"l%d",reg.r&0x0fffffff); break;
		case (KFST>>28): sprintf(st,"r%d",(reg.r&0x0fffffff)>>3); break;
		default: sprintf(st,"?!0x%08x!?",reg.r); break;
	}
}

	//showflags=1: pseudoasm
	//showflags=2: machine code bytes
void kasm87_showdebug (long showflags, char *debuf, long debuflng)
{
	rtyp *rp;
	long i, j, k, didlab = 0;
	char st[4][64], st2[260], *cptr, *cptr2, *cp0, *cp1;

	if (debuflng <= 0) return;
	debuf[0] = 0;
	cp0 = debuf; cp1 = &debuf[debuflng-1]; cp1[0] = 0;
	if (showflags&1)
	{
		for(i=0;i<gecnt;i++)
		{
			for(j=2;j>=0;j--) getvarnam(gasm[i].r[j],st[j]);
			if (gasm[i].n >= 3) getvarnam(rxi[gasm[i].rxi],st[3]); else st[3][0] = 0;

			if (gasm[i].f == NUL) { k = _snprintf(cp0,cp1-cp0,"%s:",st[0]); if (k >= 0) cp0 += k; didlab = 1; continue; }
			if (!didlab) { k = _snprintf(cp0,cp1-cp0,"   "); if (k >= 0) cp0 += k; } else didlab = 0;
			if (gasm[i].f == IF0) { k = _snprintf(cp0,cp1-cp0,"IF !(%s) GOTO %s\n",st[2],st[1]); if (k >= 0) cp0 += k; continue; }
			if (gasm[i].f == IF1) { k = _snprintf(cp0,cp1-cp0,"IF (%s) GOTO %s\n",st[2],st[1]);  if (k >= 0) cp0 += k; continue; }
			if (gasm[i].f == GOTO) { k = _snprintf(cp0,cp1-cp0,"GOTO %s\n",st[1]);               if (k >= 0) cp0 += k; continue; }
			if (gasm[i].f == RETURN) { k = _snprintf(cp0,cp1-cp0,"RETURN %s\n",st[1]);           if (k >= 0) cp0 += k; continue; }
			if (gasm[i].f == USERFUNC)
			{
				getfuncnam(&gasm[i],st2);
				k = _snprintf(cp0,cp1-cp0,"%s = %s(",st[0],st2); if (k >= 0) cp0 += k;
				for(j=0;j<gasm[i].n;j++)
				{
					if (j) { k = _snprintf(cp0,cp1-cp0,","); if (k >= 0) cp0 += k; }
					if (newvarnam[newvar[gasm[i].g].proti+j] == 'D')
						{ k = _snprintf(cp0,cp1-cp0,"&"); if (k >= 0) cp0 += k; }
					if (newvarnam[newvar[gasm[i].g].proti+j] == 'C')
						{ k = _snprintf(cp0,cp1-cp0,"$"); if (k >= 0) cp0 += k; }
					if (j < 2) rp = &gasm[i].r[j+1]; else rp = &rxi[gasm[i].rxi+j-2];
					getvarnam(*rp,st[2]);
					k = _snprintf(cp0,cp1-cp0,"%s",st[2]); if (k >= 0) cp0 += k;
				}
				k = _snprintf(cp0,cp1-cp0,")\n"); if (k >= 0) cp0 += k;
				continue;
			}
			switch(gasm[i].f)
			{
				case RND: case NRND:
					getfuncnam(&gasm[i],st2); strcat(st2,"()"); break;

				case MOV:   strcpy(st2,"%s"); break;
				case NEGMOV:strcpy(st2,"-%s"); break;
				case NEQU0: strcpy(st2,"%s != 0"); break;

				case FABS: case SGN: case UNIT: case FLOOR: case CEIL: case ROUND0: case ROUND0_32: case SIN: case COS: case TAN:
				case ASIN: case ACOS: case ATAN: case SQRT: case EXP: case FACT: case LOG:
					getfuncnam(&gasm[i],st2); strcat(st2,"(%s)"); break;

				case TIMES: case SLASH: case PERC: case PLUS: case MINUS:
				case LES: case LESEQ: case MOR: case MOREQ: case EQU: case NEQU: case LAND: case LOR:
					strcpy(st2,"%s "); getfuncnam(&gasm[i],&st2[strlen(st2)]); strcat(st2," %s"); break;

				case POW: case MIN: case MAX: case FADD: case FMOD: case ATAN2: case LOGB: case PEEK:
					getfuncnam(&gasm[i],st2); strcat(st2,"(%s,%s)"); break;

				case POKE: case POKETIMES: case POKESLASH: case POKEPERC: case POKEPLUS: case POKEMINUS:
					getfuncnam(&gasm[i],st2); strcat(st2,"(%s,%s,%s)"); break;
			}
			k = _snprintf(cp0,cp1-cp0,"%s = ",st[0]); if (k >= 0) cp0 += k;
			k = _snprintf(cp0,cp1-cp0,st2,st[1],st[2],st[3]); if (k >= 0) cp0 += k;
			k = _snprintf(cp0,cp1-cp0,"\n"); if (k >= 0) cp0 += k;
		}
		if (didlab) { k = _snprintf(cp0,cp1-cp0,"\n"); if (k >= 0) cp0 += k; } //last label has no code
	}
	if ((showflags&2) && (compcode))
	{
		for(i=0;i<kasm87leng;i++)
		{
			if (!(i&15))
			{
				if (i)
				{
					k = _snprintf(cp0,cp1-cp0,"\n",i); if (k >= 0) cp0 += k;

					for(j=i+1;j<kasm87leng;j++) if (compcode[i] != compcode[j]) break;
					if (j-i >= 16)
					{
						j = ((j-i)&~15);
						k = _snprintf(cp0,cp1-cp0,"%06x  %02x * 0x%x\n",i,compcode[i],j); if (k >= 0) cp0 += k;
						if (j > 16) { k = _snprintf(cp0,cp1-cp0,"..\n"); if (k >= 0) cp0 += k; }
						i += j; if (i >= kasm87leng) break;
					}

				}
				k = _snprintf(cp0,cp1-cp0,"%06x  ",i); if (k >= 0) cp0 += k;
			}
			k = _snprintf(cp0,cp1-cp0,"%02x ",compcode[i]); if (k >= 0) cp0 += k;
		}
		k = _snprintf(cp0,cp1-cp0," (%d bytes)\n",kasm87leng); if (k >= 0) cp0 += k;
	}
	//else { k = _snprintf(cp0,cp1-cp0,"\n"); if (k >= 0) cp0 += k; }
	//if ((showflags&4) && (compcode)) { i = 0; cp0 = kdisasm((char *)compcode,kasm87leng,cp0,cp1-cp0,&i); }
}