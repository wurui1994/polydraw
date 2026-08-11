# 05 — 图形子系统（GPU 抽象）+ Offscreen 渲染

## 1. 范围与优先级

> **这是当前最高优先级。** EVAL 宿主脚本通过 `glXxx` 外部函数调用图形 API。目标是尽快打通"加载 `.pss` → 编译 host 块 → 执行渲染调用 → **出图**"的完整流程，让脚本能真正跑起来并产生可验证的画面。

EVAL 宿主脚本通过 `glXxx` 外部函数调用图形 API。原版用 fixed-function OpenGL 1.x + ARB 扩展。现代化方案：

- **C 实现**：OpenGL 3.3 Core Profile（GLAD 加载，GLFW 窗口）。
- **JS 实现**：WebGL2（GLSL ES 3.0）。
- **两种渲染模式**（同等支持，共享同一套 EVAL 侧 API）：
  - **窗口渲染**：可见窗口，交互（鼠标/键盘），主循环。
  - **Offscreen 渲染**：无窗口（headless），渲染到 FBO/离屏缓冲，导出帧到 PNG/缓冲区。**这是验收渲染正确性的主要手段**（见 §11）。

两者共享同一套"EVAL 侧 API 语义"，差异只在后端实现。

## 1.1 Offscreen 渲染（验收优先路径）

为方便自动化检查与无显示器的 CI 环境，offscreen 渲染是**第一类**支持，而非附属功能。

**C 实现 offscreen**（**已完成**，`c_impl/src/render/gl_offscreen.{h,c}` + `gl_renderer.{h,c}`）：
- 上下文：无窗口、无显示服务器，镜像 moderngl `create_standalone_context()`。
  - macOS：CGL，`kCGLPFAOpenGLProfile` + `kCGLOGLPVersion_3_2_Core`，**不挂 drawable**（FBO 渲染不需要）。
  - Linux：EGL surfaceless（`EGL_MESA_platform_surfaceless`，回退 `eglGetDisplay`）+ 无 surface 上下文（GLAD 加载）。
  - Windows：TODO（WGL 隐藏窗口）。
- 渲染目标：固定尺寸 FBO（RGBA8 + Depth24 renderbuffer）。
- 管线：GLCmd 流（跨后端契约）→ 立即模式状态机（当前色/texcoord/normal + 矩阵栈）→ 图元镶嵌（QUADS/POLYGON/QUAD_STRIP/FAN/STRIP → 三角形）→ 交错 VBO/VAO → 适配后的 GLSL 330 core 程序 → `glDrawArrays` → `glReadPixels` → `stb_image_write` PNG（`c_impl/third_party/`）。
- 投影矩阵：逐位复刻 `pyref/software_renderer.py` 的 `mat_perspective`/`mat_ortho`（numpy 转置存储，`m[3,2]` 是 w 系数）——**不是**标准 gluPerspective 布局，这是与参考软件渲染器像素对齐的关键。
- 混合语义复刻参考：RGB 覆盖（src=ONE, dst=ZERO）、alpha 取 max（`glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX)`）。
- 图元镶嵌顺序、varying 语义、默认 `glDisable(GL_DEPTH_TEST/CULL_FACE)` 均与参考一致。
- CLI：`polydraw-render foo.pss [--frame N] [--w W] [--h H] [--fovy DEG] [-o out.png]`
  （默认 frame 30 / 640x480 / fovy 73.74 = setfov(90) 有效值 / 时钟 1/60 确定性）。
- 验证：`ken/balls.pss` f5、`ken/ceilflor2.pss` f2 与 `pyref/golden/*.png` 像素对比，diff>2 像素 < 0.01%（余量为 GPU 光栅化规则的边缘像素差异）。

**JS 实现 offscreen**：
- 浏览器：`OffscreenCanvas` + WebGL2（可在 Worker 里）；`canvas.convertToBlob()` 导出 PNG。
- Node：`headless-gl`（社区库，纯软件/硬件 GL）或 `skia-canvas`；若无 GL，降级为"解释器跑 EVAL + 软件光栅化"的最小验收路径。

**验收用法**：
```
# 渲染单帧，肉眼/工具比对
polydraw render balls.pss --single 30 --w 640 --h 480 --out balls_f30.png
# 渲染序列（动画）
polydraw render drawsph.pss --frames 60 --out drawsph_%04d.png
```


---

## 2. 挑战：fixed-function → modern GL

EVAL 脚本里大量 fixed-function 调用：
```
glBegin(GL_QUADS);
glColor(1,0,0); glVertex(-1,-1,0); glVertex(1,-1,0); glVertex(1,1,0); glVertex(-1,1,0);
glEnd();
glPushMatrix(); glTranslate(...); glRotate(...); glScale(...);
```

modern GL（3.3 Core / WebGL2）**没有这些**。解决方案：**在宿主侧实现一个 fixed-function 兼容层**，把状态累积成顶点缓冲，最终用 `glDrawArrays` 提交。

### 2.1 兼容层状态机

