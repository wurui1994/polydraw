/* port/pd_port.h —— 本机（arm64 / macOS）编原版 polydraw 用的补丁头。
 *
 * 一条规矩：**原文一个字节都不动**。缺的东西一律放这儿，用 `clang -include` 强制
 * 在每份翻译单元最前面包进去。
 *
 * 为什么只剩这么几行：Ken 自己在 `eval.c:281-295` 已经写了一整块"不是 MSVC 就这样"
 * 的等价物（`__cdecl` / `_inline` / `LL` / `__int64` / `_snprintf` / `stricmp`），
 * 所以这儿只补他漏掉的那几个。
 */
#ifndef PD_PORT_H
#define PD_PORT_H

/* `eval.h:15-21` 用的是**一个下划线**的 `_cdecl`（老 MSVC 的写法），而 `eval.c` 里
   那块等价物只定义了两个下划线的 `__cdecl`。不补这一个，那七句原型全成了
   `void *_cdecl kasm87(...)` -> "expected ';' after top level declarator"。 */
#ifndef _MSC_VER
#define _cdecl
#endif

/* `__forceinline`：eval_test.c 与 polydraw.c 各有几处，只在 MSVC 那条分支里用到，
   但留着不亏 —— clang 认 `__attribute__((always_inline))`。 */
#if !defined(_MSC_VER) && !defined(__forceinline)
#define __forceinline inline __attribute__((always_inline))
#endif

/* `memicmp`（polydraw.c 里 20 处）：MSVC 的"大小写不敏感的 memcmp"。
   BSD 那边没有，自己写一个 —— 语义照 MSVC：回 <0 / 0 / >0。 */
#if !defined(_MSC_VER)
#include <ctype.h>
#include <string.h>
static int pd_memicmp (const void *a, const void *b, unsigned long n)
{
	const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
	unsigned long i;
	for(i=0;i<n;i++)
	{
		int x = tolower(p[i]), y = tolower(q[i]);
		if (x != y) return(x-y);
	}
	return(0);
}
#define memicmp pd_memicmp
#endif

#endif
