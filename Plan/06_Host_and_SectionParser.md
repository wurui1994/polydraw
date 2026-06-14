# 06 — 宿主程序与 `.pss` 分块解析

## 1. 宿主职责

1. 加载 `.pss` 文件，按 `@h/@v/@g/@f` 分块。
2. 把 host 块交给 EVAL 编译器（`kasm87` 等价物）。
3. 把 v/g/f 块交给 GPU 驱动（经 GLSL 适配方言，见 `05_Graphics.md`）。
4. 注册外部函数/变量表（让 EVAL 脚本能调 `glBegin` 等）。
5. 主循环：每帧调用编译好的 EVAL 函数 → 触发图形调用 → swap buffers。
6. 监听编辑器改动 → 增量重编译。
7. 冻结保护：检测卡顿 → 通知 EVAL 退出。
8. 错误高亮、日志窗口。

---

## 2. `.pss` 分块解析（复刻 `txt2sec`, `polydraw.c:1741`）

### 2.1 块标记语法

| 行首标记                         | 块类型 | 备注                                  |
| -------------------------------- | ------ | ------------------------------------- |
| *(无，最开始的代码)*             | host   | 默认；或用 `@h` 显式标记              |
| `@h`                             | host   | 多个 host 时取最后一个（`polydraw.txt:170`） |
| `@v` 或 `@v:name` 或 `@v(:name)` | vertex shader | 可选名字                          |
| `@g` 或 `@g,GL_IN,GL_OUT,N:name` | geometry shader | 可选 IO 类型与名字             |
| `@f` 或 `@f:name`                | fragment shader | 可选名字                          |
| `@(:name)`                       | 同上块类型 | 继续上一个 v/g/f 的类型              |

### 2.2 几何着色器声明行

`@g,GL_TRIANGLES,GL_TRIANGLE_STRIP,1024:myname`：
- `GL_TRIANGLES`：输入图元类型。
- `GL_TRIANGLE_STRIP`：输出图元类型。
- `1024`：最大顶点数。
- `myname`：名字。

解析 `polydraw.c:1788` 列出所有支持的 `GL_*` 常量。现代化版本复用同一表。

### 2.3 数据结构

```c
typedef struct {
    int   typ;       // 0=host, 1=geom, 2=vert, 3=frag (沿用 polydraw.c 约定)
    int   cnt;       // 同类型第几个 (0-based)
    int   i0, i1;    // 在源码里的起止字节偏移
    int   linofs;    // 块起始行号 (错误高亮用)
    int   nxt;       // 同类型链表 (找"最后一个 host")
    char  nam[32];   // 名字
    int   geo_in, geo_out, geo_nverts;  // 仅 geom
} Section;
```

### 2.4 增量重编译判定

`polydraw.c:1909`：对每个块，若 `(typ, i1-i0, 内容)` 任一变化 → 该块需重编译。host 块重编译会重建 EVAL 函数；v/g/f 块重编译会重建着色器 program。

---

## 3. 外部函数注册（`kasm87addext` 等价物）

`polydraw.c:2070` 的 `myext[]` 表（约 160 项）定义了 EVAL 脚本能用的全部符号。现代化版本**完整复刻**，分类组织：

### 3.1 常量（`double` 变量，EVAL 读）

`GL_POINTS/GL_LINES/.../GL_QUADS/GL_POLYGON`、`GL_*_ADJACENCY*`、`GL_COLOR_BUFFER_BIT/...`、`KGL_BGRA32/KGL_FLOAT/KGL_VEC4/...`、`KGL_LINEAR/NEAREST/MIPMAP*`、`KGL_REPEAT/CLAMP/CLAMP_TO_EDGE`、`GL_TEXTURE0`、`GL_NONE/FRONT/BACK/FRONT_AND_BACK`、`GL_ZERO/SRC_COLOR/...`、`GL_DEPTH_TEST`。

### 3.2 变量（每帧由宿主更新）

`xres`、`yres`、`mousx`、`mousy`、`bstatus`、`numframes`、`keystatus[256]`。

### 3.3 函数（C 函数指针，EVAL 调用）

按 `polydraw.c:2096-2235` 的签名表，含参数原型（`,&`/`,,$`/`,,,)` 等表示 double/double*/char*/参数数）：

