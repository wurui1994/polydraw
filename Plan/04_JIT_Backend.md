# 04 — JIT 后端（sljit）与解释器回退

## 1. 核心约束（用户要求）

> "jit 执行是核心。我们允许解释执行作为回退，但不允许绕过跨平台 jit 的汇编，js 的实现除外。"

含义：
- **C 实现的 JIT 必须用跨平台 JIT 库（sljit），不得手写任何架构相关汇编。**
- **JS 实现豁免**：用 V8 的 `new Function()` 做 JIT，不视为"汇编"。
- 解释器作为 JIT 不可用 / 编译失败时的回退，**永远可用**。

---

## 2. sljit 选型理由（基于内在知识）

| 维度        | sljit                                                            | GNU lightning           | LuaJIT DynASM                | asmjit            |
| ----------- | ---------------------------------------------------------------- | ----------------------- | ---------------------------- | ----------------- |
| 语言        | **C**                                                            | C                       | C (+Lua 预处理器)            | C++               |
| 许可        | **BSD-2-Clause**（可静态/动态链接，无 copyleft）                 | LGPL（静态链接有顾虑）  | MIT                          | BSD               |
| 架构        | x86, x86-64, ARM32, ARM64, MIPS, PPC, RISC-V, s390x              | x86/x64/ARM/ARM64/PPC   | x86/x64/ARM/ARM64/MIPS       | x86/x64/ARM/ARM64 |
| 集成        | **单文件** `sljit_src/sljit_src.c` + `sljit_src/sljitLir.h`      | 库                      | 每个 .dasml 需 Lua 生成 .c   | 多头文件库        |
| 是否手写汇编| **否**（API 是 RISC 后端：`sljit_emit_op1/op2/emit_jump`）       | 否                      | **是**（每架构手写模板）    | 否                |
| 生产验证    | **PCRE2**（正则 JIT）、Ruby、PHP                                 | GNU Smalltalk           | LuaJIT                       | 较多              |
| 浮点支持    | `SLJIT_FR0..FR5`、`SLJIT_ADD_F64/MUL_F64/...`                    | 有                      | 有                           | 有                |
| W^X 处理    | 内部 `mmap`+`mprotect`/`VirtualProtect`                          | 内部                    | 内部                         | 内部              |

**结论**：sljit 是唯一同时满足"单文件 C、BSD、跨全架构、不手写汇编、生产级浮点支持"的选项。DynASM 因为"每架构手写模板"违反"不允许绕过汇编"；lightning 的 LGPL 在静态链接分发时有顾虑；asmjit 是 C++ 且体积大。

### 2.1 sljit 核心概念（速查）

```c
// 1. 创建编译器
sljit_compiler *C = sljit_create_compiler(NULL);

// 2. 声明栈帧（局部变量槽）
sljit_emit_enter(C, 0,             // args 进寄存器数
                    SLJIT_ARG_TYPE_F64, // 返回 double
                    SLJIT_FR0, SLJIT_FR0, // 保留/临时寄存器
                     numLocalFloats);

// 3. 发射指令
sljit_emit_op1(C, SLJIT_MOV_F64,
    SLJIT_MEM1(SLJIT_SP), slotOff,           // dst = [SP+off]
    SLJIT_MEM1(SLJIT_S0), argOff);           // src = arg
sljit_emit_op2(C, SLJIT_MUL_F64,
    SLJIT_FR0, SLJIT_FR0, SLJIT_FR1);

// 4. 条件跳转
struct sljit_jump *j = sljit_emit_cmp(C, SLJIT_EQUAL_F64, ...);
sljit_set_label(j, someLabel);

// 5. 调用 C 函数（外部 gl 函数 / 数学库）
sljit_emit_ijump(C, SLJIT_CALL1, SLJIT_IMM, (sljit_sw)&sin);
sljit_emit_return(C, SLJIT_MOV_F64, SLJIT_FR0, SLJIT_FR0);

// 6. 生成可执行代码
void *code = sljit_generate_code(C, 0, NULL);
// code 是可调用的函数指针
```

---

## 3. IR → sljit 翻译

遍历优化后的 `Instr[]`，逐条翻译。每个 `LOCAL` 寄存器映射到一个 `[SLJIT_SP + off]` 栈槽。

### 3.1 寄存器族映射

