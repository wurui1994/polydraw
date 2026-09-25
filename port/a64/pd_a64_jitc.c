/* port/a64/pd_a64_jitc.c —— **arm64 的 JIT**（`kasm87` 在这台机器上的等价物）。
 *
 * 为什么要有这一份：原文的 `kasm87`（`COMPILE==1`）把 `gasm[]` 那串三地址指令
 * 直接吐成 x87 机器码；arm64 上那一份等于没有，所以先前走的是 `COMPILE==0` 的
 * 纯 C 解释器（`kasm87c_run`）—— 每条指令要算三个操作数地址 + 过一趟 switch，
 * 于是 `ken/balls.pss` 这种每帧几千个球的脚本一帧要几百毫秒到秒级，语料里两份直接超时。
 * 这一份把**同一串 `gasm[]`** 吐成 A64 机器码，一格 `kcd` 编一次、之后每帧直接跳进去。
 *
 * ## 形状（为什么这么切）
 *
 * * **入口与 ABI 一个字节都不改**：编出来的函数签名就是 `double f(char *parmdat, kcd_t *)`
 *   —— 与 `kasm87c_run` 一模一样。挂点只有 `pd_a64_fill` 里那一句（原来直接调解释器）。
 *   于是"实参怎么摊进 parmdat""脚本函数怎么递归""宿主函数怎么调"全都照旧。
 * * **一格 kcd 编一次**，结果挂在 `kcd->jit`（见下面那张小表：kcd 是原文的结构体，
 *   不能加字段，所以拿一张按 kcd 指针查的表）。
 * * **遇到不会编的指令就整份放弃**（回 0），调用方退回解释器。所以这一份是
 *   **增量安全**的：编得出来的脚本快，编不出来的照旧对。
 * * 三个开关（环境变量）：`PD_JIT=0` 关掉、`PD_JIT=1` 开（默认）、
 *   `PD_JIT=2` **差分**：每次调用两条路都跑一趟、答案不一样就印出来。
 *   差分那一档是这一份的主判据 —— 位级对不上当场看得见。
 *
 * ## 寄存器怎么用
 *
 *   x19 = parmdat          （KESP / KPTR 那两族的底）
 *   x20 = kcd
 *   x21 = kcd->globval     （KEDX 那族的底）
 *   x22 = gstatmem         （KGLB 那族的底）
 *   x23 = gvlp（进来那一刻） （KECX 那族的底 —— 与解释器一样：`plst` 是在
 *                            `gvlp += stackdoubs` **之前**算的）
 *   x24 = &gvlp            （RETURN 那一刻要 `gvlp -= stackdoubs`）
 *   x25/x26 = 算地址用的零碎
 *   d0..d3 = 算术用的零碎
 *
 * 操作数地址**能在编译期算出来的就算出来**（KEDX/KGLB/KIMM/KECX/KESP 全是
 * "底 + 常量"），只有 KPTR 那一族要在运行期先取一次指针。
 */

#if (COMPILE == 0)

#include <sys/mman.h>

/* 一格编译缓冲。`bad` 一立起来就整份放弃（见头注）。 */
typedef struct
{
	unsigned int *c;   /* 指令流（A64 定长 4 字节） */
	long n, max;       /* 已写几条 / 最多几条 */
	int bad;           /* 碰上不会编的东西 */
} pd_jb;

static void pd_e (pd_jb *b, unsigned int w)
{
	if (b->n >= b->max) { b->bad = 1; return; }
	b->c[b->n++] = w;
}

/* ── A64 编码（每一条都照 ARM ARM 抄，写在注里省得下次再翻） ─────────────────── */

/* MOVZ/MOVK Xd, #imm16, LSL #(16*hw) */
static void pd_movz (pd_jb *b, int rd, unsigned long v, int hw)
	{ pd_e(b,0xD2800000u | ((unsigned)hw<<21) | (((unsigned)v&0xffff)<<5) | (unsigned)rd); }
static void pd_movk (pd_jb *b, int rd, unsigned long v, int hw)
	{ pd_e(b,0xF2800000u | ((unsigned)hw<<21) | (((unsigned)v&0xffff)<<5) | (unsigned)rd); }

/* Xd = 一格 64 位常量（四条，够用且不用管字面量池） */
static void pd_imm64 (pd_jb *b, int rd, unsigned long long v)
{
	pd_movz(b,rd,(unsigned long)( v        & 0xffff),0);
	pd_movk(b,rd,(unsigned long)((v>>16)   & 0xffff),1);
	pd_movk(b,rd,(unsigned long)((v>>32)   & 0xffff),2);
	pd_movk(b,rd,(unsigned long)((v>>48)   & 0xffff),3);
}

/* ADD Xd,Xn,Xm / ADD Xd,Xn,#imm12 / MOV Xd,Xn */
static void pd_addx (pd_jb *b, int rd, int rn, int rm)
	{ pd_e(b,0x8B000000u | ((unsigned)rm<<16) | ((unsigned)rn<<5) | (unsigned)rd); }
static void pd_movx (pd_jb *b, int rd, int rn)
	{ pd_e(b,0xAA0003E0u | ((unsigned)rn<<16) | (unsigned)rd); }   /* ORR Xd,XZR,Xn */

/* LDR/STR Dt,[Xn,#off]（off 必须是 8 的倍数且 off/8 <= 4095） */
static void pd_ldrd (pd_jb *b, int dt, int rn, long off)
	{ pd_e(b,0xFD400000u | ((unsigned)(off>>3)<<10) | ((unsigned)rn<<5) | (unsigned)dt); }
static void pd_strd (pd_jb *b, int dt, int rn, long off)
	{ pd_e(b,0xFD000000u | ((unsigned)(off>>3)<<10) | ((unsigned)rn<<5) | (unsigned)dt); }
