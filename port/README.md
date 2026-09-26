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

## 第 21 个洞：**`rnd` 从一开始就是错的、`nrnd` 几乎死循环**（`krand` 的 32 位回绕）

`eval/kasm_math.c:4`：

```c
static long krand () { kholdrand = (unsigned long)((kholdrand*(214013*2)+2531011*2)>>1); return(kholdrand); }
```

那句 `(unsigned long)` 在 32 位 x86 上是**32 位**：乘加自然回绕、`>>1` 之后落在
`[0, 2^31)`，正好配 `oneover2_31`（`RND` 就是 `krand()*oneover2_31`，要的是 `[0,1)`）。
LP64 上 `unsigned long` 是 64 位，**不回绕**：

* 量到 `(){rnd}` = **-1043597862.5**（不是 `[0,1)`）—— 所有用 `rnd` 的脚本
  画出来的东西都不对，只是"画出来了"所以一直没人看；
* `NRND` 那个 Box-Muller 的拒绝采样 `do{…}while(r>=1)` 几乎**永不接受**
  （x、y 都是天文数字），每次接受的概率约 2^-33 —— 这就是**两份超时的真正原因**
  （`ken/balls.pss` / `particules_sparks`；采样 4004/4004 个样本全在 `nrnd` 里）。

改法（`tools/mkmath.mjs` 生成 `port/a64/kasm_math_a64.c`，原文零改动）：
把截断挪到**移位之前** —— `((unsigned int)(和) >> 1)`。
**只把 `unsigned long` 换成 `unsigned int` 是不够的**：那样 `>>1` 还在 64 位里做，
"先移位再截断"与"先回绕再移位"不是一回事（量到的还是 8515129.92）。

效果：`(){rnd}` = 0.92、一千个样本均值 0.5038、`nrnd` = 0.594；
**`ken/balls.pss` 从"超时（>3s/帧）"变成 25.7ms/帧（38.9 fps）**。

**这一格的教训**：`bench/scan-a64.sh` 里"超时"那一类不要当成"算得久"就放过 ——
先拿 `/usr/bin/sample` 打一发。4004/4004 个样本全落在一个函数里的形状，
不是"慢"，是"死循环"。

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
fib(20)（脚本函数递归）                        1743.7 us -> 273.2 us （6.4x，值相同）
ken/ceilflor2.pss  --gui                        79.0 fps -> 103.3 fps
ken/texture.pss    --gui                        60.8 fps -> 120.5 fps
tigrou/clock.pss   --gui                        59.8 fps -> 120.0 fps
```

**脚本自己那些函数**也接上了：`USERFUNC` 撞上 `pd_a64_owns` 的入口时，JIT 把操作数
地址摆成 `double *p[17]` 那张表（栈上 `PD_POFF` 那一段），原样递给现成的
`pd_a64_call_script` —— 那一份本来就是为"变参入口"写的。**还有一处非补不可**：
`pd_a64_call_script` 自己也要问一句 JIT（它是递归那条路的唯一入口），
不问的话被调那一份永远在解释器上跑 —— `fib(20)` 先前只快 1.18 倍就是这个洞。

**判据是差分**：`PD_JIT=2` 每次调用两条路都跑一趟、位级对不上就印。
`bench/test-a64.sh` 在差分档下一条"不一致"都没有（那一档里 `x*=x` 与 `static s++`
两份会 FAIL —— 差分把副作用做了两遍，不是 JIT 错）。

接了哪些指令、还欠什么，看那份文件的头注。现在还欠的是：原型里带字符串的那一族
（`C`，`glsettex("x.jpg")` 那种 —— 解释器那侧第 18 个洞补的正是它）、
`n > 8` 的宿主调用、以及**寄存器分配**（现在每条指令都老老实实"读内存-算-写内存"，
省下来的是"算三个地址 + 一趟 switch + 一层间接"，不是寄存器）。

### 现在**默认是开的**。A/B 的账（同一个二进制，各跑一趟 53 份）

```
PD_JIT=0   ok 47 / 空画面 2 / 着色器错 2 / 崩 0 / 超时 2
PD_JIT=1   ok 48 / 空画面 1 / 着色器错 2 / 崩 0 / 超时 2
逐份判定**一处真差都没有**（唯一那一处是 ballsk —— 时间相关脚本的抖动，见下一节）
每帧时间（ok 那批里 >3ms 的）：ribbons_invasion 3.53x、snake_tube 2.60x、
  sphere 1.59x、snake_stars 1.51x、town_textured 1.09x；tree 0.82x（唯一变慢的，未查）
