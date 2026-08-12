# 08 — 测试与兼容性策略

## 1. 测试金字塔

```
            ┌─────────────────────┐
            │ 端到端示例脚本 (E2E) │   ← 每个 .pss 能渲染
            ├─────────────────────┤
            │ 差分测试 (differential)│  ← Plan A vs B, JIT vs interp, C vs JS
            ├─────────────────────┤
            │ 行为契约测试          │   ← EVAL 语言规则逐条
            ├─────────────────────┤
            │ 单元测试              │   ← lexer/parser/IR/optimizer/各 pass
            └─────────────────────┘
```

## 2. 单元测试（每模块）

### 2.1 Lexer (`01_Lexer.md`)
- 大小写归一、注释剥离、字符串字面量、十六进制数字、`.5e-3`。
- 错误位置映射：构造已知偏移的错误，验证 `origOffset`/`origLine`。

### 2.2 Parser (`03_Parser.md`)
- 每个运算符优先级：`2+3*4`=14、`2^3^2`=64（左结合，经原版神谕验证）、`-2^2`=-4。
- 一元符号链：`--x`=x、`+-+x`=-x。
- 函数重载：`LOG(2)`、`LOG(2,10)`。
- 空参数：`f(,)=0`。
- 语句：`if/while/for/do-while/goto/return/break/continue/enum/static`。
- 错误消息：缺 `)`、`FOR needs 3 fields` 等。

### 2.3 IR & Optimizer (`02_IR_and_Optimizer.md`)
- 每个 pass 独立：去重常量、死常量消除、`NEGMOV`→`MOV`、死代码消除。
- `--no-opt` 模式：优化器关闭后 IR 与原版未优化输出一致。

### 2.4 JIT 后端 (`04_JIT_Backend.md`)
- sljit 代码生成：简单表达式 `x*y+sin(z)` 能正确执行。
- 调用约定：带参函数、调用外部 C 函数。
- 冻结保护：`while(1){}` 可被外部停止。

### 2.5 图形 (`05_Graphics.md`)
- fixed-function 累积：`glBegin(GL_QUADS); 4×glVertex; glEnd()` 生成 4 顶点的 VBO。
- GLSL 适配：一组旧 GLSL 片段翻译后能在 modern GL 编译。
- 纹理上传：各 colmode 的字节布局正确。

## 3. 差分测试（核心质量门）

### 3.1 Plan A vs Plan B（解析器）
- 随机生成 1000+ 表达式（覆盖所有运算符、嵌套、函数调用、数组）。
- 两个解析器产出 IR，规范化（消除临时槽编号差异）后比对。
- 失败 → 排查哪个解析器偏离原版语义。

### 3.2 JIT vs 解释器（C 实现）
- 同一 IR，分别用 JIT 与解释器执行。
- 结果**逐位相同**（`memcmp` double 字节）。
- 覆盖：所有操作码、循环、用户函数调用、数组边界检查。

### 3.3 C 实现的 JIT vs **原版 eval.c 神谕**
- 把原版 `eval.c` 编译成独立可执行（`EVALTEST` 模式，见 `eval.c:1` 的 makefile）。
- 对一组表达式，原版 dump 其 `gasm[]`（`kasm87_showdebug(1,...)`），与我们 IR 比对**语义**（操作码序列 + 操作数）。
- 这是最高权威：原版是行为定义。

### 3.4 C vs JS（跨实现）
- EVAL 纯数学函数（不含 `gl` 调用）：C 与 JS 实现输出逐位相同。
- 图形部分：允许驱动差异，但顶点数据（位置/颜色/纹理坐标）一致。

## 4. 行为契约测试（EVAL 语言规则）

逐条覆盖 `eval.txt` + `Plan.md §4`：