/* LDR/STR Xt,[Xn,#off] */
static void pd_ldrx (pd_jb *b, int xt, int rn, long off)
	{ pd_e(b,0xF9400000u | ((unsigned)(off>>3)<<10) | ((unsigned)rn<<5) | (unsigned)xt); }
static void pd_strx (pd_jb *b, int xt, int rn, long off)
	{ pd_e(b,0xF9000000u | ((unsigned)(off>>3)<<10) | ((unsigned)rn<<5) | (unsigned)xt); }

/* 一元浮点：FMOV/FNEG/FABS/FSQRT/FRINTM(floor)/FRINTP(ceil)/FRINTZ(trunc) */
static void pd_f1 (pd_jb *b, unsigned int op, int dd, int dn)
	{ pd_e(b,op | ((unsigned)dn<<5) | (unsigned)dd); }
#define PD_FMOV   0x1E604000u
#define PD_FNEG   0x1E614000u
#define PD_FABS   0x1E60C000u
#define PD_FSQRT  0x1E61C000u
#define PD_FRINTM 0x1E654000u
#define PD_FRINTP 0x1E64C000u
#define PD_FRINTZ 0x1E65C000u

/* 二元浮点：FADD/FSUB/FMUL/FDIV */
static void pd_f2 (pd_jb *b, unsigned int op, int dd, int dn, int dm)
	{ pd_e(b,op | ((unsigned)dm<<16) | ((unsigned)dn<<5) | (unsigned)dd); }
#define PD_FADD 0x1E602800u
#define PD_FSUB 0x1E603800u
#define PD_FMUL 0x1E600800u
#define PD_FDIV 0x1E601800u

/* FCMP Dn,Dm / FCMP Dn,#0.0 */
static void pd_fcmp (pd_jb *b, int dn, int dm)
	{ pd_e(b,0x1E602000u | ((unsigned)dm<<16) | ((unsigned)dn<<5)); }
static void pd_fcmp0 (pd_jb *b, int dn)
	{ pd_e(b,0x1E602008u | ((unsigned)dn<<5)); }

/* FCSEL Dd,Dn,Dm,cond（条件真取 Dn） */
static void pd_fcsel (pd_jb *b, int dd, int dn, int dm, int cond)
	{ pd_e(b,0x1E600C00u | ((unsigned)dm<<16) | ((unsigned)cond<<12) | ((unsigned)dn<<5) | (unsigned)dd); }

/* CSET Wd,cond = CSINC Wd,WZR,WZR,!cond */
static void pd_cset (pd_jb *b, int rd, int cond)
	{ pd_e(b,0x1A9F07E0u | ((unsigned)(cond^1)<<12) | (unsigned)rd); }

/* SCVTF Dd,Wn（有符号 32 位整数 -> **double**）/ FCVTZS Xd,Dn（朝零截断）
 *
 * `0x1E620000` 里 bits23:22 = 01 是"目标是 double"。**先前写的是 `0x1E220000`
 * （bits23:22 = 00 = single）**，于是六个比较与 LAND/LOR/NEQU0/SGN/UNIT 算出来的
 * 是一格 float 的位模式塞在 d 里 —— 当条件用（非 0 即真）居然还对，所以 16 份
 * 表达式判据全过；一旦把比较结果**当数用**（`(a<b)*c`）就整份画不出来。
 * 语料扫描里 `空画面` 从 1 涨到 5 就是它。
 */
static void pd_scvtf_w (pd_jb *b, int dd, int wn)
	{ pd_e(b,0x1E620000u | ((unsigned)wn<<5) | (unsigned)dd); }
static void pd_fcvtzs_x (pd_jb *b, int xd, int dn)
	{ pd_e(b,0x9E780000u | ((unsigned)dn<<5) | (unsigned)xd); }

/* AND Xd,Xn,Xm / CMP Xn,Xm / CSEL Xd,Xn,Xm,cond / MOVZ 0 */
static void pd_andx (pd_jb *b, int rd, int rn, int rm)
	{ pd_e(b,0x8A000000u | ((unsigned)rm<<16) | ((unsigned)rn<<5) | (unsigned)rd); }
static void pd_cmpx (pd_jb *b, int rn, int rm)
	{ pd_e(b,0xEB00001Fu | ((unsigned)rm<<16) | ((unsigned)rn<<5)); }   /* SUBS XZR,Xn,Xm */
static void pd_cselx (pd_jb *b, int rd, int rn, int rm, int cond)
	{ pd_e(b,0x9A800000u | ((unsigned)rm<<16) | ((unsigned)cond<<12) | ((unsigned)rn<<5) | (unsigned)rd); }

/* 分支：B（imm26，单位是指令）/ B.cond（imm19）/ BLR Xn / RET */
static void pd_b (pd_jb *b, long rel)
	{ pd_e(b,0x14000000u | ((unsigned)rel & 0x03ffffffu)); }
static void pd_bcond (pd_jb *b, int cond, long rel)
	{ pd_e(b,0x54000000u | (((unsigned)rel & 0x7ffffu)<<5) | (unsigned)cond); }
static void pd_blr (pd_jb *b, int rn)
	{ pd_e(b,0xD63F0000u | ((unsigned)rn<<5)); }
static void pd_ret (pd_jb *b)
	{ pd_e(b,0xD65F03C0u); }

/* 条件码（够用的那几个） */
#define PD_EQ 0
#define PD_NE 1
#define PD_LT 11   /* 浮点：小于（无序时假） */
#define PD_LE 13
#define PD_GT 12
#define PD_GE 10
#define PD_MI 4
#define PD_PL 5
#define PD_HS 2    /* 无符号 >= */