```

### 那两份退步是怎么查出来的（连着定出第 19、20 两个洞）

先前 A/B 出来的差是两份：`heightmap` 与 `texture3d` 各从 `ok(2 色)` 退成 `空画面(1 色)`。
查法与结论（顺序就是下次该照着走的顺序）：

1. **同一个二进制 A/B**（`PD_JIT=0` 对 `PD_JIT=1`）先把"真退步"与"判据自己不稳"分开
   —— 先前以为坏了四份，其中两份（`ballsk`/`curvybuild`）是 `klock()` 那类时间相关脚本
   的假红（见上一节）。
2. **补一格"单条指令走解释器"的退路**（`pd_a64_jit_one`）。有了它才能**按指令二分**
   （`PD_JITFB=a,b[,gecnt]`）；只按"哪一族指令"猜是没信息的 —— 一族一关就等于整份退回。
3. 二分路上先撞出**第 19 个洞**（`USERFUNC` 里"函数指针形参"那一支读**全局** `gasm[i].g`
   —— 与第 14 个洞同一个毛病，`tools/mkrun.mjs` 的 `fixGlobGasm` 补的）。
4. 再二分到 `ken/heightmap.pss` 主函数**第 117 条**：`GLSETTEX`、`n=5`、原型 **`dDddd`**。
   这一格就是**第 20 个洞**：原文那张 switch 按 `n` 只枚举 `d…dD…D`（**指针必须在末尾**），
   `dDddd` 五条 `strncmp` 一条都不中 ⇒ **`glsettex(0,buf,w,h,colmode)` 在 COMPILE==0 上
   压根不会被调，一声不响地跳过**。而 JIT 按 AAPCS64 摆一摆就真调了，于是两条路画出来
   的东西不一样。
5. 处理口径：**JIT 只发解释器也会发的那些形状**（指针必须是后缀，带 `C` 的只认第 18 个洞
   那五种），形状不对就走退路。理由是"两条路答案相同"比"多调一个函数"重要 ——
   真要治的是解释器那张表（补 `dDddd` 那一族），那是另一刀、另一份判据，
   而且那一刀会**改变出图**（heightmap 那张图会真贴上纹理），得单独对照。
6. 还有一条顺手排掉的：脚本自己那些函数（`pd_a64_owns`）先前编成"摆 `p[17]` 表 +
   调 `pd_a64_call_script`"会崩（`DRAWTOPO`、原型 `dddDDDd`），现在一律走退路 ——
   递归那一半的收益本来也不在这儿，在 `pd_a64_call_script` 自己会问一句 JIT。

### 三把查错用的开关（这一族的判据）

* `PD_JIT=2` —— **差分**：每次调用两条路都跑、位级对不上就印。查"算错了"最快的一格；
* `PD_JITMAX=n` —— 只编指令数 ≤ n 的那些 kcd。一份脚本里每个函数是一格 kcd，
  所以拿它二分"是哪一份函数的锅"（`tigrou/ballsk.pss` 那次就是这么定到 `drawsph` 的）；
* `PD_JITNO=f1,f2,…` —— 拒编含这几号指令的整份（编号是 `eval.c:251` 那张 enum）。
  二分"是哪一族指令的锅"。

### 一个**不能用来判 JIT 对错**的信号：`bench/scan-a64.sh` 的颜色数

`tigrou/ballsk.pss` 这类脚本的 `dt` 是从 `klock()` 来的（真实时间），球带重力往下掉 ——
**跑得越久掉得越低**，够久就掉出视野、整张图变成一色。于是同一个二进制、同一条命令
连跑三趟，颜色数可以是 43 也可以是 1。所以"空画面从 1 涨到 5"这种账
**必须拿同一个二进制 `PD_JIT=0` 与 `PD_JIT=1` 各跑一趟**对着看，
不能拿今天的扫描去比前天的记录（我先前就这么误判过一次，以为 JIT 弄坏了四份）。

## 整份语料的账（`bench/scan-a64.sh`，53 份）

**ok 50 / 空画面 1 / 着色器错 2 / 崩 0 / 超时 0**
（一路是 ok 34 崩 10 -> ok 42 空 7 -> ok 48 超时 2 -> **现在**）。
**剩下的三份全在我们之外**：1 份是驱动语义（`geo_duptris`，见下）、
2 份是 macOS legacy GL 只到 GLSL 1.20。换句话说**这台机器上画得出来的都画出来了**。
表落 `bench/out/scan.tsv`，每份的 polydraw 诊断落 `bench/out/scanlog/`。

* **超时 0** —— 第 21 个洞（`krand` 不回绕、`nrnd` 几乎死循环）清完，
  先前那两份"超时"（balls / particules_sparks）**不是算得久，是死循环**；
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

## 第 22~24 个洞：**`glsettex(号,数组,…)` 这一族从来没有真的上传过**

用户报的两件事（`ken/texture3d.pss` 驱动喊 "GLD_TEXTURE_INDEX_3D is unloadable …
using zero texture"、`curvybuild`/`heightmap` 画面不对）是**同一条链**上的三个洞。

### 22：指针不在末尾的原型，一个都没被调

`glsettex(0,buf,w,h,颜色档)` 的原型是 `dDddd`、三维那档 `dDdddd`、两参那档 `dDdd`。
原文那张解释器 switch（`kasm_interp.c`）**只枚举了 `d…dD…D`（指针必须是后缀）**，
五条 `strncmp` 一条都不中 ⇒ 一声不响地跳过。先前 JIT 那侧为了"与解释器一致"也把
这些形状退回去（那是第 20 个洞的结论），于是两条路都不调。

正本是原版那台 x87 JIT（`COMPILE==1`），**它是调的**。所以两头都补：
`tools/mkrun.mjs` 给解释器加了那三格（`dDdd`/`dDddd`/`dDdddd`），
`pd_a64_jitc.c` 把形状那道门**整个去掉** —— JIT 摆实参本来就是按 AAPCS64
（`d` 走 d0..d7、`D`/`C` 走 x0..x7，两条独立序列），任意次序都对。
**真变参那一族自动排除**：`myprintf`/`myprintg` 的原型里有 `e`
（`kasm_comp.c:244` 把 `.` 记成 `e`），"只认 d/D/C"那个循环挡住了。

### 23：名字 0 那格贴图在 Apple 上装不进去

polydraw 不走 `glGenTextures`，拿"第几格贴图"当 GL 名字用（14 处调用点全是
`glBindTexture(tex[itex].tar,itex)` 或 `fontid`，**没有一处是"绑 0 去解绑"**）。
于是第 0 格绑的是**默认贴图对象**，Windows 那些驱动许、Apple 的 Metal 后端不许。
`port/a64/pd_gl_texdbg.c` 那一层把**名字 0 换成一格真的 `glGenTextures` 名字**
（按 target 各一格 —— 同一个名字不许换 target）。

### 24：那句范围检查在这条路上永远是假

`kglsettexarray3`（`pd_host_gl.c`）拷数据之前要确认"数组在脚本自己那块内存里"：

```c
if ((((int)p) < ((int)gevalfunc)) || (((int)p)+((xs*ys*zs*evalvalperpix)<<3) >
     ((int)gevalfunc)+gevalfuncleng)) return(-1.0);
