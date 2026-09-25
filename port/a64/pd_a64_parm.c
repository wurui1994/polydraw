/* port/a64/pd_a64_parm.c —— 参数区（parmdat）的指针宽度 4 -> 8。
 *
 * ## 这是原文第 5 个洞，也是出图剩下的最后一格
 *
 * 原文是 32 位 x86 的口径：参数区里 **double 占 8 字节、指针占 4 字节**。
 *   * 写的一边：`kasm_interp.c:250/271` 的 `j += 4`（"4 byte variable/function pointer"）；
 *   * 分偏移的一边：`kasm_comp.c:169/174` 的 `espoff += 4`；
 *   * 读的一边：`kasm_interp.c:79` 的 `p[j] = (*(double **)p[j]) + q` 与
 *     `:152` 的 `*(long *)(…)` —— 这两处用的是 `double **` / `long`，
 *     **在 arm64 上是 8 字节**。
 *
 * 于是 `ldr x9,[x9,#8]` 读到半个指针加半个别的东西，段错误。一个指针参数侥幸能跑
 * （它后面没东西了），两个就错位 —— Ken 自带的例子 #4 就崩在这儿。
 *
 * ## 怎么补（不动原文一个字节）
 *
 * 两头一起改成 8：
 *   1. **写的一边**：这一份接管 `kasm87c` / `kasm87cp`（缝合文件把原文那两个改名成
 *      `*_x86`）。顺带修了原文的一处：它们读的是**全局** `newvar[]`/`gnumarg`
 *      （上一次编译留下的），我们读 `kcd->newvar`/`kcd->gnumarg` —— 多脚本才对；
 *   2. **分偏移的一边**：`espoff` 在 `kasm87comp`（62KB 的函数）中间，不去 fork 它 ——
 *      改为**事后重映射**：kcd 造好之后，把每一格 KESP/KPTR 操作数的偏移
 *      按"指针 8 字节"重算一遍。`newvar[]` 里记着每个参数的旧基址，
 *      逐个累加出新基址，然后 `gasm[]` / `rxi[]` / `newvar[]` 上所有
 *      KESP/KPTR 家族的偏移一起换。
 *
 * 为什么重映射是安全的：一格操作数的偏移要么正好落在某个参数的基址上，
 * 要么是"基址 + 格内偏移"（数组那种）。所以按"不大于它的最大基址"找归属、
 * 再把格内偏移原样带过去，两种形状都对。
 */
#if (COMPILE == 0)

#include <stdarg.h>

/* 一个参数在参数区里占几个字节。三档照 `kasm_comp.c:167-175` 的分法：
     double（parnum < 0 且家族是 KESP）-> 8；
     `&x`（家族是 KPTR）               -> 旧 4 / 新 8；
     函数指针（parnum >= 0）           -> 旧 4 / 新 8。 */
static long pd_a64_parm_isdouble (const newvartyp *nv)
{
	return((nv->parnum < 0) && ((nv->r&0xf0000000) == KESP));
}

#define PD_MAXARG 64

/* 第 14 个洞：**`gnumarg` 不是"参数个数"**，它是"参数列表解析完时的 `newvarnum`"。
 *
 * `newvar[]` 前头先摆的是全局 `STATIC` 声明（`kasm_main.c:222` 那一趟 parse_static
 * 攒出来的 `globnewvarnum` 格，家族在 `kasm_main.c:277` 被改成 KGLB），
 * 参数是接在它们**后面**的（`kasm_comp.c:163` 起）。所以真正的参数是
 * `newvar[globnewvarnum … gnumarg-1]` —— 而 kcd 里压根没记 globnewvarnum。
 *
 * 量到的证据（`ken/curvybuild.pss`，函数 `(N,A,B)` 三个数组参数）：
 *   gnumarg=13，newvar[0..9] 是 WALL/SECT/NUMSECTS/… 全是 e…（KGLB），
 *   newvar[10..12] 才是 N/A/B（a… = KPTR）。
 * 按"前 13 格都是参数"排新基址，A 就拿到了第 11 格的 88，于是
 * `*(double **)(parmdat+88)` 读到的是栈上的垃圾（量到 0x3），MOV 上段错误。
 *
 * 不用 globnewvarnum 也能分：参数的家族只会是 KESP（double）或 KPTR（`&x`/数组/
 * 函数指针），全局那些一律是 KGLB。所以按家族筛一遍就得到参数表。
 */
static long pd_a64_parms (const kcd_t *kcd, long *idx, long max)
{
	long i, n = 0;
	for(i=0;i<kcd->gnumarg;i++)
	{
		long fam = kcd->newvar[i].r&0xf0000000;
		if ((fam != KESP) && (fam != KPTR)) continue;   /* 全局（KGLB）不算参数 */
		if (n < max) idx[n] = i;
		n++;
	}
	if (n > max) n = max;
	return(n);
}

/* 一个旧偏移换成新的：找"不大于它的最大基址"那一格，把格内偏移原样带过去。
   于是"正好落在基址上"与"基址 + 格内偏移（数组）"两种形状都对。 */
static long pd_a64_remap (long o, const long *oldb, const long *newb, long n)
{
	long i, best = -1;
	for(i=0;i<n;i++)
		if ((oldb[i] <= o) && ((best < 0) || (oldb[i] > oldb[best]))) best = i;
	if (best < 0) return(o);
	return(newb[best] + (o-oldb[best]));
}

