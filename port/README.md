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

5. **`parmdat` 里指针占 4 个字节**（已补）。`kasm87cp`/`kasm87c`
   （`kasm_interp.c:236/257`）写变参时 `j += 4` 就是"指针 4 字节"，
   `kasm_comp.c:169/174` 分偏移也是 4，而 `kasm87c_run` 读它用的是
   `*(double **)` / `*(long *)`（在 arm64 上 8 字节）。一个指针参数侥幸能跑，
   **两个就错位**。
   —— `port/a64/pd_a64_parm.c`：写的一边接管 `kasm87c`/`kasm87cp`（顺带把原文读
   **全局** `newvar`/`gnumarg` 改成读 `kcd->` 那一份）；分偏移那一边**不 fork
   那个 62KB 的 `kasm87comp`**，改成 kcd 造好之后**事后重映射**（按 `newvar[]`
   里记的旧基址逐个累加出新基址，`gasm[]`/`rxi[]`/`newvar[]` 上所有
   KESP/KPTR 的偏移一起换）。

6. **`KIMM` 必须是 long 常量**（已补）。`kasm_state.c:36` 是
   `#define KIMM 0xb0000000` —— 类型是 **unsigned int**，于是
   `kasm_interp.c:36` 的 `((long) -KIMM)` 走无符号算术得 `0x50000000`，
   而不是 `-0xb0000000`。32 位上 `0x50000000+0xb0000000+idx` 溢出回绕**正好**
   得到 idx，arm64 上不回绕 —— `gevalext[0x100000000+idx]` 当场读飞。
   —— 缝合文件里 `#undef KIMM` / `#define KIMM 0xb0000000L`，一行。

   这两格补上之后 Ken 自带的 main 从"例子 #4 崩"走到了 **#1~#13 全对**
   （`sillydualfunc`、`dumbanglefunc`、`getunitvector`、`getcol`、
   数组按指针传的 `1,2,3 / 3,5,3 / 8,8,3` 全逐个对上）。

7. **宿主函数是用变参函数指针调的**（已补）。`kasm_interp.c:140` 把它声明成
   `double (__cdecl *)(double,...)`，然后 `dafunc(*p[1],*p[2],*p[3])`。
   在 x86 上变参与定参的调用约定一样（全压栈），所以没事；
   **Apple 的 arm64 上变参实参一律走栈**，而 `qglVertex3d(double,double,double)`
   是定参、从 d0/d1/d2 取 —— 于是只有第一个实参对。
   量到的：脚本写 `glVertex(-1,-1,-2)`，`qglVertex3d` 收到 `(-1,-2,-1)`。
   —— `port/a64/pd_a64_run.c`（**由 `tools/mkrun.mjs` 生成**，`diff` 可审）：
   那个大 switch 里 52 处 `dafunc(…)` 全换成精确原型的强转；
   而**脚本自己的函数是真变参**（走 thunk），`pd_a64_owns()` 把它分出去，
   自己摊一份 parmdat 直接递归调 `kasm87c_run`。
   补完之后 `qglColor3d(1,0,0)` / `qglVertex3d(-1,-1,-2)` 逐个对上。

   原文的既有缺口（不是我们弄的）：那个 switch 的原型只认 `d`/`D`，没有 `C`
   （char *）—— 这一格后来单独补了，见下面**第 18 个洞**（它是"空画面"的总根）。
   `printf` 那一族仍然不接（`myprintf` 自己是真变参，按定参强转也不对）。

8. **polydraw 的控制台要转到 stderr**。`kputs`（`pd/pd_cons.c:4`）往编辑器那个
   控制台窗口写，我们的窗口是空壳 —— 于是着色器编译错误、脚本编译错误、
   `compile frag#0` 这种进度**一个字都看不见**，查问题等于闭着眼睛。
   —— `polydraw.a64.stitch.c` 把它改名成 `kputs_win32`，真名归
   `port/a64/pd_cons_a64.c`（印到 stderr）。开了之后第一次看到
   `GL_VERSION: 2.1 Metal - 91.7` / `GLSL_VERSION: 1.20` / `compile vert#0` ——
   一切正常，于是排掉了"着色器没过"这条。