```

两层问题：三处 `(int)` 在 LP64 上把地址截成低 32 位；**就算按真指针比也过不去** ——
它比的是 `gevalfunc` 那块（`COMPILE==1` 的布局：static 数据跟代码块一起分配），
而 `COMPILE==0` 这条路上 static 在 `gstatmem`（`kasm_state.c:68`，还是 eval 那个
翻译单元里的 static，polydraw 这边看不见）。所以这一档**把它去掉**：这个指针是
我们自己的解释器/JIT 从操作数表里取的，与别的实参一样可信；尺寸那两道门还在。

分身在 `port/a64/pd_gl_settexarr_a64.c`（原文一个字节没动，缝合文件里把那三个名字
改成 `*_win32`）。**查法**：`PD_TEXDBG=1` 下只有 `CreateEmptyTexture` 那趟
`image3D`、**没有 `sub3D`` ——那就是在这一句退出去了（三维那两格的钩子在
`pd_gl_cgl.c` 的 `wglGetProcAddress` 里，它们走 `glfp[]`，`#define` 那一招钩不到）。

### 这三刀之后（`bench/scan-a64.sh`）

* **`curvybuild` 2 色 -> 19579 色**（用户报的"渲染不对"就是它）；
* **`texture3d` 2 色 -> 454 色**，驱动那句 unloadable 没了；
* `heightmap` 2 -> 1 色：它的 256×256 贴图现在真上去了（`sub2D 256x256`），
  画面仍是空的 —— 那是**另一件事**（几何/相机那一半），不是这三刀的退步；
* 分类：ok 49 / 空画面 2 / 着色器错 2 / 崩 0 / 超时 0
  （`heightmap` 从"2 色算 ok"掉进"空画面"——那个 2 色本来也是空的，
  这张表的 `ok` 判据是"颜色数 ≥ 2"，太松，见下）。

### 判据本身的一个洞（自己踩的）

攒批那一轮我拿四份探针脚本的 PNG md5 当"逐字节相同"的判据，其中两份
（`many` / `one`，没有 `@f` 段）**两档都是全黑** —— 那是"两边都不画"白拿分。
根因：`polydraw.c:3589` 是 `if (shadn[2]) Draw(…)` —— **没有片元着色器段就压根不画**
（原版就这样；语料 53 份每一份都有 `@f`，所以只有我的探针踩到了）。
带 `@f` 的那两份（`many_sh`/`one_sh`）是真画了（2 色、1324 格白），那一半的结论仍然成立。
**以后写探针必须带 `@f`，而且先确认它非黑再拿它当判据。**


## 第 25 个洞：GLSL 1.20 那道"上限"其实是我们没开扩展

