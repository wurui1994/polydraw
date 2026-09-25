
static long gkasm87cptr;
typedef struct
{
		//0xc7 0x05 [gkasm87cptr] [imm32] ;mov gkasm87cptr, kcd
		//0xe9 [imm32]                    ;jmp kasm87c()-eip;
	char codestub[16]; //Must be first in structure

	double *globval;     long gccnt, gstnum, arrnum;
	gasmtyp *gasm;       long gecnt;
	rtyp *rxi;           long numrxi;
	evalextyp *gevalext; long gevalextnum;
	newvartyp *newvar;   long newvarnum, gnumarg;
	char *newvarnam;     long newvarplc;
	long stackdoubs;

	char data[0]; //Must be last in structure
} kcd_t; //Kasm87C Data

double kasm87c_run (char *parmdat, kcd_t *kcd)
{
	double *p[17];
	long i, j, k, plst[16];

	if (kcd->gecnt <= 0) return(0.0);

	plst[((unsigned long)KECX)>>28] = ((long)gvlp        -KECX);
	plst[((unsigned long)KEDX)>>28] = ((long)kcd->globval-KEDX);
	plst[((unsigned long)KESP)>>28] = ((long)parmdat     -KESP);
	plst[((unsigned long)KPTR)>>28] = ((long)parmdat     -KPTR);
	plst[((unsigned long)KIMM)>>28] = ((long)            -KIMM);
	plst[((unsigned long)KGLB)>>28] = ((long)gstatmem    -KGLB);

	gvlp += kcd->stackdoubs; //FIX:could do stack overflow check here
	for(i=0;i<kcd->gecnt;i++)
	{
		//{
		//char mybuf[260]; getfuncnam(&kcd->gasm[i],mybuf); //For debugging only
		//printf("%3d: %10s %08x %08x %08x\n",i,mybuf,kcd->gasm[i].r[0].r,kcd->gasm[i].r[1].r,kcd->gasm[i].r[2].r);
		//}

		for(j=2;j>=0;j--)
		{
			p[j] = (double *)(plst[((unsigned long)kcd->gasm[i].r[j].r)>>28]+(long)kcd->gasm[i].r[j].r);
			if ((kcd->gasm[i].r[j].r&0xf0000000) == KPTR) p[j] = (*(double **)p[j]) + (kcd->gasm[i].r[j].q);
			if ((kcd->gasm[i].r[j].r&0xf0000000) == KIMM) p[j] = (double *)(((long)kcd->gevalext[((long)p[j])].ptr)+kcd->gasm[i].r[j].q*8);
			if ((kcd->gasm[i].r[j].r&0xf0000000) == KGLB) p[j] = (double *)((gstatmem + ((long)p[j]))+kcd->gasm[i].r[j].q*8);
			if ((kcd->gasm[i].r[j].r&0xf0000000) == KEDX) p[j] += kcd->gasm[i].r[j].q;
		}
		switch(kcd->gasm[i].f)
		{
			case NUL:   break;
			case GOTO:  i = kcd->gasm[i].r[0].r; break; //r[1].r is label, r[0].r is gasm index
			case RETURN:gvlp -= kcd->stackdoubs; return(*p[1]);
			case RND:   (*p[0]) = ((double)krand())*(double)oneover2_31; break;
			case NRND:  (*p[0]) = nrnd(); break;
			case MOV:   (*p[0]) = (*p[1]); break;
			case NEGMOV:(*p[0]) = -(*p[1]); break;
			case NEQU0: (*p[0]) = ((*p[1]) != 0); break;
			case IF0:   if ((*p[2]) == 0) { i = kcd->gasm[i].r[0].r; } break; //r[1].r is label, r[0].r is gasm index
			case IF1:   if ((*p[2]) != 0) { i = kcd->gasm[i].r[0].r; } break; //r[1].r is label, r[0].r is gasm index
			case FABS:  (*p[0]) = fabs(*p[1]); break;
			case SGN:   (*p[0]) = ((*p[1])>0) - ((*p[1])<0); break;
			case UNIT:  (*p[0]) = ((*p[1])==0)*.5 + ((*p[1])>0); break;
			case FLOOR: (*p[0]) = floor(*p[1]); break;
			case CEIL:  (*p[0]) = ceil(*p[1]); break;
			case ROUND0: case ROUND0_32: if (*p[1] >= 0) (*p[0]) = floor(*p[1]); else (*p[0]) = -floor(-(*p[1])); break;
			case SIN:   (*p[0]) = sin(*p[1]); break;
			case COS:   (*p[0]) = cos(*p[1]); break;
			case TAN:   (*p[0]) = tan(*p[1]); break;
			case ASIN:  (*p[0]) = asin(*p[1]); break;
			case ACOS:  (*p[0]) = acos(*p[1]); break;
			case ATAN:  (*p[0]) = atan(*p[1]); break;
			case SQRT:  (*p[0]) = sqrt(*p[1]); break;
			case EXP:   (*p[0]) = exp(*p[1]); break;
			case FACT:  (*p[0]) = fact(*p[1]); break;
			case LOG:   (*p[0]) = log(*p[1]); break;
			case TIMES: (*p[0]) = (*p[1])*(*p[2]); break;
			case SLASH: (*p[0]) = (*p[1])/(*p[2]); break;
			case PERC:  (*p[0]) = (*p[1])-floor((*p[1])/fabs(*p[2]))*fabs(*p[2]); break;
			case PLUS:  //no break intentional
			case FADD:  (*p[0]) = (*p[1])+(*p[2]); break;
			case MINUS: (*p[0]) = (*p[1])-(*p[2]); break;
			case POW:   (*p[0]) = pow(*p[1],*p[2]); break;
			case MIN:   if ((*p[2]) < (*p[1])) (*p[0]) = (*p[2]); else (*p[0]) = (*p[1]); break;
			case MAX:   if ((*p[2]) > (*p[1])) (*p[0]) = (*p[2]); else (*p[0]) = (*p[1]); break;
			case FMOD:  (*p[0]) = fmod(*p[1],*p[2]); break;
			case ATAN2: (*p[0]) = atan2(*p[1],*p[2]); break;
			case LOGB:  (*p[0]) = log(*p[1])/log(*p[2]); break;
			case LES:   (*p[0]) = ((*p[1]) <  (*p[2])); break;
			case LESEQ: (*p[0]) = ((*p[1]) <= (*p[2])); break;
			case MOR:   (*p[0]) = ((*p[1]) >  (*p[2])); break;
			case MOREQ: (*p[0]) = ((*p[1]) >= (*p[2])); break;
			case EQU:   (*p[0]) = ((*p[1]) == (*p[2])); break;
			case NEQU:  (*p[0]) = ((*p[1]) != (*p[2])); break;
			case LAND:  (*p[0]) = ((*p[1]) && (*p[2])); break;
			case LOR:   (*p[0]) = ((*p[1]) || (*p[2])); break;
			case PEEK:
				{
				j = (long)(*p[2]);
				k = kcd->newvar[kcd->gasm[i].r[1].nv].maxind; //quick&dirty bounds check; if 2^x, use "and"
				if ((k) && (!((k-1)&k))) j &= (k-1); else if ((unsigned long)j >= (unsigned long)k) j = 0;
				(*p[0]) = p[1][j];
				}
				break;
			case POKE: case POKETIMES: case POKESLASH: case POKEPERC: case POKEPLUS: case POKEMINUS:
				{
				rtyp *rp = &kcd->rxi[kcd->gasm[i].rxi];
				p[3] = (double *)(plst[((unsigned long)rp->r)>>28]+(long)rp->r);
				if ((rp->r&0xf0000000) == KPTR) p[3] = (*(double **)p[3]) + (rp->q);
				if ((rp->r&0xf0000000) == KIMM) p[3] = (double *)(((long)kcd->gevalext[((long)p[3])].ptr)+rp->q*8);
				if ((rp->r&0xf0000000) == KGLB) p[3] = (double *)((gstatmem + ((long)p[3]))+rp->q*8);

				j = (long)(*p[2]);
				k = kcd->newvar[kcd->gasm[i].r[1].nv].maxind; //quick&dirty bounds check; if 2^x, use "and"
				if ((k) && (!((k-1)&k))) j &= (k-1); else if ((unsigned long)j >= (unsigned long)k) j = 0;
				switch(kcd->gasm[i].f)
				{
					case POKE:      p[1][j] = (*p[3]); break;
					case POKETIMES: p[1][j] *= (*p[3]); break;
					case POKESLASH: p[1][j] /= (*p[3]); break;
					case POKEPERC:  p[1][j] -= floor((p[1][j])/fabs(*p[3]))*fabs(*p[3]); break;
					case POKEPLUS:  p[1][j] += (*p[3]); break;
					case POKEMINUS: p[1][j] -= (*p[3]); break;
				}
				}
				break;
			case USERFUNC:
				{
				char *cptr;
				rtyp *rp;
				double (__cdecl *dafunc)(double,...);
				if (kcd->gasm[i].n > 16) { gvlp -= kcd->stackdoubs; return(*p[0]); } //Display error!
				for(j=kcd->gasm[i].n;j>2;j--)
				{
					rp = &kcd->rxi[kcd->gasm[i].rxi+j-3];
					p[j] = (double *)(plst[((unsigned long)rp->r)>>28]+(long)rp->r);
					if ((rp->r&0xf0000000) == KPTR) p[j] = (*(double **)p[j]) + (rp->q);
					if ((rp->r&0xf0000000) == KIMM) p[j] = (double *)(((long)kcd->gevalext[((long)p[j])].ptr)+rp->q*8);
					if ((rp->r&0xf0000000) == KGLB) p[j] = (double *)((gstatmem + ((long)p[j]))+rp->q*8);
				}
				if ((kcd->newvar[kcd->gasm[i].g].r&0xf0000000) == KIMM)
					  dafunc = ((double (__cdecl *)(double,...))kcd->gevalext[kcd->newvar[kcd->gasm[i].g].r&0x0fffffff].ptr);
				else dafunc = ((double (__cdecl *)(double,...))*(long *)(plst[((unsigned long)KESP)>>28]+kcd->newvar[gasm[i].g].r));

				cptr = &kcd->newvarnam[kcd->newvar[kcd->gasm[i].g].proti];
				switch(kcd->gasm[i].n) //This seems to be the only way to do pure C implementation; it sucks!
				{
					case 1:
						if (!strncmp(cptr,"d",1)) { (*p[0]) = dafunc(*p[1]); break; }
						break;
					case 2:
						if (!strncmp(cptr,"dd",2)) { (*p[0]) = dafunc(*p[1],*p[2]); break; }
						if (!strncmp(cptr,"dD",2)) { (*p[0]) = dafunc(*p[1], p[2]); break; }
						break;
					case 3:
						if (!strncmp(cptr,"ddd",3)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3]); break; }
						if (!strncmp(cptr,"ddD",3)) { (*p[0]) = dafunc(*p[1],*p[2], p[3]); break; }
						if (!strncmp(cptr,"dDD",3)) { (*p[0]) = dafunc(*p[1], p[2], p[3]); break; }
						break;
					case 4:
						if (!strncmp(cptr,"dddd",4)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4]); break; }
						if (!strncmp(cptr,"dddD",4)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3], p[4]); break; }
						if (!strncmp(cptr,"ddDD",4)) { (*p[0]) = dafunc(*p[1],*p[2], p[3], p[4]); break; }
						if (!strncmp(cptr,"dDDD",4)) { (*p[0]) = dafunc(*p[1], p[2], p[3], p[4]); break; }
						break;
					case 5:
						if (!strncmp(cptr,"ddddd",5)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5]); break; }
						if (!strncmp(cptr,"ddddD",5)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4], p[5]); break; }
						if (!strncmp(cptr,"dddDD",5)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3], p[4], p[5]); break; }
						if (!strncmp(cptr,"ddDDD",5)) { (*p[0]) = dafunc(*p[1],*p[2], p[3], p[4], p[5]); break; }
						if (!strncmp(cptr,"dDDDD",5)) { (*p[0]) = dafunc(*p[1], p[2], p[3], p[4], p[5]); break; }
						break;
					case 6:
						if (!strncmp(cptr,"dddddd",6)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6]); break; }
						if (!strncmp(cptr,"dddddD",6)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5], p[6]); break; }
						if (!strncmp(cptr,"ddddDD",6)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4], p[5], p[6]); break; }
						if (!strncmp(cptr,"dddDDD",6)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3], p[4], p[5], p[6]); break; }
						if (!strncmp(cptr,"ddDDDD",6)) { (*p[0]) = dafunc(*p[1],*p[2], p[3], p[4], p[5], p[6]); break; }
						if (!strncmp(cptr,"dDDDDD",6)) { (*p[0]) = dafunc(*p[1], p[2], p[3], p[4], p[5], p[6]); break; }
						break;
					case 7:
						if (!strncmp(cptr,"ddddddd",7)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7]); break; }
						if (!strncmp(cptr,"ddddddD",7)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6], p[7]); break; }
						if (!strncmp(cptr,"dddddDD",7)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5], p[6], p[7]); break; }
						if (!strncmp(cptr,"ddddDDD",7)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4], p[5], p[6], p[7]); break; }
						if (!strncmp(cptr,"dddDDDD",7)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3], p[4], p[5], p[6], p[7]); break; }
						if (!strncmp(cptr,"ddDDDDD",7)) { (*p[0]) = dafunc(*p[1],*p[2], p[3], p[4], p[5], p[6], p[7]); break; }
						if (!strncmp(cptr,"dDDDDDD",7)) { (*p[0]) = dafunc(*p[1], p[2], p[3], p[4], p[5], p[6], p[7]); break; }
						break;
					case 8:
						if (!strncmp(cptr,"dddddddd",8)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8]); break; }
						if (!strncmp(cptr,"dddddddD",8)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7], p[8]); break; }
						if (!strncmp(cptr,"ddddddDD",8)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6], p[7], p[8]); break; }
						if (!strncmp(cptr,"dddddDDD",8)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5], p[6], p[7], p[8]); break; }
						if (!strncmp(cptr,"ddddDDDD",8)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4], p[5], p[6], p[7], p[8]); break; }
						if (!strncmp(cptr,"dddDDDDD",8)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3], p[4], p[5], p[6], p[7], p[8]); break; }
						if (!strncmp(cptr,"ddDDDDDD",8)) { (*p[0]) = dafunc(*p[1],*p[2], p[3], p[4], p[5], p[6], p[7], p[8]); break; }
						if (!strncmp(cptr,"dDDDDDDD",8)) { (*p[0]) = dafunc(*p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8]); break; }
						break;
					case 9:
						if (!strncmp(cptr,"ddddddddd",9)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],*p[9]); break; }
						if (!strncmp(cptr,"ddddddddD",9)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8], p[9]); break; }
						if (!strncmp(cptr,"dddddddDD",9)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7], p[8], p[9]); break; }
						if (!strncmp(cptr,"ddddddDDD",9)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6], p[7], p[8], p[9]); break; }
						if (!strncmp(cptr,"dddddDDDD",9)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5], p[6], p[7], p[8], p[9]); break; }
						if (!strncmp(cptr,"ddddDDDDD",9)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4], p[5], p[6], p[7], p[8], p[9]); break; }
						if (!strncmp(cptr,"dddDDDDDD",9)) { (*p[0]) = dafunc(*p[1],*p[2],*p[3], p[4], p[5], p[6], p[7], p[8], p[9]); break; }
						if (!strncmp(cptr,"ddDDDDDDD",9)) { (*p[0]) = dafunc(*p[1],*p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]); break; }
						if (!strncmp(cptr,"dDDDDDDDD",9)) { (*p[0]) = dafunc(*p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]); break; }
						break;
					case 10: (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],*p[9],*p[10]); break;
					case 11: (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],*p[9],*p[10],*p[11]); break;
					case 12: (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],*p[9],*p[10],*p[11],*p[12]); break;
					case 13: (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],*p[9],*p[10],*p[11],*p[12],*p[13]); break;
					case 14: (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],*p[9],*p[10],*p[11],*p[12],*p[13],*p[14]); break;
					case 15: (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],*p[9],*p[10],*p[11],*p[12],*p[13],*p[14],*p[15]); break;
					case 16: (*p[0]) = dafunc(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],*p[9],*p[10],*p[11],*p[12],*p[13],*p[14],*p[15],*p[16]); break;
					//how to do unlimited cases without using switch?
				}
				break;
				}
		}
	}
	gvlp -= kcd->stackdoubs; return(*p[0]);
}