/* 底寄存器（见头注那张表） */
#define PD_XPARM 19
#define PD_XKCD  20
#define PD_XGLOB 21
#define PD_XSTAT 22
#define PD_XECX  23
#define PD_XGVLP 24
#define PD_XS0   25
#define PD_XS1   26
#define PD_XS2   27

/**
 * **一格操作数的地址** -> `(*rn, *off)`，之后 `LDR/STR d,[*rn,#*off]` 就能用。
 *
 * 与解释器那五行（`kasm_interp.c:52-56`）逐条对着看：
 *   p = plst[fam>>28] + r          -> 底 + (r & 0x0fffffff)
 *   KPTR: p = *(double **)p + q    -> 运行期取一次指针，再加 q*8
 *   KIMM: p = gevalext[(long)p].ptr + q*8   -> **编译期**就能算出绝对地址
 *   KGLB: p = gstatmem + (long)p + q*8
 *   KEDX: p += q
 * 注意 KECX / KESP **不加 q**（原文就没加）。
 */
static void pd_opaddr (pd_jb *b, kcd_t *kcd, rtyp *r, int scratch, int *rn, long *off)
{
	unsigned long fam = (unsigned long)r->r & 0xf0000000u;
	long o = (long)((unsigned long)r->r & 0x0fffffffu);
	int base;

	switch(fam)
	{
		case KECX: base = PD_XECX;  break;
		case KESP: base = PD_XPARM; break;
		case KEDX: base = PD_XGLOB; o += (long)r->q*8; break;
		case KGLB: base = PD_XSTAT; o += (long)r->q*8; break;
		case KIMM:
			pd_imm64(b,scratch,(unsigned long long)((char *)kcd->gevalext[o].ptr + (long)r->q*8));
			*rn = scratch; *off = 0; return;
		case KPTR:
			/* 指针本身在 parmdat 里，偏移 o；取出来之后加 q*8。 */
			if ((o < 0) || (o & 7) || ((o>>3) > 4095)) { b->bad = 1; *rn = PD_XPARM; *off = 0; return; }
			pd_ldrx(b,scratch,PD_XPARM,o);
			base = scratch; o = (long)r->q*8;
			break;
		default: b->bad = 1; *rn = PD_XPARM; *off = 0; return;   /* KEAX/KFST/… 这条路上不该有 */
	}
	if ((o >= 0) && !(o & 7) && ((o>>3) <= 4095)) { *rn = base; *off = o; return; }
	/* 偏移太大或者不对齐：把整个地址算进 scratch。 */
	pd_imm64(b,scratch,(unsigned long long)o);
	pd_addx(b,scratch,base,scratch);
	*rn = scratch; *off = 0;
}

/** 把一格操作数**读**进 d 寄存器。 */
static void pd_load (pd_jb *b, kcd_t *kcd, rtyp *r, int dd, int scratch)
{
	int rn; long off;
	pd_opaddr(b,kcd,r,scratch,&rn,&off);
	pd_ldrd(b,dd,rn,off);
}

/** 把 d 寄存器**写**回一格操作数。 */
static void pd_store (pd_jb *b, kcd_t *kcd, rtyp *r, int dd, int scratch)
{
	int rn; long off;
	pd_opaddr(b,kcd,r,scratch,&rn,&off);
	pd_strd(b,dd,rn,off);
}

/* FMOV Dd,Xn（整数位模式搬进 d）—— 拿来造浮点常量。 */
static void pd_fmov_dx (pd_jb *b, int dd, int xn)
	{ pd_e(b,0x9E670000u | ((unsigned)xn<<5) | (unsigned)dd); }
/* Dd = 一格 double 常量（走 x25） */
static void pd_fconst (pd_jb *b, int dd, double v)
{
	unsigned long long u; memcpy(&u,&v,8);
	pd_imm64(b,PD_XS0,u);
	pd_fmov_dx(b,dd,PD_XS0);
}
/* STP/LDP Xt1,Xt2,[SP,#off]（off 是 8 的倍数、7 位有符号） */
static void pd_stp (pd_jb *b, int t1, int t2, long off)
	{ pd_e(b,0xA9000000u | ((unsigned)((off>>3)&0x7f)<<15) | ((unsigned)t2<<10) | (31u<<5) | (unsigned)t1); }
static void pd_ldp (pd_jb *b, int t1, int t2, long off)
	{ pd_e(b,0xA9400000u | ((unsigned)((off>>3)&0x7f)<<15) | ((unsigned)t2<<10) | (31u<<5) | (unsigned)t1); }
/* ADD/SUB SP,SP,#imm12 */
static void pd_spadd (pd_jb *b, long v)
	{ pd_e(b,0x910003FFu | ((unsigned)v<<10)); }
static void pd_spsub (pd_jb *b, long v)
	{ pd_e(b,0xD10003FFu | ((unsigned)v<<10)); }

#define PD_FRAME 256
/* 栈上给 `double *p[17]` 留的那一段（脚本函数那一支要把操作数地址摆成一张表，
   然后原样递给 `pd_a64_call_script` —— 那一份是现成的、已经判过的）。 */
#define PD_POFF 112

/* SUB Xd,Xn,Xm */
static void pd_subx (pd_jb *b, int rd, int rn, int rm)
	{ pd_e(b,0xCB000000u | ((unsigned)rm<<16) | ((unsigned)rn<<5) | (unsigned)rd); }
/* ORR Xd,Xn,Xm */
static void pd_orrx (pd_jb *b, int rd, int rn, int rm)
	{ pd_e(b,0xAA000000u | ((unsigned)rm<<16) | ((unsigned)rn<<5) | (unsigned)rd); }

/* 一格调用：绝对地址进 x25，BLR。**跨调用只有 x19..x27 是活的**（都是被调用方保存的），
   所有中间值都在内存里 —— 这是"一条指令一趟内存"这个笨办法唯一的好处。 */