```c
typedef struct {
    // 矩阵栈（modelview + projection）
    MatStack modelview, projection;
    // 当前顶点属性
    double curColor[4], curNormal[3], curTexCoord[4];
    // 当前图元类型 + 顶点累积缓冲
    GLenum  primMode;
    DynArray<Vertex> verts;   // 累积到 glEnd 才 flush
    // 纹理绑定状态
    int    activeTexUnit;
    GLuint boundTex[16];
} FFState;
```

`glBegin(m)` → 设 `primMode`，开始累积；`glVertex(...)` → push 一个 `Vertex{pos,color,normal,texcoord}` 到 `verts`；`glEnd()` → 把 `verts` 上传到 VBO，`glDrawArrays(primMode, 0, n)`。

### 2.2 内置着色器

fixed-function 兼容层自带一个"passthrough"着色器，把累积的顶点属性透传到 `gl_Position`，颜色/纹理坐标作 varying。当用户脚本用 `@v/@f` 提供自定义着色器时，切换到用户着色器（其顶点属性由我们的 VBO 供给）。

### 2.3 矩阵

`glPushMatrix/glPopMatrix/glTranslate/glRotate/glScale/gluPerspective/gluLookAt`：自己实现矩阵栈与 4x4 矩阵运算（无需 GL 工具库）。`ftransform()` 在 GLSL 适配里替换为 `proj * modelview * gl_Vertex`，所以我们要把 modelview/projection 作为 uniform 传给用户着色器。

### 2.4 深度/混合/剔除

- `glEnable/glDisable(GL_DEPTH_TEST)`、`glBlendFunc`、`glCullFace`、`glLineWidth`：直接映射 modern GL 同名调用。
- `glAlphaEnable/glAlphaDisable`（已废弃）：映射到 `glEnable(GL_BLEND)+glBlendFunc(...)` / 深度测试开关。

---

## 3. 纹理

原版支持 256 个用户纹理槽 (`MAXUSERTEX`)，支持 1D/2D/3D/cube，从文件 (`glsettex(i,"file.png")`) 或 EVAL 数组加载。

### 3.1 纹理对象表

```c
typedef struct {
    GLuint glTex;
    int    target;   // GL_TEXTURE_1D/2D/3D/CUBE_MAP
    int    xsiz, ysiz, zsiz;
    int    colmode;  // KGL_BGRA32 / KGL_FLOAT / KGL_VEC4 / ...
    int    filter, wrap;
} Tex;
Tex textures[256];
```

### 3.2 颜色模式 (`colmode`)

复刻 `polydraw.txt:218` 的常量表。从 EVAL 数组上传时按 colmode 解析字节布局（`KGL_BGRA32` = 1 个 32 位值/像素，`KGL_VEC4` = 4 个 float/像素，等）。

### 3.3 文件加载

- **C**：用 `stb_image`（vendored 单头）替代 `kplib.c` 的 PNG/JPG/GIF/... 解码。`stb_image` 覆盖 PNG/JPG/TGA/BMP/PSD/GIF/HDR/PIC/PNM，比 kplib 更广且更小。
- **JS**：浏览器原生 `Image` + canvas / `createImageBitmap`；Node 用 `sharp` 或纯 JS 解码（优先浏览器原生）。
- ZIP 挂载 (`mountzip`)：C 用 `miniz`（vendored）；JS 用浏览器 `DecompressionStream` 或 `fflate`（轻量）。

### 3.4 capture-to-texture (`glcapture/glcaptureend`)

- **C**：FBO（`glGenFramebuffers`）渲染到纹理。
- **JS**：WebGL2 framebuffer。

---

## 4. GLSL 适配方言（关键）

用户的 `@v/@g/@f` 块是旧版 GLSL（1.10~1.20 风格）。modern GL 需要 `#version`。我们在宿主侧做**源到源翻译**：

### 4.1 翻译规则

| 旧符号                      | 翻译为 (3.3 Core / GLSL ES 3.0)                              |
| --------------------------- | ------------------------------------------------------------ |
| `gl_FragColor`              | `out vec4 _fragColor;` (声明) + 替换所有写入                  |
| `ftransform()`              | `_proj * _modelview * gl_Vertex` (注入 uniform mat4)          |
| `gl_ModelViewProjectionMatrix` | uniform mat4 `_mvp`                                        |
| `gl_ModelViewMatrix`        | uniform mat4 `_modelview`                                    |
| `gl_ProjectionMatrix`       | uniform mat4 `_proj`                                         |
| `gl_Vertex/gl_Color/gl_Normal/gl_MultiTexCoordN` | `layout(location=N) in vec4 ...`（从我们的 VBO 供给） |
| `varying`                   | `out`(vertex) / `in`(fragment)                              |
| `texture2D(s,uv)`           | `texture(s,uv)`                                              |
| `textureCube(s,v)`          | `texture(s,v)`                                              |
| `gl_FragCoord`              | 原生保留                                                     |
| `discard`                   | 原生保留                                                     |

### 4.2 注入头

