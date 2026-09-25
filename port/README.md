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

再往后（出图那一半）：`kplib.c` 在 arm64 上**零错误**直接编过；`polydraw.c` 现在
也**编过了（0 错误）**，靠的是 `port/a64/winshim/` 那几份**假头文件** ——
`pd/pd_head.h` 那 18KB 一个字节都没改。

三处不显然的地方：

* **`gl/gl.h`**：Windows 那份只有 GL 1.1，GL 2.0 那一批（`glUniform*` /
  `glCreateShader` …）在 polydraw 里是**自己一张函数指针表**。macOS 的
  `OpenGL/gl.h` 把它们当真函数声明了 —— 43 个 redefinition。办法是先把 SDK 那份
  包进来（真函数照旧叫原名），**然后把那 43 个名字 `#define` 成 `pd_*`**：
  宏从那一行往后生效，于是 `pd_head.h` 的指针表与后面全部调用点一致改名，自己一套；
* **`-fms-extensions`**：`10000000000000I64` 这种 MSVC 整数后缀
  （`pd_host_gl.c:722`）。字面量后缀是 pp-number 的一部分，宏碰不到它，
  只能靠编译器开关；
* **那三个 `-Wno-`**：原文是 C89，clang 16 起把 implicit-int /
  implicit-function-declaration / int-conversion 提成了错误。

## 出图那一半：现在到哪儿了

`bench/build-a64.sh` 一路走到 **`bench/out/polydraw_a64`（连上了）**。三份新实现：

* `pd_win_a64.c` —— 81 个 win32 函数。计时（`QueryPerformanceCounter` ->
  `clock_gettime`，频率报 1e9）、`GetModuleFileName`（`_NSGetExecutablePath`）、
  ini 三个（真读写文件）是**真的**；窗口/菜单/光标/字体/对话框空壳回成功；
  线程/管道/进程/MIDI 空壳回失败（polydraw 判返回值，回失败它就走单线程那条路）；
* `pd_gl_cgl.c` —— `wgl*` 走 **CGL 离屏上下文**（不要 GLFW/NSOpenGL：不碰 AppKit、
  不要主线程与 run loop）+ 一张 FBO 当"默认帧缓冲"。`wglGetProcAddress` 是
  `dlsym(RTLD_DEFAULT, 名字)`（legacy GL 的符号全在 OpenGL.framework 里），
  找不着就再试 `*EXT` / `*ARB`。`SwapBuffers` 就是**"一帧画完"的钩子**：数帧、
  到点 `glReadPixels` 写 PNG（PNG 写出器也在这份里 —— kplib 只读不写，
  deflate 用存储块）；
* `pd_main_a64.c` —— `main()` 把 `.pss` 读进一格全局，**`GetWindowText` 回它**
  （假编辑框，于是 `pd_script.c` 的 `Draw` 拿到的就是脚本正文），然后交给原文的
  `WinMain`。命令行给 `/bench:N` —— 那是先前给 `polydraw.c` 加的插桩，它自己会
  第 30 帧起计时、跑满 N 帧退出，于是**不用重写帧循环，只要喂它**。

**现状（别夸大）**：跑得起来、不崩、PNG 写出来了 —— 但**画面是全透明黑，
几何没落上去**。已经排掉的一条：读像素前把我们的 FBO 绑回来（polydraw 自己也用
FBO，画完会 bind 回 0，而离屏根本没有"0"那张）—— 绑了还是黑。

下一个探针（按这个顺序，别猜）：
1. **`Draw` 大概率压根没被调**。`pd_win.c:721` 是 `if (shadn[2]) Draw(...)`，而
   `shadn[2]` 是"片元着色器有几个"。链路是这样的（照 `pd_script.c` 读的）：
   `txt2sec` 按行首的 `@` 分段，`typ` 0=`@h`（主脚本）/1=`@v`/2=`@g`/3=`@f`；
   `setShaders:171` 那个循环 `if (!tsec[tseci].typ) continue;` —— **主脚本那一段
   直接跳过**；而 `:180` 的 `if (!needrecompile) return;` 对"只有主脚本"的
   `.pss`（`ken/balls.pss`、`ceilflor2.pss` 都是）恰好成立，于是 `shadn[2]` 一直是 0。
   所以要么 Windows 上有别处给它塞了一个**默认片元着色器**（`setshader_int(0,-1,0)`
   那一句最可疑），要么我们喂进去的文本没被 `txt2sec` 当成一段。
   **量过了**（lldb 直接读那几个 static，没碰原文）：
   `tsecn=1 / tsec[0].typ=0 / shadn={0,0,0} / gevalfunc=0` ——
   假编辑框是好的（`tsec[0]` 的 120 字节就是我们喂的脚本），拦住的就是 `shadn[2]`。
2. **给脚本补一个 `@f` 段，`Draw` 就进去了** —— 然后崩在 `kasm87c_run + 428`，
   与真 `.pss`（`ken/ceilflor2.pss`）**同一处**。那一行是 `kasm_interp.c:79`：

   ```c
   if ((r&0xf0000000) == KPTR) p[j] = (*(double **)p[j]) + q;
   ```

   `plst[KPTR>>28] = (long)parmdat-KPTR`，于是 `p[j]` 指着 `parmdat` 里某一格，
   再按 `double **` 解引用 —— 在 arm64 上读 **8 个字节**，而写那一格的
   `kasm87cp` 是按"指针 4 字节"排的偏移（`j += 4`），编译期给参数分偏移那一段
   （`newvar[].r` 的 KESP 偏移）也是 4。反汇编 `ldr x9,[x9,#8]` 正好对上。

   **所以出图剩下的就是第 5 个洞那一件事**：参数区的指针宽度 4 -> 8。
   要动"编译期分偏移"与 `kasm87cp`/`kasm87c` 写变参那两处 —— 都在原文里，
   得按现在这套办法（缝合文件里 `#define` 改名 + 新文件）换掉那几格。