static void pd_call (pd_jb *b, const void *fn)
{
	pd_imm64(b,PD_XS0,(unsigned long long)(unsigned long)fn);
	pd_blr(b,PD_XS0);
}

/* ── 开头与收尾 ─────────────────────────────────────────────────────────────── */

static void pd_prologue (pd_jb *b, kcd_t *kcd)
{
	pd_spsub(b,PD_FRAME);
	pd_stp(b,29,30, 0);
	pd_stp(b,19,20,16);
	pd_stp(b,21,22,32);
	pd_stp(b,23,24,48);
	pd_stp(b,25,26,64);
	pd_stp(b,27,28,80);
	pd_movx(b,PD_XPARM,0);                       /* x19 = parmdat */
	pd_movx(b,PD_XKCD,1);                        /* x20 = kcd */
	pd_ldrx(b,PD_XGLOB,PD_XKCD,(long)((char *)&kcd->globval - (char *)kcd));
	pd_imm64(b,PD_XSTAT,(unsigned long long)(unsigned long)&gstatmem);
	pd_ldrx(b,PD_XSTAT,PD_XSTAT,0);              /* x22 = gstatmem（它本身是一格 long） */
	pd_imm64(b,PD_XGVLP,(unsigned long long)(unsigned long)&gvlp);
	pd_ldrx(b,PD_XECX,PD_XGVLP,0);               /* x23 = 进来那一刻的 gvlp（KECX 的底） */
	/* gvlp += stackdoubs（原文在算完 plst 之后才加，所以 x23 留的是加之前那一份） */
	pd_imm64(b,PD_XS0,(unsigned long long)(kcd->stackdoubs*8));
	pd_addx(b,PD_XS1,PD_XECX,PD_XS0);
	pd_strx(b,PD_XS1,PD_XGVLP,0);
}

/* 收尾：d0 是返回值。`gvlp -= stackdoubs` 照原文那句**相对**减法（万一被调用方
   自己没还干净，相对减能保住那份偏差，绝对赋值会把它抹平 —— 与解释器保持一致）。 */
static void pd_epilogue (pd_jb *b, kcd_t *kcd)
{
	pd_ldrx(b,PD_XS1,PD_XGVLP,0);
	pd_imm64(b,PD_XS0,(unsigned long long)(kcd->stackdoubs*8));
	pd_subx(b,PD_XS1,PD_XS1,PD_XS0);
	pd_strx(b,PD_XS1,PD_XGVLP,0);
	pd_ldp(b,29,30, 0);
	pd_ldp(b,19,20,16);
	pd_ldp(b,21,22,32);
	pd_ldp(b,23,24,48);
	pd_ldp(b,25,26,64);
	pd_ldp(b,27,28,80);
	pd_spadd(b,PD_FRAME);
	pd_ret(b);
}

/* ADD Xd,Xn,#imm12 / ADD Xd,Xn,Xm,LSL #3 */
static void pd_addimm (pd_jb *b, int rd, int rn, long v)
	{ pd_e(b,0x91000000u | ((unsigned)v<<10) | ((unsigned)rn<<5) | (unsigned)rd); }
static void pd_addx_lsl3 (pd_jb *b, int rd, int rn, int rm)
	{ pd_e(b,0x8B000000u | ((unsigned)rm<<16) | (3u<<10) | ((unsigned)rn<<5) | (unsigned)rd); }

/* 一格操作数的**地址**（不是值）算进 rd。 */
static void pd_addrof (pd_jb *b, kcd_t *kcd, rtyp *r, int rd, int scratch)
{
	int rn; long off;
	pd_opaddr(b,kcd,r,scratch,&rn,&off);
	if (!off) { if (rn != rd) pd_movx(b,rd,rn); return; }
	if (off <= 4095) { pd_addimm(b,rd,rn,off); return; }
	pd_imm64(b,PD_XS2,(unsigned long long)off);
	pd_addx(b,rd,rn,PD_XS2);
}

/* 下标那一夹：照原文那一行 —— 2 的幂用 and，否则越界归 0。`maxind` 是编译期常量。 */
static void pd_bounds (pd_jb *b, long k, int rj)
{
	if ((k) && (!((k-1)&k)))
	{
		pd_imm64(b,PD_XS2,(unsigned long long)(k-1));
		pd_andx(b,rj,rj,PD_XS2);
		return;
	}
	pd_imm64(b,PD_XS2,(unsigned long long)k);
	pd_cmpx(b,rj,PD_XS2);
	pd_cselx(b,rj,31,rj,PD_HS);   /* 无符号 >= 就换成 XZR（0） */
}

/* 分支要回填的地方（A64 的相对偏移单位是"条指令"）。 */
typedef struct { long at; long tgt; int cond; } pd_fix;   /* cond < 0 表示无条件 B */

/* 几格小包装：体里就是原文那一行，这样 JIT 只管"摆参数、跳过去"，
   不用猜 `krand` 的返回类型、也不用自己拼 `log(a)/log(b)`。 */
static double pd_jit_rnd  (void) { return(((double)krand())*(double)oneover2_31); }
static double pd_jit_nrnd (void) { return(nrnd()); }
static double pd_jit_fact (double x) { return(fact(x)); }
static double pd_jit_logb (double x, double y) { return(log(x)/log(y)); }

/* ── 一条 gasm -> 一串 A64 ────────────────────────────────────────────────────
 *
 * 口径就是 `kasm_interp.c` 那张 switch，一条一条对着抄。**每条指令都是
 * "从内存读操作数 -> 算 -> 写回内存"**（没有跨指令的寄存器分配）—— 这一版就图个
 * 老实：比解释器省的是"每条算三个地址 + 一趟 switch + 一层间接"，不是寄存器。
 * 寄存器分配是下一刀（那时候再谈与 x87 那一份比）。
 */
