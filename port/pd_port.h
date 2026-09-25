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

/* `min`/`max`/`_alloca`/`stricmp`/`_snprintf`/`_vsnprintf`：`eval.c` 自己有一份
   （`kasm_head.h:227` 与 `eval.c:281-295`），但 `polydraw.c` 那个翻译单元没有。
   一律 `#ifndef` 起来 —— eval 那边后面再 `#define` 的是同样的内容，同样的宏重定义是允许的。 */
#ifndef _MSC_VER
#include <strings.h>
#include <alloca.h>
#include <stdarg.h>
#ifndef min
#define min(a,b)  (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a,b)  (((a) > (b)) ? (a) : (b))
#endif
#ifndef _alloca
#define _alloca alloca
#endif
#ifndef stricmp
#define stricmp strcasecmp
#endif
#ifndef _snprintf
#define _snprintf snprintf
#endif
#ifndef _vsnprintf
#define _vsnprintf vsnprintf
#endif
#endif

/* SEH（`pd_script.c:295` 的 `__try`/`__except`）—— 那是 Windows 的结构化异常，
   clang 在 arm64 上不支持（`SEH '__try' is not supported on this target`）。
   这儿把它降成"没有守护"：脚本崩了就整个进程崩。
   为什么不用 sigsetjmp 做个等价的：`sigsetjmp` 必须落在 `safeevalfunc` 自己的栈帧里，
   宏能写出来，但"正常走完之后要撤掉 handler"那一步宏插不进去 —— 留着一个指向
   已经返回的栈帧的 jmp_buf 比不守护更危险。等真需要了再做（要改 pd_script.c 那一格）。 */
#if !defined(_MSC_VER) && !defined(__try)
#define __try if (1)
#define __except(x) else if (0)
#endif

#endif
