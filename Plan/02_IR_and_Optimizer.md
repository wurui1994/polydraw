# 02 — 中间表示 (IR) 与优化器

## 1. 目标

定义编译器中段的 IR —— **沿用原版 `gasm[]` 三元地址设计**，因为它天然解耦前端（解析器）与后端（解释器/JIT）。优化器对 `gasm[]` 做等价改写。

## 2. 原版 IR 回顾（`eval.c:410-423`）

```c
typedef struct {
    long f;              // 操作码，见 enum (eval.c:242): NUL/GOTO/RETURN/RND/NRND/MOV/...
                         //   FABS/SGN/UNIT/FLOOR/CEIL/ROUND0/SIN/COS/TAN/... (1参)
                         //   TIMES/SLASH/PERC/PLUS/MINUS/LES/.../POW/MIN/MAX/FMOD/ATAN2/LOGB (2参)
                         //   POKE/POKETIMES/.../USERFUNC (内存写/用户函数调用)
    long g;              // 附加信息 (用户函数索引等)
    long n;              // 输入数: 0/1/2 (POKE 类为 2 但语义是写)
    rtyp r[MAXPARMS];    // r[0]=输出, r[1]=第1输入, r[2]=第2输入
    long rxi;            // >2 个输入时指向 rxi[] 扩展表
} gasmtyp;

typedef struct {
    long r;   // 高4位=寄存器族, 低28位=字节偏移
    long q;   // 附加 (数组下标偏移、用户函数编号)
    long nv;  // newvar 索引 (调试/外部函数解析)
} rtyp;
```

### 2.1 寄存器族（高 4 位）

| 族      | 值           | 含义                                   |
| ------- | ------------ | -------------------------------------- |
| `KECX`  | `0x10000000` | 局部变量 (临时槽，按 `globi*8` 偏移)   |
| `KEDX`  | `0x20000000` | 常量/字符串/数组数据区                  |
| `KESP`  | `0x40000000` | 函数参数 (栈传入)                       |
| `KEIP`  | `0x80000000` | 跳转目标 (label)                        |
| `KPTR`  | `0xa0000000` | 指向参数的指针 (函数指针参数)           |
| `KIMM`  | `0xb0000000` | 外部 (`evalextyp`) 符号 (gl 函数/常量)  |
| `KGLB`  | `0xe0000000` | 全局 static 变量                        |
| `KSTR`/`KARR` | `0xc0/0xd0` | 字符串表/数组表（最终折叠进 `KEDX`）    |

### 2.2 关键不变量

- 每条 `gasm[i]` 的输出 `r[0]` 是一个**新的**局部槽（SSA 风格的副作用，原版靠优化器合并）。
- 跳转类 (`GOTO`/`IF0`/`IF1`)：`r[1]` = label 编号（`KEIP`），`r[0]` = 目标 `gasm` 索引（解释器快捷路径，`COMPILE==0` 时由 `eval.c:6812` 填好）。
- 用户函数调用 (`USERFUNC`)：`g` = `newvar` 索引，参数放 `r[1..]` 与 `rxi[]`。

## 3. 现代化 IR（等价重表达）

为可读性与跨语言一致，C 与 JS 两份实现用**同一份 IR 定义**（C 是 struct，JS 是 TS interface）：

```c
typedef struct {
    Op op;            // 强类型 enum (与原版语义 1:1)
    int32_t aux;      // 原 g: 用户函数索引 / 数组维度信息
    uint8_t nIn;      // 输入数
    Reg out;          // r[0]
    Reg in[2];        // r[1], r[2]
    int32_t extraIdx; // rxi 索引 (nIn>2 时)
} Instr;
```

`Reg` 是带标签的寄存器引用：

```c
typedef struct {
    RegFamily fam : 4;   // LOCAL/CONST/PARAM/LABEL/PTR/EXT/GLOBAL/STR/ARR
    uint32_t   off : 28; // 字节偏移或索引
    int32_t    aux;      // 数组下标调整 / 函数指针偏移
} Reg;
```