每个用户着色器前自动加：
```glsl
#version 330 core        // 或 #version 300 es (WebGL2)
layout(location=0) in vec4 gl_Vertex;
layout(location=1) in vec4 gl_Color;
layout(location=2) in vec3 gl_Normal;
layout(location=3) in vec4 gl_MultiTexCoord0;
// ... up to gl_MultiTexCoord7
uniform mat4 _modelview, _proj, _mvp;
// (fragment) out vec4 _fragColor;
```

并做正则/AST 级替换（简单正则即可，因为旧 GLSL 符号集有限且固定）。

### 4.3 几何着色器

`@g` 块的输入/输出类型在 `@g,GL_...,GL_...,N` 行声明（`polydraw.c:1788` 解析）。modern GL 用 `layout(...) in; layout(triangle_strip, max_vertices=N) out;`。版本 < 1.50 时由我们注入 1.50 头。

### 4.4 ARB 汇编 (`!!`)

现代化版本**不支持**，编译时报错："ARB assembly shaders are deprecated and not supported in the modernized PolyDraw." 涉及的脚本（`*_asm.pss`）视为历史遗产，提供等价的非 asm 版本（多数 `ken/` 里都有配对的非 asm 版）。

---

## 5. 着色器程序管理

`glsetshader("vname","fname")` / `glsetshader("v","g","f")`：按名字选着色器组合，link 成 program，缓存。`polydraw.c` 用 `shadprogi[]` 记录链接关系，我们沿用。

`glGetUniformLoc("name")` → `glGetUniformLocation`；`glUniform*` → 对应 modern GL 调用；数组形式 `glUniform1fv(loc,n,&arr[0])` 对应 `glUniform1fv`。

---

## 6. 绘制全屏四边形 (`glquad(mode)`)

`glquad(0)` alpha / `glquad(1)` opaque：直接画一个覆盖 NDC 的四边形，用当前着色器。这是 post-processing 模式。

---

## 7. 计时 (`klock` / `glklockstart` / `glklockelapsed`)

- `klock()`：编译后流逝的秒数（高精度时钟）。`klock(1..9)`：本地日期时间分量；负数 UTC（`polydraw.txt:352`）。
- `glklockstart/glklockelapsed`：GPU 计时查询（`GL_TIME_ELAPSED`）。modern GL 用 `glQueryCounter`。
- `numframes`：自编译以来的帧数。

---

## 8. 输入

- `xres/yres`：视口尺寸（resize 时更新）。
- `mousx/mousy`：鼠标相对渲染窗左上角。
- `bstatus`：鼠标按键位掩码（bit0 左，bit1 右，bit2 中）。
- `keystatus[256]`：按键状态，按**扫描码**索引（`polydraw.txt:391` 的 PC AT 扫描码）。modern 窗口系统（GLFW/GLFW）提供的是 key code，需要一张 key code → 扫描码映射表，覆盖 `polydraw.txt` 列出的常用键。未映射的键尽力而为。

---

## 9. 文本绘制 (`printg`)

6x8 像素字体（原版内置）。现代化版本：
- 内置同一份 6x8 位图字体（数据从原版提取或重画）。
- 用纹理化 quads 绘制（每个字符一个 quad，纹理图集）。
- `printf` 写到控制台（C: stdout/log 窗口；JS: `console.log` 或 on-screen 日志）。

---

## 10. 测试（以 offscreen 出图为验收）

- **首要验收**：每个 `ken/`、`tigrou/` 示例脚本能用 offscreen 模式渲染出图（`polydraw render foo.pss --single N --out foo.png`），人工/工具比对画面合理（容许 GPU 驱动差异）。这是"渲染全流程打通"的可执行判据。
- fixed-function 兼容层：`glBegin/glVertex/glEnd` 累积的几何与原版 GL 渲染一致（位置/颜色/法线/纹理坐标）。
- GLSL 适配方言：所有示例的 `@v/@f` 块能在 modern GL 上编译通过。
- 窗口模式与 offscreen 模式产出**像素一致**的帧（同一 GL 上下文语义）。

## 11. 渲染全流程里程碑（当前主线）

打通渲染的渐进式子步骤（对应重排后的 M2-host → M3-render）：

1. **FBO + 离屏上下文**：建立 offscreen GL 上下文 + FBO（color/depth），`glReadPixels` 出图。先不接 EVAL，验证"清屏 → 画一个三角形 → 出 PNG"。
2. **fixed-function 兼容层（最小集）**：`glBegin/glVertex/glEnd/glColor/glPushMatrix/glTranslate/...` 累积到 VBO，passthrough 着色器出图。跑 `balls.pss`（纯 fixed-function）。
3. **GLSL 适配方言**：`@v/@f` 块的旧符号翻译，passthrough → 用户着色器切换。跑 `drawsph.pss` / `interference.pss`。
4. **纹理**：`stb_image` 加载 + colmode 上传 + capture-to-texture。
5. **窗口模式**：GLFW 窗口 + 输入 + 主循环，与 offscreen 共享渲染层。

每步都用 offscreen 出图验收，不依赖人工盯窗口。