先前记的是"`gspiral` 用了整数位运算、`mipmap` 用了 `texture2DLod`，macOS 的 legacy GL
最高 GLSL 1.20，要它们得换 core profile —— 另一条路"。**那个结论是错的。**
探针（`/tmp/pdprobe/glext.c`，CGL 起个 2.1 上下文问 `GL_EXTENSIONS`）量到这台机器有：

* **`GL_EXT_gpu_shader4`**（整数位运算 `& | ^ << >>`、整型的各种内建）；
* **`GL_ARB_shader_texture_lod`**（片元里的 `texture2DLod`）。

所以只要在着色器源码前面塞两行 `#extension … : enable` 就行，**脚本一个字都不用改**。
落点在 `port/a64/pd_gl_cgl.c` 的 `wglGetProcAddress`：把 `glShaderSource` 换成包了一层的
（polydraw 那一族 GL 2.0 入口全从这张表来），前缀后头补一句 `#line 1`，
不然 `glsl_geterrorlines` 报的行号整体偏。

**踩过一次：`#version` 必须是第一句。** 几何着色器那一族（`@g`）polydraw 自己会写
`#version 150` —— 一律塞最前面的话 `geo_test` / `geo_duptris` 当场报
"#version must occur before any other statement"（`ok 2777 色 -> 着色器错`）。
所以要先找 `#version`，有它就塞在那一行**后头**。

账：`mipmap` **着色器错 -> ok**；`gspiral` 那五条错剩一条 ——
`f*f*npoints*(1.0/16.0)`（`npoints` 是 `uniform int`）。**`int -> float` 的隐式提升是
GLSL 1.30 才有的**，`EXT_gpu_shader4` 不给；NVIDIA 的编译器宽松所以原版能过，
Apple 严格。那一句按 1.20 的规矩是非法的 —— 属于"参考自己错"那一类，记在这儿不动它
（要治得改脚本，而脚本是语料，不许动）。

现在的分类：**ok 50 / 空画面 2 / 着色器错 1 / 崩 0 / 超时 0**。



## 立即模式**攒批**（`port/a64/pd_gl_imm.c`）：`balls` 窗口里 30 -> 115 fps

### 为什么慢（两个探针量出来的，前一个结论是错的）

`ken/balls.pss` 一帧 16384 个球、每球一趟 `glBegin(GL_POLYGON) … glEnd()`，
我们 25~42 ms/帧，而原版（Windows 原生 GL）**2.229 ms/帧** —— 差十几倍。
`/usr/bin/sample` 说 **81% 的样本在 `qglEnd -> glEnd_Exec -> gldEndPrimitiveBuffer
-> AGX…dispatchThreads`**，看着像"每个 glEnd 一趟 Metal 派发"。

**第一个探针否掉了这个结论**：写两份最小脚本（16384 个三角形，一份分开发、一份一趟发），
**没有着色器**时 0.958 vs 1.120 ms —— 分开发反而略快，所以"每趟 glEnd 的派发"本身不贵。

**第二个探针（给两份都加上 balls 那个 `@v`/`@f`）才指对**：
**52.46 vs 1.495 ms/帧 —— 35 倍**。结论是：Apple 的 legacy GL 只有在
**绑了用户着色器**的时候才为每一趟 `glBegin…glEnd` 重新发一趟 compute 变换内核
（~3µs/趟）。语料里一半脚本带 `@v`/`@f`，所以这一格是那一半的主成本。

顺带用同一组变体把 balls 自己拆开了（都是 30 帧）：只物理不画 1.29ms、
去掉着色器段 2.53ms、原样 41.9ms —— **代价全在"图元数 × 有着色器"这一格上**。

### 怎么做的：在**宿主调用那一层**拦，polydraw 一个字节都不动

脚本碰 GL 只有一条路（`myext[]` 里的 `qgl*`），而 JIT 在**编译期**就知道被调的是哪个
函数指针（`pd_a64_jitc.c` 的 USERFUNC 那一格）。于是：

* `qglBegin` / `qglEnd` / `qglVertex` / `qglTexCoord` / `qglColor` / `qglNormal3d`
  换成 `pd_imm_*`：顶点攒进数组、`glEnd` 时三角化（fan / strip / quad 各按 GL 的次序），
  **一个 GL 都不发**；
* **别的宿主调用**在调用之前先发一格 `pd_imm_break()` —— 状态要变了，攒着的那批
  得按旧状态先画掉。解释器那条退路（`pd_a64_jit_one`）头一句也 break，所以不漏；
* 一帧收尾（`kasm87c`/`kasm87cp` 返回处）再 `pd_imm_flush()`：polydraw 自己那些 GL
  （控制台文字、贴图、清屏）都在那之后，不会插到批中间。

