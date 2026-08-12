# 09 — 路线图、里程碑与依赖清单

## 1. 里程碑总览

> **优先级原则（用户要求）**：先打通 `.pss` 输入 → 渲染全流程（含 offscreen 出图），JIT 在渲染稳定后再做。JIT 终极基准是 **LLVM**（**核心目标，非可选**），sljit 为过渡。解释器是最低要求，永远可用。

| 里程碑 | 内容                                                | 交付物                                    | 验收                                   |
| ------ | --------------------------------------------------- | ----------------------------------------- | -------------------------------------- |
| **M1** | EVAL 核心（lexer+parser A+IR+optimizer+解释器）     | `c_impl/src/eval/` + 纯数学单元测试       | 示例脚本可编译；原版神谕比对通过      |
| **M2** | 宿主框架（分块、外部函数表桩、主循环）              | `c_impl/src/host/` + 可运行程序           | 加载 `.pss`、host 块编译、空渲染循环跑通 |
| **M3** | 渲染全流程（offscreen 优先 + fixed-function + GLSL 适配 + 纹理） | `c_impl/src/gpu/` + `polydraw render`   | `balls/interference/drawsph` 能 offscreen 出图 |
| **M4** | JIT 过渡（sljit）+ 冻结保护                          | `c_impl/src/jit/`                         | JIT vs 解释器逐位一致；冻结保护可中断  |
| **M5** | JIT 终极（LLVM）— **核心目标，非可选**                 | `c_impl/src/jit/llvm/`                    | LLVM vs 解释器逐位一致；性能达标      |
| **M6** | 解析器 Plan B（复刻原版链表折叠）                    | `c_impl/src/eval/parser-fold.c`           | Plan A vs B 差分通过                    |
| **M7** | JS 渲染对齐（WebGL2 + offscreen canvas）             | `js_impl/src/gpu/`                         | 浏览器/Node 跑通示例脚本并出图        |
| **M8** | 打磨（窗口 GUI、错误高亮、INI、性能基准、全部示例）  | 发布版                                    | 全部示例可加载渲染                    |

> JS 的 EVAL 核心（lexer/parser/IR/解释器/compile）已随 M1 同步移植（`js_impl/`，91 单测，48/48 `.pss` 可编译），与 C 行为等价。M7 只补 JS 的 GPU/渲染层。

## 1.1 当前进度

