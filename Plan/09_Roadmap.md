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
- **M5**（LLVM）：✅ **核心目标已完成**。系统 LLVM 22.1.8（`/opt/homebrew/opt/llvm`），`llvm-c` 头文件齐全。`c_impl/src/eval/pd_jit_llvm.c` 已实现：IR→LLVM IR（基本块 + `fadd/fmul/fdiv/fsub` + `call @sin/cos/...` + `br`/条件分支）、冻结保护（基本块入口插入 `shouldQuit` 检查，实测 `while(1){}` 在探针置位后 ~5ms 内返回）、运行时（`LLJIT` 适配 `pd_run_jit` 调用约定，与 sljit 共享 dispatch）。
  - **验证（test_jit，40 项全绿）**：① Part A 纯 EVAL 逐位差分（算术/控制流/函数/RNG/数组 + `fact`/`nrnd` 等）覆盖 LLVM 后端（dispatch 优先 LLVM）；② Part B 5 个目标 `.pss`（`drawsph/balls2k/metaballs/ballsk/disco ball`）GLCmd glbuf 逐命令 diff；③ **Part C 三方逐位一致（M5 验收）**：新增 `c_three_*` 7 项，强制 LLVM-only / sljit-only / 解释器 三路 `pd_run_jit` 并断言结果 `memcmp` 全字节相等，直接满足路线图 M5 验收「LLVM JIT vs 解释器 vs sljit 三方逐位一致」。
  - 修复：`disco ball` 的 flaky 测试根因是 test harness 的 `glcmd_copy` 未深拷贝 `SETTEXDATA` 像素（`pd_tex_free_all` 后悬垂指针，堆布局依赖导致 ~25% 偶发内容差异）→ 改为深拷贝 `SETTEXDATA`/`MULTMATRIX` 像素并对应释放；同时 `make_placeholder` 由 `rand()` 改为 (x,y) 纯函数，彻底消除不确定性。stress 200 次 0 失败。
- **M6**（解析器 Plan B）：✅ **`c_impl/src/eval/parser-fold.c` 已实现**——严格复刻 `eval.c:parsefunc` 的链表折叠（node 列表 + gop/gnext + 优先级 0..6 逐遍折叠，eval.c:4205），含两处一元符号规则（表达式开头的隐式 0 节点「-x^2」hack eval.c:1995；运算符后的 negit 翻转 eval.c:2019）。`p->useFold` 时所有表达式（含括号/调用实参/数组下标）走折叠解析器；`pd_compile_fold_host` 暴露 Plan B 入口。
  - **顺带修复 Plan A 一元符号语义缺陷**（此前 `3*-2^2`=-12 而原版=12、`-2+3`=-5 而原版=1 等）：`parse_expr_prec` 现按 fresh（minPrec==0，隐式 0 左操作数、符号作二元运算）/ 非 fresh（negit 即时取反操作数、`^` 绑定在取负之后）两条规则解析，与原版完全一致。
  - **差分验证（test_fold）**：① 57 条神谕实测表达式 A==B==原版；② ~80 条差分语料（含负号链/负指数/嵌套括号/比较/逻辑/数组/循环/递归/static）A==B；③ 2000 条随机 fuzz × 3 组随机变量赋值 = 6000 次求值 A==B。三方等价成立。
  - 全量测试 201 项全绿（新增 test_fold 3 项，含 57 条神谕用例）。
