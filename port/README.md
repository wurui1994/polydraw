# `port/` —— 在 arm64 macOS 上跑原版 polydraw

一条规矩：**`polydraw_src/` 下面的原文一个字节都不动**。缺的东西全在这个目录里，
靠三种手段挂上去：

* `clang -include port/pd_port.h` —— 补 MSVC 的关键字（只剩 `_cdecl` 与 `memicmp`，
  别的 Ken 自己在 `eval.c:281-295` 已经写了非 MSVC 的等价物）；
* `polydraw_src/*.a64.stitch.c` —— arm64 专用的缝合文件（新材料），它可以
  **换掉某一格**、也可以在两个 `#include` 之间插 `#define` 改名；
* `port/a64/*.c` —— 那些"换掉的格子"与新写的实现。

编：`bench/build-a64.sh`。

## 原文在非 x86 上的四个洞（都是它自己写着的，不是猜的）

`COMPILE` 的默认值在 `eval/kasm_state.c:4-11`：x86 是 1（真编译），别的平台是 0
（"Virtual machine (PowerPC)"）。可是那条 0 的路**从来没通过**：

1. **`kasm_main.c:403`**（`kasm87` 的收尾）：`??? not implemented .. need to fix .. sorry :/`。
   —— 换成 `port/a64/kasm_main_a64.c`。差别只有十几行，`diff -u` 一眼看完：
   那四句"写 `v-FUNCBYTEOFFS` 三个头字"挪进 `#if (COMPILE != 0)` 里
   （`FUNCBYTEOFFS` 是 0，往那儿写就是往代码段写，直接 SIGBUS）。

2. **`kasm_interp.c:336-354`**（`kasm87c_copyglob2struct` 的 `codestub`）：
   非 x86 上只有一段 "PPC guess" 的注释，然后一句 hack ——
   *This temp hack allows 1 script in memory to run on a non-x86 platform*：
   把 kcd 塞进全局 `gkasm87cptr`，**直接把解释器入口的地址当函数指针交回去**。
   于是多函数脚本必崩（子函数拿到 main 的 kcd），而且交回去的是代码段地址。
   —— `port/a64/pd_a64_jit.c` 给每份脚本造一格真 thunk（14 条 arm64 指令，
   全立即数，不需要重定位），代码放自己 mmap 的页里（写 RW / 跑 RX，
   macOS 的 arm64 不给 RWX，而 kcd 那块数据还得能写）。

3. **`kasm_comp.c:1-32`**（`kasm87free` / `kasm87jumpback`）：读的是 `f-FUNCBYTEOFFS`
   那三个头字，COMPILE==0 时压根没人写过它们 —— `kasm87free` 会 `free()` 一个
   不是 malloc 来的地址，当场 abort（真踩过：值算对了，收尾 `Abort trap: 6`）。
   —— `port/a64/pd_a64_api.c` 接管这两个名字，记账挂在 thunk 那一格的尾巴上。

4. **`kcd` 里那份 `gevalext` 抄本没人回填** —— 递归与向后引用直接段错误。
   `kasm87c_copyglob2struct` 把 `gevalext[]` 抄进 kcd 的时候，**自己那一格的 `.ptr`
   还是空的**（编译次序是 i=funcnt-1 往 0 走，所以"后面的调前面的"碰得上，
   自己调自己碰不上），拿到 0 就跳过去。x86 那条路靠 `patch[]` 事后回填
   （`kasm_main.c:397`）；解释器没有码字要补，要补的是那份抄本。
   —— `pd_a64_refresh_ext`，在 `kasm87` 收尾时逐格刷一遍。

5. **还没补：`parmdat` 里指针占 4 个字节**。`kasm87cp`/`kasm87c`
   （`kasm_interp.c:236/257`）写变参时 `j += 4` 就是"指针 4 字节"，而
   `kasm87c_run` 读它用的是 `*(long *)`（在 arm64 上是 8 字节）。
   一个指针参数还侥幸能跑，**两个就错位**。这是原文 32 位假设的核心，
   要动的是"编译期给参数分偏移"那一段（`newvar[].r` 的 KESP 偏移）。

   现状：Ken 自带的例子 #1~#3 全对（`20°C = 68°F`、`hypot(3,4) = 5`、
   `sillypifunc` 五个值逐个对），**#4（两个函数指针）崩**。
   polydraw 自己**不吃这条路** —— 它调脚本是 `gevalfunc()`，零个参数
   （`polydraw.c:480/2036/2290`），所以这个洞不挡出图。

## 现在能跑到哪儿

```
$ bench/build-a64.sh          # 编（clang -O2，arm64）
$ bench/test-a64.sh           # 判据：16/16
```

那 16 格压的是：算术与优先级、`for` 循环、`*=`/`-=`、多函数脚本、
**递归**（`f(5)=120`、`fib(7)=13`）、`static`、内建函数。

量尺（`bench/out/eval_bench`，换掉了 Ken 那个永远印 `0 cc` 的 main）——
这台机器（arm64 macOS，clang -O2，**纯 C 解释器**，不是 JIT）：

```
(x)x+1                                0.0140 us/趟
(x)sin(x)*cos(x)+sqrt(x)              0.0385 us/趟
(x){s=0;for(i=0;i<x;i++)s=s+i*i;s} 1000  28.80 us/趟（≈ 28.8ns 一圈，一圈三个算子）
kasm87() 编译本身                     0.0137 ms/趟
```

**这不是 x87 JIT 那把尺子** —— 那把尺子只在 Windows/x86 上（`bench/build.cmd`
加 `/bench:N`）。这一列是"解释器在 arm64 上的地板"，真的 arm64 JIT 要拿它当参照物。

下一步（ADR-0045 D2）：真的 arm64 后端 —— 不再走解释器，把 `gasm[]` 直接
落成 arm64 机器码。thunk 这一格已经把"生成可执行内存"这条路走通了。

再往后（出图那一半）：`kplib.c` 在 arm64 上**零错误**直接编过了；`polydraw.c`
只差 `windows.h`（`pd/pd_head.h:10`）。计划是给 `port/a64/winshim/` 放几份
**假头文件**（`windows.h` / `process.h` / `gl/gl.h`），这样 `pd_head.h` 那 18KB
一个字节都不用改；win32 的实现（78 个函数，绝大多数在 `pd_win.c` 那份编辑器里）
按"出图只要 `pd_host_gl.c` + `pd_script.c`"的口径挑着补。
