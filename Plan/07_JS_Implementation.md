# 07 — JavaScript 实现

## 1. 定位

JS 实现独立于 C 实现，但**行为等价**（同一份 EVAL 语言契约 + 同一组宿主 API）。目标：在浏览器里运行 PolyDraw，加载 `ken/`/`tigrou/` 示例脚本并渲染。

## 2. JIT 路径（用户豁免）

> "jit 执行是核心... js 的实现除外。"

JS 的 JIT 路径是：**把 EVAL IR 翻译成 JavaScript 源码字符串，用 `new Function()` 编译**。V8 把它 JIT 成机器码。这是 JS 的天然 JIT，不涉及手写汇编，符合豁免。

### 2.1 为什么这是合法的"JIT"

- `new Function('x', 'return x*x')` 让 V8 把字符串编译成优化的机器码（TurboFan）。
- 生成的 JS 与手写 JS 性能相当。
- 它是 JS 引擎提供的标准 JIT 入口，不是"绕过汇编"——是站在更高层让引擎做 JIT。

### 2.2 翻译策略：IR → JS 字符串

遍历 `Instr[]`，每个 `LOCAL` 寄存器 → JS 局部变量；每条指令 → 一行 JS：

```js
// IR:  v3 = SIN v1
// → JS:
let v3 = Math.sin(v1);
```

控制流用 labeled `break`/`continue` + `do...while(false)` 模拟 `goto`（JS 无 goto）：

```js
// IR:  L1: ... GOTO L1
// → JS:
L1: do {
  ...
  continue L1;   // goto L1
} while(false);
```

更复杂的跳转用 `switch`-state-machine（每个 label 一个 case，循环分派），但能纯线性时优先 labeled loop。

### 2.3 用户函数

```js
// IR:  function f(a,b){...}
// → JS:
function _f(a, b) { ... return v_ret; }
// 调用点:  v = _f(x, y);
```

### 2.4 外部函数

```js
// 脚本里 glBegin(GL_QUADS) →
host.glBegin(host.GL_QUADS);
```

`host` 对象持有所有 `glXxx` 的 JS 实现，直接调用 WebGL2。

### 2.5 冻结保护

生成的 JS 在每个循环顶部插入：
```js
if (_quit) return NaN;
```
配合 `requestAnimationFrame`，每帧调用一次 EVAL 函数；若超时，下一帧前 `_quit` 已置位，函数退出。

### 2.6 数组边界检查

直接内联 JS：
```js
// arr size 是 2 的幂:
v_i = v_i & (size - 1);
// 否则:
v_i = (v_i < 0 || v_i >= size) ? 0 : v_i;
```
（用 `Float64Array` 作为 EVAL 数组存储，按 colmode 解释。）

### 2.7 字符串与 printf

`printf("%f",x)` → `host.printf("%f",x)` → `console.log` 或日志窗。

---

## 3. 解释器回退（JS）

当 `new Function` 失败（罕见，如脚本过大）或调试时，走 JS 解释器：直接遍历 `Instr[]` 的 switch，与 C 解释器结构相同（`04_JIT_Backend.md` §5）。两条路径共用同一份 IR。

---

## 4. 项目结构

```
js_impl/
├── package.json         # 零运行时依赖（dev 依赖: vite/typescript）
├── tsconfig.json
├── src/
│   ├── eval/
│   │   ├── lexer.ts
│   │   ├── parser-pratt.ts      # Plan A
│   │   ├── parser-fold.ts       # Plan B (复刻)
│   │   ├── ir.ts                # Instr 类型
│   │   ├── optimizer.ts
│   │   └── compile.ts           # 顶层: src → IR
│   ├── backend/
│   │   ├── jit-js.ts            # IR → JS 字符串 → new Function
│   │   └── interp.ts            # IR → 解释执行
│   ├── gpu/
│   │   ├── webgl2.ts            # WebGL2 封装
│   │   ├── fixedfunc.ts         # glBegin/glVertex 累积层
│   │   ├── glsl-adapter.ts      # 旧 GLSL → GLSL ES 3.0
│   │   └── textures.ts
│   ├── host/
│   │   ├── sections.ts          # @v/@g/@f/@h 分块
│   │   ├── externs.ts           # 外部函数表 (host 对象)
│   │   └── mainloop.ts
│   └── main.ts                  # 入口
├── public/
│   ├── index.html
│   └── samples/                 # 拷贝的示例 .pss (供下拉选择)
└── tests/
```

## 5. 语言：TypeScript

- 用 TS 提供类型安全（IR 类型、AST 节点）。
- 编译到 ES2020+（现代浏览器）。
- 单页应用，vite 打包。

## 6. UI

- 三栏布局：左代码（CodeMirror 6 with EVAL 语法高亮），右渲染 canvas，下日志。
- 顶部工具栏：示例下拉、运行/停止、JIT/解释切换（调试用）。
- 拖拽 `.pss` 文件加载。
- `localStorage` 存最近编辑内容。

## 7. 与 C 实现的一致性

- **共享同一份 IR 定义**（手动保持 TS interface 与 C struct 对齐）。
- **共享同一份外部函数表**（手动保持符号与签名一致）。
- 差分测试：同一脚本在 C 与 JS 实现里，EVAL 函数的纯数学部分输出逐位相同（图形部分允许驱动差异）。

## 8. 浏览器限制与对策

| 限制                         | 对策                                                    |
| ---------------------------- | ------------------------------------------------------- |
| 无文件系统自由访问           | `<input type=file>` / 拖拽 / 内置示例下拉                |
| 无法调 MIDI (`playnote`)     | 用 Web Audio API + SoundFont，或忽略（标记为可选）       |
| 无 `sleep()`                 | 用 `await new Promise(r=>setTimeout(r,ms))`（async 模式） |
| 同源策略加载纹理             | 用 `crossorigin` 或内嵌示例图为 data URI                |
| 单线程                       | EVAL 函数同步执行；冻结保护靠合作式 `_quit` 检查         |
| 大数组性能                   | `Float64Array`（typed array，连续内存）                 |

> `playnote`/`sleep` 等在浏览器里降级或忽略，不影响核心渲染功能。

## 9. 测试

- 用 vitest 跑 EVAL 编译器单元测试（lexer/parser/optimizer）。
- 浏览器端到端：每个示例脚本能加载、编译、渲染一帧不报错（用 headless gl 或 puppeteer + WebGL）。
- IR 差分：C 与 JS 实现对同一表达式产出等价 IR。