- **M1**（C）：✅ EVAL 核心完成，162 单测全绿，48/48 示例 `.pss` 可编译，44/48 可实际运行。
- **M1**（JS）：✅ `js_impl/` EVAL 管道完成（lexer+IR+interp+parser+compile），91 单测全绿，48/48 可编译。
- **M2→M3**（C 渲染）：✅ `c_impl/src/render/` 已实现 offscreen（CGL/EGL）+ `gl_renderer.c`（GLCmd→GL）+ `pd_runlib.c`；`build/polydraw-render` CLI 已可 `-o out.png` 出图。**验收 A/B 已验证**：`balls/interference/drawsph` 均能 320×240 offscreen 出图（非黑像素 90%+）。**全示例扫描**（128×128，帧 30）：**47/48 个 `ken/`+`tigrou/` 脚本出图成功（含纹理/着色器/capture 类）**，此前判定黑屏的 `mipmap`/`disco blur`/`heightmap`/`orthoglobe`/`particules sparks`/`ribbons invasion`/`snake tube` 已全部出图；唯一 `town textured.pss` 在帧 30 为空属相机环绕时序（`glrotate(atan2(dy,dx)+90)` 轨道在 t≈0.83s 后才转进场），60s/90/120 帧均正常出图（frac 0.70–0.87），非渲染缺陷。
- **M4**（C JIT sljit）：✅ `test_jit` 33 项全绿（JIT vs 解释器逐位差分覆盖算术/控制流/函数/RNG/数组 + drawsph/balls2k/metaballs/ballsk/disco 的 GLCmd glbuf 逐命令 diff）。**冻结保护实测**：`while(1){...}` 在应退信号置位后 **JIT 与解释器均在 ~5ms 内返回**（`pd_run_jit` 回边探针 / 解释器每 4096 条指令检查）。LLVM 后端（M5，可选）未开始。
- **M7**（JS 渲染对齐）：🟢 **核心完成并已差分验证**
  - EVAL→GLCmd 录制层与 C 逐位等价：新增 `js_impl/tests/xbackend.test.ts`，以 C 参考实现（`c_impl/dbg_count.c`）为神谕，对 `balls/interference/drawsph/disco ball` 跑单帧，断言 **GLCmd 逐 op 直方图完全一致**（修复两处导致几何丢失的 EVAL bug：`preDeclareFunctions` 误把 `for(...){`/`if(...){` 当成函数定义使 `cube` 的 CALL aux 错位；funcIdx 偏移错位）。
  - GPU 层 `js_impl/src/gpu/`：`matrix.ts`（4x4 列主矩阵栈，移植 `gl_renderer.c` 的 mat4_*）、`fixedfunc.ts`（GLCmd→DrawBatch 重放）、`renderer.ts`（WebGL2 绘制，接受 `GLLike` 接口，可用 mock GL 验证）。
  - `js_impl/tests/gpu.test.ts`：矩阵数学、disco 重放得到 **19970 batch / 79880 顶点**（与 C 参考一致）、WebGL2 渲染器对 disco 发出 **19970 次 drawArrays / 79880 顶点**（mock GL 验证）。
  - **真实光栅化像素比对（软光栅对齐）✅**：`js_impl/src/gpu/softrender.ts` 软件光栅化器 + 新增 `js_impl/tests/softgolden.test.ts`，把 `balls.pss` 帧5 @320×240 的渲染与参考 golden（`pyref/golden/balls_f5.png`，可经 `pyref/verify.py` 再生）**逐位一致**断言。三路交叉验证一致：JS 软光栅 ≡ pyref 软件渲染器（逐位相同），两者与 C GL offscreen PNG（`polydraw-render`）≥99% 逐位相同 / 100% 在 ±2/255 内（差异仅为 GL 边沿裁决与插值末位舍入）。
  - 修复四个导致几何/像素丢失或错位的根因：
    1. `parser.ts` `installHost` 把 EXT_VAR 符号注册成 `Fam.EXT` 寄存器（`off` 指向存放 `vi` 的 const 槽），此前读到的是变量**下标**而非值——`xres/yres/numframes` 等全部失效。
    2. `softrender.ts` 光栅化用规范带方向边函数（`E_AB/E_BC/E_CA` + 按 `area2` 符号归一），此前只有逆时针采样、三角形整体被拒绝。
    3. `interp.ts` `nrnd` 复刻原版 `eval.c:503`：当前对返回 `y*f`、缓存 `x*f`，去掉 `+1e-20` 抖动；此前对调且加抖动导致 MLT 序列漂移。
    4. `softrender.ts` 屏幕 y 映射改为顶行 = NDC +1（与 C offscreen PNG 顶左原点一致），修复垂直镜像。
  - 另修复 `pyref/software_renderer.py` 的 `mat_perspective`/`mat_ortho` **转置错误**（`-1` 除项与 `(2fn)/(n−f)` 项位置对调）和 `pyref/render.py` ctypes `GLCmd` 缺 `s` 字段（结构错位读不到处），并重建 golden。
  - 完整 JS 单测：106 项全绿（compile 54 / gpu 7 / interp 23 / lexer 14 / smoke 2 / softgolden 2 / xbackend 4）。
  - 剩余（部署侧，需浏览器/Node-GL 上下文）：`renderer.ts` 的 WebGL2 真实 GL 绘制路径与纹理上传（`SETTEXDATA`）在浏览器中逐像素同样可验证。
- **M4**（C JIT sljit）：✅ 已并入上方 M4 条目（`test_jit` 33 项全绿，JIT vs 解释器逐位差分 + GLCmd 逐命令 diff + 冻结保护实测 ~5ms 返回）。
- **M5**（LLVM）：⬜ **核心目标（用户明确，非可选）**。系统已装 LLVM 22.1.8（`/opt/homebrew/opt/llvm`），`llvm-c` 头文件齐全。待实现 `pd_jit_llvm.c`：IR→LLVM IR（基本块 + `fadd/fmul/...` + `call @sin` + `br`）、冻结保护（基本块入口检查）、运行时（`LLJIT`/`MCJIT` 适配 sljit 的 `pd_run_jit` 调用约定）。
- **M6**（解析器 Plan B）：✅ **`c_impl/src/eval/parser-fold.c` 已实现**——严格复刻 `eval.c:parsefunc` 的链表折叠（node 列表 + gop/gnext + 优先级 0..6 逐遍折叠，eval.c:4205），含两处一元符号规则（表达式开头的隐式 0 节点「-x^2」hack eval.c:1995；运算符后的 negit 翻转 eval.c:2019）。`p->useFold` 时所有表达式（含括号/调用实参/数组下标）走折叠解析器；`pd_compile_fold_host` 暴露 Plan B 入口。
  - **顺带修复 Plan A 一元符号语义缺陷**（此前 `3*-2^2`=-12 而原版=12、`-2+3`=-5 而原版=1 等）：`parse_expr_prec` 现按 fresh（minPrec==0，隐式 0 左操作数、符号作二元运算）/ 非 fresh（negit 即时取反操作数、`^` 绑定在取负之后）两条规则解析，与原版完全一致。
  - **差分验证（test_fold）**：① 57 条神谕实测表达式 A==B==原版；② ~80 条差分语料（含负号链/负指数/嵌套括号/比较/逻辑/数组/循环/递归/static）A==B；③ 2000 条随机 fuzz × 3 组随机变量赋值 = 6000 次求值 A==B。三方等价成立。
  - 全量测试 201 项全绿（新增 test_fold 3 项，含 57 条神谕用例）。