| IR `RegFamily` | sljit 表示                                         |
| -------------- | -------------------------------------------------- |
| `LOCAL`        | `SLJIT_MEM1(SLJIT_SP), off`（每个局部一个 double 槽） |
| `CONST`        | `sljit_emit_const` + `MOV_F64` 加载到 FR，或直接 `SLJIT_F64_IMM`（sljit 支持浮点立即数有限，大常量走数据区） |
| `PARAM`        | `SLJIT_MEM1(SLJIT_S0/S1)`（参数区）                |
| `GLOBAL`       | `SLJIT_MEM1(SLJIT_R0)`（指向 global static 基址）  |
| `EXT`          | 经函数指针 `SLJIT_IMM, (sljit_sw)&extFuncPtr` 调用 |
| `LABEL`        | `sljit_label*`                                     |

### 3.2 操作码翻译表

| `Op`         | sljit 发射                                                      |
| ------------ | --------------------------------------------------------------- |
| `MOV`        | `SLJIT_MOV_F64`                                                 |
| `PLUS/MINUS/TIMES/SLASH` | `SLJIT_ADD/SUB/MUL/DIV_F64`                         |
| `POW`        | `ijump CALL2 pow`                                               |
| `SIN/COS/...`| `ijump CALL1 sin`                                               |
| `LES/EQU/...`| `SLJIT_LESS_F64/SLJIT_EQUAL_F64`（结果 0/1 double）             |
| `LAND/LOR`   | 短路：`cmp + jump` 序列（产生 0.0/1.0 double）                  |
| `IF0 label`  | `sljit_emit_cmp(C, SLJIT_EQUAL_F64, FR0, FR0_zero) → set_label` |
| `GOTO label` | `sljit_emit_jump(C, SLJIT_JUMP) → set_label`                    |
| `USERFUNC f` | `ijump CALL f.codePtr`                                          |
| `PEEK arr[i]`| `MOV_F64` from `[base + idx*8]`（含边界检查，见 §6）            |
| `POKE`       | `MOV_F64` to `[base + idx*8]`                                   |

### 3.3 标签与跳转的两趟

sljit 是单趟流式发射，但允许"label 先用后定义"：`sljit_emit_jump` 返回 `jump*`，稍后 `sljit_set_label(jump, label)`。所以：
- 第一遍：遍历 IR，记录每个 label 锚点位置；跳转指令先创建 `jump*`，登记"待绑定"。
- 第二遍（sljit 生成后）：调用 `sljit_set_label` 绑定。

### 3.4 用户函数调用

每个 EVAL 用户函数编译成**独立的 sljit 代码块**，有自己的 `sljit_emit_enter`。`USERFUNC` 指令发射 `sljit_emit_ijump(SLJIT_CALL, IMM, funcEntry)`，参数按 §5 的调用约定放进参数槽。

---

## 4. 调用约定（EVAL 函数 ↔ 宿主）

### 4.1 入口签名

EVAL 主函数对外是一个 `double (*)(void)`（无参，读全局）或带参版本。sljit 生成的代码遵循 sljit 自定义 ABI（`sljit_emit_enter` 决定），宿主通过 sljit 的 `sljit_create_compiler` + 类型转换得到可调用指针。

> sljit 不生成遵循平台 ABI 的代码，而是用自己的寄存器约定；调用方也通过 sljit 的 wrapper（或我们写一个薄 thunk）进入。这对"宿主 C 代码调用 EVAL 函数"完全够用，因为调用点都经过我们的统一入口。

### 4.2 外部函数 (`EXT`) 调用

`glVertex(...)` 等外部函数是普通 C `__cdecl`/平台 ABI 函数。sljit 的 `sljit_emit_ijump(SLJIT_CALLN, IMM, &cfunc)` 能调用任意 C 函数指针，**sljit 内部处理 ABI 转换**（这正是 sljit 的核心价值：调用 C 函数时它生成正确的平台调用序列，我们不写汇编）。

- double 参数：放 `SLJIT_FR0..`
- double* / char* 参数：放 `SLJIT_R0..`
- 返回 double：`SLJIT_FR0`

---

## 5. 解释器回退（复刻 `kasm87c_run`）

当：
- sljit 不可用（编译时 `POLYDRAW_NO_JIT`），
- 或运行时 `sljit_generate_code` 失败，
- 或脚本被标记为"解释执行"（`--interp` 调试开关），

走解释器。它复刻 `eval.c:5579` 的 `kasm87c_run`：

