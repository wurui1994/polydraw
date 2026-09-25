/* 生成 port/a64/pd_a64_run.c —— `kasm87c_run` 的 arm64 版（Apple 变参 ABI 那一刀）。
 *
 * 只改两处，别的逐字节照抄 `eval/kasm_interp.c:28-234`：
 *   1. 那个大 switch 里 52 处 `dafunc(…)` 全换成**精确原型**的强转；
 *   2. switch 前面插一档：目标是我们自己的 thunk（脚本函数，真变参）时不走 switch。
 * `diff -u` 看得见全部改动。
 */
import { readFileSync, writeFileSync } from 'node:fs';

const D = '/Users/wurui/Documents/polydraw-bench/';
const SRC = `${D}polydraw_src/eval/kasm_interp.c`;
const OUT = `${D}port/a64/pd_a64_run.c`;

const lines = readFileSync(SRC, 'latin1').split('\n');
/* 行号（1 起）：28 是签名、234 是收尾的 `}`。对不上就当场报，别默默生成错的。 */
const want = (n, s) => {
  if (!lines[n - 1].includes(s)) throw new Error(`第 ${n} 行不是 "${s}"：${lines[n - 1]}`);
};
want(28, 'double kasm87c_run (char *parmdat, kcd_t *kcd)');
want(234, '}');
want(233, 'gvlp -= kcd->stackdoubs; return(*p[0]);');

const body = lines.slice(27, 234);           // 28..234

/* 一处 `dafunc(…)` 的强转。类型**从实参表自己读**（比看 strncmp 的原型串稳：
   case 10~16 那七行压根没有原型串，是全 double）：`*p[k]` -> double、`p[k]` -> double *。 */
let patched = 0;
const fixCall = (line) => {
  const at = line.indexOf('dafunc(');
  if (at < 0) return line;
  const open = at + 'dafunc'.length;
  let depth = 0;
  let close = -1;
  for (let i = open; i < line.length; i++) {
    if (line[i] === '(') depth++;
    else if (line[i] === ')') { depth--; if (depth === 0) { close = i; break; } }
  }
  if (close < 0) throw new Error(`实参表没闭合：${line}`);
  const types = line.slice(open + 1, close).split(',').map((a) => {
    const t = a.trim();
    if (/^\*p\[\d+\]$/.test(t)) return 'double';
    if (/^p\[\d+\]$/.test(t)) return 'double *';
    throw new Error(`认不出这个实参：|${t}|  ${line}`);
  }).join(',');
  patched++;
  return `${line.slice(0, at)}((double (__cdecl *)(${types}))dafunc)${line.slice(open, close + 1)}${line.slice(close + 1)}`;
};

