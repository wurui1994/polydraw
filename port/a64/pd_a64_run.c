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
/* JIT 在这个文件**后头**才 include 进来（它要 kcd_t），所以也先报个名。 */
static void *pd_a64_jitfn (kcd_t *kcd);

/* —— 本机补的诊断（`PD_RUNDBG=1` 打开）——
 *
 * `plst[]` 只填了六族（KECX/KEDX/KESP/KPTR/KIMM/KGLB），十六格里剩下的从来没人写。
 * 要是有操作数带着别的族进到这儿，算出来的 `p[j]` 就是栈上的垃圾 ——
 * 语料里十份脚本崩在 TIMES / PEEK / POKE 上，形状正是这个。
 * 所以先把十六格全填成毒值，再逐个操作数查：撞上毒值就把族号印出来（每族只印一次）。
 */
#define PD_PLST_POISON ((long)0x5bad5bad5bad5badLL)
static int pd_run_dbg = -1;
static void pd_a64_chk (const long *plst, long r, long op, long which)
{
	static int seen[16];
	long fam = ((unsigned long)r)>>28;
	if (plst[fam] != PD_PLST_POISON) return;
	if (seen[fam]) return;
	seen[fam] = 1;
	fprintf(stderr,"[run] plst 那一族没人填：fam=%lx r=%08lx（第 %ld 条指令的第 %ld 个操作数）\n",
		fam,(unsigned long)r,op,which);
}

/* 第二格诊断：**算完的 p[j] 落在哪儿**。`plst` 那一格只能查"族有没有人填"，
 * 查不出"族对、偏移不对"。这儿把已知的几块地盘列出来（kcd 那一大块、gvl 值栈、
 * parmdat、gstatmem），不落在里头就印一行。每个"族+指令"只印一次，免得刷屏。 */
static void pd_a64_chkp (const kcd_t *kcd, const char *parmdat, long r, const double *q, long op, long which)
{
	static long seen[64]; static int nseen = 0;
	long fam = ((unsigned long)r)>>28, key = (fam<<20)+(op&0xfffff), i;
	const char *pc = (const char *)q;
	const char *lo, *hi;
	if (!q) return;
	/* kcd 那一大块（头 + data 里的 globval/gasm/rxi/gevalext/newvar/newvarnam） */
	lo = (const char *)kcd; hi = lo + sizeof(kcd_t)
		+ kcd->gccnt*(long)sizeof(double) + kcd->gstnum + kcd->arrnum
		+ kcd->gecnt*(long)sizeof(gasmtyp) + kcd->numrxi*(long)sizeof(rtyp)
		+ kcd->gevalextnum*(long)sizeof(evalextyp) + kcd->newvarnum*(long)sizeof(newvartyp)
		+ kcd->newvarplc;
	if ((pc >= lo) && (pc < hi)) return;
	if ((pc >= (const char *)gvl) && (pc < (const char *)(gvl+65536))) return;   /* 值栈 */
	if ((pc >= parmdat) && (pc < parmdat+sizeof(double)*16)) return;             /* 参数区 */
	if (gstatmem && (pc >= (const char *)gstatmem) && (pc < (const char *)gstatmem+kcd->arrnum)) return;
	for(i=0;i<nseen;i++) if (seen[i] == key) return;
	if (nseen < 64) seen[nseen++] = key;
	fprintf(stderr,"[run] 指针落在地盘外：fam=%lx r=%08lx p=%p（第 %ld 条指令的第 %ld 个操作数）"
		" gstatmem=%p arrnum=%ld\n",
		fam,(unsigned long)r,(const void *)q,op,which,(const void *)gstatmem,(long)kcd->arrnum);
}

/* 第三格诊断：**执行前** p[0..2] 里有没有落在头一页的（基址 0 + 小偏移）。
 * 崩的地址是 0x0 / 0x12 这种，说明某一族的基址压根是 0（最可能是 gstatmem==0
 * 却仍有 KGLB 操作数，或 gevalext[].ptr 是空）。这儿把指令号、opcode 和三个
 * 操作数的 r/q 原样印出来，印完直接 return 免得真的崩。 */
