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
   （char *）—— 所以 `printf("…")` 这种带字符串的宿主函数在 COMPILE==0 那条路上
   本来就不会被调（而且 `myprintf` 自己是真变参，按定参强转也不对）。

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

**现状**：`ken/ceilflor2.pss`（1086 种颜色）与 `tigrou/clock.pss`（159 种）
**真出图了**，而且是**每一帧都在画**（默认存最后一帧）。判据
`bench/render-a64.sh` **2/5**。

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

## 整份语料的账（`bench/scan-a64.sh`，53 份）

**ok 37 / 空画面 6 / 着色器错 2 / 崩 6 / 超时 2**（上一轮是 ok 34 / 崩 10）。
表落 `bench/out/scan.tsv`，每份的 polydraw 诊断落 `bench/out/scanlog/`。

* **崩 6**：`curvybuild` / `drawcone2` / `drawcone2_asm` / `heightmap` 是段错误
  （crash 地址很小，像"基址 0 + 偏移"），`balls2k` / `metaballs_cube` 是
  **rc=134（abort）** —— 那是 malloc 发现堆被写坏，形状与段错误不同族，
  最像 PEEK/POKE 那个"quick&dirty bounds check"里 `newvar[…].maxind` 取到了
  不对的那一格（`kcd->newvar` 是抄本，`nv` 下标若过期，`k` 就是垃圾、界就没了）；
* **空画面 6**：`cubetex` / `geo_duptris` / `geo_test` / `orthoglobe` /
  `ballsk` / `metaballs`；
* **着色器错 2**：`gspiral`（`&` 用在 int 上）、`mipmap`（`texture2DLod` 没声明）
  —— 都是 GLSL 1.20 的上限（macOS legacy profile），不是移植的洞；
* **超时 2**：`balls`（16384 个球在解释器上跑）、`particules_sparks`。

一组 ms/帧（离屏 320x240 + 纯 C 解释器）：`tree` 39、`gpgpu` 96、`disco_ball` 100、
`drawsph_asm` 146、`snake_tube` 239、`menger_sponge` 370 … `interference_asm` 1310、
`sphere_ellipsis` 1526、`texture3d` 1652。

### 查"解释器里的野指针"用 `PD_RUNDBG=1`

那份 fork 里有一格可选诊断：先把 `plst[]` 十六格填成毒值，再逐个操作数查
"这一族有没有人填过"（每族只印一次）。第 12 个洞就是这么逼出来的 ——
它先排掉了 fam=0（KEAX，NUL 占位）与 fam=8（KEIP，跳转标签，本来就不该解引用），
才把注意力留给 KGLB。

## 出图判据现在的账## 出图判据现在的账（`bench/render-a64.sh` 3 过 / 1 红 / 1 不计）

* `ken/ceilflor2.pss` 1096 色、`ken/texture.pss` 98 色、`tigrou/clock.pss` 167 色 —— 过；
* `ken/orthoglobe.pss` —— **红**。已经量清的一半：把它的片元着色器换成一句
  `gl_FragColor = vec4(1,.5,0,1)`，画面就出来了（52 个抽样点是橙的）——
  所以**几何是落上去的**，问题在那句 `texture2D(tex0,…)` 取回来是空的。
  两步探针跑过了，**已经把锅定到 texcoord 那一头**：
    - 探针 A（把采样换成 `texture2D(tex0, t.xy*0.5+0.5)`）：出现白与黑两种非背景色
      —— **纹理是上去了、采得到东西**，所以 `GL_BGRA_EXT` + `GL_UNSIGNED_BYTE`
      那个上传组合没问题；
    - 探针 B（原式不动，只把 alpha 强制成 1）：全是 `(0,0,0,255)` ——
      **原式那组 texcoord 采回来就是黑的**。
    - 探针 C（把 `t` 直接当颜色输出）：`t.x`/`t.y` 逐点在变 ——
      **`gl_MultiTexCoord0` 传对了**；
    - 探针 D（把算出来的 `u`/`v` 当颜色，并用 `u!=u` 检 NaN）：u/v 都在变，
      **蓝通道全 0 = 没有 NaN**。于是"NaN 采回黑"那条也排掉了。
  现在最像的一条：**u 与 v 都是负数**（`u = acos(…)/(-2π) - panx ∈ [-0.5,0]-panx`、
  `v = acos(t.y)/(-π) ∈ [-1,0]`），而 `pd_host_gl.c:147` 那一句会把
  wrap 强制成 `KGL_CLAMP_TO_EDGE`（NPOT 纹理那条规矩）—— 负的 texcoord 一律
  夹到边缘那一列/行上，采出来就是一条黑边。
  量过了：**earth.jpg 是 512x256（正好是 2 的幂）**，`pd_host_gl.c:140-150` 那条
  强制 CLAMP 的规矩只对 cubemap（`xs*6 == ys`）生效 —— 所以 wrap 没被按，
  CLAMP 那条猜错了。

  试过并**退掉**的一刀（记在这儿免得再试一遍）：把 `glTexImage2D`/`glTexSubImage2D`/
  `gluBuild2DMipmaps` 也按那 43 个名字的办法改名，包一层把
  `GL_BGRA + GL_UNSIGNED_BYTE` 换成 `GL_UNSIGNED_INT_8_8_8_8_REV`
  （Apple 的驱动偏爱后者）。结果**四个例子一起挂**，而且只在 stdout 不是终端时挂
  （tty 下与 lldb 下都正常）—— 那种"看起来与 I/O 有关"的崩通常是别处的内存问题被
  时序放大了。已经 `git checkout` 退回去，绿的状态保住。真要做这一刀得先把那个崩查清，
  不能揣着一个会挂的改动往前走。
* `ken/gspiral.pss` —— **不计**。片元着色器用了 `&` / `>>`（整数位运算），
  那是 GLSL 1.30 起才有的；macOS 的 legacy profile 最高 GL 2.1 / GLSL 1.20，
  编译期就报 `'&' does not operate on 'int' and 'int'`。要它得换 core profile（3.2+），
  可是 core 里没有固定管线，而 polydraw 的 `glBegin/glEnd` 一族要固定管线 ——
  那是另一条路，不在这一轴里。

这一轮补的两格（都不是脚本的欠账，是移植的洞）：

* **数据文件要按 cwd 找**。`GetModuleFileName` 原先报可执行文件的真路径，
  polydraw 拿它切出 `gexedironly` 再 `kzaddstack(gexedironly)`（`pd_win.c:528`）——
  二进制在 `bench/out/` 下，而 `earth.jpg` / `ken/` / `tigrou/` 在仓库根，图一张都找不到。
  改成报 **cwd + 可执行文件名**（Unix 上"在哪儿跑就从哪儿找数据"的常规）；
* **鼠标默认报渲染窗格的正中**，不是 (0,0)。不少脚本拿 `mousx/mousy` 定位几何 ——
  `orthoglobe` 是 `z = mousy/yres*4`，报 0 的话 z=0，而 `gluPerspective` 的近平面是 0.1，
  整个扇面被近平面裁掉。`PD_MOUSE=x,y` 可以改（要复现某一帧时用）。
  就是这一格让 `ken/texture.pss` 从空画面变成 98 色。