9. **FBO 只建一次、存图取 viewport 那一块**。先前按 viewport 重建 FBO，
   而 polydraw 是**先画、我们后才知道 viewport 多大** —— 重建把第 0 帧的画面
   丢掉了。现在一次建 2048x1536，存图只读当前 viewport 的子矩形。
   另外 `glBindFramebufferEXT(…,0)` 翻成"绑我们那张"（离屏没有 0 号那张）——
   不过量下来 polydraw 这一趟压根没调它（那三个 FBO 入口有一个是空的）。

## 又四个洞（14~17）：全是 32 位 x86 的口径撞上 LP64

这四格合起来把语料里最后 **6 份崩**（4 段错误 + 2 abort）全清掉了。共同点是
**原文自己写着口径**（注释里就有"4 byte"、"(~parnum)*4"），只是那口径等于
`sizeof(long)==4`。

14. **`gnumarg` 不是"参数个数"**（已补）。`newvar[]` 前头先摆全局 `STATIC` 声明
    （`kasm_main.c:222` 那一趟 `parse_static`，家族在 `:277` 被改成 KGLB），
    参数接在**后面**；`gnumarg` 是"参数列表解析完时的 `newvarnum`"。
    我们的参数区宽度重映射按"前 `gnumarg` 格都是参数"排新基址 —— 真参数被全局
    挤到后面去了。量到的（`ken/curvybuild.pss`，函数 `(N,A,B)`）：`gnumarg=13`，
    `newvar[0..9]` 是 WALL/SECT/NUMSECTS/… 全是 `e…`（KGLB），`newvar[10..12]`
    才是 N/A/B（`a…` = KPTR）；A 拿到基址 88，`*(double **)(parmdat+88)` 读到栈上
    垃圾 `0x3`，MOV 上段错误。
    —— `pd_a64_parm.c` 的 `pd_a64_parms()`：**按家族筛**（KESP/KPTR 才是参数），
    widen 与 `kasm87c`/`kasm87cp` 填参都走这一串。
    好了四份：`curvybuild` / `heightmap` / `drawcone2` / `drawcone2_asm`。

15. **`texttrans` 位图按"long 是 4 字节"算大小**（已补）。`kasm_cpu.c:73` 是
    `long *texttrans`，写的一边 `texttrans[i>>5] |= (1<<i)`（一格 long 管 32 个
    字符），可 `kasm_main.c:29` 算的是 `((len+31)>>5)<<2` —— 每 32 个字符 4 字节。
    量到的：`tigrou/balls2k.pss` 的 eval 段（bakz 约 5.5KB）要 1384 字节、
    只 malloc 了 `max(692,1024)=1024`，越界 360 字节正好压在紧接着 malloc 的
    `tbufmal` 上：tbuf 前 25 个字节变成位图垃圾，括号扫描报 `ERROR: too many }`。
    —— 已有的 `kasm_main_a64.c` 替身里改成按 `sizeof(long)` 算（`tools/mkmain.mjs`）。

16. **入口选 `kasm87c` 还是 `kasm87cp` 看的是 `newvar[0]`**（已补）。
    `kasm_interp.c:333` 那一句判的是第一个 newvar —— 与第 14 个洞同一个根：
    带全局时那是个全局。于是 `drawsph(cx,cy,cz,cr)`（第一个参数是 double）被选成
    `kasm87cp`：arm64 上 double 走 d0、指针走 x0，`parmdat[0]` 拿到的是垃圾。
    —— `pd_a64_copyglob2struct` 自己按参数表判，不看原文选的那一档。

