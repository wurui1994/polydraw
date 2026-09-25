import { readFileSync, writeFileSync } from 'node:fs';
const D = '/Users/wurui/Documents/polydraw-bench/';
const src = readFileSync(`${D}polydraw_src/eval/kasm_main.c`, 'latin1').split('\n');
/* 原文（1 起）：403 `#else`、404 `??? not implemented`、405 `#endif`、
   406 空行、407 注释、408~411 那四句写头字、412 空行、413 起收尾。 */
const want = (n, s) => {
  if (!src[n - 1].includes(s)) throw new Error(`第 ${n} 行不是 "${s}"：${src[n - 1]}`);
};
want(403, '#else');
want(404, '??? not implemented');
want(405, '#endif');
want(408, '= jumpbacknum;');
want(411, 'memcpy((void *)(((long)v)+kasm87leng)');
want(29, '(((i+31)>>5)<<2)');

/* 第 15 个洞：`texttrans` 那块位图按"long 是 4 字节"算大小。
 *
 * `kasm_cpu.c:73` 是 `static long *texttrans`，写的一边是
 * `texttrans[i>>5] |= (1<<i)`（一格 long 管 32 个字符）；
 * 可第 29 行算的是 `texttransn = ((len+31)>>5)<<2` —— **每 32 个字符 4 字节**。
 * 32 位 x86 上 sizeof(long)==4，正好；LP64 上一格是 8 字节，于是要的是它的两倍。
 *
 * 量到的：`tigrou/balls2k.pss` 的 eval 段（bakz 约 5.5KB）要 1384 字节，
 * 只 malloc 了 max(692,1024)=1024 —— 越界 360 字节，正好压在紧接着 malloc 的
 * `tbufmal` 头上：tbuf 前 25 个字节变成位图的垃圾，于是括号扫描报
 * "ERROR: too many }"；-O2 上还会在后面 free 的时候 abort（rc=134，堆被写坏）。
 *
 * 改法：按 `sizeof(long)` 算。`texttransn` 还被 `kasm_comp.c:1440` 当"字符数上限"
 * 用（原文自己把字节数当字符数），翻倍只是把那个上限放宽，不影响别的。
 */
const fixTexttrans = (l) => l.replace('(((i+31)>>5)<<2)',
  '(((i+31)>>5)*(long)sizeof(long)) /*本机改：一格 long 在 LP64 上是 8 字节，见头注第 15 个洞*/');

const head = src.slice(0, 402).map(fixTexttrans);   // 1..402
const hdrwrite = src.slice(405, 411);      // 406..411（空行 + 注释 + 四句）
const tail = src.slice(411);               // 412..

const note = [
  '#else',
  '',
  '\t\t//—— 这里是本机（arm64/osx）补上的那一格 ——',
  '\t\t//原文这条分支只有一句 `??? not implemented .. need to fix .. sorry :/`：',
  '\t\t//COMPILE==0 时 kasm87comp 回来的不是一块机器码，而是 kasm87c/kasm87cp 的地址',
  '\t\t//（kasm_interp.c 末尾那个"一次只跑一份脚本"的 hack：它把 kcd 放进全局',
  '\t\t//gkasm87cptr，然后把解释器入口当函数指针交回来）。所以：',
  '\t\t//  * 没有码块要合并 —— v 已经是可调用的东西；',
  '\t\t//  * **绝不能**写 v-FUNCBYTEOFFS 那三个头字：FUNCBYTEOFFS 是 0，v 指着',
  '\t\t//    kasm87c 的代码段，写下去直接 SIGBUS。上面那四句因此挪进了 #if 里。',
  '\t\t//v 是 port/a64/pd_a64_jit.c 造的那一格 thunk（见那份文件的头注）。gstatmem 原文',
  '\t\t//是写进头字 +8 的，这儿记进 thunk 的账里 —— kasm87free 要靠它把那块也放掉。',
  '\tpd_a64_set_statmem(v,gstatmem);',
  '',
  '\t\t//再把每一格 kcd 里那份 gevalext 抄本按现在的 gevalext[] 刷一遍 —— 递归与向后',
  '\t\t//引用要靠它（抄的时候自己那一格的 .ptr 还是空的）。x86 那条路是上面 patch[]',
  '\t\t//那个循环干的同一件事，解释器没有码字要补，要补的是那份抄本。',
  '\tfor(i=0;i<funcnt;i++) pd_a64_refresh_ext((void *)gevalext[gevalextnum-i].ptr,gevalext,gevalextnum);',
  '\tkasm87leng = 0;',
  '#endif',
];

const out = [
  '/* port/a64/kasm_main_a64.c —— `eval/kasm_main.c` 的 arm64 替身。',
  ' *',
  ' * 与原文的差别只有两处：`#if (COMPILE != 0)` 那条分支的收尾，以及 `texttrans`',
  ' * 那块位图的大小（第 15 个洞，见 tools/mkmain.mjs 的注）。',
  ' * `diff -u polydraw_src/eval/kasm_main.c port/a64/kasm_main_a64.c` 看得见全部改动。',
  ' * 原文一个字节都没动 —— 这一份是新材料，只有 arm64 的缝合文件包含它。',
  ' */',
  ...head,
  ...hdrwrite,
  ...note,
  ...tail,
];
/* 注释是中文（UTF-8），而原文那几百行必须按字节原样留着（latin1）——
   所以把新写的那几行先转成 UTF-8 的字节，再当 latin1 串拼进去。 */
const u8 = (s) => Buffer.from(s, 'utf8').toString('latin1');
writeFileSync(`${D}port/a64/kasm_main_a64.c`, out.map((l) => (/[^\x00-\x7f]/.test(l) ? u8(l) : l)).join('\n'), 'latin1');
console.log(`写了 ${out.length} 行`);
