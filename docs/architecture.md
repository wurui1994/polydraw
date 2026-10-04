# 架构与移植方法

> 逐案的证据与探案过程在 [port/README.md](../port/README.md)；本文按主题归纳"是怎么搭的"。

## 一条规矩与三种手段

**规矩：`polydraw_src/` 下的原文一个字节都不动。** 这句话先站得住，靠的是拆分：
原文三份大文件已按声明拆成 36 个模块（`eval/` 14、`pd/` 13、`kp/` 9），**逐字节可复原**
（工具 `omni c split`，ADR-0046，见 [c-split.md](c-split.md)）；缝合文件 `*.stitch.c`
就是一串 `#include`，接起来等于原文。缺的东西再全在 `port/` 里挂上去，靠三种手段：

1. `clang -include port/pd_port.h` —— 补 MSVC 的关键字（只剩 `_cdecl` 与 `memicmp`，
   别的 Ken 自己在 `eval.c` 里已写了非 MSVC 的等价物）；
2. `polydraw_src/*.a64.stitch.c` —— arm64 专用的**缝合文件**（新材料，不算改原文）：
   它就是一串 `#include` 原文的模块，可以**把某一格换成 `port/a64/` 的实现**，
   也可以在两个 `#include` 之间插 `#define` 改名/换常量（如 `#undef KIMM` 修 LP64 那格）；
3. `port/a64/*.c` —— "换掉的格子"与新写的实现（GL、GUI、JIT、win32 垫片）。

这套缝合由 `bench/build-a64.sh` 编成五个产物：

| 产物 | 组成 | 用途 |
| --- | --- | --- |
| `eval_a64` | `eval.a64.stitch.c` + `pd_gl_imm_stub.c` | Ken 自带 eval 测试 main 的等价替代判据 |
| `eval_bench` | 同上 + `port/a64/eval_bench.c` | 量尺（自带那个 main 在非 x86 上永远印 `0 cc`） |
| `kplib.o` | `kplib.stitch.c` | 原文零改动直接编过 |
| `polydraw.o` | `polydraw.a64.stitch.c`（+ `port/a64/winshim/` 假 windows.h / gl.h） | 主程序，只到 .o |
| `polydraw_a64` | polydraw.o + kplib.o + eval.o + `pd_win_a64` + `pd_gl_cgl` + `pd_main_a64` + `pd_gui_glfw` + `pd_gl_imm` | 完整可执行 |

## win32 垫片：`port/a64/pd_win_a64.c`

81 个 win32 函数，按"polydraw 需要什么"分三类：

* **真实现**：计时（`QueryPerformanceCounter` → `clock_gettime`，频率报 1e9）、
  `GetModuleFileName`（报 **cwd** + 可执行名 —— 数据文件按 cwd 找）、ini 读写；
* **空壳回成功**：窗口/菜单/光标/字体/对话框（编辑框是空的，脚本正文由
  `pd_main_a64.c` 塞进一格全局、`GetWindowText` 回它）；
* **空壳回失败**：线程/管道/进程/MIDI（polydraw 判返回值，回失败即走单线程路径）。

**看门狗那一格**：原文用线程给脚本计时，超时置 `gshaderstuck`；垫片回
`WAIT_OBJECT_0` 让它永不触发 —— 代价是脚本死循环会挂住进程，出图这条路可接受。

## GL 上下文三档

| 档 | 文件 | 用途 |
| --- | --- | --- |
| CGL 离屏 + FBO | `port/a64/pd_gl_cgl.c` | macOS 出图/扫描：不碰 AppKit、不要主线程与 run loop；`SwapBuffers` 即"一帧画完"的钩子（数帧、到点 `glReadPixels` 写 PNG；PNG 写出器在这份里 —— kplib 只读不写，deflate 用存储块） |
| EGL surfaceless | `port/x64/pd_gl_egl.c` | linux 无头：EGL 平台要**点名** `EGL_PLATFORM_SURFACELESS_MESA`，mesa llvmpipe 光栅；不传 profile 属性 = 兼容档（polydraw 的 `glBegin/glEnd` 要固定管线） |
| GLFW 窗口 | `port/a64/pd_gui_glfw.c` | `--gui`：legacy 2.1 上下文（要固定管线，所以不能 core profile）；GUI 档**不建 FBO**，上下文归窗口 |

`wglGetProcAddress` = `dlsym(RTLD_DEFAULT, 名字)`，找不着再试 `*EXT`/`*ARB`
（x64 那条再退一次 `glfwGetProcAddress`）。同一挂点还做了两件事：

* **着色器源码前缀注入**：`glShaderSource` 包一层，在 `#version` 之后塞
  `#extension GL_EXT_gpu_shader4 : enable` / `GL_ARB_shader_texture_lod`（补一句
  `#line 1` 保住报错行号）—— `mipmap` 的 `texture2DLod` 与整数位运算因此直接通了；