17. **多维数组的维度表一格是 4 字节，两头都写成 `long *`**（已补）。
    `kasm_state.c:99` 的注就写着 `ArrayDims:{(~parnum)*4}`；可
    `kasm_parse.c:144` 写（`*(long *)&newvarnam[newvarplc] = i; newvarplc += 4;`，
    只保了 4 字节却写 8 个）、`:1086` 读（`((long *)…)[l]`，按 8 字节一格）。
    `static planes[6][4]` 于是读出 `0x0000000400000006`，"维度合并乘数"是 2.75e19，
    进常量表后折出来的下标是 `0x7fffffffffffffff` —— 被 `kasm_opt.c:339` 的越界闸
    拦下 -> `kasmoptimizations()` 回 -1 -> `kasm87comp` 回 0（**这条路不设
    `kasm87err`**，所以外头只看到"编译失败"）-> `kasm87` 走清理，
    `free(gevalext[j].ptr-FUNCBYTEOFFS)` 去 free 我们 mmap 的 thunk 槽 -> **rc=134**。
    —— `tools/mkparse.mjs` 生成 `port/a64/kasm_parse_a64.c`（只差那两处强转，
    `diff -u polydraw_src/eval/kasm_parse.c` 看得见），缝合文件改包它。
    好了两份：`balls2k` / `metaballs_cube`。

查这四格的路子记一笔：`PD_RUNDBG=1` 那把诊断加了第三格 —— **执行前**查
`p[0..2]` 有没有落在头一页（基址 0 + 小偏移），撞上就把指令号、opcode、三个操作数的
`r/q` 和**整张参数表**印出来。第 14 个洞就是那张表一眼看出来的（前十格全是 `e…`）。
abort 那一族则要另一条路：`lldb -o "b malloc_error_break"` 看是谁在 free ——
它把"堆被写坏"与"free 了不是 malloc 来的东西"分开了，后者直接指到清理路径。

## 第 18 个洞：带字符串的宿主函数**一个都没被调**（`glsettex` / `glsetshader`）

这一格把"空画面"从 7 份降到 1 份（ok 42 -> **48**）。

原文那个 USERFUNC 的 switch（`kasm_interp.c:242` 起）只认原型里的 `d`（double）与
`D`（double *），**没有 `C`（char *）那一档** —— 一路 `strncmp` 全不中，
然后 `break`：函数**不调、不报错、返回值也不写**。于是：

* `glsettex(0,"earth.jpg")` 从来没执行 -> `tex[0]` 没建 -> `tex[0].tar` 还是 0；
* 接着脚本 `glbindtexture(0)` 拿 `tar=0` 去调 GL，`GL_INVALID_ENUM`；
* 采样器落在**默认贴图**上 —— Apple 对"装不进去的贴图"回的是常量白。
  所以 `texture2D()` 一律白，整幅图看着就是空的。

量到的（新加的 `PD_TEXDBG=1`，见 `port/a64/pd_gl_texdbg.c`）：

```
[tex] bind tar=0 name=0 err=INVALID_ENUM      <- glsettex 没跑，tar 还是 0
```

补完之后同一句变成建贴图 + 上传，`512x256 err=-`：

```
[tex] bind tar=de1 name=0 err=-
[tex] image2D tar=de1 lev=0 ifmt=4 512x256 fmt=80e1 typ=1401 px=0x0 err=-
```

带字符串的原型全语料只有五种（`pd_script.c` 那张 `myext[]` 里数过）：
`C`（mountzip / glgetuniformloc / glgetattribloc）、`dC`（glsettex）、
`dCd`（glsettex 三参）、`CC`（glsetshader 两参）、`CCC`（glsetshader 三参）。
五种全补在 `tools/mkrun.mjs` 里（字符串操作数本身就是串的地址 —— KSTR 在
`kasm_comp.c:313` 被改成 `KEDX+gccnt*8`，指进 globval，强转 `char *` 即可）。

**一个坑**：原型串**不是 NUL 结尾**的，后面紧跟着函数名（量到 `|dCGLSETTEX|`）——
所以只能像原文那样按长度 `strncmp`。先头用 `strcmp` 写，五条全不中，
现象与没补一模一样（"改了没效果"先怀疑这个）。

**第二个坑（同一个根）**：找 `C` 也不能用 `strchr` —— 它会一路扫进后面的函数名，
于是 `gltexcoord`（`|ddGLTEXCOORD|`）、`glcolor`、`glprogramlocalparam` 这些
**压根没有字符串参**的调用也全进这一档。行为上无害（下面五条全不中就落回原路），
但 `ken/curvybuild.pss` 跑两帧就白进 34 万次。现在按 `memchr(cptr,'C',cn)` 收在
前 `n` 个字符里（`n` = `kcd->gasm[i].n`，原型串长度正好等于它）。

