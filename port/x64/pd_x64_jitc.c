/* port/x64/pd_x64_jitc.c —— **x86-64 的 JIT**（`port/a64/pd_a64_jitc.c` 的孪生兄弟）。
 *
 * 为什么还要一份：原文的 `kasm87`（COMPILE==1）吐的是 **32 位 x87** 机器码 ——
 * 段寄存器、`fld`/`fstp` 那一套、以及一堆 `(long)` 当指针用的地方，在 LP64 上
 * 一个字节都不能照搬。所以 x86-64 上走的还是 `COMPILE==0` 那条路（纯 C 解释器
 * + 我们自己的 thunk），JIT 则是这一份：**同一串 `gasm[]`，SSE2 + SysV 的码**。
 *
 * ## 与 arm64 那一份的关系
 *
 * 结构逐条对应（`pd_op` 那张 switch 的口径就是 `kasm_interp.c` 那张 switch），
 * **共用的那几格提到了 `pd_a64_jitc.c` 前头**：`pd_jit_rnd/nrnd/fact/logb`、
 * `pd_a64_jit_one`（"这一条走解释器"的退路）、`pd_jit_want_fb`（PD_JITNO/PD_JITFB
 * 那两把二分开关）、`pd_jit_lastfb`，以及外面那层 `pd_a64_jitfn` 的 kcd 缓存与
 * `PD_JIT` 三档（0 关 / 1 开 / 2 差分）。这一份只提供 `pd_jit_build`。
 *
 * ## 寄存器怎么用（SysV AMD64）
 *
 *   rbx = parmdat        （KESP / KPTR 那两族的底）
 *   r12 = kcd
 *   r13 = kcd->globval   （KEDX 那族的底）
 *   r14 = gstatmem       （KGLB 那族的底）
 *   r15 = gvlp（进来那一刻）（KECX 那族的底）
 *   rbp = &gvlp          （RETURN 那一刻要 `gvlp -= stackdoubs`）
 *   这六个都是**被调用方保存**的，所以跨调用活着；进来推一遍、回去弹一遍。
 *   rax / r10 / r11 = 算地址与整数零碎（**都不是传参寄存器**，所以摆实参的时候
 *                     拿它们当草稿纸不会踩到已经摆好的那几格）
 *   xmm0..xmm5 = 算术零碎；摆实参时 xmm0..7 是 double 的八格
 *
 * ## 三个必须记住的 ABI 细节
 *
 *   1. **栈对齐**：`call` 那一刻 rsp 必须 16 对齐。进来 rsp≡8，推 6 个（48 字节）
 *      还是 ≡8，所以 prologue 末尾要 `sub rsp,8`；
 *   2. 指针实参只有**六格**（rdi/rsi/rdx/rcx/r8/r9），比 arm64 少两格 ——
 *      语料里最多的是 `CdCdC`（3 格），够；超了退回解释器；
 *   3. `roundsd`（floor/ceil/trunc）是 **SSE4.1**。Rosetta 2 与 qemu 都给，
 *      但真要碰上没有的机器：`pd_jit_build` 开头问一句 `__builtin_cpu_supports`，
 *      没有就整份不编（退回解释器，答案照旧对）。
 */

#if (COMPILE == 0) && (defined(__x86_64__) || defined(_M_X64))

#include <sys/mman.h>

/* ── 一格编译缓冲（x86 是变长的，所以单位是**字节**）───────────────────────────── */
typedef struct
{
	unsigned char *c;
	long n, max;
	int bad;
	long nfb;
} px_jb;

static void px_b1 (px_jb *b, unsigned v)
{
	if (b->n >= b->max) { b->bad = 1; return; }
	b->c[b->n++] = (unsigned char)(v&0xff);
}
static void px_b4 (px_jb *b, unsigned long long v)
	{ px_b1(b,(unsigned)(v&0xff)); px_b1(b,(unsigned)((v>>8)&0xff)); px_b1(b,(unsigned)((v>>16)&0xff)); px_b1(b,(unsigned)((v>>24)&0xff)); }
static void px_b8 (px_jb *b, unsigned long long v)
	{ px_b4(b,v); px_b4(b,v>>32); }

/* 寄存器编号（x86-64 那张表） */
#define PX_RAX 0
#define PX_RCX 1
#define PX_RDX 2
#define PX_RBX 3
#define PX_RSP 4
#define PX_RBP 5
#define PX_RSI 6
#define PX_RDI 7
#define PX_R8  8
#define PX_R9  9
#define PX_R10 10
#define PX_R11 11
#define PX_R12 12
#define PX_R13 13
#define PX_R14 14
#define PX_R15 15

/* 底寄存器与草稿纸（见头注那张表） */
#define PX_PARM PX_RBX
#define PX_KCD  PX_R12
#define PX_GLOB PX_R13
#define PX_STAT PX_R14
#define PX_ECX  PX_R15
#define PX_GVLP PX_RBP
#define PX_S0   PX_RAX
#define PX_S1   PX_R10
#define PX_S2   PX_R11

/* 指针实参那六格（SysV） */
static const int px_argi[6] = { PX_RDI, PX_RSI, PX_RDX, PX_RCX, PX_R8, PX_R9 };

/* REX：0x40 | W<<3 | R<<2 | X<<1 | B。一格都不需要的时候**不发**（省字节，也更像
   编译器出的码 —— 反汇编对着看的时候少一行噪音）。 */
static void px_rex (px_jb *b, int w, int reg, int idx, int base)
{
	unsigned r = 0x40u | (w ? 8u : 0u) | ((reg >= 8) ? 4u : 0u) | ((idx >= 8) ? 2u : 0u) | ((base >= 8) ? 1u : 0u);
	if (r != 0x40u) px_b1(b,r);
}

