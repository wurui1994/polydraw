# PolyDraw 现代化改造 — 主计划

> 作用域：本文档是总纲。各子系统的细节设计见 `Plan/` 目录下的独立文档。
> 约束：**原始代码 (`polydraw_src/`) 不允许任何改动**。所有新实现放在仓库根目录的新目录下。

---

## 1. 项目背景与目标

### 1.1 PolyDraw 是什么

PolyDraw 是 Ken Silverman & Tigrou 的 OpenGL/GLSL 脚本工具。一个 `.pss` 脚本由 3~4 个"块"组成：

| 块标记     | 含义                                | 由谁编译            |
| ---------- | ----------------------------------- | ------------------- |
| *(默认/host)* | 宿主脚本，类 C 语法，调用 OpenGL API | **EVAL 编译器**（本项目的核心） |
| `@v`       | 顶点着色器 (GLSL)                    | GPU 驱动            |
| `@g`       | 几何着色器 (GLSL，可选)              | GPU 驱动            |
| `@f`       | 片段着色器 (GLSL)                    | GPU 驱动            |

宿主脚本的"语言"是 Ken 的 **EVAL 语言**：一个类 C 的、大小写不敏感的、所有变量都是 `double` 的脚本语言，由 `eval.c` 中的 `kasm87()` 编译。本项目的现代化改造 **只针对 EVAL 宿主语言 + 整个宿主程序框架**；GLSL 块原样传给现代 GPU 驱动（见 §5）。

### 1.2 现状诊断（已对源码逐段核实）

对 `polydraw_src/eval.c` (8254 行) 的核查结论：

- 第 240 行：`#define COMPILE 0` —— **JIT 路径 (`COMPILE!=0`) 在当前源码里已经被禁用**。
- 第 6812、7322、7372 行的 `#if (COMPILE == 0)` / `#if (COMPILE != 0)` 显示：编译产物走的是 **`kasm87c_run()` 解释器**（第 5579 行的 switch 分派字节码）。
- 历史上 x87 JIT 代码存在，但 git 历史中 "remove asm / remove asm 2" 两次提交已把真正发射 x87 机器码的 `compcode[]` 写入路径剥离。当前 `compcode` 仅被 `kasm87_showdebug()` 用作调试打印缓冲。

**这意味着：原始仓库目前是"解释执行"的。本项目要求恢复并升级为"跨平台 JIT 为主、解释执行为回退"。**

### 1.3 目标

1. **双语言实现**：同时交付 (a) 一个 C/C++ 实现（桌面、跨平台），(b) 一个 JavaScript 实现（浏览器 / Node）。
2. **JIT 为核心**：
   - C 实现使用 **sljit** 作为跨平台 JIT 后端 —— **不写任何手写汇编**，由 sljit 抽象掉 x86/x64/ARM/ARM64 差异。
   - JS 实现把 EVAL IR 翻译成 JS 源码字符串，交给 V8 的 `new Function()` JIT —— 这是 JS 的天然 JIT 路径，明确豁免汇编（用户要求）。
3. **解释执行作为回退**：当 JIT 不可用（如 sljit 未初始化、运行在不支持 W^X 的环境、JIT 编译失败）时，自动降级为字节码解释器。两条路径共用同一份 IR。
4. **语法解析**：主方案采用**优雅的生成器 (generator) + Pratt 运算符优先**解析器（清晰、可读、易维护）；备选方案实现**与原版严格等效的递归下降/链表折叠解析器**，用于校验和行为兼容。
5. **GPU 抽象**：用 OpenGL 3.3 Core（C，桌面）与 WebGL2（JS，浏览器）替换已废弃的 fixed-function + ARB 扩展风格。
6. **零改动原代码**：`polydraw_src/` 保持只读。新代码在 `c_impl/`（C 实现）和 `js_impl/`（JS 实现）下。

---

## 2. 关键设计决策（含理由）

### 2.1 复用原版的 IR 作为编译流水线的脊柱