`printf` 那一族仍然不接：`myprintf` 自己是真变参，按定参强转不对。

### `curvybuild` 那 20% 的 `mysleep` 是脚本自己要的，不是错

采样（`/usr/bin/sample`）显示 `ken/curvybuild.pss` 的主线程里 **552/2773 帧样本
（20%）在 `mysleep` → `usleep`**，调用点是 `kasm87c_run+3988`（USERFUNC 那一档）。
一开始怀疑是刚补的 `C` 原型派发调错了函数（`myprintf` 0x1000009e0 与
`mysleep` 0x100004fd0 只差一格）。**结论是：没有错。** `lldb` 在 `mysleep`
上断一次，栈就是 `kasm87c_run <- kasm87cp <- WinMain`，而脚本第 173 行写着
`Sleep(15);` —— 它自己限帧。全语料 53 份里**只有这一份**调 `Sleep`，
所以另外两份超时仍然是真在算（JIT 的活），与限帧无关。

教训与"归因前先探针"那条一样：**符号地址相邻不构成证据，断一次栈只要几秒。**

### 两把新诊断

* `PD_TEXDBG=1` —— 贴图那一路的 `glBindTexture`/`glTexImage2D`/`glTexSubImage2D`/
  `glTexParameteri` 逐个记参数 + 紧跟着的 `glGetError()`（`port/a64/pd_gl_texdbg.c`，
  缝合文件用 `#define` 把调用点换过去，定义那边仍是真名）；
* `PD_RUNDBG=1` 多了一行"带字符串的原型没接：|…|" —— 以后再冒出别的原型直接点名。

## GUI 那条腿（`--gui`：真窗口 + 实时循环）

`polydraw_a64 ken/ceilflor2.pss --gui --size 640x480` —— 开窗口、实时跑、
**关窗就退**。判据 `bench/gui-a64.sh` **3/3**（ceilflor2 83 fps、texture 120、
clock 120，都是 1280x960 的 Retina 帧缓冲）。

形状上**一个渲染路径都没改**（离屏那条腿原样绿着），只补了四件事：

* `port/a64/pd_gui_glfw.c` —— GLFW 开窗口。**上下文归窗口**（`wglCreateContext` /
  `wglMakeCurrent` 在 GUI 档下转过去），而且 GUI 档**不建 FBO**：直接画进窗口那张
  默认帧缓冲。离屏那条路非得有 FBO 是因为**离屏根本没有 0 号那张**；有窗口就不用了，
  于是"绑 0"那个包装（`pd_bindfb_wrap`）在 GUI 下正好是原意。
  为什么用 GLFW 而不是 Cocoa：要的是等价实现，而 GLFW 给的正是 **legacy 2.1 上下文**
  （polydraw 的 `glBegin/glEnd` 要固定管线，core profile 没有）；
* `port/a64/pd_gui_bridge.c` —— 往 polydraw 里喂输入的**唯一通道**。
  `dkeystatus[256]` / `dbstatus` / `popts` 都是 `pd_head.h` 里的 **static**，
  只有 polydraw 那个翻译单元看得见 —— 所以这份桥必须**包在缝合文件末尾**；
* 键盘按 **DOS/DirectInput 扫描码**映射（脚本读的是 `keystatus[0xcd]` 这种），
  表在 pd_gui_glfw.c 里，列了方向键/WASD/空格/Shift/Ctrl/Esc/回车/数字/字母；
* 退出：`PeekMessage` 在窗口该关的时候投一条 **WM_QUIT** —— 原文
  `pd_win.c:703` 见到它就 `goto quitit`，于是原文那个帧循环一个字不用改。

两格量出来才知道的事：

* **渲染窗格要铺满窗口，得按原文自己的开关 `popts.fullscreen`**
  （`pd_win.c:238`：`oglxres = xres; oglyres = yres;`），在 `CreateWindow` 壳子里按下
  —— 它正好在 `resetwindows()` 算布局之前。**试过并退掉**的一刀：包 `glViewport`
  一律撑满窗口。不行 —— `glcapture()` 那一族自己会设小视口做渲染到纹理，
  一律覆盖就把那条路整条打断（`tigrou/clock.pss` 整张帧缓冲非黑像素 **0**）；