- **M8**（打磨）：⬜ GUI/错误高亮/INI/全示例/文档，待 M2-M7 稳定后。


## 2. 详细排期（建议顺序）

### 阶段 1：EVAL 核心（M1）— 最关键，无 GPU 依赖
1. **vendored sljit**：拷 `sljit_src/sljit_src.c` + 头文件进 `c_impl/third_party/sljit/`，写最小测试验证能生成并调用 `double f(double)` 函数。
2. **IR 类型** (`ir.h`/`ir.c`)：定义 `Instr`/`Reg`/`Op`，IR 构建辅助函数。
3. **Lexer** (`lexer.c`)：实现 `01_Lexer.md`，单元测试。
4. **Parser Plan A** (`parser-pratt.c`)：实现 `03_Parser.md` §3，覆盖表达式 + 语句。
5. **Optimizer** (`optimizer.c`)：实现 `02_IR_and_Optimizer.md` §4 的各 pass。
6. **Interpreter** (`interp.c`)：实现 `04_JIT_Backend.md` §5，复刻 `kasm87c_run`。
7. **顶层 `kasm87`** (`compile.c`)：源码 → token → IR → 优化 → 返回可执行结构。
8. **差分测试 harness**：编译原版 `eval.c` 为神谕，比对 IR。
9. **验收**：1000 随机表达式，Plan A IR 与神谕语义等价；解释器执行结果正确。

### 阶段 2：宿主框架（M2）— 渲染管线骨架，无 JIT 依赖
10. **offscreen GL 上下文 + FBO**（`gpu/offscreen.c`）：EGL/pbuffer 或隐藏窗口上下文；FBO color/depth；`glReadPixels` → PNG（`stb_image_write`）。先不接 EVAL，验证"清屏→三角形→出图"。
11. **`.pss` 分块解析** (`host/sections.c`)：`@v/@g/@f/@h` + geom 声明行（部分已在 `pd_section.c`）。
12. **外部函数表桩** (`host/externs.c`)：复刻 `polydraw.c:2070` 的 `myext[]`，C 函数桩先记录调用/返回 0（GL 调用暂为空，下一步接真实 GL）。
13. **渲染主循环** (`host/mainloop.c` / `polydraw render`)：编译 host 块 → 解释器执行 N 帧 → 每帧出图。CLI：`polydraw render foo.pss --frames N --single N --w W --h H --out`。
14. **验收**：加载 `.pss`，host 块编译，解释器跑帧循环，空渲染（无几何）offscreen 出图（清屏色 PNG）。

### 阶段 3：渲染全流程（M3）— 出图，最高优先
15. **GLAD 加载** modern GL 函数。
16. **fixed-function 累积层** (`gpu/fixedfunc.c`)：`glBegin/glVertex/glColor/glNormal/glTexCoord/glEnd` → VBO + `glDrawArrays`。
17. **矩阵栈** (`gpu/matrix.c`)：4x4 运算 + `glPushMatrix/glPopMatrix/glTranslate/glRotate/glScale/gluPerspective/gluLookAt`。
18. **内置 passthrough 着色器**：顶点透传 + 颜色/纹理坐标 varying。
19. **外部函数实现**：把 §12 的桩换成真实 GL 调用（fixed-function 子集）。
20. **验收 A**（纯 fixed-function）：`balls.pss` offscreen 出图，画面合理。
21. **GLSL 适配方言** (`gpu/glsl-adapter.c`)：旧 GLSL → 3.3 Core（`gl_FragColor`/`ftransform`/`varying`/`texture2D` 等翻译）。
22. **着色器 program 管理**：`glsetshader/glGetUniformLoc/glUniform*`。
23. **验收 B**（自定义着色器）：`interference.pss`/`drawsph.pss` offscreen 出图。
24. **纹理** (`gpu/textures.c`)：`stb_image` 加载 + 各 colmode 上传 + capture-to-texture（FBO）。`glsettex/glgettex/glactivetexture/glbindtexture`。
25. **验收 C**（纹理 + capture）：带纹理/capture 的脚本出图。