- **M8**（打磨）：✅ **完成**（窗口 GUI + 黑窗已修 + LLVM JIT 已接入生产渲染器 + INI 持久化 + 全 40 脚本位级一致）。仅剩编辑器侧错误高亮（需 ImGui，未 vendored）显式推迟。
  - **窗口模式 GUI**（`c_impl/src/view_main.c` + `build/polydraw-view`，GLFW 3.4/3.5 via brew）：复用与 offscreen 完全相同的 EVAL→GLCmd→`pd_gl_renderer_*` 管线。
    - **架构（本会话重做，真正快）**：**直接渲染进 GLFW 窗口自己的 GL 上下文**（`pd_gl_renderer_create_ex(...,own_offscreen=0)` + `set_render_to_default(1)`，即 `pd_gl_renderer_render` 画进默认帧缓冲 0），随后 `glfwSwapBuffers`。**没有任何每帧 GPU→CPU→GPU 像素往返**——这正是原版的做法，也是流畅的关键。早期的“CGL 离屏渲染 + 回读 + 全屏 quad 上传纹理”路径每帧一次 `glReadPixels`+`glTexSubImage2D` 往返，在 macOS 上慢到 <1fps，已废弃。`--once` 无窗口模式仍走 CGL 离屏 FBO 回读（与 `polydraw-render` 位级一致）。
    - **本会话修掉的真凶（黑窗 + 卡顿根因）**：
      1. **帧 0 初始化被跳过**：事件循环首帧因 `dt>0` 立即把 `frame` 推进到 1，`pdrl_run_frame(ctx,0)`（脚本里 `if(numframes==0)` 的粒子初始化 / 纹理上传）从未执行 → 场景画在垃圾状态 → 全黑。修复：启动前显式 `run(0)` **并 render** 一次。
      2. **纹理上传被 `glcmd_reset` 丢弃**：`pdrl_run_frame` 内部 `glcmd_reset` 会清空命令缓冲。仅 `run(0)` 记录 `SETTEXDATA` 而不紧接着 `render`，下一帧 `run(frame)` 就把上传命令冲掉 → 纹理从未进 GL → 黑（heightmap 这类 `glsettex(array)` 脚本尤其明显）。修复：启动块 `run(0)+render` 让上传在渲染时真正执行；之后每帧 `run(frame)+render`（纹理对象已常驻 GL）。
      3. **逐帧全量重放 O(N²)**：实时循环里每显示一帧都把 `0..frame` 全部 `run_frame` 重跑一遍，到帧 300 时每显示帧要做 300 次 EVAL，整体 O(N²) 退化到 ~1fps。修复：**增量播放**——`prog.globals` 跨 `run_frame` 调用持久（仅绘制缓冲被 `glcmd_reset`），故每显示帧只 `run` 新增的那一帧（`last_rendered+1..frame`）；仅在后退/重启时全量重放。另修正了步进循环“每显示帧最多进 1 帧”的 break 条件错误（原 `if(acc<SEC) break` 实际会一口气连进 ~15 帧）。
    - **验证**：GLFW-direct 窗口（pre-swap 回读）对 balls(65536)/heightmap(~15000)/disco(31000)/drawsph(65531)/ballsk(4794)/interference(65536) 均非黑；`polydraw-view --once 30` 对全部 40 个 `ken/`+`tigrou/` 脚本逐像素对比 `polydraw-render`，**全 40 个 ≤2/255 差异**（位级一致），满足成功标准 #3。
    - **实测帧率（GLFW-direct，640×480，LLVM JIT 默认开）**：interference **60fps**（vsync 封顶）/ heightmap **60fps** / drawsph≈53 / ballsk≈168（见基准）/ balls **~13fps**——最后者是该脚本每帧绘制数百个细分球体的真实 GL 光栅开销（与 JIT 无关；EVAL 部分已被 LLVM 提速 ~1.5×）。相较之前的 <1fps 已是数量级提升，窗口真正可用。
    - 交互：空格暂停/继续、R 重启、←/→ 单帧步进、Esc/Q 退出。
  - **全示例**：✅ 53/53 `ken/`+`tigrou/` 脚本 @128×128 帧30 均出非平凡 PNG（含纹理/shader/capture 类）。
  - **性能基准（已完成，详见 `Plan/10_Performance.md`）**：新增 `tools/bench`（纯 EVAL）与 `tools/framebench`（真实整帧 EVAL+GL 回读）两套基准，对比 interp / LLVM / sljit。关键结论：
    - LLVM JIT（核心目标）在**计算密集**例子稳定 **1.4–1.6×**，把 balls2k(37.7→59.1)/particules sparks(45→68.6)/ballsk(110→168)/heightmap(343→540) 拉过或接近 60fps；轻计算例子因 JIT 调用约定开销反略慢（0.3–1.0×，属固有特性，非 bug）。
    - **draw-call 密集例子（disco ball 14fps / snake tube 42 / drawsph 53）即使 LLVM 也远低于 60fps**：瓶颈在 GL 光栅与 draw call 提交（disco ball ≈19970 个 draw call，CPU 提交主导，EVAL 仅占 ~1.5/70ms），与 JIT 无关。→ 后续优化方向为**实例化渲染（instancing）**，属较大重构、正确性风险高，列为非阻塞已知优化项。
    - sljit（过渡后端）始终 ≈1.0×（调用委派解释器），符合“过渡”定位。
  - **LLVM JIT 已接入生产渲染器（本会话）**：`pdrl_run_frame_jit` 现被 `polydraw-render` 与 `polydraw-view` 真正调用——默认**自动优先 LLVM**（其次 sljit），提供 `--jit`/`--no-jit` 强制开关与 `pd_jit_backend_name()` 诊断输出。三方逐位一致（解释器/LLVM/sljit）不再只在 `test_jit` 内验证，而在**真实渲染管线**逐像素成立：40/40 脚本 `view --once --jit` vs `render`（解释器）差异 ≤2/255。性能基准结论（上）即运行时实际可得。
  - **配置持久化**：✅ `polydraw-view` 现已**写入** `polydraw.ini`（`[last] script` + `[window] w/h/fovy`，改写式保留其他键），无脚本参数时自动重开上次脚本。
  - **用户手册**：✅ `polydraw.txt` 已追加「C Implementation」章节，记录 `polydraw-render`/`polydraw-view` 用法、`--jit`/`--no-jit`、窗口控制键、INI 持久化。
  - **剩余（明确推迟，非阻塞）**：编辑器侧错误高亮（需 ImGui GUI 编辑器，未 vendored）——属 IDE 功能，不影响渲染/CLI 验收，列为后续可选项。


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