* **`/WxH` 要给帧缓冲的尺寸，不是窗口尺寸**。Retina 上帧缓冲是窗口的两倍，
  按窗口给的话画面只铺满四分之一（量到：非黑 307200 / 1228800）。
  鼠标也跟着按帧缓冲坐标报，两头才一致。

判据那两条是**看不见窗口也能量的**：整张帧缓冲里非黑像素的个数（读在 swap 之前）、
以及标题栏那一行 fps（原文每秒往标题写一次，我们把 `SetWindowText` 转给
`glfwSetWindowTitle`）。抽样点靠不住 —— `clock.pss` 是黑底细线，3x3 九个点一个都碰不上。

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

**现状**：53 份语料里 **48 份真出图**（判据 `bench/render-a64.sh` **4/4**），
而且是**每一帧都在画**（默认存最后一帧）。

最后那一格（已补）：**看门狗**。`pd_script.c:554-568` 里那条线程只负责"脚本超时
就报 stuck" —— 脚本本来就是主线程自己调的（`:564` 的 `safeevalfunc()`）。
可是我们把 `_beginthreadex` 回 0、`WaitForSingleObject` 回 `WAIT_TIMEOUT`，
于是第一帧之后 `gshaderstuck = 1`，脚本再也不跑了 —— "画面全黑"查到最后
就是这一格。改成回非 0 句柄 + `WAIT_OBJECT_0` 即可（代价：没有看门狗了，
脚本死循环会挂住进程；出图这条路可以接受）。

## 性能：`/bench:N` 那把插桩在 arm64 上也通了

`polydraw_bench.txt`（`/bench:N` 自己写的，两列是 fps 与 ms/帧，
口径是 30 帧暖机 + N 帧计时）：

```
ken/ceilflor2.pss     1779 fps   0.562 ms/帧
tigrou/clock.pss      1322 fps   0.756 ms/帧
```

注意口径：这是**离屏 320x240 + 纯 C 解释器**，与 Windows 上那把
（x87 JIT + 真窗口 + vsync 关掉）不是同一件事，**不能直接对比**。
它现在能当的是"arm64 这条腿自己的前后对比"。

## 真的 arm64 JIT（`port/a64/pd_a64_jitc.c`）

先前这条腿上**只有解释器** —— `kasm87`（`COMPILE==1` 那份 x87 JIT）在 arm64 上等于
没有。现在有一份把**同一串 `gasm[]` 三地址指令**吐成 A64 机器码的 JIT：一格 `kcd`
编一次，之后每帧直接跳进去。挂点只有 `pd_a64_fill` 里那一句（编不出来就退回解释器）。

量出来的账（`PD_JIT=0` 对 `PD_JIT=1`，同一台机器同一趟）：

```
(x){s=0;for(i=0;i<1000;i++)s=s+sqrt(i*x);s}   92.98 us -> 8.87 us   （10.5x，值逐位相同）
ken/ceilflor2.pss  --gui                        79.0 fps -> 103.3 fps
ken/texture.pss    --gui                        60.8 fps -> 119.9 fps
tigrou/clock.pss   --gui                        59.8 fps -> 59.8 fps （已经顶在 vsync 上）
```

**判据是差分**：`PD_JIT=2` 每次调用两条路都跑一趟、位级对不上就印。
`bench/test-a64.sh` 在差分档下一条"不一致"都没有（那一档里 `x*=x` 与 `static s++`
两份会 FAIL —— 差分把副作用做了两遍，不是 JIT 错）。

接了哪些指令、还欠什么，看那份文件的头注。现在还欠的最大一格是**脚本自己那些函数**
（`pd_a64_owns` 那一支）：它们的入口是真变参（`kasm87c(double first, ...)`），
Apple 的 arm64 变参实参全走栈，得单独铺一趟 —— 碰上就整份退回解释器。

## 整份语料的账（`bench/scan-a64.sh`，53 份）

**ok 48 / 空画面 1 / 着色器错 2 / 崩 0 / 超时 2**（三轮前是 ok 34 / 崩 10，
上一轮 ok 42 / 空画面 7）。表落 `bench/out/scan.tsv`，每份的 polydraw 诊断落
`bench/out/scanlog/`。