/* switch 前面那一档：脚本自己的函数（我们的 thunk）是**真变参**，不能按定参调。 */
const SCRIPT_PATH = [
  '',
  '\t\t\t\t//—— 这里是本机（arm64/osx）补上的那一格，见 port/a64/pd_a64_run.c 的头注 ——',
  '\t\t\t\t//脚本自己的函数走的是 pd_a64_jit.c 造的 thunk（尾跳到真变参的 kasm87c/',
  '\t\t\t\t//kasm87cp）。它跟宿主那些定参的 C 函数调用约定不一样，所以先分出去：',
  '\t\t\t\t//自己摊一份 parmdat，直接递归调 kasm87c_run，压根不经过变参。',
  '\t\t\t\tif (pd_a64_owns((void *)dafunc))',
  '\t\t\t\t\t{ (*p[0]) = pd_a64_call_script((void *)dafunc,cptr,p,kcd->gasm[i].n); break; }',
  '',
  '\t\t\t\t//—— 第 18 个洞：原文那个 switch 只认 `d`/`D`，**没有 `C`（char *）那一档** ——',
  '\t\t\t\t//于是 `glsettex(0,"earth.jpg")` / `glsetshader("v","f")` 这些带字符串的宿主',
  '\t\t\t\t//函数在 COMPILE==0 那条路上**压根不会被调**（switch 一路 strncmp 全不中，',
  '\t\t\t\t//直接 break）—— 而且一声不响：`tex[0].tar` 还是 0，接着 `glbindtexture(0)`',
  '\t\t\t\t//拿 tar=0 去调，GL 报 INVALID_ENUM，采样器读到默认贴图 -> 采出来是白的。',
  '\t\t\t\t//量到的（PD_TEXDBG=1）：`[tex] bind tar=0 name=0 err=INVALID_ENUM`。',
  '\t\t\t\t//带字符串的原型全语料只有五种（`pd_script.c` 那张 myext[] 里数过）：',
  '\t\t\t\t//  C（mountzip/glgetuniformloc/glgetattribloc）、dC（glsettex）、',
  '\t\t\t\t//  dCd（glsettex 三参）、CC（glsetshader 两参）、CCC（glsetshader 三参）。',
  '\t\t\t\t//注意：原型串**不是 NUL 结尾**的，后面紧跟着函数名（量到 |dCGLSETTEX|），',
  '\t\t\t\t//所以只能像原文那样按长度 strncmp，strcmp 会全不中。',
  '\t\t\t\t//字符串操作数在 globval 里（KSTR 在 kasm_comp.c:313 被改成 KEDX+gccnt*8），',
  '\t\t\t\t//所以 p[j] 本身就是串的地址，强转 char * 即可。',
  '\t\t\t\t//printf 那一族仍然不接：`myprintf` 自己是真变参，按定参强转不对。',
  '\t\t\t\t//同理，找 `C` 也只能在前 n 个字符里找：用 strchr 会一路扫进后面的函数名，',
  '\t\t\t\t//于是 `gltexcoord`（|ddGLTEXCOORD|）这种压根没有字符串参的调用也会进这一档',
  '\t\t\t\t//—— 行为上无害（下面全不中就落回原路），但 curvybuild 两帧就白进 18 万次。',
  '\t\t\t\tlong cn = kcd->gasm[i].n;',
  '\t\t\t\tif (memchr(cptr,\'C\',cn))',
  '\t\t\t\t{',
  '\t\t\t\t\tif ((cn == 1) && (!strncmp(cptr,"C",1)))',
  '\t\t\t\t\t\t{ (*p[0]) = ((double (__cdecl *)(char *))dafunc)((char *)p[1]); break; }',
  '\t\t\t\t\tif ((cn == 2) && (!strncmp(cptr,"dC",2)))',
  '\t\t\t\t\t\t{ (*p[0]) = ((double (__cdecl *)(double,char *))dafunc)(*p[1],(char *)p[2]); break; }',
  '\t\t\t\t\tif ((cn == 2) && (!strncmp(cptr,"CC",2)))',
  '\t\t\t\t\t\t{ (*p[0]) = ((double (__cdecl *)(char *,char *))dafunc)((char *)p[1],(char *)p[2]); break; }',
  '\t\t\t\t\tif ((cn == 3) && (!strncmp(cptr,"dCd",3)))',
  '\t\t\t\t\t\t{ (*p[0]) = ((double (__cdecl *)(double,char *,double))dafunc)(*p[1],(char *)p[2],*p[3]); break; }',
  '\t\t\t\t\tif ((cn == 3) && (!strncmp(cptr,"CCC",3)))',
  '\t\t\t\t\t\t{ (*p[0]) = ((double (__cdecl *)(char *,char *,char *))dafunc)((char *)p[1],(char *)p[2],(char *)p[3]); break; }',
  '\t\t\t\t\tif (pd_run_dbg) fprintf(stderr,"[run] 带字符串的原型没接：|%.*s| n=%ld\\n",(int)cn,cptr,(long)cn);',
  '\t\t\t\t}',
  '',
];

const POISON_FILL = [
  '',
  '\t\t//—— 本机补的：先把十六格填成毒值（见上面 pd_a64_chk 的注） ——',
  '\tif (pd_run_dbg < 0) pd_run_dbg = (getenv("PD_RUNDBG") != 0);',
  '\tif (pd_run_dbg) { for(i=0;i<16;i++) plst[i] = PD_PLST_POISON; }',
  '',
];
const CHK = (ind, r, op, which) => `${ind}if (pd_run_dbg) pd_a64_chk(plst,${r},${op},${which});`;