`eval.c` 最聪明的设计是 `gasm[]` —— 一组三元地址 IR 指令：

```c
typedef struct {
    long f;            // 操作码 (NUL/GOTO/RETURN/MOV/SIN/PLUS/...)
    long g;            // 附加信息
    long n;            // 输入数 (1 或 2)
    rtyp r[MAXPARMS];  // r[0]=输出, r[1..]=输入; rtyp.r 高4位=寄存器族(KECX/KEDX/KEIP...), 低28位=偏移
    long rxi;          // 额外寄存器索引
} gasmtyp;
```

解析器（`parsefunc`）产出 `gasm[]`，优化器（`kasmoptimizations`）改写 `gasm[]`，然后：
- 解释器 `kasm87c_run` 直接 switch 分派 `gasm[]`；
- JIT 后端遍历 `gasm[]` 发射机器码。

**这是完美的解耦点。我们的现代化版本沿用这个三段式流水线：**

```
源码字符串 ──lexer──► token流 ──parser──► gasm[] IR ──optimizer──► gasm[] (优化后)
                                                                        │
                                                          ┌─────────────┴──────────────┐
                                                          ▼                            ▼
                                                  IR→sljit 后端 (JIT)         IR→解释器 (回退)
                                                          │                            │
                                                          ▼                            ▼
                                               可执行机器码函数指针              switch 分派循环
```

见 `Plan/02_IR_and_Optimizer.md`。

### 2.2 解析器：优雅优先，等效兜底

原版 `parsefunc` 用了一种**独特的"链表折叠"算法**：把表达式拆成操作数与运算符的扁平链表 (`gop[]`/`gnext[]`)，再按优先级从高到低扫描折叠（`eval.c:4206`）。它不是教科书式的递归下降，但它是**有效且经过实战验证**的。

我们的策略（详见 `Plan/03_Parser.md`）：

- **主解析器 (Plan A)**：**Pratt parser**（运算符优先 + 左结合/右结合绑定权值），用 ES6 generator / C 函数指针表实现。代码量小、可读性高、易扩展运算符。这是"最优雅"的方案。
- **备选解析器 (Plan B)**：**严格复刻原版**的链表折叠算法，作为参考实现与 fuzzing 对照基准。两者输出同一份 `gasm[]` IR，保证行为一致。
- 语句级（`if/while/for/goto/return/static/enum`）用经典递归下降，两套解析器共享这部分。

### 2.3 JIT 后端：sljit 过渡 → LLVM 终极（C）+ V8（JS）

> **JIT 当前可选。** 先打通渲染全流程（M2/M3），解释器驱动渲染；渲染稳定后再做 JIT。

C 侧分两步：**sljit**（过渡，单文件、零依赖，验证 IR→机器码链路）→ **LLVM JIT**（终极基准，平台最优代码，经 `llvm-c`）。两者共用同一份 EVAL IR 与遍历骨架，差别只在"发射"层。编译开关 `POLYDRAW_JIT=llvm|sljit|off`。

| 关注点           | C：sljit（过渡）                          | C：LLVM JIT（终极）                | JS 实现：V8                                  |
| ---------------- | ------------------------------------------ | ---------------------------------- | -------------------------------------------- |
| 是否手写汇编     | 否（跨平台 RISC 后端指令）                 | 否（生成 LLVM IR，LLVM 后端发码）  | 不适用 —— 把 IR 翻译成 JS 字符串，V8 自身做 JIT |
| 支持架构         | x86/x64/ARM32/ARM64/MIPS/PPC/RISC-V        | LLVM 支持的全部（x86-64/ARM64…）   | V8 所运行的任何架构                           |
| 许可证           | BSD-2-Clause                               | Apache-2.0 w/ LLVM-exception       | (JS 侧无第三方库)                            |
| 集成方式         | vendored 单文件                            | 系统探测（不 vendor），找不到降级  | 原生 `new Function()`                         |
| 回退             | sljit 失败 → 解释器                        | LLVM 不可用 → sljit → 解释器       | `new Function` 抛错 → 走解释器（eval 沙箱）  |

