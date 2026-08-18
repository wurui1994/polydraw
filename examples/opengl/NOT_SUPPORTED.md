# examples/opengl — pss 改写与不支持清单

本目录将 `/Users/wurui/Documents/opengl` 下的 C/GLUT/GLU/glaux 例子改写为 pss 脚本。

## 改写原则

pss 是源语言，底层跑在 **现代 OpenGL（C 端）与 WebGL（JS 端）** 上。这两个后端都 **没有 legacy/GLU/GLUT 便利 API**（没有 `gluPerspective`/`gluLookAt`/`gluCylinder`/`glutSolid*`/`glOrtho`/`glClipPlane`/显示列表/位图字体等）。兼容层（如 `glVertex`）由实现端负责，pss 源码可直接使用 `glBegin/glVertex/glRotate/...` 等调用，不必为"避开 legacy"而扭曲表达。

凡是只能靠 GLU/GLUT/glaux/文件系统/矢量导出才能完成的，列入下方不支持项。能用 `glBegin/glVertex/glRotate/glTranslate/glColor/glsettex/glcapture` + 基础图元手搓等价几何的，都已改写为 `.pss`。

## 已改写的例子（可运行）

| pss 文件 | 原例子 | 说明 |
|----------|--------|------|
| 01_hello_rect.pss | basic/opengl.c, basic/center.c | 矩形（GL_QUADS 替代 glRectf） |
| 02_house.pss | basic/house.c | 多边形房屋 |
| 03_point.pss | basic/point.c | 单点（glPointSize 已支持） |
| 04_rect.pss | basic/rect.c | 矩形（GL_QUADS 替代 glRectf/glOrtho） |
| 05_rotate_points.pss | basic/rotate.c | 绕 (1,1,1) 旋转三点（klock 驱动） |
| 06_five_circles.pss | basic/fivecircle.c | 五个同心圆（GL_LINE_LOOP） |
| 07_auto_rotate_cube.pss | basic/keyrotate.c | 自动旋转线框盒（键盘见下） |
| 09_element.pss | glaux/element.c | 点/线/线环/多边形基元 |
| 10_ortho_poly_UNSUPPORTED.pss | glaux/example.c | 透视相机替代 glOrtho（见下） |
| 11_transform_cube.pss | glaux/transform.c | 变换线框盒（手搓边替代 auxWireCube） |
| 12_mesh_surface.pss | mesh/mesh.c | 鞍面线框网格（z=x²−y²，GL_LINE_STRIP 闭合替代 glPolygonMode(LINE)） |
| 13_saddle.pss | mesh/saddle.c | 鞍面线框网格（z=x²−y²，GL_LINE_STRIP 闭合替代 glPolygonMode(LINE)） |
| 14_wire_sphere.pss | mesh/sphere.c, mesh/drawsphere.cpp | 经纬线框球（替代 gluSphere） |
| 18_curve.pss | curve/curve.c | 参数曲线（GL_LINE_STRIP） |
| 19_curve2.pss | curve/curve2.c | 多色曲线 |
| 20_axis_grid.pss | curve/axis.c, curve/grid.c | 坐标轴 + 网格 |
| 21_curve_grid.pss | curve/curve+grid.c | 曲线 + 网格 |
| 24_shader_hello.pss | shader/hello.c | 自定义 GLSL shader |
| 25_offscreen_capture.pss | shader/offscreen.c | glcapture 抓帧（见下 FBO 限制） |
| 26_texture_procedural.pss | picture/texture.c | 程序化纹理贴图（替代文件加载） |
| 27_bmp_load_UNSUPPORTED.pss | picture/readbmp/soiljpg/bmpinfo | 程序化纹理替代（见下文件加载） |
| 28_peaks.pss | (MATLAB peaks, user fun) | 自定义高度场 peaks 线框网格（GL_LINE_STRIP 闭合，fun() 可替换） |

## 不支持项（pss 兼容层未提供）

1. **`glOrtho` / `gluOrtho2D`（正交投影）**
   原 `glaux/example.c`、`basic/rect.c`、`basic/fivecircle.c`、`basic/rotate.c` 等用正交投影做像素级 2D 映射。现代 GL/WebGL 无 legacy 投影矩阵 API，pss 也未暴露 `glOrtho`。已改用 `setfov` 透视相机 + 缩放几何表达。

2. **`glPointSize`（已支持）**
   `glPointSize(size)` 现已支持：渲染器新增 `GLCMD_POINTSIZE` 命令与一个 `u_pointsize` uniform，默认与自定义 shader 的 vertex shader 都会在 `main()` 开头写入 `gl_PointSize = u_pointsize;`，`GL_POINTS` 基元也已被 `end_primitive` 正确收集并以 `GL_POINTS` 提交绘制。默认点大小 1px，可用 `glPointSize(9)` 等放大。`basic/point.c`、`basic/rotate.c` 已使用。