/* modrm：寄存器对寄存器（mod=11） */
static void px_rr (px_jb *b, int reg, int rm)
	{ px_b1(b,0xC0u | (unsigned)((reg&7)<<3) | (unsigned)(rm&7)); }

/* modrm：`[base+disp32]`（mod=10）。**一律 disp32** —— 少一个"挑 disp8"的岔路。
   base 的低三位是 100（rsp/r12）时 x86 规定要跟一格 SIB，所以补 0x24。 */
static void px_mem (px_jb *b, int reg, int base, long disp)
{
	px_b1(b,0x80u | (unsigned)((reg&7)<<3) | (unsigned)(base&7));
	if ((base&7) == 4) px_b1(b,0x24);
	px_b4(b,(unsigned long long)(unsigned int)(int)disp);
}

/* modrm：`[base+idx*8]`（也走 mod=10 + disp32=0，省得管 rbp/r13 那个特例） */
static void px_mem_idx8 (px_jb *b, int reg, int base, int idx)
{
	px_b1(b,0x80u | (unsigned)((reg&7)<<3) | 4u);
	px_b1(b,(unsigned)((3<<6) | ((idx&7)<<3) | (base&7)));
	px_b4(b,0);
}

/* ── 整数那几条 ─────────────────────────────────────────────────────────────── */

/* movabs Rd, imm64（B8+r，REX.W 的那一档） */
static void px_movabs (px_jb *b, int rd, unsigned long long v)
	{ px_b1(b,0x48u | ((rd >= 8) ? 1u : 0u)); px_b1(b,0xB8u + (unsigned)(rd&7)); px_b8(b,v); }

/* mov Rd, Rs（8B /r，reg=目的） */
static void px_movrr (px_jb *b, int rd, int rs)
	{ if (rd == rs) return; px_rex(b,1,rd,0,rs); px_b1(b,0x8B); px_rr(b,rd,rs); }
/* mov Rd, [base+disp] / mov [base+disp], Rs */
static void px_ldq (px_jb *b, int rd, int base, long disp)
	{ px_rex(b,1,rd,0,base); px_b1(b,0x8B); px_mem(b,rd,base,disp); }
static void px_stq (px_jb *b, int rs, int base, long disp)
	{ px_rex(b,1,rs,0,base); px_b1(b,0x89); px_mem(b,rs,base,disp); }
/* lea Rd, [base+disp] / lea Rd, [base+idx*8] */
static void px_lea (px_jb *b, int rd, int base, long disp)
	{ px_rex(b,1,rd,0,base); px_b1(b,0x8D); px_mem(b,rd,base,disp); }
static void px_lea_idx8 (px_jb *b, int rd, int base, int idx)
	{ px_rex(b,1,rd,idx,base); px_b1(b,0x8D); px_mem_idx8(b,rd,base,idx); }

/* 双寄存器的算术（reg=目的、rm=源，都是 mod=11） */
static void px_alu (px_jb *b, unsigned op, int rd, int rs)
	{ px_rex(b,1,rd,0,rs); px_b1(b,op); px_rr(b,rd,rs); }
#define PX_ADD 0x03u
#define PX_SUB 0x2Bu
#define PX_AND 0x23u
#define PX_OR  0x0Bu
#define PX_CMP 0x3Bu
#define PX_XOR 0x33u
#define PX_TEST 0x85u

/* add/sub Rsp, imm32（81 /0 与 81 /5） */
static void px_spadd (px_jb *b, long v)
	{ px_b1(b,0x48); px_b1(b,0x81); px_rr(b,0,PX_RSP); px_b4(b,(unsigned long long)(unsigned int)(int)v); }
static void px_spsub (px_jb *b, long v)
	{ px_b1(b,0x48); px_b1(b,0x81); px_rr(b,5,PX_RSP); px_b4(b,(unsigned long long)(unsigned int)(int)v); }

/* push / pop */
static void px_push (px_jb *b, int r)
	{ if (r >= 8) px_b1(b,0x41); px_b1(b,0x50u + (unsigned)(r&7)); }
static void px_pop (px_jb *b, int r)
	{ if (r >= 8) px_b1(b,0x41); px_b1(b,0x58u + (unsigned)(r&7)); }

/* setcc Rb（0F 90+cc）+ movzx Rd, Rb（REX.W 0F B6）—— 合起来就是 arm64 的 CSET */
#define PX_O 0x0
#define PX_NO 0x1
#define PX_B 0x2
#define PX_AE 0x3
#define PX_E 0x4
#define PX_NE 0x5
#define PX_BE 0x6
#define PX_A 0x7
#define PX_P 0xA
#define PX_NP 0xB
static void px_setcc (px_jb *b, int cc, int rd)
{
	px_rex(b,0,0,0,rd);                /* r8..r15 的低字节要 REX 才点得到 */
	px_b1(b,0x0F); px_b1(b,0x90u + (unsigned)cc); px_rr(b,0,rd);
}
static void px_movzx8 (px_jb *b, int rd, int rs)
	{ px_rex(b,1,rd,0,rs); px_b1(b,0x0F); px_b1(b,0xB6); px_rr(b,rd,rs); }
static void px_cset (px_jb *b, int cc, int rd)
	{ px_setcc(b,cc,rd); px_movzx8(b,rd,rd); }

/* cmov Rd, Rs（REX.W 0F 40+cc） */
static void px_cmov (px_jb *b, int cc, int rd, int rs)
	{ px_rex(b,1,rd,0,rs); px_b1(b,0x0F); px_b1(b,0x40u + (unsigned)cc); px_rr(b,rd,rs); }

