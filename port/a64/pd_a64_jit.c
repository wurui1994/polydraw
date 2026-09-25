/* port/a64/pd_a64_jit.c —— arm64 的 codestub：把 kasm87 的产物变成真能调用的函数指针。
 *
 * ## 为什么需要这一份
 *
 * 原文在非 x86 上有三个洞，都在 `#else` 分支里明写着（不是我猜的）：
 *
 *   1. `kasm_interp.c:336-354`：`codestub` 的机器码只有 x86 那一份。非 x86 上它写的是
 *      "PPC guess" 一段注释，然后一句 hack —— 把 kcd 塞进全局 `gkasm87cptr`、
 *      **直接把解释器入口 `kasm87c`/`kasm87cp` 的地址当函数指针交回去**。
 *      注释自己说了：`This temp hack allows 1 script in memory to run`。
 *      于是多函数脚本必崩（子函数拿到的是 main 的 kcd），而且交回去的是**代码段地址**；
 *   2. `kasm_main.c:403`（`kasm87` 的收尾）：`??? not implemented .. need to fix .. sorry :/`；
 *   3. `kasm_comp.c:22-32`（`kasm87free`）与 `:1-20`（`kasm87jumpback`）读的是
 *      `f-FUNCBYTEOFFS` 那三个头字 —— COMPILE==0 时 `FUNCBYTEOFFS` 是 0，
 *      于是它们把代码/结构体头当头字读，`free()` 一个代码段地址直接 abort。
 *
 * ## 这一份怎么补
 *
 * 一格 96 字节的**槽**（自己 mmap 的可执行页里），前 56 字节是 14 条 arm64 指令：
 *
 *     movz/movk x16 = &gkasm87cptr     (4 条，全立即数，不需要重定位)
 *     movz/movk x17 = kcd              (4 条)
 *     str  x17, [x16]                  (1 条)
 *     movz/movk x16 = kasm87c / kasm87cp (4 条)
 *     br   x16                         (1 条)
 *
 * x16/x17 是 IP0/IP1，调用边界上本来就是可丢的，所以这是一次干净的尾跳转：
 * d0-d7 与栈上那些变参一个字节都没动过 —— 正好是 `double(double,...)` 要的。
 * 后 40 字节是我们自己的记账（kcd / gstatmem / 在用标记），`kasm87free` 靠它收尾。
 *
 * 为什么不写进 `kcd->codestub`（原文的地方）：那 16 个字节在 malloc 的块里，
 * 而 macOS 的 arm64 不给 RWX；把 kcd 整块设成 RX，解释器就没法往 `globval` 写了。
 * 所以代码与数据分开：数据留在 malloc 里，代码在我们自己的页里。
 */
#if (COMPILE == 0)

#include <sys/mman.h>
#include <unistd.h>

/* 这三个定义在后面两份里（pd_a64_parm.c / 本文件用得到的那两个入口），
   先报个名 —— C 里 static 的前向声明加后面的定义是合法的。 */
static void pd_a64_widen_parms (kcd_t *kcd);
double __cdecl kasm87c (double first, ...);
double __cdecl kasm87cp (double *first, ...);

#define PD_SLOT 96
#define PD_SLOT_CODE 56
#define PD_PAGE (64*1024)
#define PD_MAXPAGE 64

static char *pd_a64_page[PD_MAXPAGE];
static long pd_a64_pagenum = 0;
static long pd_a64_used[PD_MAXPAGE];

/* movz/movk 一族：`0xD2800000`（movz）/ `0xF2800000`（movk），
   位段是 hw<<21 | imm16<<5 | Rd（ARMv8 C6.2.190/C6.2.187）。 */
static void pd_a64_movabs (unsigned int *o, int rd, unsigned long long v)
{
	int i;
	for(i=0;i<4;i++)
	{
		unsigned int im = (unsigned int)((v>>(i*16))&0xffff);
		o[i] = ((i == 0) ? 0xD2800000u : 0xF2800000u) | ((unsigned int)i<<21) | (im<<5) | (unsigned int)rd;
	}
}

/* 换一页的保护位。写的时候 RW、跑的时候 RX —— 绝不要 RWX（arm64 macOS 不给）。 */
static int pd_a64_protect (long pi, int exec)
{
	return(mprotect(pd_a64_page[pi],PD_PAGE,exec ? (PROT_READ|PROT_EXEC) : (PROT_READ|PROT_WRITE)) == 0);
}

/* 找一格空槽：先在已有的页里找"标记是 0"的（`kasm87free` 放回来的），再往后铺，
   都没有就新开一页。回 `{页号, 槽地址}`，开不出来回 0。 */
