import { readFileSync, writeFileSync } from 'node:fs';
const D = '/Users/wurui/Documents/polydraw-bench/';
const src = readFileSync(`${D}polydraw_src/eval/kasm_parse.c`, 'latin1').split('\n');

/* 第 17 个洞：多维数组的"每一维有多大"那张表，一格是 **4 字节**，可两头都写成
 * `long *` —— 32 位 x86 上 sizeof(long)==4 正好，LP64 上一格变 8 字节。
 *
 * 表在哪：`kasm_state.c:99` 的注就写着 `ArrayDims:{(~parnum)*4}` ——
 * 每一维 4 字节，摆在 `newvarnam[newvar[i].proti]` 起。
 *   * 写的一边（第 144 行）：`*(long *)&newvarnam[newvarplc] = i; newvarplc += 4;`
 *     —— 写 8 个字节只进 4 个，且 `checkvarchars(newvarplc+4)` 只保了 4 个；
 *   * 读的一边（第 1086 行）：`((long *)&newvarnam[newvar[i].proti])[l]`
 *     —— 按 8 字节一格读，于是 `static planes[6][4]` 读出来是
 *     `0x0000000400000006`（两维挤在一格里），乘出来的"维度合并乘数"是 2.75e19。
 *
 * 量到的：那个乘数进了常量表，折出来的下标 `(long)p2` 是 0x7fffffffffffffff，
 * `kasm_opt.c:339` 的越界闸把它拦下 -> `kasmoptimizations()` 回 -1 ->
 * `kasm87comp` 回 0（**这一条路不设 kasm87err**，所以外头只看到"编译失败"）->
 * `kasm87` 走清理，`free(gevalext[j].ptr - FUNCBYTEOFFS)` 去 free 我们的 thunk 槽
 * （mmap 的页，不是 malloc 的）-> `rc=134`。tigrou/balls2k 与 metaballs_cube 两份
 * 就崩在这儿，形状是 abort 而不是段错误。
 *
 * 改法：两处都换成 `int *`（4 字节，与那张表的口径一致）。原文一个字节没动 ——
 * 这一份是生成出来的替身，`diff -u polydraw_src/eval/kasm_parse.c` 看得见全部改动。
 */
const want = (n, s) => {
  if (!src[n - 1].includes(s)) throw new Error(`第 ${n} 行不是 "${s}"：${src[n - 1]}`);
};
want(144, '*(long *)&newvarnam[newvarplc] = i; newvarplc += 4;');
want(1086, '((long *)&newvarnam[newvar[i].proti])[l];');

const NOTE = '/*本机改：一维 4 字节（见 tools/mkparse.mjs 第 17 个洞）*/';
src[143] = src[143].replace('*(long *)&newvarnam[newvarplc] = i;',
  `*(int *)&newvarnam[newvarplc] = i; ${NOTE}`);
src[1085] = src[1085].replace('((long *)&newvarnam[newvar[i].proti])[l];',
  `((int *)&newvarnam[newvar[i].proti])[l]; ${NOTE}`);

const out = [
  '/* port/a64/kasm_parse_a64.c —— `eval/kasm_parse.c` 的 arm64 替身（**tools/mkparse.mjs 生成**）。',
  ' *',
  ' * 与原文的差别只有两处强转：多维数组的维度表一格是 4 字节，原文两头都写成',
  ' * `long *`（32 位 x86 上正好）。`diff -u polydraw_src/eval/kasm_parse.c',
  ' * port/a64/kasm_parse_a64.c` 看得见全部改动；原文一个字节都没动。',
  ' */',
  ...src,
];
const u8 = (s) => Buffer.from(s, 'utf8').toString('latin1');
writeFileSync(`${D}port/a64/kasm_parse_a64.c`,
  out.map((l) => (/[^\x00-\x7f]/.test(l) ? u8(l) : l)).join('\n'), 'latin1');
console.log(`写了 ${out.length} 行`);