static void pd_op (pd_jb *b, kcd_t *kcd, long i, pd_fix *fix, long *nfix)
{
	gasmtyp *a = &kcd->gasm[i];
	long f = a->f;

	switch(f)
	{
		case NUL: case NOP: return;

		case GOTO:
			fix[(*nfix)].at = b->n; fix[(*nfix)].tgt = a->r[0].r; fix[(*nfix)].cond = -1; (*nfix)++;
			pd_b(b,0);
			return;

		case RETURN:
			pd_load(b,kcd,&a->r[1],0,PD_XS0);
			pd_epilogue(b,kcd);
			return;

		case IF0: case IF1:
			pd_load(b,kcd,&a->r[2],1,PD_XS0);
			pd_fcmp0(b,1);
			fix[(*nfix)].at = b->n; fix[(*nfix)].tgt = a->r[0].r;
			fix[(*nfix)].cond = (f == IF0) ? PD_EQ : PD_NE; (*nfix)++;
			pd_bcond(b,0,0);
			return;

		/* 一元：照原文的次序（p[0] = op(p[1])） */
		case MOV:   pd_load(b,kcd,&a->r[1],0,PD_XS0); pd_store(b,kcd,&a->r[0],0,PD_XS0); return;
		case NEGMOV:pd_load(b,kcd,&a->r[1],1,PD_XS0); pd_f1(b,PD_FNEG,0,1);  pd_store(b,kcd,&a->r[0],0,PD_XS0); return;
		case FABS:  pd_load(b,kcd,&a->r[1],1,PD_XS0); pd_f1(b,PD_FABS,0,1);  pd_store(b,kcd,&a->r[0],0,PD_XS0); return;
		case SQRT:  pd_load(b,kcd,&a->r[1],1,PD_XS0); pd_f1(b,PD_FSQRT,0,1); pd_store(b,kcd,&a->r[0],0,PD_XS0); return;
		case FLOOR: pd_load(b,kcd,&a->r[1],1,PD_XS0); pd_f1(b,PD_FRINTM,0,1);pd_store(b,kcd,&a->r[0],0,PD_XS0); return;
		case CEIL:  pd_load(b,kcd,&a->r[1],1,PD_XS0); pd_f1(b,PD_FRINTP,0,1);pd_store(b,kcd,&a->r[0],0,PD_XS0); return;
		/* ROUND0 是"朝零取整"（原文写成 x>=0 ? floor(x) : -floor(-x)）= FRINTZ。 */
		case ROUND0: case ROUND0_32:
			pd_load(b,kcd,&a->r[1],1,PD_XS0); pd_f1(b,PD_FRINTZ,0,1); pd_store(b,kcd,&a->r[0],0,PD_XS0); return;
		case NEQU0:
			pd_load(b,kcd,&a->r[1],1,PD_XS0); pd_fcmp0(b,1);
			pd_cset(b,PD_XS1,PD_NE); pd_scvtf_w(b,0,PD_XS1); pd_store(b,kcd,&a->r[0],0,PD_XS0); return;
		/* SGN = (x>0)-(x<0)；UNIT = (x==0)*.5 + (x>0)（原文两行） */
		case SGN:
			pd_load(b,kcd,&a->r[1],1,PD_XS0); pd_fcmp0(b,1);
			pd_cset(b,PD_XS1,PD_GT); pd_cset(b,PD_XS2,PD_MI);
			pd_subx(b,PD_XS1,PD_XS1,PD_XS2); pd_scvtf_w(b,0,PD_XS1);
			pd_store(b,kcd,&a->r[0],0,PD_XS0); return;
		case UNIT:
			pd_load(b,kcd,&a->r[1],1,PD_XS0); pd_fcmp0(b,1);
			pd_cset(b,PD_XS1,PD_EQ); pd_scvtf_w(b,2,PD_XS1);
			pd_cset(b,PD_XS1,PD_GT); pd_scvtf_w(b,3,PD_XS1);
			pd_fconst(b,0,0.5); pd_f2(b,PD_FMUL,2,2,0); pd_f2(b,PD_FADD,0,2,3);
			pd_store(b,kcd,&a->r[0],0,PD_XS0); return;

		/* 二元算术 */
		case TIMES: case SLASH: case PLUS: case FADD: case MINUS:
			pd_load(b,kcd,&a->r[1],1,PD_XS0);
			pd_load(b,kcd,&a->r[2],2,PD_XS0);
			pd_f2(b,(f == TIMES) ? PD_FMUL : (f == SLASH) ? PD_FDIV : (f == MINUS) ? PD_FSUB : PD_FADD,0,1,2);
			pd_store(b,kcd,&a->r[0],0,PD_XS0); return;
		/* PERC：x - floor(x/|y|)*|y|（原文那一行，不是 fmod） */
		case PERC:
			pd_load(b,kcd,&a->r[1],1,PD_XS0);
			pd_load(b,kcd,&a->r[2],2,PD_XS0);
			pd_f1(b,PD_FABS,3,2);
			pd_f2(b,PD_FDIV,0,1,3); pd_f1(b,PD_FRINTM,0,0);
			pd_f2(b,PD_FMUL,0,0,3); pd_f2(b,PD_FSUB,0,1,0);
			pd_store(b,kcd,&a->r[0],0,PD_XS0); return;
		/* 比较：无序（NaN）一律当假 —— 与 C 的 `<` 一致。`!=` 用 NE（它含"无序"，
		   正是 C 里 NaN != x 为真）。 */
		case LES: case LESEQ: case MOR: case MOREQ: case EQU: case NEQU:
			pd_load(b,kcd,&a->r[1],1,PD_XS0);
			pd_load(b,kcd,&a->r[2],2,PD_XS0);
			pd_fcmp(b,1,2);
			pd_cset(b,PD_XS1,(f == LES) ? PD_MI : (f == LESEQ) ? 9 /*LS*/ :
				(f == MOR) ? PD_GT : (f == MOREQ) ? PD_GE : (f == EQU) ? PD_EQ : PD_NE);
			pd_scvtf_w(b,0,PD_XS1); pd_store(b,kcd,&a->r[0],0,PD_XS0); return;
		case LAND: case LOR:
			pd_load(b,kcd,&a->r[1],1,PD_XS0);
			pd_load(b,kcd,&a->r[2],2,PD_XS0);
			pd_fcmp0(b,1); pd_cset(b,PD_XS1,PD_NE);
			pd_fcmp0(b,2); pd_cset(b,PD_XS2,PD_NE);
			if (f == LAND) pd_andx(b,PD_XS1,PD_XS1,PD_XS2); else pd_orrx(b,PD_XS1,PD_XS1,PD_XS2);
			pd_scvtf_w(b,0,PD_XS1); pd_store(b,kcd,&a->r[0],0,PD_XS0); return;
		/* MIN/MAX 照原文那两行（拿 p[2] 与 p[1] 比，真取 p[2]） */
		case MIN: case MAX:
			pd_load(b,kcd,&a->r[1],1,PD_XS0);
			pd_load(b,kcd,&a->r[2],2,PD_XS0);
			pd_fcmp(b,2,1);
			pd_fcsel(b,0,2,1,(f == MIN) ? PD_MI : PD_GT);
			pd_store(b,kcd,&a->r[0],0,PD_XS0); return;

		/* ── 要调外头函数的那几族 ───────────────────────────────────────────────
		   都走"把实参摆进 d0(/d1)、BLR、结果在 d0"。调用前后没有活着的中间值
		   （全在内存里），所以不用存恢复任何 d 寄存器。
		   `RND`/`NRND`/`FACT`/`LOGB` 用这一份里的小包装，省得猜 `krand` 的返回类型
		   与 ABI —— 包装体里写的就是原文那一行，位级一致。 */
		case RND:  pd_call(b,(const void *)pd_jit_rnd);  pd_store(b,kcd,&a->r[0],0,PD_XS0); return;
		case NRND: pd_call(b,(const void *)pd_jit_nrnd); pd_store(b,kcd,&a->r[0],0,PD_XS0); return;
		case SIN: case COS: case TAN: case ASIN: case ACOS: case ATAN: case EXP: case LOG: case FACT:
		{
			const void *fn = (f == SIN) ? (const void *)(double (*)(double))sin :
				(f == COS) ? (const void *)(double (*)(double))cos :
				(f == TAN) ? (const void *)(double (*)(double))tan :
				(f == ASIN) ? (const void *)(double (*)(double))asin :
				(f == ACOS) ? (const void *)(double (*)(double))acos :
				(f == ATAN) ? (const void *)(double (*)(double))atan :
				(f == EXP) ? (const void *)(double (*)(double))exp :
				(f == LOG) ? (const void *)(double (*)(double))log :
				(const void *)pd_jit_fact;
			pd_load(b,kcd,&a->r[1],0,PD_XS0);
			pd_call(b,fn);
			pd_store(b,kcd,&a->r[0],0,PD_XS0);
			return;
		}
		case POW: case FMOD: case ATAN2: case LOGB:
		{
			const void *fn = (f == POW) ? (const void *)(double (*)(double,double))pow :
				(f == FMOD) ? (const void *)(double (*)(double,double))fmod :
				(f == ATAN2) ? (const void *)(double (*)(double,double))atan2 :
				(const void *)pd_jit_logb;
			/* 先把两格实参读到 d2/d3（d0/d1 要留给调用约定，而 pd_load 会用 x25 算地址） */
			pd_load(b,kcd,&a->r[1],2,PD_XS0);
			pd_load(b,kcd,&a->r[2],3,PD_XS0);
			pd_f1(b,PD_FMOV,0,2); pd_f1(b,PD_FMOV,1,3);
			pd_call(b,fn);
			pd_store(b,kcd,&a->r[0],0,PD_XS0);
			return;
		}

		/* ── 数组那两族（下标在运行期，`maxind` 在编译期） ─────────────────────── */
		case PEEK:
			pd_load(b,kcd,&a->r[2],2,PD_XS0);                  /* d2 = 下标 */
			pd_fcvtzs_x(b,PD_XS1,2);                           /* x26 = (long)下标 */
			pd_bounds(b,kcd->newvar[a->r[1].nv].maxind,PD_XS1);
			pd_addrof(b,kcd,&a->r[1],PD_XS0,PD_XS0);           /* x25 = 数组底 */
			pd_addx_lsl3(b,PD_XS0,PD_XS0,PD_XS1);
			pd_ldrd(b,0,PD_XS0,0);
			pd_store(b,kcd,&a->r[0],0,PD_XS0);
			return;
		case POKE: case POKETIMES: case POKESLASH: case POKEPERC: case POKEPLUS: case POKEMINUS:
		{
			rtyp *rp = &kcd->rxi[a->rxi];
			pd_load(b,kcd,rp,3,PD_XS0);                        /* d3 = 右边那个值 */
			pd_load(b,kcd,&a->r[2],2,PD_XS0);                  /* d2 = 下标 */
			pd_fcvtzs_x(b,PD_XS1,2);
			pd_bounds(b,kcd->newvar[a->r[1].nv].maxind,PD_XS1);
			pd_addrof(b,kcd,&a->r[1],PD_XS0,PD_XS0);
			pd_addx_lsl3(b,PD_XS0,PD_XS0,PD_XS1);              /* x25 = &p[1][j] */
			if (f == POKE) { pd_strd(b,3,PD_XS0,0); return; }
			pd_ldrd(b,1,PD_XS0,0);
			if (f == POKEPERC)
			{
				pd_f1(b,PD_FABS,2,3);
				pd_f2(b,PD_FDIV,0,1,2); pd_f1(b,PD_FRINTM,0,0);
				pd_f2(b,PD_FMUL,0,0,2); pd_f2(b,PD_FSUB,0,1,0);
			}
			else pd_f2(b,(f == POKETIMES) ? PD_FMUL : (f == POKESLASH) ? PD_FDIV :
				(f == POKEPLUS) ? PD_FADD : PD_FSUB,0,1,3);
			pd_strd(b,0,PD_XS0,0);
			return;
		}

		/* ── 宿主函数那一族（`USERFUNC`）───────────────────────────────────────
		 *
		 * 解释器在这儿最亏：每次调用都要按 `n` 过一趟 switch、再 `strncmp` 原型串、
		 * 才知道该怎么强转。而**原型串与被调的函数在编译期就定了**，所以 JIT 直接把
		 * 实参摆到位就行 —— 这就是这一族该由 JIT 干的理由。
		 *
		 * ABI：AAPCS64（Apple 的非变参就是它）—— **double 走 d0..d7、指针走 x0..x7，
		 * 两条序列各自数**。所以 `dCd` 这种原型是 d0=第一格、x0=第二格、d1=第三格。
		 *
		 * 这一版**只接宿主函数**（`n<=8`、原型只有 d/D）。脚本自己那些函数
		 * （`pd_a64_owns`）是真变参入口，Apple 上变参实参全走栈 —— 那是下一刀。
		 */
		case USERFUNC:
		{
			char *cptr;
			void *dafunc;
			long j, nd = 0, np = 0;
			if ((a->n < 1) || (a->n > 8)) { b->bad = 1; return; }
			if ((kcd->newvar[a->g].r & 0xf0000000) != KIMM) { b->bad = 1; return; }
			dafunc = (void *)kcd->gevalext[kcd->newvar[a->g].r & 0x0fffffff].ptr;
			cptr = &kcd->newvarnam[kcd->newvar[a->g].proti];
			/* 原型串**不是 NUL 结尾**的（后面紧跟函数名），所以只看前 n 个字符。
			   `C`（`char *`）与 `D`（`double *`）在这一层是同一件事：递的都是**操作数的
			   地址**（串操作数在 `globval` 里 —— KSTR 在 `kasm_comp.c:313` 被改成
			   `KEDX+gccnt*8`，所以那一格的地址就是串的地址。解释器那侧第 18 个洞
			   补的正是这一族）。 */
			for(j=0;j<a->n;j++)
				if ((cptr[j] != 'd') && (cptr[j] != 'D') && (cptr[j] != 'C')) { b->bad = 1; return; }
			/* **脚本自己那些函数**（`pd_a64_owns`）：入口是真变参（`kasm87c(double,...)`），
			   Apple 上变参实参全走栈 —— 与其在这儿重铺一遍，不如把操作数地址摆成
			   `double *p[17]` 那张表，原样递给现成的 `pd_a64_call_script`
			   （它就是为这件事写的，已经判过）。省下来的仍然是"每次调用一趟 strncmp"。 */
			if (pd_a64_owns(dafunc))
			{
				for(j=1;j<=a->n;j++)
				{
					rtyp *rp = (j <= 2) ? &a->r[j] : &kcd->rxi[a->rxi+j-3];
					pd_addrof(b,kcd,rp,PD_XS1,PD_XS0);
					pd_strx(b,PD_XS1,31,PD_POFF+j*8);
				}
				pd_imm64(b,0,(unsigned long long)(unsigned long)dafunc);
				pd_imm64(b,1,(unsigned long long)(unsigned long)cptr);
				pd_addimm(b,2,31,PD_POFF);
				pd_imm64(b,3,(unsigned long long)a->n);
				pd_call(b,(const void *)pd_a64_call_script);
				pd_store(b,kcd,&a->r[0],0,PD_XS0);
				return;
			}
			for(j=1;j<=a->n;j++)
			{
				rtyp *rp = (j <= 2) ? &a->r[j] : &kcd->rxi[a->rxi+j-3];
				if (cptr[j-1] == 'd')
				{
					if (nd >= 8) { b->bad = 1; return; }
					pd_load(b,kcd,rp,(int)nd,PD_XS0);
					nd++;
				}
				else   /* 'D' 与 'C' 都是"递地址" */
				{
					if (np >= 8) { b->bad = 1; return; }
					pd_addrof(b,kcd,rp,(int)np,PD_XS0);
					np++;
				}
			}
			pd_call(b,dafunc);
			pd_store(b,kcd,&a->r[0],0,PD_XS0);
			return;
		}

		default: b->bad = 1; return;
	}
}