详见 `Plan/04_JIT_Backend.md`。

### 2.4 冻结保护 / 跳回机制

原版 `kasm87jumpback()`（`eval.c:6287`）通过把循环回跳改写为 0 偏移，让失控的 EVAL 脚本在下一个回跳点跳出，从而避免 `TerminateThread`。在现代版本里：

- **C/JIT 路径**：在每次循环回跳前插入一条"检查 `g_should_quit` 标志"的 sljit 比较-跳转；宿主线程从另一个线程置位即可优雅退出。
- **C/解释器路径**：解释器主循环每 N 条指令检查一次标志。
- **JS 路径**：JS 单线程，依靠 `requestAnimationFrame` 帧间隔 + 在循环里插入合作式 `if (globalQuit) return` 检查（生成的 JS 代码里自动注入）。

### 2.5 GPU / 图形抽象（当前主线）

原版直接调 fixed-function OpenGL + ARB 扩展。现代化方案（见 `Plan/05_Graphics.md`）：

- **C 实现**：OpenGL 3.3 Core Profile（跨平台用 GLAD/GLFW 加载）。fixed-function 调用（`glBegin/glVertex/glPushMatrix/...`）用一个**保留的矩阵/顶点状态机**在宿主侧模拟，最终落到 `glDrawArrays`。GLSL 块加 `#version 330` 包装，`ftransform()`/`gl_FragColor` 等遗留符号做源到源的翻译。
- **JS 实现**：WebGL2（GLSL ES 3.0）。同样的宿主侧 fixed-function 模拟层用纯 JS 实现，EVAL 调 `glVertex` 时累积顶点，`glEnd` 时 flush。
- **Offscreen 渲染（验收主线）**：C 用 EGL/pbuffer 或隐藏窗口 + FBO，`glReadPixels` → PNG（`stb_image_write`），CLI `polydraw render foo.pss --frames/--single/--out`；JS 用 `OffscreenCanvas`。**渲染正确性以 offscreen 出图为首要判据**，不依赖人工盯窗口。

### 2.6 多平台宿主窗口

- **C 实现**：GLFW（窗口 + 上下文 + 输入）。文本编辑器用 Dear ImGui（跨平台）替代 Win32 Edit 控件。或者提供"无 GUI"模式：从命令行加载 `.pss` 并 headless 渲染。
- **JS 实现**：浏览器 canvas + `<textarea>` 代码编辑器（或 CodeMirror 轻量封装）。

---

## 3. 目录结构

```
polydraw/
├── polydraw_src/            # 【只读】原始 Ken Silverman 源码 (eval.c/kplib.c/polydraw.c)
├── ken/  tigrou/            # 【只读】示例 .pss 脚本
├── polydraw.txt  eval.txt   # 【只读】原始文档
│
├── Plan/                    # 本设计文档集（本次产出）
│   ├── Plan.md              # 主计划（本文件）
│   ├── 01_Lexer.md
│   ├── 02_IR_and_Optimizer.md
│   ├── 03_Parser.md
│   ├── 04_JIT_Backend.md
│   ├── 05_Graphics.md
│   ├── 06_Host_and_SectionParser.md
│   ├── 07_JS_Implementation.md
│   ├── 08_Testing_and_Compat.md
│   └── 09_Roadmap.md
│
├── c_impl/                  # C/C++ 现代化实现
│   ├── third_party/         # vendored: stb_image(_write), GLFW, miniz; sljit (可选)
│   ├── src/
│   │   ├── eval/            # EVAL 编译器：lexer, parser(plan A & B), ir, optimizer
│   │   ├── host/            # 宿主程序：.pss 分块、外部函数注册、主循环、offscreen
│   │   ├── gpu/             # OpenGL 3.3 Core 抽象 + offscreen(FBO) + fixed-function 模拟
│   │   ├── jit/             # JIT 后端：sljit (过渡) + llvm (终极) + 回退解释器
│   │   └── main.c           # 入口（含 `polydraw render` offscreen CLI）
│   └── tests/
│
└── js_impl/                 # JavaScript 实现（EVAL 核心已完成）
    ├── src/
    │   ├── eval/            # EVAL 编译器（TS）：lexer, parser, ir（已完成，91 单测）
    │   ├── backend/         # 解释器（已完成）+ IR→JS JIT（待）
    │   ├── gpu/             # WebGL2 抽象 + OffscreenCanvas（待）
    │   └── host/
    └── tests/
```