static int pd_a64_chk0 (const kcd_t *kcd, long i, double **p)
{
	long j, bad = -1;
	for(j=0;j<3;j++) if (((unsigned long)p[j]) < 4096UL) { bad = j; break; }
	if (bad < 0) return(0);
	fprintf(stderr,"[run] 操作数落在头一页：第 %ld 条指令 f=%d 的第 %ld 个操作数 p=%p\n",
		i,(int)kcd->gasm[i].f,bad,(void *)p[bad]);
	for(j=0;j<3;j++)
		fprintf(stderr,"[run]   r[%ld]: r=%08lx q=%ld p=%p\n",
			j,(unsigned long)kcd->gasm[i].r[j].r,(long)kcd->gasm[i].r[j].q,(void *)p[j]);
	fprintf(stderr,"[run]   gstatmem=%p kcd->arrnum=%ld kcd->globval=%p gccnt=%ld gstnum=%ld\n",
		(void *)gstatmem,(long)kcd->arrnum,(void *)kcd->globval,(long)kcd->gccnt,(long)kcd->gstnum);
	fprintf(stderr,"[run]   gnumarg=%ld newvarnum=%ld\n",(long)kcd->gnumarg,(long)kcd->newvarnum);
	for(j=0;j<kcd->gnumarg;j++)
		fprintf(stderr,"[run]   newvar[%ld]: r=%08lx parnum=%d maxind=%d nam=%s\n",
			j,(unsigned long)kcd->newvar[j].r,(int)kcd->newvar[j].parnum,(int)kcd->newvar[j].maxind,
			&kcd->newvarnam[kcd->newvar[j].nami]);
	fflush(stderr);
	return(1);
}

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
	/* **这儿也要问一句 JIT**（`port/a64/pd_a64_jitc.c`）：脚本函数的递归全走这条路，
	   不问的话被调那一份永远在解释器上跑 —— `fib(20)` 量出来只快 1.18 倍就是这个。
	   那一份在这个文件后头才 include 进来，所以上头报了个名。 */
	{
		double (*jf)(char *, kcd_t *) = (double (*)(char *, kcd_t *))pd_a64_jitfn(kcd);
		if (jf) return(jf(parmdat,kcd));
	}
	return(kasm87c_run(parmdat,kcd));
}

