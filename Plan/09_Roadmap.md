# 09 — 路线图、里程碑与依赖清单

## 1. 里程碑总览

| 里程碑 | 内容                                                | 交付物                                    | 验收                                   |
| ------ | --------------------------------------------------- | ----------------------------------------- | -------------------------------------- |
| M1     | EVAL 核心（lexer+parser A+IR+optimizer+解释器）     | `c_impl/src/eval/` + 纯数学单元测试       | 1000 表达式差分通过；原版神谕比对通过  |
| M2     | JIT 后端（sljit）+ 冻结保护                          | `c_impl/src/jit/`                         | JIT vs 解释器逐位一致；冻结保护可中断  |
| M3     | 宿主框架（分块、外部函数、主循环、GLFW+ImGui）       | `c_impl/src/host/` + 可运行程序           | 加载 `.pss`、编译、空渲染循环          |
| M4     | GPU 完整（fixed-function 模拟、GLSL 适配、纹理）     | `c_impl/src/gpu/`                         | `balls/interference/drawsph` 可渲染    |
| M5     | 解析器 Plan B（复刻原版链表折叠）                    | `c_impl/src/eval/parser-fold.c`           | Plan A vs B 差分通过                    |
| M6     | JS 实现（与 C 行为等价）                             | `js_impl/`                                | 浏览器跑通示例脚本                      |
| M7     | 打磨（错误高亮、INI、性能基准、全部示例）            | 发布版                                    | 52 个示例脚本可加载，主要示例可渲染    |

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

### 阶段 2：JIT（M2）
10. **sljit 后端** (`jit/sljit-backend.c`)：IR → sljit，先支持纯表达式（无控制流）。
11. **控制流**：`IF0/IF1/GOTO` + label 两趟绑定。
12. **用户函数调用** (`USERFUNC`)。
13. **外部函数调用** (`EXT`，调 C 函数指针)。
14. **冻结保护**：`g_shouldQuit` 注入。
15. **数组边界检查** 的 JIT 发射。
16. **验收**：JIT vs 解释器逐位一致；冻结保护 100ms 内中断。

### 阶段 3：宿主框架（M3）
17. **GLFW 集成**：窗口、GL 3.3 context、输入。
18. **ImGui 集成**：代码编辑器、日志窗、布局。
19. **`.pss` 分块解析** (`sections.c`)：`@v/@g/@f/@h` + geom 声明行。
20. **外部函数表** (`externs.c`)：复刻 `polydraw.c:2070` 的 `myext[]`，C 函数桩先返回 0。
21. **主循环** (`mainloop.c`)：编译 → 调用 → swap；增量重编译。
22. **验收**：加载 `.pss`，host 块编译，调用桩函数，空渲染循环跑通。

### 阶段 4：GPU（M4）
23. **GLAD 加载** modern GL 函数。
24. **fixed-function 累积层** (`gpu/fixedfunc.c`)：`glBegin/glVertex/.../glPushMatrix/...`。
25. **矩阵栈** (`gpu/matrix.c`)：4x4 运算。
26. **内置 passthrough 着色器**。
27. **GLSL 适配方言** (`gpu/glsl-adapter.c`)：旧 GLSL → 3.3 Core。
28. **着色器 program 管理**：`glsetshader/glGetUniformLoc/glUniform*`。
29. **纹理** (`gpu/textures.c`)：`stb_image` + 各 colmode + capture。
30. **外部函数实现**：把 §21 的桩换成真实 GL 调用。
31. **验收**：`balls.pss`/`interference.pss`/`drawsph.pss` 可渲染。

### 阶段 5：解析器 Plan B（M5）
32. **链表折叠解析器** (`parser-fold.c`)：严格复刻 `eval.c:parsefunc` + 折叠循环。
33. **差分 harness 扩展**：Plan A vs B vs 神谕三方比对。
34. **验收**：fuzzing 1000 次，A 与 B 等价。

### 阶段 6：JS 实现（M6）
35. **TypeScript 项目骨架**：vite + vitest。
36. **移植 lexer/parser/IR/optimizer/解释器**（与 C 同构）。
37. **IR → JS 字符串 JIT** (`backend/jit-js.ts`)。
38. **WebGL2 + fixed-function 层** (`gpu/`)。
39. **GLSL 适配**（GLSL ES 3.0）。
40. **UI**（canvas + CodeMirror + 日志）。
41. **验收**：浏览器加载示例脚本并渲染。