> 第三方依赖一律 **vendored**（拷进 `third_party/`），避免运行时联网拉取，并锁定版本。JS 侧尽量零依赖。

---

## 4. EVAL 语言要点（行为契约，所有实现必须遵守）

来自 `eval.txt` + `polydraw.txt`，并经示例脚本核对：

- 大小写**不敏感**（标识符在词法阶段统一转大写）。
- 所有变量/常量都是 `double`。`int(x)` 是向 0 取整；`floor`/`ceil` 向 ±∞。
- 运算符优先级（`eval.c:7352`，从高到低）：`^` > `* / %` > `+ -` > `< <= > >=` > `== !=` > `&&` > `||`。**`^` 是幂运算不是异或**。
- 数组边界检查：大小是 2 的幂则用掩码回绕；否则越界索引强制为 0。
- 语句：`if`/`else`/`while`/`do-while`/`for`/`goto`/`return`/`break`/`continue`/`enum`/`static`/标签。`switch` 不支持。
- 函数：主函数在最前且无名；末表达式是无 `;` 的返回值；多函数脚本每个函数体必须 `{}`。
- 参数类型：`a`=double, `&a`=double*, `$a`=char*, `a[n]`=数组, `a(,,)`=函数指针。
- 内建：`PI`、`RND`/`NRND`、`ABS/SIN/COS/.../FACT`、`ATAN2/MIN/MAX/POW/FMOD/LOG(1或2参)`。`ATN=ATAN`，`SQR=SQRT`。
- 宿主扩展（由 `polydraw.c` 经 `kasm87addext` 注入，约 160 项）：所有 `glXxx`、`xres/yres/mousx/mousy/bstatus`、`keystatus[256]`、`klock/numframes`、`rgb/rgba/noise/printf/printg/srand/sleep/playnote/mountzip` 等。
- 注释：`//` 到行尾，`/* */` 块。
- 字符串：仅字面量 `""`，不能存进变量；`\\"` 转义引号。

每条都要有对应的等价性测试（见 `Plan/08_Testing_and_Compat.md`）。

---

## 5. GLSL 块的处理（脚本里 `@v/@g/@f` 之后的文本）

这部分**不经 EVAL 编译器**。宿主的"分块解析器"（`txt2sec`，`polydraw.c:1741`）按行首 `@v`/`@g`/`@f`/`@h` 切片，把每块原样字符串交给 GPU 驱动编译。现代化版本沿用此设计：

- 宿主把 GLSL 文本 + 适配方言头（`#version 330 core` 或 `#version 300 es`）交给驱动。
- 遗留符号翻译表（`ftransform`→矩阵乘 `gl_Position`；`gl_FragColor`→`out vec4 fragColor`；`varying`→`in/out`；`texture2D/textureCube`→`texture()`）。
- ARB 汇编 (`!!...`) 块：在现代化版本里**降级为不支持并报错**（原版也只有少数 `_asm` 脚本用到，且现代驱动多已弃用）。ARB 脚本视为历史遗产。

---

## 6. 里程碑（高层；详细排期见 `Plan/09_Roadmap.md`）

> **优先级原则**：先打通 `.pss` 输入 → 渲染全流程（含 offscreen 出图），JIT 在渲染稳定后再做。解释器是最低保证（永远可用）。JIT 终极基准是 **LLVM**，sljit 为过渡。