double __cdecl kasm87cp (double *first, ...)
{
	va_list marker;
	kcd_t *kcd;
	long i, j;
	char parmdat[sizeof(double)*16];

	kcd = (kcd_t *)gkasm87cptr;
	*(double **)&parmdat[0] = first;
	va_start(marker,first); j = 4;
	for(i=1;i<kcd->gnumarg;i++)
	{
		if ((newvar[i].parnum < 0) && ((newvar[i].r&0xf0000000) == KESP))
			  { *(double *)&parmdat[j] = va_arg(marker,double); j += 8; } //8 byte variable
		else { *(long   *)&parmdat[j] = va_arg(marker,long  ); j += 4; } //4 byte variable/function pointer
	}
	va_end(marker); //Keep for compatibility

	return(kasm87c_run(parmdat,kcd));
}

double __cdecl kasm87c (double first, ...)
{
	va_list marker;
	kcd_t *kcd;
	long i, j;
	char parmdat[sizeof(double)*16];

	kcd = (kcd_t *)gkasm87cptr;
	*(double *)&parmdat[0] = first;
	va_start(marker,first); j = 8;
	for(i=1;i<gnumarg;i++)
	{
		if ((newvar[i].parnum < 0) && ((newvar[i].r&0xf0000000) == KESP))
			  { *(double *)&parmdat[j] = va_arg(marker,double); j += 8; } //8 byte variable
		else { *(long   *)&parmdat[j] = va_arg(marker,long  ); j += 4; } //4 byte variable/function pointer
	}
	va_end(marker); //Keep for compatibility

	return(kasm87c_run(parmdat,kcd));
}