/* 把这一格 kcd 里所有 KESP/KPTR 的偏移从"指针 4 字节"换成"指针 8 字节"。 */
static void pd_a64_widen_parms (kcd_t *kcd)
{
	long oldb[PD_MAXARG], newb[PD_MAXARG], pidx[PD_MAXARG];
	long n, i, j, no = 0, anyptr = 0;

	n = pd_a64_parms(kcd,pidx,PD_MAXARG);
	if (n <= 0) return;

	for(i=0;i<n;i++)
	{
		oldb[i] = kcd->newvar[pidx[i]].r&0x0fffffff;   /* 旧基址就记在操作数里 */
		newb[i] = no;
		no += 8;                                       /* 新口径：每格都是 8 */
		if (!pd_a64_parm_isdouble(&kcd->newvar[pidx[i]])) anyptr = 1;
	}
	if (!anyptr) return;                          /* 全是 double：旧新一样，不用动 */

	/* 先换操作数（它们要按**旧**基址找归属），再换 newvar 自己。 */
	for(i=0;i<kcd->gecnt;i++)
		for(j=0;j<MAXPARMS;j++)
		{
			long fam = kcd->gasm[i].r[j].r&0xf0000000;
			if ((fam != KESP) && (fam != KPTR)) continue;
			kcd->gasm[i].r[j].r = fam + pd_a64_remap(kcd->gasm[i].r[j].r&0x0fffffff,oldb,newb,n);
		}
	for(i=0;i<kcd->numrxi;i++)
	{
		long fam = kcd->rxi[i].r&0xf0000000;
		if ((fam != KESP) && (fam != KPTR)) continue;
		kcd->rxi[i].r = fam + pd_a64_remap(kcd->rxi[i].r&0x0fffffff,oldb,newb,n);
	}
	for(i=0;i<kcd->newvarnum;i++)
	{
		long fam = kcd->newvar[i].r&0xf0000000;
		if ((fam != KESP) && (fam != KPTR)) continue;
		kcd->newvar[i].r = fam + pd_a64_remap(kcd->newvar[i].r&0x0fffffff,oldb,newb,n);
	}
}

/* ── 写的一边：`kasm87c` / `kasm87cp` ──
 * 与原文的差别有三处：指针那一档 `j += 4` 改成 8；`newvar`/`gnumarg` 换成
 * `kcd->` 那一份（原文读的是全局，多脚本时那是上一次编译留下的）；
 * 参数是按家族筛出来的那一串，不是 `newvar[1 … gnumarg)`（第 14 个洞）。 */
static double pd_a64_fill (char *parmdat, kcd_t *kcd, va_list *m, long j)
{
	long pidx[PD_MAXARG], n, i;
	n = pd_a64_parms(kcd,pidx,PD_MAXARG);
	for(i=1;i<n;i++)
	{
		if (j+8 > (long)(sizeof(double)*16)) break;    /* parmdat 只有 16 格 */
		if (pd_a64_parm_isdouble(&kcd->newvar[pidx[i]]))
			  { *(double *)&parmdat[j] = va_arg(*m,double); }
		else  { *(void  **)&parmdat[j] = va_arg(*m,void * ); }
		j += 8;
	}
	/* **JIT 那条路**（`port/a64/pd_a64_jitc.c`）：编得出来就跳编出来那一份，
	   编不出来（碰上还没接的指令）回 0，照旧走解释器。挂点只有这一句 ——
	   ABI 与 `kasm87c_run` 逐字相同，所以上头那一串"实参怎么摊"一个字节都不用改。 */
	{
		double (*jf)(char *, kcd_t *) = (double (*)(char *, kcd_t *))pd_a64_jitfn(kcd);
		if (jf)
		{
			if (pd_jit_get_mode() < 2) return(jf(parmdat,kcd));
			/* `PD_JIT=2` 差分：两条路都跑一趟，位级对不上就印。
			   **它会把副作用做两遍**（脚本要是写全局/数组，第二趟看到的是第一趟改过的）
			   —— 所以这一档只拿来查纯算术那一类（`bench/test-a64.sh`）。 */
			{
				double da = jf(parmdat,kcd);
				double dc = kasm87c_run(parmdat,kcd);
				if (memcmp(&da,&dc,sizeof(double)))
					fprintf(stderr,"[jit] 差分不一致：jit %.17g / 解释器 %.17g\n",da,dc);
				return(dc);
			}
		}
	}
	return(kasm87c_run(parmdat,kcd));
}

double __cdecl kasm87c (double first, ...)
{
	va_list m;
	kcd_t *kcd = (kcd_t *)gkasm87cptr;
	char parmdat[sizeof(double)*16];
	double d;
	*(double *)&parmdat[0] = first;
	va_start(m,first);
	d = pd_a64_fill(parmdat,kcd,&m,8);
	va_end(m);
	return(d);
}

double __cdecl kasm87cp (double *first, ...)
{
	va_list m;
	kcd_t *kcd = (kcd_t *)gkasm87cptr;
	char parmdat[sizeof(double)*16];
	double d;
	*(double **)&parmdat[0] = first;
	va_start(m,first);
	d = pd_a64_fill(parmdat,kcd,&m,8);   /* 原文这儿是 4（指针 4 字节） */
	va_end(m);
	return(d);
}

#endif