1. **M1 — EVAL 核心** ✅：lexer + parser(Plan A) + IR + 解释器。能编译并执行纯数学表达式，48/48 示例脚本可编译。*(无 GPU)*
2. **M2 — 宿主框架**：`.pss` 分块、外部函数表（桩）、offscreen GL 上下文 + FBO、渲染主循环（`polydraw render`，解释器驱动）。*(无 JIT)*
3. **M3 — 渲染全流程** ⭐当前主线：fixed-function 模拟、GLSL 适配方言、纹理、capture-to-texture。`balls/interference/drawsph` 能 **offscreen 出图**（验收主线）。
4. **M4 — JIT 过渡（sljit）**：可选；解释器作回退；冻结保护。渲染稳定后再做。
5. **M5 — JIT 终极（LLVM）**：可选；平台最优代码，性能基准。
6. **M6 — 解析器 Plan B**：复刻原版链表折叠算法，作为对照基准与 fuzzing 甲虫。
7. **M7 — JS 渲染对齐**：WebGL2 + OffscreenCanvas，与 C 像素等价。（JS 的 EVAL 核心已在 M1 随 C 同步移植完成。）
8. **M8 — 打磨**：窗口 GUI（GLFW+ImGui）、错误位置高亮、INI 配置、性能基准对比原版。


---

## 7. 风险与对策

| 风险                                              | 对策                                                                             |
| ------------------------------------------------- | -------------------------------------------------------------------------------- |
| sljit 在目标平台不可用（如某些 sealed 环境）      | 解释器是同等公民，永远可用；启动时探测并自动降级                                  |
| fixed-function → modern GL 的语义鸿沟（光照/纹理坐标自动生成等） | 只支持脚本里**显式**用到的子集；逐个示例脚本验证；不模拟未使用的固定管线状态      |
| GLSL 老语法（`gl_FragColor` 等）在新驱动上失败     | 源到源翻译器，带详尽测试矩阵                                                      |
| EVAL 的"末表达式即返回值"等微妙规则               | 用原版 `eval.c` 编译同样输入作为 differential testing 的神谕                        |
| 浮点精度差异（x87 80bit vs SSE 64bit vs JS 64bit）| 文档明确统一为 IEEE-754 binary64；对精度敏感的内建（`FACT`/`NRND`）逐位对照实现    |
| 数组边界检查语义（2 的幂回绕 vs 强制 0）           | IR 层统一处理，JIT 与解释器共用同一段检查代码                                      |

---

## 8. 文档索引

| 文档                         | 内容                                                                 |
| ---------------------------- | -------------------------------------------------------------------- |
| `01_Lexer.md`                | 词法分析：大小写归一化、注释剥离、字符串字面量、`texttrans` 错误定位 |
| `02_IR_and_Optimizer.md`     | `gasm[]` IR 设计、寄存器族编码、优化 pass 清单                       |
| `03_Parser.md`               | Pratt (Plan A) + 链表折叠 (Plan B) 双解析器；语句级递归下降          |
| `04_JIT_Backend.md`          | **sljit 过渡 → LLVM 终极**、IR→机器码翻译、解释器回退、冻结保护、调用约定（JIT 可选，渲染优先） |
| `05_Graphics.md`             | OpenGL 3.3 Core / WebGL2、**offscreen 渲染**、fixed-function 模拟、GLSL 适配方言（当前主线） |
| `06_Host_and_SectionParser.md` | `.pss` 分块、外部函数注册、主循环、输入/计时                         |
| `07_JS_Implementation.md`    | JS/TS 实现的结构、IR→JS 字符串翻译、V8 JIT、模块边界                 |
| `08_Testing_and_Compat.md`   | 等价性测试矩阵、differential testing、fuzzing                        |
| `09_Roadmap.md`              | 详细排期、验收标准、依赖清单                                         |