/* ── SSE2 那几条（前缀 -> REX -> 0F -> 操作码，次序不能换）────────────────────── */

/* movsd Xd, [base+disp] / movsd [base+disp], Xs */
static void px_movsd_ld (px_jb *b, int xd, int base, long disp)
	{ px_b1(b,0xF2); px_rex(b,0,xd,0,base); px_b1(b,0x0F); px_b1(b,0x10); px_mem(b,xd,base,disp); }
static void px_movsd_st (px_jb *b, int xs, int base, long disp)
	{ px_b1(b,0xF2); px_rex(b,0,xs,0,base); px_b1(b,0x0F); px_b1(b,0x11); px_mem(b,xs,base,disp); }
/* movsd Xd, [base+idx*8] / movsd [base+idx*8], Xs */
static void px_movsd_ldi (px_jb *b, int xd, int base, int idx)
	{ px_b1(b,0xF2); px_rex(b,0,xd,idx,base); px_b1(b,0x0F); px_b1(b,0x10); px_mem_idx8(b,xd,base,idx); }
/* movsd Xd, Xs（寄存器对寄存器） */
static void px_movsd_rr (px_jb *b, int xd, int xs)
	{ if (xd == xs) return; px_b1(b,0xF2); px_rex(b,0,xd,0,xs); px_b1(b,0x0F); px_b1(b,0x10); px_rr(b,xd,xs); }

/* 双操作数的 SSE（F2 0F op /r，reg=目的） */
static void px_sse2 (px_jb *b, unsigned op, int xd, int xs)
	{ px_b1(b,0xF2); px_rex(b,0,xd,0,xs); px_b1(b,0x0F); px_b1(b,op); px_rr(b,xd,xs); }
#define PX_ADDSD 0x58u
#define PX_MULSD 0x59u
#define PX_SUBSD 0x5Cu
#define PX_DIVSD 0x5Eu
#define PX_SQRTSD 0x51u
#define PX_MINSD 0x5Du
#define PX_MAXSD 0x5Fu

/* 带 66 前缀那几条：ucomisd / andpd / xorpd / pxor */
static void px_sse66 (px_jb *b, unsigned op, int xd, int xs)
	{ px_b1(b,0x66); px_rex(b,0,xd,0,xs); px_b1(b,0x0F); px_b1(b,op); px_rr(b,xd,xs); }
#define PX_UCOMISD 0x2Eu
#define PX_ANDPD 0x54u
#define PX_XORPD 0x57u
#define PX_PXOR 0xEFu

/* roundsd Xd, Xs, imm8（66 0F 3A 0B /r ib）—— SSE4.1，1=floor 2=ceil 3=trunc */
static void px_roundsd (px_jb *b, int xd, int xs, int mode)
{
	px_b1(b,0x66); px_rex(b,0,xd,0,xs);
	px_b1(b,0x0F); px_b1(b,0x3A); px_b1(b,0x0B); px_rr(b,xd,xs); px_b1(b,(unsigned)mode);
}
#define PX_FLOOR 1
#define PX_CEIL  2
#define PX_TRUNC 3

/* cvtsi2sd Xd, Rs32（F2 0F 2A，**不带 REX.W** = 源是 32 位有符号）
   —— 这一格就是 arm64 的 `SCVTF Dd,Wn`（那边踩过"写成 single"的坑，这边不存在
   这个岔路：2A 只有整数宽度可挑，目的一定是 double）。 */
static void px_cvtsi2sd (px_jb *b, int xd, int rs)
	{ px_b1(b,0xF2); px_rex(b,0,xd,0,rs); px_b1(b,0x0F); px_b1(b,0x2A); px_rr(b,xd,rs); }
/* cvttsd2si Rd64, Xs（F2 REX.W 0F 2C）= arm64 的 `FCVTZS Xd,Dn`（朝零截断） */
static void px_cvttsd2si (px_jb *b, int rd, int xs)
	{ px_b1(b,0xF2); px_rex(b,1,rd,0,xs); px_b1(b,0x0F); px_b1(b,0x2C); px_rr(b,rd,xs); }
/* movq Xd, Rs（66 REX.W 0F 6E）—— 拿来造浮点常量与那两格掩码 */
static void px_movq_xr (px_jb *b, int xd, int rs)
	{ px_b1(b,0x66); px_rex(b,1,xd,0,rs); px_b1(b,0x0F); px_b1(b,0x6E); px_rr(b,xd,rs); }

/* Xd = 一格 double 常量（gp 是草稿纸 —— 调用方得保证它此刻是闲的） */
static void px_fconst (px_jb *b, int xd, double v, int gp)
{
	unsigned long long u; memcpy(&u,&v,8);
	px_movabs(b,gp,u); px_movq_xr(b,xd,gp);
}
/* Xd = |Xs| / -Xs（掩码走 gp，一条 andpd/xorpd 了事） */
static void px_fabs (px_jb *b, int xd, int xs, int gp)
	{ px_movabs(b,gp,0x7fffffffffffffffULL); px_movq_xr(b,xd,gp); px_sse66(b,PX_ANDPD,xd,xs); }
static void px_fneg (px_jb *b, int xd, int xs, int gp)
	{ px_movabs(b,gp,0x8000000000000000ULL); px_movq_xr(b,xd,gp); px_sse66(b,PX_XORPD,xd,xs); }

/* ── 跳转与调用 ─────────────────────────────────────────────────────────────── */

/* jmp rel32（E9）/ jcc rel32（0F 80+cc）。回填的是那四个字节，所以
   发的时候先占位，`at` 记的是**那四个字节的字节偏移**。 */
static long px_jmp (px_jb *b)
	{ px_b1(b,0xE9); px_b4(b,0); return(b->n-4); }