冲的时候就是一趟 `glDrawArrays` + 四个客户端数组（位置/色/纹理坐标/法向）。
两个边角写在源文件头注里：**半格图元被打断**要 `materialize()`（真开一趟 glBegin 补发）、
开过 `GL_COLOR_ARRAY` 之后**当前色按规范是未定义的**（每趟冲完摆回去）。

### 判据：`PD_IMM=0` 关掉，同一个二进制 A/B

四份探针脚本（确定性的，不读 `klock`）**PD_IMM=0 与 1 的 PNG md5 逐字节相同**，
而时间：`many_sh` 23.54 -> 2.03、`one_sh` 5.47 -> 2.36、`many` 2.14 -> 1.21、
`one` 1.15 -> 1.13 ms/帧。语料 A/B 见下一节。

**别拿 `balls.pss` 的 md5 判对错**：它是 `klock()` 驱动的，两趟本来就不一样
（那条规矩已经记在"颜色数不可复现"那一节里）。

### 两处**必须**跟着做的（都是先量出退步、再补的）

1. **解释器那条路也要过攒批那一层**（`tools/mkrun.mjs` 插的 `pd_imm_call`）。
   脚本自己的函数走解释器，它里头要是调了 GL，直接调真 `qglVertex` 就没有 `glBegin`
   与之配套。所以那一句在生成器里，不是手改生成出来的文件。
2. **不是所有宿主调用都该打断攒批**，只有会动 GL 状态的才该
   （`pd_imm_isgl`：名字以 `GL` 开头的那一族 + `PRINTG` + `SETFOV`）。
   先前"除了那几格一律 break"，于是 `tigrou/snake stars.pss` 那种
   **在 `glBegin…glEnd` 里每个顶点调一次脚本函数**（`align(…)`）的写法每个顶点都要
   `materialize()` 一趟 —— **2.6 -> 22.9 ms/帧，比不攒批还慢 9 倍**。
3. **图元少的帧不攒**（`PD_IMM_MINPRIM = 64`，按上一帧的图元数定这一帧攒不攒）。
   攒批的本钱是"客户端数组那一套让驱动换一种顶点布局"，图元少的时候赚不回来：
   `creepers_asm` 2.14 -> 3.37、`interference` 2.49 -> 3.20 ms/帧（min-of-3）。
   加了这道门之后那三份回到 ±2% 以内。

### 语料的账（`bench/scan-a64.sh`，同一个二进制各跑一趟）

* **分类不动**：两档都是 ok 50 / 空画面 1 / 着色器错 2 / 崩 0 / **超时 0**。
  （中间有一趟量到"`geo_duptris` 从空画面变成 ok"，**复跑之后两档都是空画面** ——
  那是颜色数不可复现那一格的老毛病，不算数。这一条留在这儿当反面例子：
  单趟扫描的颜色数不能当结论。）
* 赚得最多的（**交错跑、看分布**，不是单趟对单趟）：
  `balls` **28.8~31.5 -> 4.2~4.8**（6~7 倍）、`tree` IMM=0 的五趟是
  16.7/62.7/44.2/32.1/34.0 而 IMM=1 是 21.1/20.4/22.1/15.2/18.0
  —— **中位数 34 -> 20，而且抖动小得多**、`snake stars` 3.11 -> 2.06、
  `ballsk` 2.56 -> 2.07、`disco ball` 18.1 -> 12.5；
* 其余在噪声里。**这台机器上 `klock()` 驱动的脚本连跑两趟能差 2~4 倍**
  （几何本身随时间长），所以判退步一定要**交错多跑几趟看分布**——
  我按单趟量过一次，得出"tree 退步 2 倍"的错结论。

### 直通那一档也不许多发 GL

攒不攒都走本文件，所以"不攒"那条路必须与原文**一样多的 GL 调用**：颜色/纹理坐标/法向
在**脚本调它们的时候**就发过去（`P_raw || !F_on` 那个条件），`glVertex` 那一格只发顶点。
先前那一版在直通路上每个顶点补发三格属性 —— 白多三倍的 GL 调用。

### 下一刀（已经做了：顶点只抄一遍）

先前 `tigrou/tree.pss` 18~22ms（参考 10.255），采样里栈顶前几名**全是攒批自己**：
`bput` 226 / `addv` 189 / `pd_imm_end` 96 / `agrow` 64，而 `kasm87c_run` 只有 76 ——
CPU 花在**顶点抄两遍**（图元那份 scratch -> 攒批那份）。

改法：顶点**直接写在攒批末尾**（先不算进 `B_n`，`glEnd` 才算）。于是三角化正好是
**恒等置换**的那几族一个字节都不用抄 —— `keep_as_is()` 那张表：
`GL_POLYGON`/`GL_TRIANGLE_FAN`/`GL_TRIANGLE_STRIP` 收三个顶点（`ken/balls.pss` 就是它）、
`GL_TRIANGLES` 收 3n 个、`GL_LINES` 收 2n 个、`GL_POINTS` 任意个。
只有扇形/条带/四边形**超过一格三角形**才抄一趟到 scratch 再展开。
两件事跟着挪了位置：目标图元族（点/线/三角）要在 `glBegin` 就定（顶点开始落地之前
"换族就先冲一趟"）；`materialize()` 从攒批末尾 replay（那些顶点还没算进 `B_n`，
所以先 flush 再 replay 不会重画）。