### 阶段 7：打磨（M7）
42. **错误高亮**：编辑器根据 `origOffset/origLine` 标红。
43. **配置持久化**：`polydraw.ini` / `localStorage`。
44. **性能基准**：对照原版。
45. **全部示例**：52 个脚本逐个验证，记录已知不支持项。
46. **文档**：用户手册（更新版 `polydraw.txt`）。

## 3. 依赖清单（全部 vendored）

### C 实现
| 依赖          | 版本    | 用途                | 许可       | 体积      |
| ------------- | ------- | ------------------- | ---------- | --------- |
| sljit         | 最新 master | JIT 后端           | BSD-2      | 单文件 ~6k 行 |
| GLFW          | 3.4     | 窗口/GL 上下文/输入 | zlib       | 库         |
| GLAD          | 生成    | GL 函数加载         | MIT        | 生成文件  |
| Dear ImGui    | 1.90+   | 编辑器/GUI          | MIT        | 库         |
| stb_image     | 最新    | 纹理解码            | Public Dom | 单头      |
| miniz         | 最新    | ZIP 支持 (`mountzip`) | MIT       | 单文件    |

### JS 实现
| 依赖          | 用途                | 许可  | 备注                |
| ------------- | ------------------- | ----- | ------------------- |
| vite          | 构建/dev server     | MIT   | dev 依赖            |
| typescript    | 类型                | Apache| dev 依赖            |
| vitest        | 测试                | MIT   | dev 依赖            |
| CodeMirror 6  | 代码编辑器（可选）  | MIT   | 可降级为 textarea   |
| fflate        | ZIP 解压（可选）    | MIT   | 浏览器原生也可      |

> 运行时尽量零依赖（除 ImGui/CM6 这类 UI 库）。

## 4. 风险缓解检查点

- **sljit 浮点 ABI**：M1 阶段第 1 步先用最小测试验证 sljit 在目标平台能正确生成 double 函数，早失败。
- **GLSL 适配覆盖率**：M4 阶段尽早跑全部示例的 v/f 块，发现适配缺口。
- **fixed-function 语义**：M4 用 `balls.pss`（纯 fixed-function，无 shader）作为兼容层首测。
- **冻结保护**：M2 完成后立即测 `while(1){}`，避免后期集成难题。

## 5. 不做的事（明确排除）

- **不修改 `polydraw_src/`**（用户约束）。
- **不支持 ARB 汇编着色器**（已弃用，`*_asm.pss` 跳过）。
- **不支持遗留的 sound script**（原版已移除）。
- **不实现 `switch` 语句**（EVAL 本身不支持，见 `eval.txt:87`）。
- **不实现数组初始化列表** `static a[3]={1,2,3}`（EVAL 不支持；但示例里有 `static a[4]={0,0,0,0}` 形式——核查：`polydraw.txt` 说不支持，但 `balls.pss` 没用到，`drawsph.pss` 用 `static clut[NMAX]={0}`——这是**全 0 初始化**，原版 `parse_static` 支持单值初始化器扩到全数组。现代化版本支持这个特例）。
- **不做 GPU compute / 现代渲染特性**（超出原版范围）。

## 6. 成功标准（最终验收）

1. ✅ `polydraw_src/` 零改动。
2. ✅ C 实现在 Linux/macOS/Windows × x64/arm64 上编译运行。
3. ✅ JS 实现在浏览器加载并渲染示例脚本。
4. ✅ JIT 是默认执行路径（sljit），解释器作为可切换回退。
5. ✅ 不含任何手写架构汇编（C 侧），JS 用 `new Function`（豁免）。
6. ✅ 主解析器是 Pratt parser（优雅），备选是原版链表折叠的严格复刻。
7. ✅ 所有 `ken/`+`tigrou/` 示例脚本能加载编译；主要示例可渲染。
8. ✅ 差分测试：Plan A vs B vs 原版神谕，三方语义等价。
9. ✅ 文档完整（Plan/ + 用户手册）。