/* 执行前那一格：见 pd_a64_chk0 的注。只插外层那个大 switch（两个 tab 的那句）。 */
const NULL_GUARD = [
  '',
  '\t\t//—— 本机补的诊断（PD_RUNDBG=1）：基址是 0 的操作数，印完就退出这一趟 ——',
  '\t\tif (pd_run_dbg && pd_a64_chk0(kcd,i,p)) { gvlp -= kcd->stackdoubs; return(0.0); }',
  '',
];

/* 第 12 个洞：`plst[KGLB]` 把 `gstatmem` 算了**两遍**。
 *
 * 原文（`kasm_interp.c:41`）是 `plst[KGLB>>28] = ((long)gstatmem - KGLB)`，
 * 于是 `p[j] = plst[…] + r = gstatmem + 偏移`；可紧接着那条 KGLB 分支又写
 * `p[j] = (double *)((gstatmem + (long)p[j]) + q*8)` —— `gstatmem` 加了两次。
 * 量到的证据：崩的地址正好是 kcd 那块堆地址的**两倍**
 * （kcd=0x78e91e9000、崩在 0xf1d18cc680）。
 *
 * 按 KIMM 那一格的对称写法，plst 这一格本该只是 `-KGLB`（基址由分支那一句加）。
 * 顺带把 `-KGLB` 写成 `-((long)KGLB)`：`KGLB` 是 unsigned int，直接取负会走无符号
 * 算术（与第 6 个洞 KIMM 同一个坑）。
 *
 * 十份脚本崩在 TIMES / PEEK / POKE 上，全是这一格 —— 它们都带 `static` 数组，
 * 所以 `gstatmem != 0`；不带 static 的脚本 gstatmem 是 0，加两遍也看不出来。
 */
const fixKglb = (line) => (line.includes('plst[((unsigned long)KGLB)>>28]')
  ? line.replace('((long)gstatmem    -KGLB)', '-((long)KGLB) /*本机改：原文把 gstatmem 算了两遍*/')
  : line);

/**
 * **第 19 个洞**：`USERFUNC` 里"函数指针形参"那一支读的是**全局** `gasm[i].g`，
 * 旁边三行读的都是 `kcd->gasm[i].g`。全局那一份是**上一次编译**留下的
 * （与第 14 个洞的 `newvar`/`gnumarg` 一模一样的毛病）——
 * 多个脚本先后编译之后，那一格取到的是别人的表，`dafunc` 直接是野指针。
 * 平时不容易撞上（要"拿函数指针当形参"那种写法），但 JIT 那格
 * "单独跑一条指令"的退路会把它放大成必崩：那时 `i` 是 0、全局 `gasm` 是别人的。
 */
const fixGlobGasm = (line) => line.replace('kcd->newvar[gasm[i].g].r',
  'kcd->newvar[kcd->gasm[i].g].r /*本机改：原文这儿读的是全局 gasm（第 19 个洞）*/');