kcd_t *kasm87c_copyglob2struct (long stackdoubs)
{
	kcd_t *kcd;
	long l;

	l = sizeof(kcd_t);
	l +=       gccnt*sizeof(  globval[0]) + gstnum + arrnum;
	l +=       gecnt*sizeof(     gasm[0]);
	l +=      numrxi*sizeof(      rxi[0]);
	l += gevalextnum*sizeof( gevalext[0]);
	l +=   newvarnum*sizeof(   newvar[0]);
	l +=   newvarplc*sizeof(newvarnam[0]);

	kcd = (kcd_t *)malloc(l); if (!kcd) return(0);

	kcd->gccnt = gccnt; kcd->gstnum = gstnum; kcd->arrnum = arrnum;
	kcd->gecnt = gecnt;
	kcd->numrxi = numrxi;
	kcd->gevalextnum = gevalextnum;
	kcd->newvarnum = newvarnum; kcd->gnumarg = gnumarg;
	kcd->newvarplc = newvarplc;

	l = (long)&kcd->data;
	kcd->globval   = (double    *)l; l +=       gccnt*sizeof(  globval[0]) + gstnum + arrnum;
	kcd->gasm      = (gasmtyp   *)l; l +=       gecnt*sizeof(     gasm[0]);
	kcd->rxi       = (rtyp      *)l; l +=      numrxi*sizeof(      rxi[0]);
	kcd->gevalext  = (evalextyp *)l; l += gevalextnum*sizeof( gevalext[0]);
	kcd->newvar    = (newvartyp *)l; l +=   newvarnum*sizeof(   newvar[0]);
	kcd->newvarnam = (char      *)l; l +=   newvarplc*sizeof(newvarnam[0]);

#if 0
	printf("kcd_t:%d",sizeof(kcd_t));
	if (gccnt|gstnum|arrnum) printf(" + globval:%d"  ,      gccnt*sizeof(globval[0]) + gstnum + arrnum);
	if (gecnt)               printf(" + gasm:%d"     ,      gecnt*sizeof(gasm[0]));
	if (numrxi)              printf(" + rxi:%d"      ,     numrxi*sizeof(rxi[0]));
	if (gevalextnum)         printf(" + gevalext:%d" ,gevalextnum*sizeof(gevalext[0]));
	if (newvarnum)           printf(" + newvar:%d"   ,  newvarnum*sizeof(newvar[0]));
	if (newvarplc)           printf(" + newvarnam:%d",  newvarplc*sizeof(newvarnam[0]));
	printf(" = %d\n",l);
#endif

	if (gccnt|gstnum|arrnum) memcpy(kcd->globval  ,globval  ,      gccnt*sizeof(  globval[0]) + gstnum + arrnum);
	if (gecnt)               memcpy(kcd->gasm     ,gasm     ,      gecnt*sizeof(     gasm[0]));
	if (numrxi)              memcpy(kcd->rxi      ,rxi      ,     numrxi*sizeof(      rxi[0]));
	if (gevalextnum)         memcpy(kcd->gevalext ,gevalext ,gevalextnum*sizeof( gevalext[0])); //need only the ptr's
	if (newvarnum)           memcpy(kcd->newvar   ,newvar   ,  newvarnum*sizeof(   newvar[0]));
	if (newvarplc)           memcpy(kcd->newvarnam,newvarnam,  newvarplc*sizeof(newvarnam[0]));

	kcd->stackdoubs = stackdoubs;

#if defined(_M_IX86) || defined(__i386__)
	*(short *)kcd->codestub = 0x5c7;
	*(long *)&kcd->codestub[2] = (long)&gkasm87cptr;
	*(long *)&kcd->codestub[6] = (long)kcd;
	kcd->codestub[10] = 0xe9;
	if ((newvar[0].parnum < 0) && ((newvar[0].r&0xf0000000) == KESP))
		  *(long *)&kcd->codestub[11] = ((long)kasm87c )-((long)&kcd->codestub[15]);
	else *(long *)&kcd->codestub[11] = ((long)kasm87cp)-((long)&kcd->codestub[15]);
#else

	//Increase codestub size to 32?
	//PPC guess:
	//lis r4,imm16hi
	//ori r4,r4,imm16lo
	//?mflr r5
	//addis r4,?r5,ha16(_glob-?)
	//stw r4,lo16(_glob-?)(r2)
	//b (kasm87c - &codestub[20?])


		//FIX: This temp hack allows 1 script in memory to run on a non-x86 platform
		//To fix it: machine code for the native architecture must be written to move and jump (like above)
	gkasm87cptr = (long)kcd;
	if ((newvar[0].parnum < 0) && ((newvar[0].r&0xf0000000) == KESP))
		  return((void *)kasm87c);
	else return((void *)kasm87cp);
#endif

	return(kcd);
}