量出来：`tree` **17.2ms**、`balls` 5.6、`snake tube` 3.8、`snake stars` 5.5；
四份探针 md5 照旧与 `PD_IMM=0` 逐字节相同，scan 的分类一格没动
（ok 50 / 空画面 1 / 着色器错 2 / 崩 0 / 超时 0）。

**GPU 那一头的两份别再当 CPU 问题查**：`metaballs cube` 32ms、`metaballs` 14ms，
采样里 CPU 基本闲着（栈顶 `__workq_kernreturn` 255 / `iokit_user_client_trap` 90，
都在等 GPU）。那两份是片元着色器里的光线步进，参考那台机器是另一颗 GPU ——
这一栏不可比，也不是我们这一侧能改的。

## x86-64 那条腿（`port/x64/` + `bench/build-x64.sh` + `bench/docker-x64.sh`）

同一份源码，第二个架构。分岔的地方**一共只有四处**，别的（LP64 那五份 fork、
解释器那 25 个洞、攒批、PNG 写出器、GUI 那一套）全是共用的：

1. **thunk**（`port/a64/pd_a64_jit.c`）：36 字节的 x86-64 版
   `movabs r10=&gkasm87cptr / movabs r11=kcd / mov [r10],r11 / movabs r10=entry / jmp r10`。
   只许用 r10/r11 —— SysV 里这两个既不传参也不是 `al`，而 `kasm87c(double,...)`
   是变参函数，callee 的 `va_start` 要读 `al`（用了几个向量寄存器）。拿 rax 当草稿纸
   就把它踩了；
2. **JIT 发射器**（`port/x64/pd_x64_jitc.c`，新）：同一串 `gasm[]` 吐成 SSE2 的码。
   公共的那几格（走解释器的退路 `pd_a64_jit_one`、`PD_JITNO`/`PD_JITFB` 那两把
   按指令二分的开关、kcd 缓存、`PD_JIT` 三档）提到了 `pd_a64_jitc.c` 前头两边共用；
3. **GL 上下文**：macOS 走 CGL（不要窗口），linux 走 **GLFW 的不可见窗口**
   （`pd_gui_open_offscreen`，与 GUI 那条腿同一份源码）。所以 linux 上连"出图"
   也要一个 `DISPLAY` —— 判据里那格显示是容器自己的 **Xvfb**，与 XQuartz 无关；
4. **GL 头与 proc 查表**：`OpenGL/gl.h` -> `GL/gl.h`（`GL_GLEXT_LEGACY` 两家认同一个
   开关，winshim 那 43 行让位一个字都不用改）；`wglGetProcAddress` 在 dlsym 之后
   再退一次 `glfwGetProcAddress`（底下是 `glXGetProcAddress`）。

### x86 特有的三个坑（都是**语义**上的，不是编码错）

* **比较的 NaN 语义与 arm64 反过来**。`ucomisd` 无序时把 ZF/PF/CF 全置 1，所以
  `a<b` **不能**写 `setb`（NaN 会给出 1）；要把两个操作数反过来比、用 `seta`
  （CF=0 且 ZF=0）。`==` 要 `sete && setnp`、`!=` 要 `setne || setp`
  （arm64 的 NE 含"无序"，正是 C 里 `NaN != x` 为真）；
* `MINSD/MAXSD` 的"无序取**第二个**操作数"正好等于 arm64 那句 `fcsel …,MI/GT`
  的语义（含 ±0 那一格），所以 MIN/MAX 一条指令就够，不用分支；
* `roundsd`（floor/ceil/trunc 与 `%`）是 **SSE4.1**。`pd_jit_build` 开头问一句
  `__builtin_cpu_supports("sse4.1")`，没有就整份不编 —— 退回解释器，答案照旧对。

### 怎么跑（两条腿，各有各的限制）

* **osx x86-64 走 Rosetta 2**（`bash bench/build-x64.sh`，产物落 `bench/out-x64-osx/`）。
  Rosetta **已经装上了**（`softwareupdate --install-rosetta --agree-to-license`）。
  这条腿上 GL 走 **CGL**（与 arm64 那份一模一样，真 GPU），所以出图判据直接可用；
  GUI 那一族**桩掉**（`port/x64/pd_gui_stub.c`）—— homebrew 那份 `libglfw.dylib`
  是 arm64 的，连不进 x86_64 的可执行文件。要 x64 的真窗口得先有一份 x86_64 的 GLFW，
  而 **arm64 原生那条腿的 GUI 是通的**，所以这一格不值得再引一套 Intel homebrew；