3. **`glShadeModel(GL_FLAT/GL_SMOOTH)`**
   pss 未暴露着色模型切换。网格例子（mesh/*）一律按单色/顶点色平铺，无法切换平面/平滑着色。

4. **`glClipPlane` + `glEnable(GL_CLIP_PLANE0/1)`**
   半空间裁剪无法表达。`mesh/paraboloid.c`、`mesh/planeclip.c` 的裁剪被丢弃，仅绘制完整几何（见 15/16）。

4b. **`glPolygonMode(GL_FRONT_AND_BACK, GL_LINE)`**
   pss 未暴露多边形填充/描边模式切换。原 `mesh/mesh.c` 用 `glPolygonMode(LINE)` + `GL_QUADS` 画**线框**网格，pss 改用逐格 `GL_LINE_STRIP` 闭合（等价于 `GL_LINE_LOOP`，后者渲染器未实现）来还原同样的线框外观。`12_mesh_surface.pss` 已据此修正。

5. **GLU 几何对象：`gluCylinder` / `gluSphere` / `gluDisk` / `gluNewQuadric`**
   无兼容层。`mesh/surface.c` 的柱/球/盘无法生成（见 17，仅以线框球作占位）。

6. **GLUT/glaux 几何：`glutWireCube` / `glutSolidCube` / `auxWireCube`**
   已用 `GL_LINE_LOOP`/`GL_LINES` 手搓线框盒替代（见 07/11）。

7. **键盘/鼠标交互：`keystatus[]` / `mousestatus[]`**
   脚本内目前无法读取按键/鼠标状态（`keyrotate.c` 的箭头键旋转改为自动旋转）。

8. **文字渲染：`printg` / `wglUseFontBitmaps` / `glRasterPos` / GLUT 位图字体**
   pss 的 `printg` 是 no-op（TODO: GPU text）。`basic/getinfo.c`、`curve/text.c` 的 GPU/驱动信息、位图文字无法输出（见 08/22）。

9. **图像文件加载：BMP / JPG 解码（`ReadBMP` / `SOIL_load_OGL_texture`）**
   pss 无文件访问、无图像解码器。纹理只能经 `glsettex` 程序化生成。`picture/readbmp.c`、`picture/soiljpg.c`、`picture/bmpinfo.c`、`picture/texture.c` 的文件加载部分无法移植（见 26/27）。

10. **矢量导出：`gl2ps`（SVG/PS 离屏导出）**
    无对应 API。`curve/gl2ps.c` 无法移植（见 23）。

11. **FBO / pbuffer 离屏渲染**
    `shader/offscreen.c` 原意是渲染到离屏 buffer 再读回。pss 的 `glcapture()` 只能抓取默认帧缓冲，不是真正的 FBO（见 25）。

12. **显示列表 `glNewList` / `glCallList` / `glGenLists`**
    无对应 API；pss 用即时模式（每帧重发顶点），不影响表达，但原例子若依赖列表优化则无对应。

13. **`glClear` / `glClearColor`**
    pss 解释器当前未注册这两个函数（调用会报 undefined function）。所有例子已移除显式清屏，依赖渲染器每帧默认清屏（与 examples/03 一致）。如需指定背景色需实现层补充。

14. **`GL_LINE_LOOP`**
    渲染器（`gl_renderer.c`）只处理 `GL_LINES` / `GL_LINE_STRIP`，不处理 `GL_LINE_LOOP`。所有原使用 `GL_LINE_LOOP` 的圆/球/盒环已改写为 `GL_LINE_STRIP` 并手动闭合（首尾顶点重复）。

15. **`glRotate` 整数参数 bug（解释器限制）**
    实测 `glRotate(35, 1, 0, 0)`（整数角度）会使后续几何消失（矩阵异常），而 `glRotate(35.0, ...)` 或 `glRotate(klock()*45, ...)`（浮点）正常。所有静态整数角度已改为浮点（如 `45` → `45.0`）规避。这是 pss 解释器的类型处理缺陷，应在实现层修复。

16. **自定义 shader 语法约束**
    pss 的 `@v`/`@f` 块必须使用内建（`gl_Vertex` / `gl_Color` / `ftransform()`），不支持现代 GLSL 的 `attribute` / `gl_ModelViewProjectionMatrix` 声明；且 `@v`/`@f` 块须置于文件**末尾**（其后的语句会被误并入 shader 源码）。

## 备注

- 原 basic/opengl.lua 是 Lua 绑定示例，非 GL 例子，未纳入。
- 坐标系：pss 默认相机在原点看向 -Z，几何放在负 Z（如 -3）处可见；原 C 例子用 `glOrtho(-200..200)` 像素坐标的，已整体缩放到 [-1,1] 区间。
- `setfov(45)` 对应原 `gluPerspective(45, ...)`。
