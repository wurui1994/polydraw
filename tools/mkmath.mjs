import { readFileSync, writeFileSync } from 'node:fs';
const D = '/Users/wurui/Documents/polydraw-bench/';
const src = readFileSync(`${D}polydraw_src/eval/kasm_math.c`, 'latin1').split('\n');

/* 第 21 个洞（**`rnd` 从一开始就是错的、`nrnd` 几乎死循环**）：
 *
 *   static long krand () { kholdrand = (unsigned long)((kholdrand*(214013*2)+2531011*2)>>1); return(kholdrand); }
 *
 * 那句 `(unsigned long)` 在 32 位 x86 上是**32 位**：乘加自然回绕，`>>1` 之后落在
 * [0, 2^31)，正好配 `oneover2_31`（`RND` 就是 `krand()*oneover2_31`，要的是 [0,1)）。
 * LP64 上 `unsigned long` 是 64 位 —— **不回绕**，于是：
 *
 *   * `RND` 回的是十亿量级的数（量到 `(){rnd}` = **-1043597862.5**，不是 [0,1)）；
 *   * `NRND` 那个 Box-Muller 的拒绝采样 `do{…}while(r>=1)` 几乎**永不接受**
 *     （x、y 都是天文数字）—— 每次接受的概率约 2^-33。
 *
 * 这一格解释了两件一直没解释的事：`bench/scan-a64.sh` 那两份"超时"
 * （`ken/balls.pss` / `particules_sparks` —— 采样 4004/4004 个样本全在 `nrnd` 里），
 * 以及所有用 `rnd` 的脚本画出来的东西都不对（只是"画出来了"所以没人看）。
 *
 * 改法：把截断挪到**移位之前** —— `(unsigned int)(和) >> 1`。
 * 注意不能只把 `(unsigned long)` 换成 `(unsigned int)`：那样 `>>1` 还是在 64 位里做的，
 * 先移位再截断与"先回绕再移位"**不是一回事**（量到的还是 8515129.92，仍然不在 [0,1)）。
 * 原文一个字节没动 —— 这一份是生成出来的替身，`diff -u` 看得见全部改动。
 */
const want = (n, s) => {
  if (!src[n - 1].includes(s)) throw new Error(`第 ${n} 行不是 "${s}"：${src[n - 1]}`);
};
want(4, 'kholdrand = (unsigned long)((kholdrand*(214013*2)+2531011*2)>>1);');

const NOTE = '/*本机改：先按 32 位回绕再移位（第 21 个洞，见 tools/mkmath.mjs）*/';
src[3] = src[3].replace('(unsigned long)((kholdrand*(214013*2)+2531011*2)>>1)',
  `((unsigned int)(kholdrand*(214013*2)+2531011*2)>>1) ${NOTE}`);

const out = [
  '/* port/a64/kasm_math_a64.c —— `eval/kasm_math.c` 的 arm64 替身（**tools/mkmath.mjs 生成**）。',
  ' *',
  ' * 与原文的差别只有**一个记号**：`krand()` 里那句 `(unsigned long)` 换成',
  ' * `(unsigned int)` —— 32 位 x86 上 unsigned long 就是 32 位，LP64 上不是，',
  ' * 于是 `rnd` 回十亿量级的数、`nrnd` 的拒绝采样几乎死循环。详见生成脚本的头注。',
  ' */',
  ...src,
];
const u8 = (s) => Buffer.from(s, 'utf8').toString('latin1');
writeFileSync(`${D}port/a64/kasm_math_a64.c`,
  out.map((l) => (/[^\x00-\x7f]/.test(l) ? u8(l) : l)).join('\n'), 'latin1');
console.log(`写了 ${out.length} 行`);