* **linux/amd64 走 docker**（`bash bench/docker-x64.sh build|test|ops|render|gui|vnc|x11`，
  产物落 `bench/out-x64/`）。镜像 `bench/Dockerfile.x64` = `arch_llvm` + mesa/glfw/
  X11 客户端库/Xvfb/x11vnc。

  **两条腿的产物不许混在一格**：一边是 ELF、一边是 Mach-O，而容器挂的是同一个仓库目录
  （踩过：osx 那趟把 linux 的 `.o` 覆盖掉，容器里就连不上了）。所以 `build-x64.sh`
  按 `uname -s` 分了 `out-x64` / `out-x64-osx` 两个落点。

* **速度这一栏两条腿都不算数**：linux 那档是 qemu 转译 + llvmpipe 软件光栅；
  osx 那档是 Rosetta 转译，而且**我们的 JIT 吐的 x86-64 码还要再被 Rosetta 翻一次**
  —— 同一个 eval 微基准交错跑三趟：arm64 **8.2/8.2/8.4 µs** vs Rosetta x64
  **17.0/16.8/18.1 µs**（≈2.05x）。要 x64 的真数字得找一台真 Intel 机器。

### 显示从哪儿来（三档，别混）

* **出图 / 扫描：无头**（`port/x64/pd_gl_egl.c`）。EGL 的 **surfaceless** 平台 +
  mesa 的 llvmpipe —— 没有 X、没有窗口、没有 surface，上下文直接建出来，照旧画进
  我们自己那张 FBO。判据里还故意 `unset DISPLAY`，顺带把"出图不要 X"这件事判了。
  `PD_EGL=0` 退回 GLFW 的不可见窗口（那条要 X，A/B 对照用）。
  三个坑写在那一份的头注里：`eglGetDisplay(EGL_DEFAULT_DISPLAY)` 没 DISPLAY 时会
  去试 X11 然后 `eglInitialize` 回 **0x3001**（要点明
  `EGL_PLATFORM_SURFACELESS_MESA`）；`EGL_SURFACE_TYPE` 要写 `EGL_PBUFFER_BIT`；
  扩展函数走 `eglGetProcAddress`（GLFW 那条路上压根没 init，不能问它）。
  **不传 profile 属性** = 兼容档（`4.6 (Compatibility Profile)`）—— polydraw 的
  `glBegin/glEnd` 要固定管线，与 macOS 上"不能用 core profile"是同一件事；
* **GUI 判据**：容器自己的 `Xvfb :99` —— 窗口那条腿走的是 GLFW，得有个 X 服务器；
* **XQuartz**：**用来验 GLFW 那条腿**（`bash bench/docker-x64.sh guix`）。
  量过的结论，别再猜：
  * X11 通（`xeyes` 能显示）；
  * **GLX 也通，但是"间接"的**。mesa 自己的 `drisw` 软件屏建不起来
    （`glx: failed to create drisw screen` —— `glxinfo -B` 就死在这儿，
    **先前据此写下"GLX 不通"是错的**），但它会退到 indirect GLX：`glxgears` 跑得动，
    `GL_RENDERER = Apple M1`、`GL_VERSION = 1.4 (2.1 Metal)`，真正渲染的是**宿主那颗 GPU**；
  * 代价：间接 GLX **一个扩展都不报**（`GL_EXTENSIONS` 空），FBO 与着色器那一族
    在这条路上没有 —— 用着色器的脚本会退化；
  * **回读的像素不可信**：`gui-a64.sh` 那半判据（非黑像素数）在这条路上
    `tigrou/clock.pss`（黑底细线）**每一帧都报"整屏非黑"**（76800/76800），明显是垃圾。
    所以 `guix` 那一档只判得了"窗口开出来了 + 帧在推进（19~20fps）+ 没报错"；
    画得对不对要用眼睛看，或者走 Xvfb / VNC。
  * 宿主那两项设置是**持久**的：不想留着就 `defaults delete org.xquartz.X11 nolisten_tcp`
    （以及 `enable_iglx`）、`xhost -`。
* **要用眼睛看动画**：`bash bench/docker-x64.sh vnc ken/balls.pss` —— 容器里 `x11vnc`
  把 `:99` 那张屏送出来，宿主 `open vnc://127.0.0.1:5900`（macOS 自带"屏幕共享"）。

### linux 链接时的一格：`-rdynamic` 不许省

polydraw 用 `wglGetProcAddress("wglSwapIntervalEXT")` 去问**我们自己导出的那一族
`wgl*` 垫片**，而我们那一份是 `dlsym(RTLD_DEFAULT,…)` 实现的。ELF 上可执行文件的
符号默认**不进动态符号表**，于是查不到 -> 弹 `wglSwapIntervalEXT() not supported`
然后退出（macOS 上可执行文件的符号一直是可见的，所以这一格是换平台才冒出来的）。
现象很像"EGL 没建起来"，其实 GL 版本那几行都已经印出来了 —— **看日志印到哪一行**。


