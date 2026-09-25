/* port/a64/pd_a64_run.c —— `kasm87c_run` 的 arm64 替身（**由 tools/mkrun.mjs 生成**）。
 *
 * ## 为什么要它：Apple 的 arm64 上"变参"与"定参"不是同一套调用约定
 *
 * 原文把宿主函数指针声明成 `double (__cdecl *)(double,...)`（`kasm_interp.c:140`），
 * 然后 `dafunc(*p[1],*p[2],*p[3])`。在 x86 上变参与定参一样（实参全压栈），所以没事；
 * **Apple 的 arm64 上变参实参一律走栈**，而 `qglVertex3d(double,double,double)` 是定参、
 * 从 d0/d1/d2 取 —— 于是只有第一个实参对。
 *
 * 量到的：脚本写 `glVertex(-1,-1,-2)`，`qglVertex3d` 收到 `(-1,-2,-1)`。
 * 这就是"画面全黑"的根：调用都发生了、GL 不报错、FBO 也绑对了，只是坐标是垃圾。
 *
 * ## 与原文的差别只有两处（`diff -u eval/kasm_interp.c` 那一段看得见）
 *
 *   1. 那个大 switch 里 **52 处** `dafunc(…)` 全换成精确原型的强转
 *      （原型串就在同一行的 `strncmp(cptr,"ddD",…)` 里：d -> double、D -> double *）；
 *   2. switch 前面插一档：**脚本自己的函数是真变参**（走 pd_a64_jit.c 的 thunk，
 *      尾跳到 `kasm87c`/`kasm87cp`），不能按定参调 —— `pd_a64_owns()` 把它分出去，
 *      自己摊一份 parmdat 直接递归调 `kasm87c_run`。
 *
 * 顺带记一笔原文的既有缺口（**不是我们弄的**）：那个 switch 的原型只认 `d`/`D`，
 * 没有 `C`（char *）那一档 —— 所以 `printf("…")` 这种带字符串的宿主函数在
 * COMPILE==0 那条路上本来就不会被调（而且 `myprintf` 自己是真变参，
 * 按定参强转也不对）。要补得另起一档。
 */
#if (COMPILE == 0)

/* 下面那个 `pd_a64_call_script` 要递归调它，所以先报个名。 */
double kasm87c_run (char *parmdat, kcd_t *kcd);

/* 脚本函数那一档：按原型串摊一份 parmdat（指针原样放，double 取值），
   然后直接递归调 `kasm87c_run` —— kcd 记在 thunk 那一格的尾巴上。 */