* **名字 0 那格贴图**：polydraw 拿"第几格"当 GL 名字，0 号是默认贴图对象，
  Apple 不许 —— `pd_gl_texdbg.c` 那层把它换成 `glGenTextures` 的真名字。

## GUI 腿（`--gui`）

一个渲染路径都没改，只补四件事：

* GLFW 开窗口（见上表）；上下文归窗口、帧缓冲尺寸按 Retina 的口径给（`/WxH` 给
  帧缓冲，不是窗口）；
* `pd_gui_bridge.c` 是喂输入的**唯一通道** —— `dkeystatus[]`/`dbstatus`/`popts`
  都是 `pd_head.h` 里的 static，所以这份桥必须包在缝合文件末尾；
* 键盘按 DOS/DirectInput **扫描码**映射（脚本读的是 `keystatus[0xcd]` 这类）；
* 退出：该关的时候投 `WM_QUIT`，原文 `pd_win.c` 见到它就 `goto quitit`，帧循环零改动。

判据不看窗口也能量：swap 前读整张帧缓冲的**非黑像素数** + 标题栏那行 fps
（`SetWindowText` 转给 `glfwSetWindowTitle`）。

## 立即模式攒批：`port/a64/pd_gl_imm.c`

背景（两个探针量出来的）：Apple legacy GL 在**绑了用户着色器**时，为每趟
`glBegin…glEnd` 重新发一趟 compute 变换内核（约 3µs/趟）；`ken/balls.pss`
一帧 16384 球就是 16384 趟 —— 41.9ms/帧里绝大部分是这一格。

做法是在**宿主调用那一层**拦（JIT 在编译期就知道被调的是哪个 `qgl*` 函数指针）：

* `qglBegin/End/Vertex/TexCoord/Color/Normal` 换成 `pd_imm_*`：顶点攒进数组、
  `glEnd` 时三角化（fan/strip/quad 按 GL 次序），一个 GL 都不发；
  **恒等置换**的那几族（POLYGON/FAN/STRIP 收三角、TRIANGLES 收 3n、LINES 收 2n、
  POINTS 任意）一个字节都不用抄 —— 顶点直接写在攒批末尾，`glEnd` 才算进 `B_n`；
* **会动 GL 状态的**别的宿主调用先 `pd_imm_break()`（`pd_imm_isgl`：`GL` 开头一族 +
  PRINTG + SETFOV；**在 glBegin…glEnd 里逐顶点调脚本函数**的那种不许打断，
  否则 `materialize()` 每顶点一趟、比不攒还慢 9 倍）；
* 一帧收尾 `pd_imm_flush()`：一趟 `glDrawArrays` + 四个客户端数组；
* 图元少的帧不攒（`PD_IMM_MINPRIM=64`，按上一帧图元数定）—— 客户端数组那套
  让驱动换顶点布局，图元少时赚不回来。

判据：四份确定性探针 `PD_IMM=0/1` 的 PNG **md5 逐字节相同**；`balls` 28.8~31.5 →
4.2~4.8 ms/帧。解释器那条路也过同一层（`tools/mkrun.mjs` 插的 `pd_imm_call`），
所以两条路一样省。

## x86-64：四处分岔，其余共用

同一份源码，第二个架构。分岔只有四处：

1. **thunk**：36 字节 x86-64 版（`movabs r10/r11` 那四句）。只许用 r10/r11 ——
   SysV 里 `kasm87c(double,...)` 是变参，callee 的 `va_start` 要读 `al`；
2. **JIT 发射器**：`port/x64/pd_x64_jitc.c`（SSE2 码），公共的退路/kcd 缓存/开关
   与 A64 那份共用（详见 [jit.md](jit.md)）；
3. **GL 上下文**：macOS 走 CGL（与 arm64 同一份），linux 走 EGL surfaceless 或
   GLFW 不可见窗口（要 X）；
4. **GL 头与 proc 查表**：`OpenGL/gl.h` → `GL/gl.h`，winshim 那 43 行让位。

**两条腿的产物不许混**：一边 ELF、一边 Mach-O，容器又挂同一个仓库目录 ——
`build-x64.sh` 按 `uname -s` 分 `bench/out-x64`（linux）/ `bench/out-x64-osx`（osx）。
Linux 链接还必须 `-rdynamic`：polydraw 拿 `wglGetProcAddress` 查**我们自己导出的**
`wgl*` 垫片，ELF 可执行文件的符号默认不进动态符号表。

## 生成器：`tools/mk*.mjs`

`pd_a64_run.c`（解释器那 52 处变参宿主调用）、`kasm_main_a64.c`、`kasm_math_a64.c`、
`kasm_parse_a64.c` 都是**由 mjs 生成**的原文替换件，而不是手抄的手改件 ——
生成器改几处、其余逐字复制，于是 `diff -u polydraw_src/eval/<原文>` 一眼审得完。
这是"原文零改动"这条规矩能守住的关键：替换件**可审**，而不是一坨 fork。