static long px_jcc (px_jb *b, int cc)
	{ px_b1(b,0x0F); px_b1(b,0x80u + (unsigned)cc); px_b4(b,0); return(b->n-4); }

/* 一格调用：绝对地址进 r11、`call r11`。**r11 不是传参寄存器**，所以摆好的实参
   （rdi/rsi/rdx/rcx/r8/r9 + xmm0-7）一个都不会被碰。 */
static void px_call (px_jb *b, const void *fn)
{
	px_movabs(b,PX_R11,(unsigned long long)(unsigned long)fn);
	px_b1(b,0x41); px_b1(b,0xFF); px_b1(b,0xD3);      /* call r11 */
}
static void px_ret (px_jb *b) { px_b1(b,0xC3); }

/**
 * **一格操作数的地址** -> `(*rn, *off)`，之后 `movsd [*rn+*off]` 就能用。
 *
 * 与解释器那五行（`kasm_interp.c:52-56`）逐条对着看 —— 与 arm64 那一份同一张表，
 * 只是 x86 的 disp32 够大、也不挑对齐，所以"偏移太大"那条岔路几乎走不到。
 * 注意 KECX / KESP **不加 q**（原文就没加）。
 */
static void px_opaddr (px_jb *b, kcd_t *kcd, rtyp *r, int scratch, int *rn, long *off)
{
	unsigned long fam = (unsigned long)r->r & 0xf0000000u;
	long o = (long)((unsigned long)r->r & 0x0fffffffu);
	int base;

	switch(fam)
	{
		case KECX: base = PX_ECX;  break;
		case KESP: base = PX_PARM; break;
		case KEDX: base = PX_GLOB; o += (long)r->q*8; break;
		case KGLB: base = PX_STAT; o += (long)r->q*8; break;
		case KIMM:
			px_movabs(b,scratch,(unsigned long long)(unsigned long)((char *)kcd->gevalext[o].ptr + (long)r->q*8));
			*rn = scratch; *off = 0; return;
		case KPTR:
			px_ldq(b,scratch,PX_PARM,o);          /* 指针本身在 parmdat 里 */
			base = scratch; o = (long)r->q*8;
			break;
		default: b->bad = 1; *rn = PX_PARM; *off = 0; return;   /* KEAX/KFST/… 这条路上不该有 */
	}
	if ((o >= -2147483647L) && (o <= 2147483647L)) { *rn = base; *off = o; return; }
	px_movabs(b,scratch,(unsigned long long)o);
	px_alu(b,PX_ADD,scratch,base);
	*rn = scratch; *off = 0;
}

static void px_load (px_jb *b, kcd_t *kcd, rtyp *r, int xd, int scratch)
	{ int rn; long off; px_opaddr(b,kcd,r,scratch,&rn,&off); px_movsd_ld(b,xd,rn,off); }
static void px_store (px_jb *b, kcd_t *kcd, rtyp *r, int xs, int scratch)
	{ int rn; long off; px_opaddr(b,kcd,r,scratch,&rn,&off); px_movsd_st(b,xs,rn,off); }
/* 一格操作数的**地址**（不是值）算进 rd */
static void px_addrof (px_jb *b, kcd_t *kcd, rtyp *r, int rd, int scratch)
{
	int rn; long off;
	px_opaddr(b,kcd,r,scratch,&rn,&off);
	if (!off) { px_movrr(b,rd,rn); return; }
	px_lea(b,rd,rn,off);
}

/* ── 开头与收尾 ─────────────────────────────────────────────────────────────── */

/* 进来的两格实参：rdi = parmdat、rsi = kcd（与 `kasm87c_run` 同一个签名）。 */
static void px_prologue (px_jb *b, kcd_t *kcd)
{
	px_push(b,PX_RBX); px_push(b,PX_RBP);
	px_push(b,PX_R12); px_push(b,PX_R13); px_push(b,PX_R14); px_push(b,PX_R15);
	px_spsub(b,8);                                /* 推 6 个之后 rsp≡8，补这一格才 16 对齐 */
	px_movrr(b,PX_PARM,PX_RDI);
	px_movrr(b,PX_KCD,PX_RSI);
	px_ldq(b,PX_GLOB,PX_KCD,(long)((char *)&kcd->globval - (char *)kcd));
	px_movabs(b,PX_S0,(unsigned long long)(unsigned long)&gstatmem);
	px_ldq(b,PX_STAT,PX_S0,0);                    /* r14 = gstatmem（它本身是一格 long） */
	px_movabs(b,PX_GVLP,(unsigned long long)(unsigned long)&gvlp);
	px_ldq(b,PX_ECX,PX_GVLP,0);                   /* r15 = 进来那一刻的 gvlp（KECX 的底） */
	/* gvlp += stackdoubs（原文在算完 plst 之后才加，所以 r15 留的是加之前那一份） */
	px_movrr(b,PX_S0,PX_ECX);
	px_movabs(b,PX_S1,(unsigned long long)(kcd->stackdoubs*8));
	px_alu(b,PX_ADD,PX_S0,PX_S1);
	px_stq(b,PX_S0,PX_GVLP,0);
}

/* 收尾：xmm0 是返回值。`gvlp -= stackdoubs` 照原文那句**相对**减法。 */
static void px_epilogue (px_jb *b, kcd_t *kcd)
{
	px_ldq(b,PX_S0,PX_GVLP,0);
	px_movabs(b,PX_S1,(unsigned long long)(kcd->stackdoubs*8));
	px_alu(b,PX_SUB,PX_S0,PX_S1);
	px_stq(b,PX_S0,PX_GVLP,0);
	px_spadd(b,8);
	px_pop(b,PX_R15); px_pop(b,PX_R14); px_pop(b,PX_R13); px_pop(b,PX_R12);
	px_pop(b,PX_RBP); px_pop(b,PX_RBX);
	px_ret(b);
}