- 图元：`glBegin/glEnd/glVertex(1-4D)/glTexCoord(2-4D)/glColor(3-4D)/glNormal(3D)`
- 矩阵：`glPushMatrix/glPopMatrix/glMultMatrix/glTranslate/glRotate/glScale/gluPerspective/gluLookAt/setfov`
- 着色器：`glGetUniformLoc/glUniform1f..4f,1i..4i,1fv..4fv,1iv..4iv/glGetAttribLoc/glVertexAttrib1f..4f/glProgramLocalParam/glProgramEnvParam/glSetShader`
- 纹理：`glsettex(多签名)/glgettex/glactivetexture/glbindtexture/gltextdisable`
- 捕获：`glcapture/glcaptureend`
- 状态：`glcullface/glblendfunc/glenable/gldisable/glalphenable/glalphadisable/gllinewidth/glswapinterval`
- 绘制：`glquad`
- 计时：`klock/glklockstart/glklockelapsed`
- 工具：`rgb/rgba/noise(1/2/3D)/printf/printg/srand/sleep/playnote/mountzip`

每个外部函数的 C 签名严格匹配原型。`EXT` 调用约定见 `04_JIT_Backend.md` §4.2。

### 3.4 JS 实现的对应物

JS 实现把每个外部符号实现为 JS 函数，挂在一个 `host` 对象上。IR→JS 翻译时把 `glBegin(...)` 编成 `host glBegin(...)`。V8 内联这些调用。

---

## 4. 主循环

```
while (!shouldClose):
    # 输入/窗口事件
    pollEvents()
    updateInputState()   # mousx/mousy/bstatus/keystatus

    # 检查是否需要重编译（编辑器内容变化）
    if editorDirty: recompile()

    # 渲染
    clearScreen()
    if gevalfunc:
        # 在独立线程或带超时检测调用 EVAL 函数
        callEvalFuncWithFreezeGuard(gevalfunc)

    swapBuffers()
    numframes++
    sleep(if requested)
```

### 4.1 冻结保护集成

`callEvalFuncWithFreezeGuard`：
- 启动一个看门狗计时器（如 2 秒）。
- 调用 EVAL 函数（在主线程或专用线程）。
- 若超时，置 `g_shouldQuit`（JIT/解释器在下个循环回跳点退出）。
- 原版用独立线程 + `kasm87jumpback`；现代化版本优先单线程合作式（更简单），超时阈值更宽松。

### 4.2 帧率显示

标题栏显示 FPS（原版行为）。

---

## 5. 编辑器与 GUI

### 5.1 C 实现

- **GLFW** 提供窗口、OpenGL 上下文、键盘/鼠标。
- **Dear ImGui** 实现代码编辑器（带行号、错误高亮、查找替换）。或更轻量：ImGui 的文本编辑器组件。
- 三窗口布局（渲染窗 + 代码窗 + 日志窗）用 ImGui docking。
- `Alt+Enter` 切换渲染窗全屏。
- `Ctrl+Enter` 手动编译。
- 选项存 `polydraw.ini`（跨平台用 INI 或 JSON）。

### 5.2 JS 实现

- 渲染窗：`<canvas>` + WebGL2 context。
- 代码窗：`<textarea>`（最小）或 CodeMirror 6（轻量、可语法高亮）。
- 日志窗：`<pre>` 元素，追加 `printf` 输出。
- 布局：CSS flexbox / grid。
- 文件加载：`<input type=file>` 或拖拽；浏览器无法自由访问文件系统，提供示例脚本下拉菜单。

### 5.3 Headless 模式（C）

`polydraw --headless script.pss --frames 100 --out frame%04d.png`：无窗口，渲染到 FBO，输出 PNG。用于自动化测试与截图比对。

---

## 6. 文件/资源加载

- `.pss` 脚本：从命令行参数或编辑器打开。
- 纹理图片：相对脚本目录。`mountzip` 把 zip/目录加入搜索路径（搜索顺序见 `polydraw.txt:437`）。
- 配置：`polydraw.ini`（C）或 `localStorage`（JS）。

---

## 7. 错误报告

- EVAL 编译错误：带行号、列号、消息 → 编辑器高亮该行（`polydraw.c:1975` 的 `badlinebits` 机制）。
- 着色器编译错误：解析驱动 `GL_PROGRAM_ERROR_STRING` / `getShaderInfoLog`，提取行号 → 高亮。
- 运行时错误：捕获 SIGFPE 等（C）或 try/catch（JS），写入日志。

---

## 8. 多平台

| 平台        | 窗口/GL          | 编辑器         | 备注                          |
| ----------- | ---------------- | -------------- | ----------------------------- |
| Linux x64   | GLFW + GLX       | ImGui          | 主开发平台                    |
| macOS arm64 | GLFW + NSOpenGL  | ImGui          | OpenGL 已 deprecated 但可用   |
| Windows x64 | GLFW + WGL       | ImGui          | sljit 支持 x64                |
| 浏览器      | canvas + WebGL2  | textarea/CM6   | JS 实现，无 EVAL C 依赖       |