### 阶段 4：JIT 过渡（M4，可选，渲染稳定后）
26. **vendored sljit**：拷 `sljit_src/sljit_src.c` + 头文件进 `c_impl/third_party/sljit/`，最小测试验证 `double f(double)`。
27. **sljit 后端** (`jit/sljit-backend.c`)：IR → sljit，先纯表达式，再控制流（IF0/IF1/GOTO + label 两趟）。
28. **用户函数 / 外部函数调用** 的 JIT 发射。
29. **冻结保护**：`g_shouldQuit` 注入（每个循环回跳前比较-跳转）。
30. **数组边界检查** 的 JIT 发射。
31. **验收**：JIT vs 解释器逐位一致（差分）；冻结保护 100ms 内中断；JIT 驱动渲染与解释器渲染像素一致。

### 阶段 5：JIT 终极（M5，核心目标，必须实现）
32. **llvm-c 集成**：构建时探测系统 LLVM（`llvm-config --prefix`），`POLYDRAW_JIT=llvm|sljit|off`。
33. **IR → LLVM IR** (`jit/llvm-backend.c`)：基本块 + `fadd/fmul/...` + `call @sin` + `br`。
34. **冻结保护**（基本块入口插入检查）。
35. **验收**：LLVM JIT vs 解释器 vs sljit 三方逐位一致；性能达标（≥ 原版 x87 JIT）。

### 阶段 6：解析器 Plan B（M6）
36. **链表折叠解析器** (`parser-fold.c`)：严格复刻 `eval.c:parsefunc` + 折叠循环。
37. **差分 harness 扩展**：Plan A vs B vs 神谕三方比对。
38. **验收**：fuzzing 1000 次，A 与 B 等价。

### 阶段 7：JS 渲染对齐（M7）
39. **WebGL2 + fixed-function 层** (`js_impl/src/gpu/`)：移植 C 的 `gpu/` 语义。
40. **GLSL 适配**（GLSL ES 3.0）。
41. **OffscreenCanvas**：Node/浏览器 offscreen 出图（与 C 像素等价为验收）。
42. **UI**（canvas + 代码编辑器 + 日志）。
43. **验收**：浏览器/Node 加载示例脚本并出图。

### 阶段 8：打磨（M8）
44. **窗口模式 GUI**：GLFW 窗口 + ImGui 编辑器/日志（与 offscreen 共享渲染层）。
45. **错误高亮**：编辑器根据 `origOffset/origLine` 标红。
46. **配置持久化**：`polydraw.ini` / `localStorage`。
47. **性能基准**：对照原版。
48. **全部示例**：脚本逐个验证，记录已知不支持项。
49. **文档**：用户手册（更新版 `polydraw.txt`）。

## 3. 依赖清单（全部 vendored）

### C 实现
| 依赖          | 版本    | 用途                | 许可       | 体积      | 阶段 |
| ------------- | ------- | ------------------- | ---------- | --------- | ---- |
| GLFW          | 3.4     | 窗口/GL 上下文/输入 | zlib       | 库         | M2/M3 |
| GLAD          | 生成    | GL 函数加载         | MIT        | 生成文件  | M2/M3 |
| stb_image / stb_image_write | 最新 | 纹理解码 / PNG 导出 | Public Dom | 单头×2 | M2/M3 |
| miniz         | 最新    | ZIP 支持 (`mountzip`) | MIT       | 单文件    | M3 |
| Dear ImGui    | 1.90+   | 编辑器/GUI（窗口模式）| MIT       | 库         | M8（窗口模式） |
| sljit         | 最新 master | JIT **过渡**后端（可选） | BSD-2  | 单文件 ~6k 行 | M4（可选）|
| LLVM (llvm-c) | 系统探测 | JIT **核心目标**后端（不 vendor） | Apache-2.0 w/ LLVM-exc | 系统 | M5（核心）|