/* 下标那一夹：照原文那一行 —— 2 的幂用 and，否则越界归 0。`maxind` 是编译期常量。
   两格草稿纸（rax 当那个 0、r11 放常量）此刻都是闲的（调用方保证）。 */
static void px_bounds (px_jb *b, long k, int rj)
{
	if ((k) && (!((k-1)&k)))
		{ px_movabs(b,PX_S2,(unsigned long long)(k-1)); px_alu(b,PX_AND,rj,PX_S2); return; }
	px_movabs(b,PX_S2,(unsigned long long)k);
	px_alu(b,PX_XOR,PX_S0,PX_S0);
	px_alu(b,PX_CMP,rj,PX_S2);
	px_cmov(b,PX_AE,rj,PX_S0);        /* 无符号 >= 就换成 0 */
}

/**
 * 浮点比较 -> 0/1 进 `rd`（64 位），口径与 arm64 那一份**逐个 NaN 情形对齐**。
 *
 * x86 的 `ucomisd` 在无序（NaN）时把 ZF/PF/CF 全置 1，所以：
 *   * `a<b` 不能写成 `setb` —— NaN 会给出 1。把两个操作数**反过来**比、用
 *     `seta`（CF=0 且 ZF=0）：真值只在"b>a 且有序"时成立，正是 C 的 `<`；
 *   * `==` 要 `sete && setnp`、`!=` 要 `setne || setp` —— arm64 的 NE 含无序，
 *     与 C 里 `NaN != x` 为真一致。
 */
static void px_cmp_bool (px_jb *b, long f, int xa, int xb, int rd)
{
	switch(f)
	{
		case LES:   px_sse66(b,PX_UCOMISD,xb,xa); px_cset(b,PX_A, rd); return;
		case LESEQ: px_sse66(b,PX_UCOMISD,xb,xa); px_cset(b,PX_AE,rd); return;
		case MOR:   px_sse66(b,PX_UCOMISD,xa,xb); px_cset(b,PX_A, rd); return;
		case MOREQ: px_sse66(b,PX_UCOMISD,xa,xb); px_cset(b,PX_AE,rd); return;
		case EQU:
			px_sse66(b,PX_UCOMISD,xa,xb);
			px_cset(b,PX_E,rd); px_cset(b,PX_NP,PX_S2); px_alu(b,PX_AND,rd,PX_S2); return;
		default:   /* NEQU */
			px_sse66(b,PX_UCOMISD,xa,xb);
			px_cset(b,PX_NE,rd); px_cset(b,PX_P,PX_S2); px_alu(b,PX_OR,rd,PX_S2); return;
	}
}

/* `x != 0`（无序算真）-> 0/1 进 rd。`xz` 是一格拿来当 0 的 xmm（会被清掉）。 */
static void px_nz_bool (px_jb *b, int x, int xz, int rd)
{
	px_sse66(b,PX_PXOR,xz,xz);
	px_cmp_bool(b,NEQU,x,xz,rd);
}

/** 发一格"走解释器"的调用（实参是 parmdat / kcd / 指令下标）。 */
static void px_fallback (px_jb *b, long i)
{
	b->nfb++;
	px_movrr(b,PX_RDI,PX_PARM);
	px_movrr(b,PX_RSI,PX_KCD);
	px_movabs(b,PX_RDX,(unsigned long long)i);
	px_call(b,(const void *)pd_a64_jit_one);
}

/* 分支要回填的地方（x86 的 rel32 是"从下一条指令算起"，所以 `at` 记那四个字节的位置）。 */
typedef struct { long at; long tgt; } px_fix;

/* ── 一条 gasm -> 一串 x86-64 ──────────────────────────────────────────────────
 *
 * 口径就是 `kasm_interp.c` 那张 switch，一条一条对着抄；与 arm64 那一份**逐个 case
 * 对得上**（那边 d0/d1/d2/d3，这边 xmm0/1/2/3；那边 x25/x26/x27，这边 rax/r10/r11）。
 * 每条指令都是"从内存读操作数 -> 算 -> 写回内存"，没有跨指令的寄存器分配。
 */