const out = [];
let guarded = 0;
for (const line of body) {
  if (line.includes('plst[((unsigned long)KECX)>>28]')) out.push(...POISON_FILL);
  if (line.includes('switch(kcd->gasm[i].n)')) out.push(...SCRIPT_PATH);
  if (line === '\t\tswitch(kcd->gasm[i].f)' || (guarded === 0 && line.trim() === 'switch(kcd->gasm[i].f)')) { out.push(...NULL_GUARD); guarded++; }
  out.push(fixGlobGasm(fixKglb(fixCall(line))));
  if (line.includes('p[j] = (double *)(plst[((unsigned long)kcd->gasm[i].r[j].r)>>28]'))
    out.push(CHK('\t\t\t', 'kcd->gasm[i].r[j].r', 'i', 'j'));
  else if (line.includes('p[3] = (double *)(plst[((unsigned long)rp->r)>>28]'))
    out.push(CHK('\t\t\t\t', 'rp->r', 'i', '3'));
  else if (line.includes('p[j] = (double *)(plst[((unsigned long)rp->r)>>28]'))
    out.push(CHK('\t\t\t\t\t', 'rp->r', 'i', 'j'));
  /* fixup 链的尾巴：这时候 p[] 已经是最终地址了，查"落在哪儿"。 */
  else if (line.includes('== KEDX) p[j] += kcd->gasm[i].r[j].q;'))
    out.push('\t\t\tif (pd_run_dbg) pd_a64_chkp(kcd,parmdat,kcd->gasm[i].r[j].r,p[j],i,j);');
  else if (line.includes('== KGLB) p[3] = '))
    out.push('\t\t\t\tif (pd_run_dbg) pd_a64_chkp(kcd,parmdat,rp->r,p[3],i,3);');
  else if (line.includes('== KGLB) p[j] = ') && line.includes('rp->q*8'))
    out.push('\t\t\t\t\tif (pd_run_dbg) pd_a64_chkp(kcd,parmdat,rp->r,p[j],i,j);');
}
if (patched !== 52) throw new Error(`应当改 52 处 dafunc(…)，实际 ${patched} 处`);