### Windows 那一格

不用做：原文自带的 win32 实现（`bench/build.cmd` 那条路）本来就是这门程序的原生腿，
GLFW 那一份是**给没有 win32 的平台补的等价实现**。真要在 Windows 上换成 GLFW，
接口就是本文件里那四个分岔点（上下文 / proc 查表 / GL 头 / 可执行文件路径）。

### 判据的账

* **osx x86-64（Rosetta）**：eval `test-a64.sh` **16/16**（PD_JIT=0 与 =1）、
  出图 **4 过 0 红**（ceilflor2 1290 / texture 2297 / orthoglobe 735 / clock 223 色）、
  `test-ops.sh` 那 83 行 **与 arm64 逐行相同（83/83）** ——
  两条腿同一个 libm，所以这一档是真正的"同一门语言，两套机器码，同一个答案"；
* **linux/amd64（qemu + llvmpipe）**：eval 16/16、`test-ops.sh` jit==解释器、
  出图 4 过 0 红、GUI（`gui-a64.sh` 在 Xvfb 上）**4 过 0 红**；
  与 arm64 那份 ops 的差**只有 4 行**（`tan`/`asin`/`acos`/`atan` 最后几个 ULP ——
  glibc 与 Apple libm 的差，两条腿自己内部都一致 ⇒ 不是 JIT 的锅）；
* `bench/test-ops.sh`（**新**，83 行按指令族，含 12 行 NaN、9 行数组、越界那一夹）
  的判法值得记：**不手算期望值**，拿已经判过的 arm64 那条腿当尺子逐行 diff ——
  手算容易把"两边都错"当成对。
* **整份语料两条腿逐份同分类**（同一天各跑一趟 `bench/scan-a64.sh`，
  x64 那趟 `BIN=bench/out-x64-osx/polydraw_x64 OUT=bench/out-x64-osx`）：
  **53/53 份分类一致** —— ok 51 / 空画面 1（`geo_duptris`）/ 着色器错 1（`gspiral`）/
  崩 0 / 超时 0。
  颜色数**不要**拿来对比：`curvybuild` 这一趟 19579 vs 1551、`heightmap` 4 vs 86 ——
  那是 `klock()` 驱动那一类自带的抖动（见"扫描尺子的颜色数不可复现"）。
  同理 `heightmap` 这一趟从"空画面"变成"ok 4 色"**不算修好了**，它只是擦过了
  "≥2 种颜色"那条线。
* **linux 那条腿的语料账**（`bash bench/docker-x64.sh scan`，**无头 EGL** + llvmpipe）：
  ok 48 / 空画面 5 / 着色器错 0 / 崩 0 / 超时 0，与 arm64 **49/53 同分类**。
  换成 EGL 之后这张表**一格没动**（先前走 Xvfb+GLX 是同一个 49/53）—— 所以那 4 份
  与"上下文怎么建"无关，是 **mesa 与 Apple 那颗 GL 的差**：
  * `geo_test` —— `gl_PositionIn` undeclared。那是 `EXT_geometry_shader4`（GLSL 1.20
    那一代）的名字，**mesa 早就不给这个扩展了**（它只有 3.2 起的 core 几何着色器，
    那边叫 `gl_in[].gl_Position`）。要它得改脚本，不在这条腿的范围里；
  * `gspiral` —— arm64 上是"着色器错"，mesa 上少报了几条（GLSL 4.60 宽一些），
    但**第 114 行那个 `float * int` 还是过不去**，仍然是"参考自己依赖非标准隐式提升"；
  * `gears` / `ribbons_invasion` —— 着色器一个错都不报，画面却是空的。
    这两份还没定到根因（下一刀：拿 `PD_GLDBG=1` 看每帧中心像素与 viewport，
    再按"三档 A/B（PD_JIT=0/1、PD_IMM=0）"先把 JIT 与攒批排除掉）。
* **GUI 判据的时间预算**：`gui-a64.sh` 的 fps 那半读的是**标题栏**，而原文每秒才写一次。
  qemu + llvmpipe 上 `SECS=5` 可能一次都没写上 —— `texture` 就这么**假红过一次**
  （同一份 `SECS=12` 再跑 145.9 fps）。`docker-x64.sh gui` 现在默认 12 秒。



* 写这一份时踩的两格（都是这门语言的性质，不是 JIT）：脚本第一个字符是 `(`
  的话整句被当成**参数表**（所以比较那几行要写成 `0+(2<3)*10`）；
  `a[9]` 与 `i=9;a[i]` 都会被前端当场判成 `array index out of bounds`，
  要试运行期那一夹得**从实参递下标进来**。