static void px_op (px_jb *b, kcd_t *kcd, long i, px_fix *fix, long *nfix)
{
	gasmtyp *a = &kcd->gasm[i];
	long f = a->f;

	switch(f)
	{
		case NUL: case NOP: return;

		case GOTO:
			fix[(*nfix)].tgt = a->r[0].r; fix[(*nfix)].at = px_jmp(b); (*nfix)++;
			return;

		case RETURN:
			px_load(b,kcd,&a->r[1],0,PX_S0);
			px_epilogue(b,kcd);
			return;

		case IF0: case IF1:
			px_load(b,kcd,&a->r[2],1,PX_S0);
			px_nz_bool(b,1,3,PX_S1);
			px_alu(b,PX_TEST,PX_S1,PX_S1);
			fix[(*nfix)].tgt = a->r[0].r;
			fix[(*nfix)].at = px_jcc(b,(f == IF0) ? PX_E : PX_NE); (*nfix)++;
			return;

		/* 一元：照原文的次序（p[0] = op(p[1])） */
		case MOV:    px_load(b,kcd,&a->r[1],0,PX_S0); px_store(b,kcd,&a->r[0],0,PX_S0); return;
		case NEGMOV: px_load(b,kcd,&a->r[1],1,PX_S0); px_fneg(b,0,1,PX_S1); px_store(b,kcd,&a->r[0],0,PX_S0); return;
		case FABS:   px_load(b,kcd,&a->r[1],1,PX_S0); px_fabs(b,0,1,PX_S1); px_store(b,kcd,&a->r[0],0,PX_S0); return;
		case SQRT:   px_load(b,kcd,&a->r[1],1,PX_S0); px_sse2(b,PX_SQRTSD,0,1); px_store(b,kcd,&a->r[0],0,PX_S0); return;
		case FLOOR:  px_load(b,kcd,&a->r[1],1,PX_S0); px_roundsd(b,0,1,PX_FLOOR); px_store(b,kcd,&a->r[0],0,PX_S0); return;
		case CEIL:   px_load(b,kcd,&a->r[1],1,PX_S0); px_roundsd(b,0,1,PX_CEIL);  px_store(b,kcd,&a->r[0],0,PX_S0); return;
		/* ROUND0 是"朝零取整"（原文写成 x>=0 ? floor(x) : -floor(-x)）= trunc。 */
		case ROUND0: case ROUND0_32:
			px_load(b,kcd,&a->r[1],1,PX_S0); px_roundsd(b,0,1,PX_TRUNC); px_store(b,kcd,&a->r[0],0,PX_S0); return;
		case NEQU0:
			px_load(b,kcd,&a->r[1],1,PX_S0);
			px_nz_bool(b,1,3,PX_S1); px_cvtsi2sd(b,0,PX_S1);
			px_store(b,kcd,&a->r[0],0,PX_S0); return;
		/* SGN = (x>0)-(x<0)；UNIT = (x==0)*.5 + (x>0)（原文两行） */
		case SGN:
			px_load(b,kcd,&a->r[1],1,PX_S0);
			px_sse66(b,PX_PXOR,3,3);
			px_cmp_bool(b,MOR,1,3,PX_S1);
			px_sse66(b,PX_PXOR,3,3);            /* px_cmp_bool 不动 xmm，这一句只为看着整齐 */
			px_cmp_bool(b,LES,1,3,PX_RCX);
			px_alu(b,PX_SUB,PX_S1,PX_RCX);
			px_cvtsi2sd(b,0,PX_S1);
			px_store(b,kcd,&a->r[0],0,PX_S0); return;
		case UNIT:
			px_load(b,kcd,&a->r[1],1,PX_S0);
			px_sse66(b,PX_PXOR,5,5);
			px_cmp_bool(b,EQU,1,5,PX_S1); px_cvtsi2sd(b,2,PX_S1);
			px_cmp_bool(b,MOR,1,5,PX_S1); px_cvtsi2sd(b,4,PX_S1);
			px_fconst(b,0,0.5,PX_S1);
			px_sse2(b,PX_MULSD,2,0);
			px_movsd_rr(b,0,2); px_sse2(b,PX_ADDSD,0,4);
			px_store(b,kcd,&a->r[0],0,PX_S0); return;

		/* 二元算术 */
		case TIMES: case SLASH: case PLUS: case FADD: case MINUS:
			px_load(b,kcd,&a->r[1],1,PX_S0);
			px_load(b,kcd,&a->r[2],2,PX_S0);
			px_movsd_rr(b,0,1);
			px_sse2(b,(f == TIMES) ? PX_MULSD : (f == SLASH) ? PX_DIVSD : (f == MINUS) ? PX_SUBSD : PX_ADDSD,0,2);
			px_store(b,kcd,&a->r[0],0,PX_S0); return;
		/* PERC：x - floor(x/|y|)*|y|（原文那一行，不是 fmod） */
		case PERC:
			px_load(b,kcd,&a->r[1],1,PX_S0);
			px_load(b,kcd,&a->r[2],2,PX_S0);
			px_fabs(b,3,2,PX_S1);
			px_movsd_rr(b,0,1); px_sse2(b,PX_DIVSD,0,3); px_roundsd(b,0,0,PX_FLOOR);
			px_sse2(b,PX_MULSD,0,3);
			px_movsd_rr(b,4,1); px_sse2(b,PX_SUBSD,4,0); px_movsd_rr(b,0,4);
			px_store(b,kcd,&a->r[0],0,PX_S0); return;
		case LES: case LESEQ: case MOR: case MOREQ: case EQU: case NEQU:
			px_load(b,kcd,&a->r[1],1,PX_S0);
			px_load(b,kcd,&a->r[2],2,PX_S0);
			px_cmp_bool(b,f,1,2,PX_S1);
			px_cvtsi2sd(b,0,PX_S1);
			px_store(b,kcd,&a->r[0],0,PX_S0); return;
		case LAND: case LOR:
			px_load(b,kcd,&a->r[1],1,PX_S0);
			px_load(b,kcd,&a->r[2],2,PX_S0);
			px_sse66(b,PX_PXOR,3,3);
			px_cmp_bool(b,NEQU,1,3,PX_S1);
			px_cmp_bool(b,NEQU,2,3,PX_RCX);
			px_alu(b,(f == LAND) ? PX_AND : PX_OR,PX_S1,PX_RCX);
			px_cvtsi2sd(b,0,PX_S1);
			px_store(b,kcd,&a->r[0],0,PX_S0); return;
		/* MIN/MAX 照原文那两行（拿 p[2] 与 p[1] 比，真取 p[2]）。
		   `MINSD xmm1,xmm2` 的语义正好是 `(xmm1<xmm2)?xmm1:xmm2`、**无序取 xmm2**
		   —— 把 p[2] 放 xmm0、p[1] 放 xmm1，与 arm64 那句 `fcsel …,MI` 逐个情形相同
		   （含 NaN 与 ±0）。 */
		case MIN: case MAX:
			px_load(b,kcd,&a->r[1],1,PX_S0);
			px_load(b,kcd,&a->r[2],0,PX_S0);
			px_sse2(b,(f == MIN) ? PX_MINSD : PX_MAXSD,0,1);
			px_store(b,kcd,&a->r[0],0,PX_S0); return;

		/* ── 要调外头函数的那几族 ───────────────────────────────────────────────
		   实参就在 xmm0（/xmm1），结果也在 xmm0。调用前后没有活着的中间值
		   （全在内存里），所以不用存恢复任何 xmm。 */
		case RND:  px_call(b,(const void *)pd_jit_rnd);  px_store(b,kcd,&a->r[0],0,PX_S0); return;
		case NRND: px_call(b,(const void *)pd_jit_nrnd); px_store(b,kcd,&a->r[0],0,PX_S0); return;
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
			px_load(b,kcd,&a->r[1],0,PX_S0);
			px_call(b,fn);
			px_store(b,kcd,&a->r[0],0,PX_S0);
			return;
		}
		case POW: case FMOD: case ATAN2: case LOGB:
		{
			const void *fn = (f == POW) ? (const void *)(double (*)(double,double))pow :
				(f == FMOD) ? (const void *)(double (*)(double,double))fmod :
				(f == ATAN2) ? (const void *)(double (*)(double,double))atan2 :
				(const void *)pd_jit_logb;
			/* 这边不用像 arm64 那样先绕 d2/d3 —— 算地址的草稿纸是 gp（rax），碰不到 xmm。 */
			px_load(b,kcd,&a->r[1],0,PX_S0);
			px_load(b,kcd,&a->r[2],1,PX_S0);
			px_call(b,fn);
			px_store(b,kcd,&a->r[0],0,PX_S0);
			return;
		}

		/* ── 数组那两族（下标在运行期，`maxind` 在编译期） ─────────────────────── */
		case PEEK:
			px_load(b,kcd,&a->r[2],2,PX_S0);                   /* xmm2 = 下标 */
			px_cvttsd2si(b,PX_S1,2);                           /* r10 = (long)下标 */
			px_bounds(b,kcd->newvar[a->r[1].nv].maxind,PX_S1);
			px_addrof(b,kcd,&a->r[1],PX_S0,PX_S2);             /* rax = 数组底 */
			px_movsd_ldi(b,0,PX_S0,PX_S1);
			px_store(b,kcd,&a->r[0],0,PX_S2);
			return;
		case POKE: case POKETIMES: case POKESLASH: case POKEPERC: case POKEPLUS: case POKEMINUS:
		{
			rtyp *rp = &kcd->rxi[a->rxi];
			px_load(b,kcd,rp,3,PX_S0);                         /* xmm3 = 右边那个值 */
			px_load(b,kcd,&a->r[2],2,PX_S0);                   /* xmm2 = 下标 */
			px_cvttsd2si(b,PX_S1,2);
			px_bounds(b,kcd->newvar[a->r[1].nv].maxind,PX_S1);
			px_addrof(b,kcd,&a->r[1],PX_S0,PX_S2);
			px_lea_idx8(b,PX_S0,PX_S0,PX_S1);                  /* rax = &p[1][j] */
			if (f == POKE) { px_movsd_st(b,3,PX_S0,0); return; }
			px_movsd_ld(b,1,PX_S0,0);
			if (f == POKEPERC)
			{
				px_fabs(b,2,3,PX_S2);                           /* 掩码走 r11 —— rax 是地址，不能碰 */
				px_movsd_rr(b,4,1); px_sse2(b,PX_DIVSD,4,2); px_roundsd(b,4,4,PX_FLOOR);
				px_sse2(b,PX_MULSD,4,2);
				px_sse2(b,PX_SUBSD,1,4); px_movsd_rr(b,0,1);
			}
			else
			{
				px_sse2(b,(f == POKETIMES) ? PX_MULSD : (f == POKESLASH) ? PX_DIVSD :
					(f == POKEPLUS) ? PX_ADDSD : PX_SUBSD,1,3);
				px_movsd_rr(b,0,1);
			}
			px_movsd_st(b,0,PX_S0,0);
			return;
		}

		/* ── 宿主函数那一族（`USERFUNC`）───────────────────────────────────────
		 *
		 * 解释器在这儿最亏：每次调用都要按 `n` 过一趟 switch、再 `strncmp` 原型串，
		 * 才知道该怎么强转。而**原型串与被调的函数在编译期就定了**。
		 *
		 * ABI：SysV AMD64 —— **double 走 xmm0..7、指针走 rdi/rsi/rdx/rcx/r8/r9，
		 * 两条序列各自数**（与 AAPCS64 同一件事，只是指针那条少两格）。
		 * 被调的都是**非变参**原型（带 `.` 的那些编译出来是 'e'，这儿一律不收），
		 * 所以不用管 `al` 要填几。
		 */
		case USERFUNC:
		{
			char *cptr;
			void *dafunc;
			long j, nd = 0, np = 0;
			if ((a->n < 1) || (a->n > 8)) { px_fallback(b,i); return; }
			if ((kcd->newvar[a->g].r & 0xf0000000) != KIMM) { px_fallback(b,i); return; }
			dafunc = (void *)kcd->gevalext[kcd->newvar[a->g].r & 0x0fffffff].ptr;
			cptr = &kcd->newvarnam[kcd->newvar[a->g].proti];
			if (getenv("PD_JITDBG") && (atol(getenv("PD_JITDBG")) >= 2))
				fprintf(stderr,"[jit]   USERFUNC 第 %ld 条：n=%ld 原型=|%.*s| 名字=%s owns=%d\n",
					i,(long)a->n,(int)a->n,cptr,&kcd->newvarnam[kcd->newvar[a->g].nami],
					pd_a64_owns(dafunc));
			/* 原型串**不是 NUL 结尾**的（后面紧跟函数名），所以只看前 n 个字符。
			   `C`（`char *`）与 `D`（`double *`）在这一层是同一件事：递的都是**操作数的
			   地址**。 */
			for(j=0;j<a->n;j++)
				if ((cptr[j] != 'd') && (cptr[j] != 'D') && (cptr[j] != 'C')) { px_fallback(b,i); return; }
			/* 脚本自己那些函数（`pd_a64_owns`）交给解释器 —— 与 arm64 那一份同一个口径
			   （那条路的收益在 `pd_a64_call_script` 自己会问一句 JIT）。 */
			if (pd_a64_owns(dafunc)) { px_fallback(b,i); return; }
			/* 立即模式攒批（`port/a64/pd_gl_imm.c`）：那几族在编译期换落点，别的宿主
			   调用之前先发一格 `pd_imm_break`。 */
			{
				void *h = pd_imm_hook(dafunc);
				if (h) dafunc = h;
				else if (pd_imm_isgl(&kcd->newvarnam[kcd->newvar[a->g].nami]))
					px_call(b,(const void *)pd_imm_break);
			}
			for(j=1;j<=a->n;j++)
			{
				rtyp *rp = (j <= 2) ? &a->r[j] : &kcd->rxi[a->rxi+j-3];
				if (cptr[j-1] == 'd')
				{
					if (nd >= 8) { px_fallback(b,i); return; }
					px_load(b,kcd,rp,(int)nd,PX_S0);
					nd++;
				}
				else   /* 'D' 与 'C' 都是"递地址" */
				{
					/* 只有六格。语料里最多的是 `CdCdC`（3 格），够；超了退回解释器。 */
					if (np >= 6) { px_fallback(b,i); return; }
					px_addrof(b,kcd,rp,px_argi[np],PX_S0);
					np++;
				}
			}
			px_call(b,dafunc);
			px_store(b,kcd,&a->r[0],0,PX_S0);
			return;
		}

		/* 没接的指令**不再整份放弃**：单独这一条走解释器。 */
		default: px_fallback(b,i); return;
	}
}