const HEAD = [
  '/* port/a64/pd_a64_run.c —— `kasm87c_run` 的 arm64 替身（**由 tools/mkrun.mjs 生成**）。',
  ' *',
  ' * ## 为什么要它：Apple 的 arm64 上"变参"与"定参"不是同一套调用约定',
  ' *',
  ' * 原文把宿主函数指针声明成 `double (__cdecl *)(double,...)`（`kasm_interp.c:140`），',
  ' * 然后 `dafunc(*p[1],*p[2],*p[3])`。在 x86 上变参与定参一样（实参全压栈），所以没事；',
  ' * **Apple 的 arm64 上变参实参一律走栈**，而 `qglVertex3d(double,double,double)` 是定参、',
  ' * 从 d0/d1/d2 取 —— 于是只有第一个实参对。',
  ' *',
  ' * 量到的：脚本写 `glVertex(-1,-1,-2)`，`qglVertex3d` 收到 `(-1,-2,-1)`。',
  ' * 这就是"画面全黑"的根：调用都发生了、GL 不报错、FBO 也绑对了，只是坐标是垃圾。',
  ' *',
  ' * ## 与原文的差别只有两处（`diff -u eval/kasm_interp.c` 那一段看得见）',
  ' *',
  ' *   1. 那个大 switch 里 **52 处** `dafunc(…)` 全换成精确原型的强转',
  ' *      （原型串就在同一行的 `strncmp(cptr,"ddD",…)` 里：d -> double、D -> double *）；',
  ' *   2. switch 前面插一档：**脚本自己的函数是真变参**（走 pd_a64_jit.c 的 thunk，',
  ' *      尾跳到 `kasm87c`/`kasm87cp`），不能按定参调 —— `pd_a64_owns()` 把它分出去，',
  ' *      自己摊一份 parmdat 直接递归调 `kasm87c_run`。',
  ' *',
  ' * 顺带记一笔原文的既有缺口（**不是我们弄的**）：那个 switch 的原型只认 `d`/`D`，',
  ' * 没有 `C`（char *）那一档 —— 所以 `printf("…")` 这种带字符串的宿主函数在',
  ' * COMPILE==0 那条路上本来就不会被调（而且 `myprintf` 自己是真变参，',
  ' * 按定参强转也不对）。要补得另起一档。',
  ' */',
  '#if (COMPILE == 0)',
  '',
  '/* 下面那个 `pd_a64_call_script` 要递归调它，所以先报个名。 */',
  'double kasm87c_run (char *parmdat, kcd_t *kcd);',
  '/* JIT 在这个文件**后头**才 include 进来（它要 kcd_t），所以也先报个名。 */',
  'static void *pd_a64_jitfn (kcd_t *kcd);',
  '',
  '/* —— 本机补的诊断（`PD_RUNDBG=1` 打开）——',
  ' *',
  ' * `plst[]` 只填了六族（KECX/KEDX/KESP/KPTR/KIMM/KGLB），十六格里剩下的从来没人写。',
  ' * 要是有操作数带着别的族进到这儿，算出来的 `p[j]` 就是栈上的垃圾 ——',
  ' * 语料里十份脚本崩在 TIMES / PEEK / POKE 上，形状正是这个。',
  ' * 所以先把十六格全填成毒值，再逐个操作数查：撞上毒值就把族号印出来（每族只印一次）。',
  ' */',
  '#define PD_PLST_POISON ((long)0x5bad5bad5bad5badLL)',
  'static int pd_run_dbg = -1;',
  'static void pd_a64_chk (const long *plst, long r, long op, long which)',
  '{',
  '\tstatic int seen[16];',
  '\tlong fam = ((unsigned long)r)>>28;',
  '\tif (plst[fam] != PD_PLST_POISON) return;',
  '\tif (seen[fam]) return;',
  '\tseen[fam] = 1;',
  '\tfprintf(stderr,"[run] plst 那一族没人填：fam=%lx r=%08lx（第 %ld 条指令的第 %ld 个操作数）\\n",',
  '\t\tfam,(unsigned long)r,op,which);',
  '}',
  '',
  '/* 第二格诊断：**算完的 p[j] 落在哪儿**。`plst` 那一格只能查"族有没有人填"，',
  ' * 查不出"族对、偏移不对"。这儿把已知的几块地盘列出来（kcd 那一大块、gvl 值栈、',
  ' * parmdat、gstatmem），不落在里头就印一行。每个"族+指令"只印一次，免得刷屏。 */',
  'static void pd_a64_chkp (const kcd_t *kcd, const char *parmdat, long r, const double *q, long op, long which)',
  '{',
  '\tstatic long seen[64]; static int nseen = 0;',
  '\tlong fam = ((unsigned long)r)>>28, key = (fam<<20)+(op&0xfffff), i;',
  '\tconst char *pc = (const char *)q;',
  '\tconst char *lo, *hi;',
  '\tif (!q) return;',
  '\t/* kcd 那一大块（头 + data 里的 globval/gasm/rxi/gevalext/newvar/newvarnam） */',
  '\tlo = (const char *)kcd; hi = lo + sizeof(kcd_t)',
  '\t\t+ kcd->gccnt*(long)sizeof(double) + kcd->gstnum + kcd->arrnum',
  '\t\t+ kcd->gecnt*(long)sizeof(gasmtyp) + kcd->numrxi*(long)sizeof(rtyp)',
  '\t\t+ kcd->gevalextnum*(long)sizeof(evalextyp) + kcd->newvarnum*(long)sizeof(newvartyp)',
  '\t\t+ kcd->newvarplc;',
  '\tif ((pc >= lo) && (pc < hi)) return;',
  '\tif ((pc >= (const char *)gvl) && (pc < (const char *)(gvl+65536))) return;   /* 值栈 */',
  '\tif ((pc >= parmdat) && (pc < parmdat+sizeof(double)*16)) return;             /* 参数区 */',
  '\tif (gstatmem && (pc >= (const char *)gstatmem) && (pc < (const char *)gstatmem+kcd->arrnum)) return;',
  '\tfor(i=0;i<nseen;i++) if (seen[i] == key) return;',
  '\tif (nseen < 64) seen[nseen++] = key;',
  '\tfprintf(stderr,"[run] 指针落在地盘外：fam=%lx r=%08lx p=%p（第 %ld 条指令的第 %ld 个操作数）"',
  '\t\t" gstatmem=%p arrnum=%ld\\n",',
  '\t\tfam,(unsigned long)r,(const void *)q,op,which,(const void *)gstatmem,(long)kcd->arrnum);',
  '}',
  '',
  '/* 第三格诊断：**执行前** p[0..2] 里有没有落在头一页的（基址 0 + 小偏移）。',
  ' * 崩的地址是 0x0 / 0x12 这种，说明某一族的基址压根是 0（最可能是 gstatmem==0',
  ' * 却仍有 KGLB 操作数，或 gevalext[].ptr 是空）。这儿把指令号、opcode 和三个',
  ' * 操作数的 r/q 原样印出来，印完直接 return 免得真的崩。 */',
  'static int pd_a64_chk0 (const kcd_t *kcd, long i, double **p)',
  '{',
  '\tlong j, bad = -1;',
  '\tfor(j=0;j<3;j++) if (((unsigned long)p[j]) < 4096UL) { bad = j; break; }',
  '\tif (bad < 0) return(0);',
  '\tfprintf(stderr,"[run] 操作数落在头一页：第 %ld 条指令 f=%d 的第 %ld 个操作数 p=%p\\n",',
  '\t\ti,(int)kcd->gasm[i].f,bad,(void *)p[bad]);',
  '\tfor(j=0;j<3;j++)',
  '\t\tfprintf(stderr,"[run]   r[%ld]: r=%08lx q=%ld p=%p\\n",',
  '\t\t\tj,(unsigned long)kcd->gasm[i].r[j].r,(long)kcd->gasm[i].r[j].q,(void *)p[j]);',
  '\tfprintf(stderr,"[run]   gstatmem=%p kcd->arrnum=%ld kcd->globval=%p gccnt=%ld gstnum=%ld\\n",',
  '\t\t(void *)gstatmem,(long)kcd->arrnum,(void *)kcd->globval,(long)kcd->gccnt,(long)kcd->gstnum);',
  '\tfprintf(stderr,"[run]   gnumarg=%ld newvarnum=%ld\\n",(long)kcd->gnumarg,(long)kcd->newvarnum);',
  '\tfor(j=0;j<kcd->gnumarg;j++)',
  '\t\tfprintf(stderr,"[run]   newvar[%ld]: r=%08lx parnum=%d maxind=%d nam=%s\\n",',
  '\t\t\tj,(unsigned long)kcd->newvar[j].r,(int)kcd->newvar[j].parnum,(int)kcd->newvar[j].maxind,',
  '\t\t\t&kcd->newvarnam[kcd->newvar[j].nami]);',
  '\tfflush(stderr);',
  '\treturn(1);',
  '}',
  '',
  '/* 脚本函数那一档：按原型串摊一份 parmdat（指针原样放，double 取值），',
  '   然后直接递归调 `kasm87c_run` —— kcd 记在 thunk 那一格的尾巴上。 */',
  'static double pd_a64_call_script (void *thunk, const char *proto, double **p, long n)',
  '{',
  '\tchar parmdat[sizeof(double)*16];',
  '\tkcd_t *kcd = *(kcd_t **)&((char *)thunk)[56];',
  '\tlong i, j = 0;',
  '\tif (!kcd) return(0.0);',
  '\tfor(i=1;i<=n;i++)',
  '\t{',
  '\t\tif (j+8 > (long)sizeof(parmdat)) break;',
  '\t\tif (proto[i-1] == \'D\') *(void **)&parmdat[j] = (void *)p[i];',
  '\t\telse                   *(double *)&parmdat[j] = *p[i];',
  '\t\tj += 8;',
  '\t}',
  '\t/* **这儿也要问一句 JIT**（`port/a64/pd_a64_jitc.c`）：脚本函数的递归全走这条路，',
  '\t   不问的话被调那一份永远在解释器上跑 —— `fib(20)` 量出来只快 1.18 倍就是这个。',
  '\t   那一份在这个文件后头才 include 进来，所以上头报了个名。 */',
  '\t{',
  '\t\tdouble (*jf)(char *, kcd_t *) = (double (*)(char *, kcd_t *))pd_a64_jitfn(kcd);',
  '\t\tif (jf) return(jf(parmdat,kcd));',
  '\t}',
  '\treturn(kasm87c_run(parmdat,kcd));',
  '}',
  '',
];
const TAIL = ['', '#endif', ''];

const u8 = (s) => Buffer.from(s, 'utf8').toString('latin1');
const enc = (l) => (/[^\x00-\x7f]/.test(l) ? u8(l) : l);
writeFileSync(OUT, [...HEAD, ...out, ...TAIL].map(enc).join('\n'), 'latin1');
console.log(`写了 ${HEAD.length + out.length + TAIL.length} 行，改了 ${patched} 处 dafunc(…)`);