* **崩 0** —— 第 14~17 个洞清完，整份语料**再没有崩的**；
* **空画面 1**：只剩 `geo_duptris`（几何着色器那一族）。已经量清的三件事：
  1. 参考（c_impl 的 `geo_duptris.pss_f30.png`）**只有 214 个非黑像素**（细白线），
     所以这一格的收益很小；
  2. **不是"macOS 没有几何着色器"** —— 探针（`/tmp/glext.c` 那十行）查过
     legacy 2.1 的扩展串：`GL_EXT_geometry_shader4` **有**，`framebuffer_blit` 也有；
     polydraw 也确实印了 `compile geom g`（着色器编译过了）；
  3. 线头是 `PD_GLDBG=1` 下第 0 帧的 **`err=0500`（GL_INVALID_ENUM）**。查下去最像的
     一条已经很具体了：脚本发的是 **`glBegin(GL_QUADS)`**（第 9 行），而它的几何着色器
     声明的入口类型是 **`GL_TRIANGLES`**（`@g,GL_TRIANGLES,GL_TRIANGLE_STRIP,12:g`）。
     `EXT_geometry_shader4` 里这两样必须配 —— NVIDIA 在 Windows 上会先把 QUADS 拆成
     三角再进几何着色器，Apple 这条驱动不拆，于是那一趟画不出东西。
     **这不是移植的洞，是驱动语义的差**：要它得在宿主那一层把 QUADS 拆成三角，
     那就改了原版的行为（而且只为这一份、214 个像素）。先记着，别顺手做。
* **着色器错 2**：`gspiral`（`&` 用在 int 上）、`mipmap`（`texture2DLod` 没声明）
  —— 都是 GLSL 1.20 的上限（macOS legacy profile），不是移植的洞；
* **超时 2**：`balls`（16384 个球）、`particules sparks`。**不是卡死，是真的在算** ——
  跑着的时候 attach 上去，栈是 `kasm87c_run <- kasm87cp <- WinMain`（深度 1，
  就是脚本主函数那一圈），两份都 > 3 s/帧。这两格属于**性能**，
  补法是 arm64 的真 JIT（ADR-0045），不是移植的洞。
  顺带记一笔：`/bench:N` 的口径是"30 帧暖机 + N 帧计时"，所以慢脚本连一个数都拿不到
  （31 帧 x 3s 就超 90s）；要量它们用 **GUI 档的标题 fps**，那个没有暖机门槛。

ms/帧（离屏 320x240 + 纯 C 解释器，`/bench:30` 的口径 = 30 帧暖机 + 30 帧计时）：
最快一档 `multiarb_asm` 0.379、`cubetex` 0.437、`ceilflor2` 0.448；
最慢一档 `curvybuild` 38.8、`tree` 24.7、`gpgpu` 14.7、`disco_ball` 10.3、
`balls2k` 8.1。

> 语料目录（`~/Documents/polydraw/{ken,tigrou}/`）里每份 `.pss` 旁边都有一张
> **`*.pss_f30.png`** —— 那是 **c_impl** 出的（`c_impl/src/render_main.c:214`
> 的 `"%s_f%d.png"`），不在 git 里。它只有约 80% 正确，但拿来判"这份到底该不该
> 画出东西"很好使：`geo_duptris` 那 2 色就是这么看出来的。

> ms/帧这一栏先前是**错的**：`polydraw_bench.txt` 每行是 `\tfps\tms/帧`，开头那个
> tab 让 `$1` 是空串，而 scan 取的是 `$2` —— 于是印的其实是 **fps**
> （"tree 39 ms/帧"实为 39 fps）。现在取 `$3`。量毫秒级的东西，列没对上就整栏失真。

### 查"解释器里的野指针"用 `PD_RUNDBG=1`

那份 fork 里有三格可选诊断：

1. 先把 `plst[]` 十六格填成毒值，再逐个操作数查"这一族有没有人填过"（每族只印一次）
   —— 第 12 个洞就是这么逼出来的（它先排掉了 fam=0（KEAX，NUL 占位）与 fam=8
   （KEIP，跳转标签，本来就不该解引用），才把注意力留给 KGLB）；