/* ── 编一整份、落到可执行内存 ──────────────────────────────────────────────────
 *
 * `GOTO`/`IF*` 的目标是 `gasm` 下标，而原文那句是 `i = r[0].r;` 之后**再过一趟
 * `i++`** —— 所以真正的落点是 `r[0].r + 1`（这一格错了就是无声的死循环）。
 * x86 的 rel32 是"从**下一条**指令算起"，而占位的四个字节就在那条指令末尾，
 * 所以 `rel = 目标字节偏移 - (那四个字节的偏移 + 4)`。
 */
static void *pd_jit_build (kcd_t *kcd)
{
	px_jb b;
	px_fix *fix;
	long *at, nfix = 0, i, n = kcd->gecnt, cap;
	void *mem;

	if (n <= 0) return(0);
	/* `roundsd` 是 SSE4.1（floor/ceil/trunc 与 % 那几族都用它）。没有就整份不编 ——
	   退回解释器，答案照旧对。Rosetta 2 与 qemu-x86_64 都给。 */
#if defined(__clang__) || defined(__GNUC__)
	if (!__builtin_cpu_supports("sse4.1")) return(0);
#endif
	cap = n*256 + 1024;
	b.c = (unsigned char *)malloc((size_t)cap);
	at  = (long *)malloc((size_t)(n+1)*sizeof(long));
	fix = (px_fix *)malloc((size_t)(n+2)*sizeof(px_fix));
	if ((!b.c) || (!at) || (!fix)) { free(b.c); free(at); free(fix); return(0); }
	b.n = 0; b.max = cap; b.bad = 0; b.nfb = 0;

	px_prologue(&b,kcd);
	for(i=0;i<n;i++)
	{
		at[i] = b.n;
		if (pd_jit_want_fb(kcd,i,n)) { px_fallback(&b,i); continue; }
		px_op(&b,kcd,i,fix,&nfix);
		if (b.bad) break;
	}
	at[n] = b.n;
	/* 掉到末尾那一句：`gvlp -= stackdoubs; return(*p[0]);`（最后一条指令的 p[0]） */
	if (!b.bad) { px_load(&b,kcd,&kcd->gasm[n-1].r[0],0,PX_S0); px_epilogue(&b,kcd); }

	for(i=0;i<nfix;i++)
	{
		long t = fix[i].tgt + 1, rel;      /* +1：原文那句 i=… 之后还有一趟 i++ */
		if ((t < 0) || (t > n)) { b.bad = 1; break; }
		rel = at[t] - (fix[i].at + 4);
		b.c[fix[i].at  ] = (unsigned char)( (unsigned long)rel        & 0xff);
		b.c[fix[i].at+1] = (unsigned char)(((unsigned long)rel >>  8) & 0xff);
		b.c[fix[i].at+2] = (unsigned char)(((unsigned long)rel >> 16) & 0xff);
		b.c[fix[i].at+3] = (unsigned char)(((unsigned long)rel >> 24) & 0xff);
	}

	mem = 0;
	if (!b.bad)
	{
		size_t len = (size_t)b.n;
		mem = mmap(0,len,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
		if (mem == MAP_FAILED) mem = 0;
		else
		{
			memcpy(mem,b.c,len);
			/* 写完再改成 RX（macOS 上不给 RWX；Linux 上给也不要）。 */
			if (mprotect(mem,len,PROT_READ|PROT_EXEC) != 0) { munmap(mem,len); mem = 0; }
		}
	}
	pd_jit_lastfb = b.nfb;
	free(b.c); free(at); free(fix);
	return(mem);
}

#endif