> 用位域替代原版的"高 4 位 + 低 28 位"整数操作，语义不变但更安全（原版 `long` 在 64 位下高位截断曾是隐患）。

### 3.1 操作码全集（`Op`）

完整复刻 `eval.c:242` 的 `enum`，重命名为强类型：

```
控制流:  NOP GOTO RETURN IF0 IF1
1元数学: RND NRND MOV NEGMOV NEQU0
         FABS SGN UNIT FLOOR CEIL ROUND0 ROUND0_32
         SIN COS TAN ASIN ACOS ATAN SQRT EXP FACT LOG
2元数学: TIMES SLASH PERC PLUS MINUS
         LES LESEQ MOR MOREQ EQU NEQU LAND LOR
         POW MIN MAX FMOD ATAN2 LOGB
内存:    PEEK POKE POKETIMES POKESLASH POKEPERC POKEPLUS POKEMINUS
调用:    USERFUNC
```

`LOGB` 是 `LOG(2参)` 的特化（解析器根据参数个数选择，`eval.c:2121`）。

### 3.2 IR 构建辅助

- `irEmit1(op, out, in1)` / `irEmit2(op, out, in1, in2)` / `irEmitCall(funcIdx, out, ins[])`
- `newLocal()` 分配新局部槽（返回 `Reg{fam=LOCAL, off=...}`）
- `newConst(double v)` 把常量放入常量表，返回 `Reg{fam=CONST, off=...}`
- label：`newLabel()` 返回 `Reg{fam=LABEL}`；`emitLabel(l)` 在当前 IR 序列插入 NOP+label 锚点；`patchJump(instrIdx, targetInstrIdx)`。

## 4. 优化器（复刻 `kasmoptimizations`, `eval.c:4474`）

优化在 `gasm[]` 上做，**与原版逐 pass 对齐**，便于差分测试。原版的 pass（按出现顺序）：

| Pass | 作用 | 原版行号 |
| ---- | ---- | -------- |
| 去重常量 | `globval[j]==globval[i]` 则合并 | 4484 |
| 死常量消除 | 未被任何指令引用的常量删除 | 4507 |
| `NEGMOV`+常量 → `MOV`+负常量 | | 4543 |
| 死代码消除（写后未被读） | `anyreadsbeforewrites` | 4275/4323 |
| `MOV` 链合并 | `a=b; ... a` → 直接用 b | (各处) |
| 常量折叠 | 编译期算 `2+3` | (kasmoptimizations 内) |
| 公共子表达式 | 谨慎，原版注释说"failed miserably"故保守 | — |

**现代化版本按相同顺序实现这些 pass**，但：
- 用更清晰的数据流框架（每个 pass 是 `optimize(IR*)` 函数）。
- 提供 `--no-opt` 开关对应原版 `kasm87optimize=0`。
- **JIT 后端依赖优化后的 IR**：常量折叠与死代码消除能显著减少 sljit 发射的指令数。

### 4.1 寄存器分配（JIT 后端专用）

原版把每个 `globi` 映射到一个 `[esp+N]` 栈槽。sljit 后端沿用"**全部溢出到栈帧**"的简单策略（EVAL 脚本通常短，寄存器分配收益有限）：

- 每个 `LOCAL` 寄存器 = sljit 栈帧中的一个 `double` 槽。
- `CONST` 进只读数据区，按 `off` 索引。
- 高频临时（`r[0]` 刚写的值）尽量留在 `SLJIT_FR0..FR3`，下一指令消费即释放（简单线性扫描）。

## 5. 调试输出（`kasm87_showdebug` 等价物）

- `--dump-ir`：打印每条 `Instr` 为 `i: OP out = in1, in2`。
- `--dump-tokens`、`--dump-sljit`（C 侧，sljit 自带 `sljit_compiler_log`）。
- JS 侧 `--dump-js`：打印生成的 JS 源码字符串（最有用的调试视图）。

## 6. 测试

- 每个 pass 独立单元测试。
- 差分测试：对一组表达式，比对优化前后 IR 的**求值结果**（不是文本）一致。
- 性能：优化前后 sljit 生成代码的指令数与运行时间。