static char *pd_a64_slot (long *pio)
{
	long pi, at;
	for(pi=0;pi<pd_a64_pagenum;pi++)
	{
		for(at=0;at<pd_a64_used[pi];at+=PD_SLOT)
			if (!*(long *)&pd_a64_page[pi][at+72]) { *pio = pi; return(&pd_a64_page[pi][at]); }
		if (pd_a64_used[pi]+PD_SLOT <= PD_PAGE)
			{ at = pd_a64_used[pi]; pd_a64_used[pi] += PD_SLOT; *pio = pi; return(&pd_a64_page[pi][at]); }
	}
	if (pd_a64_pagenum >= PD_MAXPAGE) return(0);
	pi = pd_a64_pagenum;
	pd_a64_page[pi] = (char *)mmap(0,PD_PAGE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
	if (pd_a64_page[pi] == (char *)MAP_FAILED) return(0);
	pd_a64_pagenum++; pd_a64_used[pi] = PD_SLOT; *pio = pi;
	return(pd_a64_page[pi]);
}

/* 造一格 thunk。`entry` 是 kasm87c 或 kasm87cp（原文那句 hack 交回来的就是它）。 */
static void *pd_a64_thunk (void *kcd, void *entry)
{
	unsigned int *o;
	char *slot;
	long pi = 0;

	slot = pd_a64_slot(&pi);
	if (!slot) return(0);
	if (!pd_a64_protect(pi,0)) return(0);

	o = (unsigned int *)slot;
	pd_a64_movabs(&o[0],16,(unsigned long long)(unsigned long)&gkasm87cptr);
	pd_a64_movabs(&o[4],17,(unsigned long long)(unsigned long)kcd);
	o[8] = 0xF9000000u | (16u<<5) | 17u;                 /* str x17, [x16]  */
	pd_a64_movabs(&o[9],16,(unsigned long long)(unsigned long)entry);
	o[13] = 0xD61F0000u | (16u<<5);                       /* br  x16         */

	*(void **)&slot[56] = kcd;                            /* 记账：这一格的 kcd     */
	*(long  *)&slot[64] = 0;                              /*       gstatmem（后补） */
	*(long  *)&slot[72] = 1;                              /*       在用             */

	if (!pd_a64_protect(pi,1)) return(0);
	__builtin___clear_cache(slot,slot+PD_SLOT_CODE);
	return(slot);
}

/* 这一格是不是我们造的（`kasm87free`/`kasm87jumpback` 要据此分流）。 */
static int pd_a64_owns (void *f)
{
	char *p = (char *)f;
	long pi;
	if (!p) return(0);
	for(pi=0;pi<pd_a64_pagenum;pi++)
		if ((p >= pd_a64_page[pi]) && (p < pd_a64_page[pi]+pd_a64_used[pi]))
			return(((p-pd_a64_page[pi])%PD_SLOT) == 0);
	return(0);
}

/* 往某一格的记账区写一个字。那一页平时是 RX，所以要临时开 RW —— 直接写会 SIGBUS
   （踩过：`pd_a64_set_statmem` 少了这一步，`1+2*3` 在算出值之前就 Bus error）。 */
static void pd_a64_poke (void *f, long off, long val)
{
	char *p = (char *)f;
	long pi;
	for(pi=0;pi<pd_a64_pagenum;pi++)
		if ((p >= pd_a64_page[pi]) && (p < pd_a64_page[pi]+pd_a64_used[pi]))
		{
			if (!pd_a64_protect(pi,0)) return;
			*(long *)&p[off] = val;
			pd_a64_protect(pi,1);
			return;
		}
}

/* 把一格还回去（记账那个字清 0，下一次 `pd_a64_slot` 就会捡它）。 */
static void pd_a64_release (void *f)
{
	pd_a64_poke(f,72,0);
}

/* `kasm87c_copyglob2struct` 的外壳：原文那一份照旧跑，回来之后做三件事 ——
   把参数区的指针宽度从 4 改成 8（第 5 个洞，见 pd_a64_parm.c）、
   把"代码段地址"换成我们的 thunk、把入口映到我们那两个 kasm87c/kasm87cp。
   kcd 从哪儿拿：原文返回前刚把它写进 `gkasm87cptr`。 */
static kcd_t *pd_a64_copyglob2struct (long stackdoubs)
{
	void *entry;
	kcd_t *kcd;

	entry = (void *)kasm87c_copyglob2struct(stackdoubs);
	if (!entry) return(0);
	kcd = (kcd_t *)gkasm87cptr;
	pd_a64_widen_parms(kcd);
	/* 原文那两个被缝合文件改名成了 `*_x86`（参数区还是 4 字节的口径），
	   照它选的那一档映到我们的同名实现上。 */
	entry = (entry == (void *)kasm87c_x86) ? (void *)kasm87c : (void *)kasm87cp;
	return((kcd_t *)pd_a64_thunk((void *)kcd,entry));
}

/* `kasm87` 的收尾里记一笔 gstatmem（原文把它写在 `v-FUNCBYTEOFFS+8` 那个头字里）。 */
static void pd_a64_set_statmem (void *f, long statmem)
{
	if (pd_a64_owns(f)) pd_a64_poke(f,64,statmem);
}

/**
 * 把这一格的 `kcd->gevalext[]` 按现在的 `gevalext[]` 刷一遍。
 *
 * 为什么非得有这一步（这是解释器那条路的第五个洞）：`kasm87comp` 在
 * `kasm87c_copyglob2struct` 里把 `gevalext[]` **抄了一份**进 kcd，而抄的时候
 * 自己那一格的 `.ptr` 还是空的 —— 编译次序是 i=funcnt-1 往 0 走，所以
 * "后面的函数调前面的"能碰上（已编好），**自己调自己（递归）与向后引用碰不上**，
 * 拿到 0 就 br 过去，段错误。
 *
 * x86 那条路是靠 `patch[]` 事后回填的（`kasm_main.c:397`：
 * `patch[j].lptr[0] += (long)gevalext[...].ptr`）；解释器没有码字要补，
 * 要补的是那份抄本。于是 `kasm87` 收尾时逐格刷一遍，等价。
 */
static void pd_a64_refresh_ext (void *f, evalextyp *src, long n)
{
	kcd_t *kcd;
	long i, m;
	if (!pd_a64_owns(f)) return;
	kcd = *(kcd_t **)&((char *)f)[56];
	if (!kcd) return;
	m = kcd->gevalextnum; if (m > n) m = n;
	for(i=0;i<m;i++) kcd->gevalext[i].ptr = src[i].ptr;
}

#endif