static double pd_a64_call_script (void *thunk, const char *proto, double **p, long n)
{
	char parmdat[sizeof(double)*16];
	kcd_t *kcd = *(kcd_t **)&((char *)thunk)[56];
	long i, j = 0;
	if (!kcd) return(0.0);
	for(i=1;i<=n;i++)
	{
		if (j+8 > (long)sizeof(parmdat)) break;
		if (proto[i-1] == 'D') *(void **)&parmdat[j] = (void *)p[i];
		else                   *(double *)&parmdat[j] = *p[i];
		j += 8;
	}
	return(kasm87c_run(parmdat,kcd));
}

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

				//—— 这里是本机（arm64/osx）补上的那一格，见 port/a64/pd_a64_run.c 的头注 ——
				//脚本自己的函数走的是 pd_a64_jit.c 造的 thunk（尾跳到真变参的 kasm87c/
				//kasm87cp）。它跟宿主那些定参的 C 函数调用约定不一样，所以先分出去：
				//自己摊一份 parmdat，直接递归调 kasm87c_run，压根不经过变参。
				if (pd_a64_owns((void *)dafunc))
					{ (*p[0]) = pd_a64_call_script((void *)dafunc,cptr,p,kcd->gasm[i].n); break; }

				switch(kcd->gasm[i].n) //This seems to be the only way to do pure C implementation; it sucks!
				{
					case 1:
						if (!strncmp(cptr,"d",1)) { (*p[0]) = ((double (__cdecl *)(double))dafunc)(*p[1]); break; }
						break;
					case 2:
						if (!strncmp(cptr,"dd",2)) { (*p[0]) = ((double (__cdecl *)(double,double))dafunc)(*p[1],*p[2]); break; }
						if (!strncmp(cptr,"dD",2)) { (*p[0]) = ((double (__cdecl *)(double,double *))dafunc)(*p[1], p[2]); break; }
						break;
					case 3:
						if (!strncmp(cptr,"ddd",3)) { (*p[0]) = ((double (__cdecl *)(double,double,double))dafunc)(*p[1],*p[2],*p[3]); break; }
						if (!strncmp(cptr,"ddD",3)) { (*p[0]) = ((double (__cdecl *)(double,double,double *))dafunc)(*p[1],*p[2], p[3]); break; }
						if (!strncmp(cptr,"dDD",3)) { (*p[0]) = ((double (__cdecl *)(double,double *,double *))dafunc)(*p[1], p[2], p[3]); break; }
						break;
					case 4:
						if (!strncmp(cptr,"dddd",4)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double))dafunc)(*p[1],*p[2],*p[3],*p[4]); break; }
						if (!strncmp(cptr,"dddD",4)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double *))dafunc)(*p[1],*p[2],*p[3], p[4]); break; }
						if (!strncmp(cptr,"ddDD",4)) { (*p[0]) = ((double (__cdecl *)(double,double,double *,double *))dafunc)(*p[1],*p[2], p[3], p[4]); break; }
						if (!strncmp(cptr,"dDDD",4)) { (*p[0]) = ((double (__cdecl *)(double,double *,double *,double *))dafunc)(*p[1], p[2], p[3], p[4]); break; }
						break;
					case 5:
						if (!strncmp(cptr,"ddddd",5)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5]); break; }
						if (!strncmp(cptr,"ddddD",5)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double *))dafunc)(*p[1],*p[2],*p[3],*p[4], p[5]); break; }
						if (!strncmp(cptr,"dddDD",5)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double *,double *))dafunc)(*p[1],*p[2],*p[3], p[4], p[5]); break; }
						if (!strncmp(cptr,"ddDDD",5)) { (*p[0]) = ((double (__cdecl *)(double,double,double *,double *,double *))dafunc)(*p[1],*p[2], p[3], p[4], p[5]); break; }
						if (!strncmp(cptr,"dDDDD",5)) { (*p[0]) = ((double (__cdecl *)(double,double *,double *,double *,double *))dafunc)(*p[1], p[2], p[3], p[4], p[5]); break; }
						break;
					case 6:
						if (!strncmp(cptr,"dddddd",6)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6]); break; }
						if (!strncmp(cptr,"dddddD",6)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double *))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5], p[6]); break; }
						if (!strncmp(cptr,"ddddDD",6)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double *,double *))dafunc)(*p[1],*p[2],*p[3],*p[4], p[5], p[6]); break; }
						if (!strncmp(cptr,"dddDDD",6)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double *,double *,double *))dafunc)(*p[1],*p[2],*p[3], p[4], p[5], p[6]); break; }
						if (!strncmp(cptr,"ddDDDD",6)) { (*p[0]) = ((double (__cdecl *)(double,double,double *,double *,double *,double *))dafunc)(*p[1],*p[2], p[3], p[4], p[5], p[6]); break; }
						if (!strncmp(cptr,"dDDDDD",6)) { (*p[0]) = ((double (__cdecl *)(double,double *,double *,double *,double *,double *))dafunc)(*p[1], p[2], p[3], p[4], p[5], p[6]); break; }
						break;
					case 7:
						if (!strncmp(cptr,"ddddddd",7)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double,double))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7]); break; }
						if (!strncmp(cptr,"ddddddD",7)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double,double *))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6], p[7]); break; }
						if (!strncmp(cptr,"dddddDD",7)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double *,double *))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5], p[6], p[7]); break; }
						if (!strncmp(cptr,"ddddDDD",7)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double *,double *,double *))dafunc)(*p[1],*p[2],*p[3],*p[4], p[5], p[6], p[7]); break; }
						if (!strncmp(cptr,"dddDDDD",7)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double *,double *,double *,double *))dafunc)(*p[1],*p[2],*p[3], p[4], p[5], p[6], p[7]); break; }
						if (!strncmp(cptr,"ddDDDDD",7)) { (*p[0]) = ((double (__cdecl *)(double,double,double *,double *,double *,double *,double *))dafunc)(*p[1],*p[2], p[3], p[4], p[5], p[6], p[7]); break; }
						if (!strncmp(cptr,"dDDDDDD",7)) { (*p[0]) = ((double (__cdecl *)(double,double *,double *,double *,double *,double *,double *))dafunc)(*p[1], p[2], p[3], p[4], p[5], p[6], p[7]); break; }
						break;
					case 8:
						if (!strncmp(cptr,"dddddddd",8)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double,double,double))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8]); break; }
						if (!strncmp(cptr,"dddddddD",8)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double,double,double *))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7], p[8]); break; }
						if (!strncmp(cptr,"ddddddDD",8)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double,double *,double *))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6], p[7], p[8]); break; }
						if (!strncmp(cptr,"dddddDDD",8)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double *,double *,double *))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5], p[6], p[7], p[8]); break; }
						if (!strncmp(cptr,"ddddDDDD",8)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double *,double *,double *,double *))dafunc)(*p[1],*p[2],*p[3],*p[4], p[5], p[6], p[7], p[8]); break; }
						if (!strncmp(cptr,"dddDDDDD",8)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double *,double *,double *,double *,double *))dafunc)(*p[1],*p[2],*p[3], p[4], p[5], p[6], p[7], p[8]); break; }
						if (!strncmp(cptr,"ddDDDDDD",8)) { (*p[0]) = ((double (__cdecl *)(double,double,double *,double *,double *,double *,double *,double *))dafunc)(*p[1],*p[2], p[3], p[4], p[5], p[6], p[7], p[8]); break; }
						if (!strncmp(cptr,"dDDDDDDD",8)) { (*p[0]) = ((double (__cdecl *)(double,double *,double *,double *,double *,double *,double *,double *))dafunc)(*p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8]); break; }
						break;
					case 9:
						if (!strncmp(cptr,"ddddddddd",9)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double,double,double,double))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],*p[9]); break; }
						if (!strncmp(cptr,"ddddddddD",9)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double,double,double,double *))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8], p[9]); break; }
						if (!strncmp(cptr,"dddddddDD",9)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double,double,double *,double *))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7], p[8], p[9]); break; }
						if (!strncmp(cptr,"ddddddDDD",9)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double,double *,double *,double *))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6], p[7], p[8], p[9]); break; }
						if (!strncmp(cptr,"dddddDDDD",9)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double *,double *,double *,double *))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5], p[6], p[7], p[8], p[9]); break; }
						if (!strncmp(cptr,"ddddDDDDD",9)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double,double *,double *,double *,double *,double *))dafunc)(*p[1],*p[2],*p[3],*p[4], p[5], p[6], p[7], p[8], p[9]); break; }
						if (!strncmp(cptr,"dddDDDDDD",9)) { (*p[0]) = ((double (__cdecl *)(double,double,double,double *,double *,double *,double *,double *,double *))dafunc)(*p[1],*p[2],*p[3], p[4], p[5], p[6], p[7], p[8], p[9]); break; }
						if (!strncmp(cptr,"ddDDDDDDD",9)) { (*p[0]) = ((double (__cdecl *)(double,double,double *,double *,double *,double *,double *,double *,double *))dafunc)(*p[1],*p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]); break; }
						if (!strncmp(cptr,"dDDDDDDDD",9)) { (*p[0]) = ((double (__cdecl *)(double,double *,double *,double *,double *,double *,double *,double *,double *))dafunc)(*p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]); break; }
						break;
					case 10: (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double,double,double,double,double))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],*p[9],*p[10]); break;
					case 11: (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double,double,double,double,double,double))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],*p[9],*p[10],*p[11]); break;
					case 12: (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double,double,double,double,double,double,double))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],*p[9],*p[10],*p[11],*p[12]); break;
					case 13: (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double,double,double,double,double,double,double,double))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],*p[9],*p[10],*p[11],*p[12],*p[13]); break;
					case 14: (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double,double,double,double,double,double,double,double,double))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],*p[9],*p[10],*p[11],*p[12],*p[13],*p[14]); break;
					case 15: (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double,double,double,double,double,double,double,double,double,double))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],*p[9],*p[10],*p[11],*p[12],*p[13],*p[14],*p[15]); break;
					case 16: (*p[0]) = ((double (__cdecl *)(double,double,double,double,double,double,double,double,double,double,double,double,double,double,double,double))dafunc)(*p[1],*p[2],*p[3],*p[4],*p[5],*p[6],*p[7],*p[8],*p[9],*p[10],*p[11],*p[12],*p[13],*p[14],*p[15],*p[16]); break;
					//how to do unlimited cases without using switch?
				}
				break;
				}
		}
	}
	gvlp -= kcd->stackdoubs; return(*p[0]);
}

#endif