| 契约                              | 测试                                       |
| --------------------------------- | ------------------------------------------ |
| 大小写不敏感                      | `Sin(0)==SIN(0)==sin(0)`                   |
| `^` 是幂                           | `2^10==1024`、`(-2)^2==4`                   |
| `int()` 向 0，`floor` 向 -∞        | `int(-1.5)==-1`、`floor(-1.5)==-2`         |
| `SGN/UNIT`                        | `sgn(-5)==-1`、`unit(-5)==0`、`unit(0)==.5`|
| `FACT` (gamma)                    | `fact(5)==120`、`fact(0)==1`、`fact(0.5)==sqrt(pi)/2` |
| `RND/NRND` + srand 可复现         | `srand(42); r1=rnd; srand(42); r2=rnd; r1==r2` |
| 数组 2 的幂回绕                   | `static a[4]; a[5]==a[1]`                  |
| 数组非 2 幂 → 越界为 0            | `static a[3]; a[5]==a[0]`（索引 5 越界→0） |
| 末表达式即返回值                  | `(x){ x+1 }` 调用返回 x+1                  |
| `LOG` 1 vs 2 参                   | `log(e)==1`、`log(100,10)==2`              |
| `ATN=ATAN`、`SQR=SQRT`            | 别名等价                                   |
| `for` 循环语义                    | `for(i=0;i<5;i++) s+=i;` → s==10           |
| `goto` + 标签                     | 经典 goto 计数循环                         |
| 字符串字面量大小写保留            | `printf("%s","Hello")` 输出 `Hello`        |

## 5. 端到端测试（示例脚本）

`ken/`（23 个）+ `tigrou/`（29 个）= 52 个示例脚本。

### 5.1 加载测试（最低要求）
- 每个 `.pss` 能被分块解析（无 host 语法错误）。
- host 块能被 EVAL 编译器编译（无错误）。
- v/g/f 块能在 modern GL / WebGL2 编译（GLSL 适配方言后）。

### 5.2 渲染测试
- Headless 模式（C）渲染 N 帧，截图。
- 与原版 `polydraw.exe` 在 Wine/Windows 虚拟机里的截图比对（容许抗锯齿/驱动差异，比较主体几何与颜色）。
- 至少这些必须可渲染：`balls.pss`、`interference.pss`、`drawsph.pss`、`ceilflor2.pss`、`driftbox.pss`、`texture.pss`、`cubetex.pss`、`menger sponge.pss`、`metaballs.pss`。

### 5.3 已知不支持
- `*_asm.pss`（ARB 汇编）：跳过，提供非 asm 替代。
- 极少数依赖特定 GPU 扩展的脚本：标记为"best effort"。

## 6. Fuzzing

- 随机生成合法 EVAL 表达式（语法引导的 fuzzer）。
- Plan A vs Plan B vs 原版神谕 三方比对。
- 随机变异现有示例脚本的 host 块，确保编译器不崩溃（优雅报错）。
- 目标：上千次迭代无未定义行为、无内存错误（C 用 ASan/UBSan；JS 用覆盖率工具）。

## 7. 性能基准

- 一组微基准（纯数学循环）：
  - `balls.pss` 的粒子更新循环（N=16384）。
  - `drawsph.pss` 的几何生成。
- 测量：编译时间、单帧执行时间。
- 对照：原版 `eval.c` 解释器（COMPILE==0）、我们的解释器、我们的 JIT。
- 目标：JIT ≥ 原版解释器 3x；解释器与原版同数量级。

## 8. 兼容性矩阵

| 维度            | 目标                                                   |
| --------------- | ------------------------------------------------------ |
| C 编译器        | GCC 11+, Clang 14+, MSVC 2022                          |
| 架构            | x86-64, arm64（macOS/Linux）                           |
| OS              | Linux, macOS, Windows                                  |
| GL              | OpenGL 3.3 Core                                        |
| 浏览器 (JS)     | Chrome/Edge/Firefox 最新版（WebGL2）                   |
| Node (JS)       | 用于跑 EVAL 编译器单元测试（无 WebGL）                 |

## 9. 持续集成

- GitHub Actions：
  - Linux x64: build + test + ASan/UBSan。
  - macOS arm64: build + test。
  - Windows x64: build + test（MSVC）。
  - JS: lint + vitest + 构建。
- 神谕差分：缓存原版 `eval.c` 编译产物，作为 CI fixture。