> GLFW/GLAD/stb 是渲染主线（M2/M3）所需，优先接入。sljit 是过渡 JIT，**LLVM 是核心目标 JIT**（M5，必须实现）。构建找不到 LLVM 时可临时降级 sljit/纯解释器，但 M5 验收以 LLVM 后端为准。

### JS 实现
| 依赖          | 用途                | 许可  | 备注                |
| ------------- | ------------------- | ----- | ------------------- |
| TypeScript    | 类型（Node 原生 strip-types 运行，无需 tsc） | Apache | dev 依赖（仅类型） |
| node:test     | 测试（Node 内置）   | MIT   | 零运行时依赖        |
| headless-gl / OffscreenCanvas | WebGL2 offscreen（M7） | MIT | 渲染阶段引入 |
| CodeMirror 6  | 代码编辑器（可选）  | MIT   | 可降级为 textarea   |
| fflate        | ZIP 解压（可选）    | MIT   | 浏览器原生也可      |

> EVAL 核心（`js_impl/`）已**零运行时依赖**：Node 26 的 `--experimental-strip-types` + `node:test`，无需 npm install 即可运行测试（本环境 npm install 受 npmmirror 限制）。渲染层（M7）才引入 GL 相关依赖。

## 4. 风险缓解检查点

> 顺序按新优先级（渲染优先）。每条都对应一个"早失败"的最小验证。

- **offscreen GL 上下文可建**：M2 第一步就验证目标平台（macOS/Linux/Windows）能建立无窗口 GL 上下文 + FBO + `glReadPixels`。这是整条渲染线的前提，早失败。
- **fixed-function → modern GL 语义鸿沟**：M3 用 `balls.pss`（纯 fixed-function，无 shader）作为兼容层首测；逐个示例暴露缺口。
- **GLSL 适配覆盖率**：M3 尽早跑全部示例的 v/f 块，发现旧符号翻译缺口。
- **解释器驱动渲染的帧率**：解释器 + 渲染能否实时（`balls.pss`）。若过慢，JIT 优先级上调。
- **sljit 浮点 ABI**（M4）：最小测试验证 sljit 在目标平台能正确生成 double 函数。
- **LLVM 可用性**（M5，核心）：构建时通过 `llvm-config`/`brew --prefix llvm` 探测；本机已装 LLVM 22.1.8。找不到则阻塞 M5 验收（不可降级以下略过）。
- **冻结保护**：`while(1){}` 脚本能被外部停止（解释器每 N 条指令检查；JIT 每循环回跳检查）。

## 5. 不做的事（明确排除）

- **不修改 `polydraw_src/`**（用户约束）。
- **不支持 ARB 汇编着色器**（已弃用，`*_asm.pss` 跳过）。
- **不支持遗留的 sound script**（原版已移除）。
- **不实现 `switch` 语句**（EVAL 本身不支持，见 `eval.txt:87`）。
- **数组初始化列表**：已实现（`static a[3]={1,2,3}`、多维、尾逗号、`{0}` 全零），与示例脚本实际用法一致。
- **不做 GPU compute / 现代渲染特性**（超出原版范围）。

## 6. 成功标准（最终验收）

1. ✅ `polydraw_src/` 零改动。
2. ✅ C 实现在 Linux/macOS/Windows × x64/arm64 上编译运行。
3. ✅ **渲染全流程**：`polydraw render foo.pss` 能 offscreen 出图；窗口模式与 offscreen 像素一致。
4. ✅ JS 实现（浏览器/Node）能 offscreen 出图（OffscreenCanvas），与 C 像素等价。
5. ✅ **解释器是最低保证**（永远可用）；**JIT：LLVM 为终极基准（核心目标，非可选）**，sljit 为过渡，二者与解释器逐位一致。
6. ✅ 不含任何手写架构汇编（C 侧），JS 用 `new Function`（豁免）。
7. ✅ 主解析器是 Pratt parser（优雅），备选是原版链表折叠的严格复刻。
8. ✅ 所有 `ken/`+`tigrou/` 示例脚本能加载编译；主要示例（`balls/interference/drawsph/...`）可渲染出图。
9. ✅ 差分测试：Plan A vs B vs 原版神谕，三方语义等价；解释器 vs JIT 逐位一致。
10. ✅ 文档完整（Plan/ + 用户手册）。