```c
double interp_run(const CompiledProgram *prog, void *params) {
    Frame frame;  // 局部槽
    for (size_t i = 0; i < prog->nInstr; ) {
        const Instr *ins = &prog->instr[i];
        double *out = resolveReg(ins->out, &frame, prog);
        double a   = *resolveReg(ins->in[0], &frame, prog);
        double b   = ins->nIn>1 ? *resolveReg(ins->in[1], &frame, prog) : 0;
        switch (ins->op) {
            case MOV:    *out = a; break;
            case PLUS:   *out = a + b; break;
            case SIN:    *out = sin(a); break;
            case IF0:    if (b == 0) i = ins->out.aux; continue;  // 跳转
            case GOTO:   i = ins->out.aux; continue;
            case RETURN: return a;
            ...
        }
        i++;
    }
    return 0;
}
```

- 与 JIT 共享**同一份 `Instr[]`**（编译一次，双路径执行）。
- 冻结保护：每 4096 条指令检查 `g_shouldQuit`。
- 用户函数调用：递归 `interp_run`（新建 Frame）。

---

## 6. 数组边界检查（IR 层统一）

原版规则（`eval.txt`）：
- 数组大小是 2 的幂：`index &= size-1`（回绕）。
- 否则：`index = (index<0||index>=size) ? 0 : index`（强制 0）。

在 IR 生成阶段（不是解析阶段）为每个 `PEEK/POKE` 插入检查指令序列：

```
POKE arr[i] v   →
  mask = arr.size - 1   (编译期常量, 当 size 是 2 的幂)
  i' = i & mask         (size 是 2 的幂)
  // 或:
  i' = (i<0 || i>=size) ? 0 : i   (size 非 2 的幂)
  *(arrBase + i'*8) = v
```

JIT 后端把这段编译为 sljit 指令；解释器逐条执行。**两条路径行为完全一致**（差分测试项）。

---

## 7. 冻结保护（`kasm87jumpback` 等价物）

### 7.1 C/JIT 路径

在 sljit 代码里，每个**循环回跳**（`while`/`for`/`do-while`/`goto` 向后）的目标前插入：

```
  if (*g_shouldQuit) goto exitLabel;   // sljit: cmp + jump
  ...loop body...
```

`g_shouldQuit` 是一个 `volatile int*`，由另一个线程（UI 线程检测到卡顿/用户点"停止"）置位。代码在下一个回跳点检测后跳出，返回一个哨兵值。这取代了原版的"改写跳转偏移"hack（更干净，且 sljit 生成的可读代码不便运行时改写）。

### 7.2 C/解释器路径

主循环每 N 条指令检查 `g_shouldQuit`。

### 7.3 JS 路径

生成的 JS 代码在每个循环顶部插入 `if (globalQuit) return NaN;`。配合 `requestAnimationFrame`，浏览器里单帧跑超时的脚本下一帧检测到标志即退出。

---

## 8. 内存管理与生命周期

- `kasm87free(func)` 等价物：`sljit_free_compiler` + 释放我们分配的常量/字符串数据区。
- `kasm87freeall()`：释放所有缓存（原版用全局静态缓存以便复用；现代化版本可保留可选 cache，或每次重新编译——现代分配器足够快）。
- 用户函数代码块随主函数一起分配/释放。

---

## 9. 调试支持

- `kasm87_showdebug(showflags, ...)` 等价：
  - flag 1（pseudo-asm）：打印 IR（见 `02_IR` 的 `--dump-ir`）。
  - flag 2（机器码字节）：sljit 无直接 dump，但我们可调 sljit 的内部日志或自己记录发射的指令序列。
  - flag 4（Intel asm）：不直接支持；建议用外部反汇编器对生成代码反汇编（调试用，非默认）。

---

## 10. 性能目标

- 纯数学循环（如 `balls.pss` 的 N=16384 粒子更新）：JIT 路径应达到原版 x87 JIT 的 **50%+** 性能（sljit 略有开销但现代 CPU 补偿）。解释器路径约为 JIT 的 1/5~1/10。
- 编译时间：与原版同数量级（sljit 编译本身很快）。

---

## 11. 测试

- JIT 与解释器对同一 IR 的执行结果**逐位相同**（差分测试）。
- 冻结保护：`while(1){}` 脚本能在 100ms 内被外部停止。
- 调用约定：每个外部 `glXxx` 函数能正确收到 double/指针参数并返回。
- 跨架构：在 x64 与 ARM64（如 Apple Silicon）上行为一致。