/* ── 编一整份、落到可执行内存、按 kcd 记住 ─────────────────────────────────────
 *
 * `GOTO`/`IF*` 的目标是 `gasm` 下标，而原文那句是 `i = r[0].r;` 之后**再过一趟
 * `i++`** —— 所以真正的落点是 `r[0].r + 1`（这一格错了就是无声的死循环）。
 */
static void *pd_jit_build (kcd_t *kcd)
{
	pd_jb b;
	pd_fix *fix;
	long *at, nfix = 0, i, n = kcd->gecnt, cap;
	void *mem;

	if (n <= 0) return(0);
	cap = n*64 + 256;
	b.c = (unsigned int *)malloc((size_t)cap*4);
	at  = (long *)malloc((size_t)(n+1)*sizeof(long));
	fix = (pd_fix *)malloc((size_t)(n+2)*sizeof(pd_fix));
	if ((!b.c) || (!at) || (!fix)) { free(b.c); free(at); free(fix); return(0); }
	b.n = 0; b.max = cap; b.bad = 0;

	pd_prologue(&b,kcd);
	for(i=0;i<n;i++)
	{
		at[i] = b.n;
		/* `PD_JITNO=f1,f2,…`：**拒编含这几号指令的整份** —— 二分"是哪一族错了"就靠它。
		   （f 的编号是 `eval.c:251` 那张 enum：TIMES=28、PEEK=48、POKE=50、USERFUNC=56…） */
		{
			const char *no = getenv("PD_JITNO");
			if (no)
			{
				char want[16]; long k;
				snprintf(want,sizeof(want),"%ld",(long)kcd->gasm[i].f);
				for(k=0;no[k];k++)
				{
					if ((k) && (no[k-1] != ',')) continue;
					if (!strncmp(&no[k],want,strlen(want))
						&& ((no[k+strlen(want)] == 0) || (no[k+strlen(want)] == ','))) { b.bad = 1; break; }
				}
				if (b.bad) break;
			}
		}
		pd_op(&b,kcd,i,fix,&nfix);
		if (b.bad) break;
	}
	at[n] = b.n;
	/* 掉到末尾那一句：`gvlp -= stackdoubs; return(*p[0]);`（最后一条指令的 p[0]） */
	if (!b.bad) { pd_load(&b,kcd,&kcd->gasm[n-1].r[0],0,PD_XS0); pd_epilogue(&b,kcd); }

	for(i=0;i<nfix;i++)
	{
		long t = fix[i].tgt + 1, rel;      /* +1：原文那句 i=… 之后还有一趟 i++ */
		if ((t < 0) || (t > n)) { b.bad = 1; break; }
		rel = at[t] - fix[i].at;
		if (fix[i].cond < 0)
		{
			if ((rel < -(1<<25)) || (rel >= (1<<25))) { b.bad = 1; break; }
			b.c[fix[i].at] = 0x14000000u | ((unsigned)rel & 0x03ffffffu);
		}
		else
		{
			if ((rel < -(1<<18)) || (rel >= (1<<18))) { b.bad = 1; break; }
			b.c[fix[i].at] = 0x54000000u | (((unsigned)rel & 0x7ffffu)<<5) | (unsigned)fix[i].cond;
		}
	}

	mem = 0;
	if (!b.bad)
	{
		size_t len = (size_t)b.n*4;
		mem = mmap(0,len,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
		if (mem == MAP_FAILED) mem = 0;
		else
		{
			memcpy(mem,b.c,len);
			/* 写完再改成 RX（arm64 macOS 不给 RWX），然后刷一次 icache。 */
			if (mprotect(mem,len,PROT_READ|PROT_EXEC) != 0) { munmap(mem,len); mem = 0; }
			else __builtin___clear_cache((char *)mem,(char *)mem+len);
		}
	}
	free(b.c); free(at); free(fix);
	return(mem);
}

/* 一格 kcd 编一次。kcd 是原文的结构体（不能加字段），所以拿一张小表记。 */
#define PD_JIT_CACHE 256
static struct { kcd_t *kcd; void *fn; } pd_jitc[PD_JIT_CACHE];
static long pd_jitcn = 0;
static int pd_jit_mode = -1;   /* -1 还没问过环境变量；0 关；1 开；2 差分 */

static int pd_jit_get_mode (void)
{
	if (pd_jit_mode < 0)
	{
		const char *s = getenv("PD_JIT");
		/* **默认先关着**：语料扫描里还有两份（`ken/heightmap.pss` / `ken/texture3d.pss`）
		   开着 JIT 会从"出得来图"退成"空画面"，根因还没定到（两份都只差 2 色对 1 色，
		   在边上，但那仍然是退步）。查错的手法与开关见 `port/README.md`。
		   等那两份清了再把默认改成开。 */
		pd_jit_mode = (!s) ? 0 : atoi(s);
		if (pd_jit_mode < 0) pd_jit_mode = 0;
	}
	return(pd_jit_mode);
}

/** 这一格 kcd 的机器码（没有/编不出来回 0 —— 调用方退回解释器）。 */
static void *pd_a64_jitfn (kcd_t *kcd)
{
	long i;
	if (!pd_jit_get_mode()) return(0);
	for(i=0;i<pd_jitcn;i++) if (pd_jitc[i].kcd == kcd) return(pd_jitc[i].fn);
	if (pd_jitcn >= PD_JIT_CACHE) return(0);
	/* `PD_JITMAX=n`：只编指令数 <= n 的那些 —— 出了错拿它二分（哪一份 kcd 的锅）。 */
	{
		const char *mx = getenv("PD_JITMAX");
		if ((mx) && (kcd->gecnt > atol(mx))) { pd_jitc[pd_jitcn].kcd = kcd; pd_jitc[pd_jitcn++].fn = 0; return(0); }
	}
	pd_jitc[pd_jitcn].kcd = kcd;
	pd_jitc[pd_jitcn].fn  = pd_jit_build(kcd);
	if (getenv("PD_JITDBG"))
	{
		fprintf(stderr,"[jit] kcd %p gecnt %ld -> %s\n",(void *)kcd,(long)kcd->gecnt,
			pd_jitc[pd_jitcn].fn ? "编出来了" : "编不出来（退回解释器）");
		if (atol(getenv("PD_JITDBG")) >= 2)
			for(i=0;i<kcd->gecnt;i++)
				fprintf(stderr,"[jit]   %3ld: f=%ld n=%ld r=%08lx/%08lx/%08lx\n",i,
					(long)kcd->gasm[i].f,(long)kcd->gasm[i].n,
					(unsigned long)kcd->gasm[i].r[0].r,(unsigned long)kcd->gasm[i].r[1].r,
					(unsigned long)kcd->gasm[i].r[2].r);
	}
	return(pd_jitc[pd_jitcn++].fn);
}

#endif