2. 算完的 `p[j]` **落在哪块地盘**（kcd / 值栈 / parmdat / gstatmem）；
3. **执行前**查 `p[0..2]` 有没有落在头一页（基址 0 + 小偏移），撞上就把指令号、
   opcode、三个操作数的 `r/q` 和**整张参数表**印出来 —— 第 14 个洞是那张表
   一眼看出来的（前十格全是 `e…`）。

abort（rc=134）那一族别用这把：走 `lldb -o "b malloc_error_break"`。
它把"堆被写坏"与"free 了不是 malloc 来的东西"分开，后者直接指到清理路径
（第 17 个洞就是这么定位的）。

## 出图判据现在的账（`bench/render-a64.sh` **4 过 / 0 红 / 1 不计**）

* `ken/ceilflor2.pss` 1082 色、`ken/texture.pss` 99 色、`tigrou/clock.pss` 161 色、
  `ken/orthoglobe.pss` 53 色 —— 全过；
* `ken/gspiral.pss` —— **不计**。片元着色器用了 `&` / `>>`（整数位运算），
  那是 GLSL 1.30 起才有的；macOS 的 legacy profile 最高 GL 2.1 / GLSL 1.20，
  编译期就报 `'&' does not operate on 'int' and 'int'`。要它得换 core profile（3.2+），
  可是 core 里没有固定管线，而 polydraw 的 `glBegin/glEnd` 一族要固定管线 ——
  那是另一条路，不在这一轴里。

### `orthoglobe` 那一红的教训：四步探针全指错了方向

它红了好几轮，根因其实是**第 18 个洞**（`glsettex` 压根没被调用）。先前那四步探针
（把采样式换掉、把 texcoord 当颜色输出、查 NaN、查 wrap 模式）得出的全部结论都是
**在默认贴图上量的**，所以"纹理是上去了、采得到东西"那一条是假的 —— 采到的白正是
Apple 对"装不进去的贴图"的回答。教训：**先确认那个宿主调用真的发生了**
（一行 `PD_TEXDBG=1` 就够），再去推着色器与采样参数。猜过并排掉的三条
（BGRA 上传组合、NaN、CLAMP_TO_EDGE）现在看全是白费。

试过并**退掉**的一刀（记在这儿免得再试一遍）：把 `glTexImage2D`/`glTexSubImage2D`/
`gluBuild2DMipmaps` 也按那 43 个名字的办法改名，包一层把
`GL_BGRA + GL_UNSIGNED_BYTE` 换成 `GL_UNSIGNED_INT_8_8_8_8_REV`
（以为 Apple 的驱动偏爱后者）。结果**四个例子一起挂**，而且只在 stdout 不是终端时挂
（tty 下与 lldb 下都正常）—— 那种"看起来与 I/O 有关"的崩通常是别处的内存问题被
时序放大了。已经 `git checkout` 退回去。现在知道 BGRA 那个组合本来就没问题
（`PD_TEXDBG` 下 `err=-`），这一刀不必再做。

这一轮补的两格（都不是脚本的欠账，是移植的洞）：

* **数据文件要按 cwd 找**。`GetModuleFileName` 原先报可执行文件的真路径，
  polydraw 拿它切出 `gexedironly` 再 `kzaddstack(gexedironly)`（`pd_win.c:528`）——
  二进制在 `bench/out/` 下，而 `earth.jpg` / `ken/` / `tigrou/` 在仓库根，图一张都找不到。
  改成报 **cwd + 可执行文件名**（Unix 上"在哪儿跑就从哪儿找数据"的常规）；
* **鼠标默认报渲染窗格的正中**，不是 (0,0)。不少脚本拿 `mousx/mousy` 定位几何 ——
  `orthoglobe` 是 `z = mousy/yres*4`，报 0 的话 z=0，而 `gluPerspective` 的近平面是 0.1，
  整个扇面被近平面裁掉。`PD_MOUSE=x,y` 可以改（要复现某一帧时用）。
  就是这一格让 `ken/texture.pss` 从空画面变成 98 色。
