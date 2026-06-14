# 03 — 解析器 (Parser)

## 1. 设计原则

用户要求："语法解析使用最优雅的方案，同时严格和原始等效的递归下降作为备选实现"。

因此提供**两套解析器**，产出**同一份 IR**（见 `02_IR_and_Optimizer.md`），通过差分测试保证一致：

| 方案     | 算法                                   | 用途                       |
| -------- | -------------------------------------- | -------------------------- |
| **Plan A** | **Pratt parser**（运算符优先，绑定权值） | 主解析器；清晰、可维护、易扩展 |
| **Plan B** | **链表折叠**（严格复刻 `eval.c:parsefunc`） | 备选/参考实现；行为神谕     |

语句级（`if/while/for/...`）两套共享，用经典递归下降。

---

## 2. 运算符优先级表（两套解析器的共同真理，来自 `eval.c:7352`）

```
优先级(数字越小越高):
  0:  ^        (右结合)        // 幂，不是异或！
  1:  * / %    (左结合)
  2:  + -      (左结合)
  3:  < <= > >= (左结合)
  4:  == !=    (左结合)
  5:  &&       (左结合)
  6:  ||       (左结合)
```

一元前缀：`+ -`（在表达式开头或 `(` 后出现时为符号，`eval.c:1995` 的 `-x^2` 修正 hack）。
一元后缀：`++ --`（仅赋值上下文）。
赋值：`= *= /= %= += -=`（每条语句最多 1 个，左值必须是可写寄存器）。

---

## 3. Plan A — Pratt Parser（主方案）

### 3.1 为什么 Pratt 最优雅

- 用**绑定权值 (binding power)** 表表达优先级与结合性，**无需为每个优先级写一个递归函数**。
- 添加新运算符只改一张表，不改代码结构。
- 代码量极小（核心 `parseExpr(minBp)` ≈ 40 行）。
- 天然支持前缀/中缀/后缀混合。

### 3.2 核心循环（伪代码）

```
fn parseExpr(minBp):
    left = parsePrefix()              // 数字、标识符、(expr)、一元+/-
    loop:
        op = peek()
        if op is infix and infixBp(op) >= minBp:
            consume(op)
            if op is right-assoc (^, =):
                right = parseExpr(infixBp(op))     // 不含等号 → 右结合
            else:
                right = parseExpr(infixBp(op) + 1) // 左结合
            left = irEmit2(toOp(op), newLocal(), left, right)
        elif op is postfix (++/--):
            ...
        else:
            break
    return left
```

### 3.3 绑定权值表

```
infixBp:
  '='  : 10  (右)    '*=' '/=' '%=' '+=' '-=' : 10 (右)
  '||' : 20  (左)
  '&&' : 30  (左)
  '==' '!=' : 40 (左)
  '<' '<=' '>' '>=' : 50 (左)
  '+' '-' : 60 (左)
  '*' '/' '%' : 70 (左)
  '^'  : 80  (右)
postfixBp:
  '++' '--' : 90
prefixBp:
  '+' '-' : 100
```

（数字本身无意义，只保证相对顺序符合 §2 表；幂高于乘除、赋值最低。）

### 3.4 函数调用与参数解析

`ident(args)`：识别为函数调用。参数按 `,` 分割，每段递归 `parseExpr(0)`；空参数（`f(,)`）插入 `MOV 0` 占位（对应 `eval.c:2055` 的"blank param → 0"）。
**参数个数解析**决定函数重载（`LOG(1)` vs `LOG(2)`、用户函数同名不同参数数，`eval.c:2138`）。

### 3.5 数组下标

`ident[expr]`：下标表达式递归解析，生成 `PEEK`（读）或 `POKE`（写，在赋值左值上下文）。
边界检查在 IR 后端统一处理（不是解析器职责）。

### 3.6 取地址 `&ident`

`&a` → 函数指针参数语义（`eval.c` 的 `KPTR`）。Pratt 的 prefix 分支识别 `&`，返回 `Reg{fam=PARAM, aux=addrOf}`。

### 3.7 优雅性的具体体现

- 整个表达式解析器 < 150 行 C / < 120 行 TS。
- 添加新二元运算符：在表里加一行。
- 添加新内建函数：在符号表里加一行（不需要改解析器）。
- 与 IR 解耦：`irEmit*` 是唯一出口。

---

## 4. Plan B — 链表折叠（备选，复刻原版）

### 4.1 原版算法（`eval.c:parsefunc` + 折叠循环 `eval.c:4206`）

1. 扫描表达式，把操作数与运算符交替塞进扁平链表 `gop[]`，链表 `gnext[]`。
2. 递归处理子表达式：遇到 `(` 找配对 `)`，按 `,` 切分参数，对每段递归 `parsefunc`。
3. 折叠阶段：对优先级 0..6 逐级扫描，找到该优先级的运算符节点，发射 `Instr`，从链表摘除操作数节点（`gnext[z] = gnext[gnext[z]]`）。