double kasm87c_run (char *parmdat, kcd_t *kcd)
{
	double *p[17];
	long i, j, k, plst[16];

	if (kcd->gecnt <= 0) return(0.0);


		//—— 本机补的：先把十六格填成毒值（见上面 pd_a64_chk 的注） ——
	if (pd_run_dbg < 0) pd_run_dbg = (getenv("PD_RUNDBG") != 0);
	if (pd_run_dbg) { for(i=0;i<16;i++) plst[i] = PD_PLST_POISON; }

	plst[((unsigned long)KECX)>>28] = ((long)gvlp        -KECX);
	plst[((unsigned long)KEDX)>>28] = ((long)kcd->globval-KEDX);
	plst[((unsigned long)KESP)>>28] = ((long)parmdat     -KESP);
	plst[((unsigned long)KPTR)>>28] = ((long)parmdat     -KPTR);
	plst[((unsigned long)KIMM)>>28] = ((long)            -KIMM);
	plst[((unsigned long)KGLB)>>28] = -((long)KGLB) /*本机改：原文把 gstatmem 算了两遍*/;

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
			if (pd_run_dbg) pd_a64_chk(plst,kcd->gasm[i].r[j].r,i,j);
			if ((kcd->gasm[i].r[j].r&0xf0000000) == KPTR) p[j] = (*(double **)p[j]) + (kcd->gasm[i].r[j].q);
			if ((kcd->gasm[i].r[j].r&0xf0000000) == KIMM) p[j] = (double *)(((long)kcd->gevalext[((long)p[j])].ptr)+kcd->gasm[i].r[j].q*8);
			if ((kcd->gasm[i].r[j].r&0xf0000000) == KGLB) p[j] = (double *)((gstatmem + ((long)p[j]))+kcd->gasm[i].r[j].q*8);
			if ((kcd->gasm[i].r[j].r&0xf0000000) == KEDX) p[j] += kcd->gasm[i].r[j].q;
			if (pd_run_dbg) pd_a64_chkp(kcd,parmdat,kcd->gasm[i].r[j].r,p[j],i,j);
		}

		//—— 本机补的诊断（PD_RUNDBG=1）：基址是 0 的操作数，印完就退出这一趟 ——
		if (pd_run_dbg && pd_a64_chk0(kcd,i,p)) { gvlp -= kcd->stackdoubs; return(0.0); }

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
				if (pd_run_dbg) pd_a64_chk(plst,rp->r,i,3);
				if ((rp->r&0xf0000000) == KPTR) p[3] = (*(double **)p[3]) + (rp->q);
				if ((rp->r&0xf0000000) == KIMM) p[3] = (double *)(((long)kcd->gevalext[((long)p[3])].ptr)+rp->q*8);
				if ((rp->r&0xf0000000) == KGLB) p[3] = (double *)((gstatmem + ((long)p[3]))+rp->q*8);
				if (pd_run_dbg) pd_a64_chkp(kcd,parmdat,rp->r,p[3],i,3);

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
					if (pd_run_dbg) pd_a64_chk(plst,rp->r,i,j);
					if ((rp->r&0xf0000000) == KPTR) p[j] = (*(double **)p[j]) + (rp->q);
					if ((rp->r&0xf0000000) == KIMM) p[j] = (double *)(((long)kcd->gevalext[((long)p[j])].ptr)+rp->q*8);
					if ((rp->r&0xf0000000) == KGLB) p[j] = (double *)((gstatmem + ((long)p[j]))+rp->q*8);
					if (pd_run_dbg) pd_a64_chkp(kcd,parmdat,rp->r,p[j],i,j);
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

				//—— 第 18 个洞：原文那个 switch 只认 `d`/`D`，**没有 `C`（char *）那一档** ——
				//于是 `glsettex(0,"earth.jpg")` / `glsetshader("v","f")` 这些带字符串的宿主
				//函数在 COMPILE==0 那条路上**压根不会被调**（switch 一路 strncmp 全不中，
				//直接 break）—— 而且一声不响：`tex[0].tar` 还是 0，接着 `glbindtexture(0)`
				//拿 tar=0 去调，GL 报 INVALID_ENUM，采样器读到默认贴图 -> 采出来是白的。
				//量到的（PD_TEXDBG=1）：`[tex] bind tar=0 name=0 err=INVALID_ENUM`。
				//带字符串的原型全语料只有五种（`pd_script.c` 那张 myext[] 里数过）：
				//  C（mountzip/glgetuniformloc/glgetattribloc）、dC（glsettex）、
				//  dCd（glsettex 三参）、CC（glsetshader 两参）、CCC（glsetshader 三参）。
				//注意：原型串**不是 NUL 结尾**的，后面紧跟着函数名（量到 |dCGLSETTEX|），
				//所以只能像原文那样按长度 strncmp，strcmp 会全不中。
				//字符串操作数在 globval 里（KSTR 在 kasm_comp.c:313 被改成 KEDX+gccnt*8），
				//所以 p[j] 本身就是串的地址，强转 char * 即可。
				//printf 那一族仍然不接：`myprintf` 自己是真变参，按定参强转不对。
				//同理，找 `C` 也只能在前 n 个字符里找：用 strchr 会一路扫进后面的函数名，
				//于是 `gltexcoord`（|ddGLTEXCOORD|）这种压根没有字符串参的调用也会进这一档
				//—— 行为上无害（下面全不中就落回原路），但 curvybuild 两帧就白进 18 万次。
				long cn = kcd->gasm[i].n;
				if (memchr(cptr,'C',cn))
				{
					if ((cn == 1) && (!strncmp(cptr,"C",1)))
						{ (*p[0]) = ((double (__cdecl *)(char *))dafunc)((char *)p[1]); break; }
					if ((cn == 2) && (!strncmp(cptr,"dC",2)))
						{ (*p[0]) = ((double (__cdecl *)(double,char *))dafunc)(*p[1],(char *)p[2]); break; }
					if ((cn == 2) && (!strncmp(cptr,"CC",2)))
						{ (*p[0]) = ((double (__cdecl *)(char *,char *))dafunc)((char *)p[1],(char *)p[2]); break; }
					if ((cn == 3) && (!strncmp(cptr,"dCd",3)))
						{ (*p[0]) = ((double (__cdecl *)(double,char *,double))dafunc)(*p[1],(char *)p[2],*p[3]); break; }
					if ((cn == 3) && (!strncmp(cptr,"CCC",3)))
						{ (*p[0]) = ((double (__cdecl *)(char *,char *,char *))dafunc)((char *)p[1],(char *)p[2],(char *)p[3]); break; }
					if (pd_run_dbg) fprintf(stderr,"[run] 带字符串的原型没接：|%.*s| n=%ld\n",(int)cn,cptr,(long)cn);
				}

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