### 4.2 复刻要点

- **必须保留** `eval.c:1995` 的"-x^2 修正"（在表达式开头插 `0` 使 `-` 变成二元减）。
- **必须保留** `eval.c:2019` 的连续 `+/-` 一元符号翻转 `negit ^= ...`。
- **必须保留** 空参数补 0、`LOG` 按参数个数变 `LOGB`、用户函数重载查找。
- 折叠用 §2 的优先级数字（与 `oprio[]` 一致）。

### 4.3 Plan B 的价值

- 行为神谕：与原版 `kasm87` 编译同样输入，比对 IR 序列。
- fuzzing 基准：随机生成表达式，Plan A 与 Plan B 必须产出语义等价的 IR。
- 历史保真：保留 Ken 的原始算法作为可读参考。

---

## 5. 语句级解析（两套共享）

经典递归下降，文法（简化）：

```
program     := { funcDef | globalDecl }
funcDef     := [type] ident '(' paramList ')' [block]
paramList   := [param {',' param}]
param       := ['&'|'$'] ident ['[' constExpr ']'] | ident '(' protoParams ')'
block       := '{' { statement } '}'
statement   := ifStmt | whileStmt | doWhileStmt | forStmt
             | 'goto' ident ';' | 'return' [expr] ';'
             | 'break' ';' | 'continue' ';'
             | 'enum' '{' ... '}' ';'
             | 'static' varDecls ';'
             | label ':' statement
             | exprStmt
ifStmt      := 'if' '(' expr ')' block ['else' block]
whileStmt   := 'while' '(' expr ')' block
forStmt     := 'for' '(' expr ';' expr ';' expr ')' block
exprStmt    := [expr] [';']            // 末表达式无 ; 即返回值
globalDecl  := 'static' varDecls ';' | 'enum' '{' ... '}' ';'
```

### 5.1 控制流的 IR 生成

- `if(cond){A}else{B}`：
  ```
  j_else = newLabel(); j_end = newLabel()
  cond_reg = parseExpr(...)
  emit IF0(j_else, cond_reg)        // cond==0 跳到 else
  parseBlock(A)
  emit GOTO(j_end)
  emitLabel(j_else)
  parseBlock(B)
  emitLabel(j_end)
  ```
- `for(init;cond;iter){body}`：与 `eval.c:2683` 的 3-label 模式一致（`fmode/fmode+1/fmode+2`）。
- `break`/`continue`：用 `breaklab`/`contlab` 参数透传（原版 `parsefunc` 的第 2、3 参数）。

### 5.2 `goto` 与标签

标签前向引用：`goto X;` 时若 `X` 未定义，登记为未解析；解析结束后扫描未解析标签报错（`eval.c:6756`）。标签可嵌套于表达式块中。

### 5.3 `enum`

`enum { A, B=5, C }`：每个名字登记为编译期常量（进 `enumval[]`/符号表）。`C` 隐式 = `B+1 = 6`。用于 `static arr[N]` 的维度（必须编译期常量）。

### 5.4 `static`

`static name[constExpr][="..."], name2["..."], ...;`：声明全局可写存储。初始化器 `= expr`（`eval.c:parse_static`, 1773）。数组维度从 `enum` 或字面量来。

---

## 6. 函数与作用域

- **作用域扁平**：EVAL 没有块作用域，所有变量是函数局部的（除非 `static`）。`for(int i...)` 中的 `i` 是函数级局部。
- **函数表** `newvar[]`（`eval.c:380`）：每个条目 `{r, maxind, parnum, proti, nami, hashn}`。`parnum<0` 表示变量/数组（取反为维度数）；`parnum>=0` 表示函数（参数数）。
- **哈希链** `newvarhash[256]`（`eval.c:379`）：按名字首字节哈希，支持函数重载查找（同名不同参数数，`eval.c:2145`）。

### 6.1 用户函数调用 (`USERFUNC`)

调用点发射 `USERFUNC` 指令，`aux` = 函数的 `newvar` 索引，输入为实参寄存器。
- 解释器：递归进入被调函数的 IR 段，新建一个栈帧（`gvlp += stackdoubs`, `eval.c:5594`）。
- JIT：sljit 后端把用户函数编译为独立的 sljit 代码块，`USERFUNC` 发射为 `CALL`。

---

## 7. 错误处理

- 解析错误带回溯的位置（token 的 `origOffset`/`origLine`）。
- 错误消息复刻原版前缀（`"ERROR: missing )"`、`"ERROR: FOR needs 3 fields"` 等）以便用户迁移。
- 最多报告 N 个错误后停止（避免雪崩）。

---

## 8. 测试

- **Plan A vs Plan B 差分**：1000 个随机表达式，IR 规范化后比对。
- **vs 原版神谕**：用原版 `eval.c`（独立编译 `evaltest`）对一组表达式编译，dump 其 `gasm[]`，与我们 IR 比对语义。
- 语句覆盖：每个示例 `.pss` 的 host 块都能无错解析。
