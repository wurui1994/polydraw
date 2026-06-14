# 01 — 词法分析器 (Lexer)

## 1. 目标

把 EVAL 源码字符串转换成 token 流，同时保留足够的源位置信息以支持精确的错误高亮（原版用 `texttrans[]` 位图记录被删除的空白）。

## 2. 原版实现的关键行为（`eval.c:6950` 的 `kasm87` 内联词法）

原版**没有独立的 token 类型**——词法与分析交织。它做的事：

1. **大小写归一化**：所有非字符串内的 `a-z` → `A-Z`（`eval.c:7062`）。
2. **空白压缩**：连续空白压成单个空格（保留一个，因为 `GOTO label` 需要分隔）。
3. **注释剥离**：`//` 到行尾；`/* */` 块（不支持嵌套）。
4. **`#opt(...)` 跳过**：预处理指令 `#opt(...)` 整段忽略（`eval.c:7054`）。
5. **字符串字面量**：`"..."` 整体保留大小写与空白；`\"` 转义引号；字符串内不做大小写归一。
6. **`texttrans` 位图**：为每个被保留的字符在 `texttrans[i>>5]` 中置位，用于把压缩后的错误位置 (`kasm87err0/err1`) 反映射回原始源码位置。

## 3. 现代化设计

### 3.1 显式 token 类型（改进点）

原版把数字、标识符、字符串都作为"字符"塞进压缩缓冲，解析时再扫描。现代化版本定义显式 token：

```
TokenKind = NUMBER | IDENT | STRING | PUNCT | KEYWORD | EOF
```

- `KEYWORD` 由 `IDENT` 在词法后或解析中查表升级（`IF/ELSE/WHILE/DO/FOR/GOTO/RETURN/BREAK/CONTINUE/ENUM/STATIC`）。注意 EVAL **大小写不敏感**，所以查表前先大写化。
- `PUNCT` 是多字符优先的最长匹配：`<= >= == != && || ++ -- += -= *= /= %= ^=`，其余单字符 `+ - * / % ^ ( ) [ ] { } ; , = < > & $ .`。
- `NUMBER` 解析 IEEE-754 双精度：`[0-9]+(.[0-9]*)?([eE][+-]?[0-9]+)?`，以及 `.5` 这种前导点形式（EVAL 允许）。
- `STRING` 内部字节原样保留（含大小写），记录在字符串表 `gstring[]`，token 携带索引。
- 标识符字符集：`[A-Za-z0-9_]`，首字符可为数字（EVAL 允许 `0xC8` 这种？—— 实际原版里 `0xc8` 是数字字面量十六进制；核查：`keystatus[0xc8]` 中的 `0xc8` 是**十六进制数字**，所以词法要把 `0x` 前缀解析为十六进制数字）。**待实现时核对 `isvarchar` (`eval.c:1060`)。**

### 3.2 错误位置映射

保留原版的 `texttrans` 思路但用更简洁的 `sourceMap: compactIndex[] → originalIndex[]` 数组：每个保留的 token 记录它在**原始**源码中的字节偏移。错误报告时直接用原始偏移，宿主据此高亮行号。

### 3.3 接口

```c
typedef struct {
    TokenKind kind;
    const char *text;   // 指向 (大写化后的) 符号文本；STRING 时为字面量字节
    size_t len;
    size_t origOffset;  // 原始源码偏移（错误定位用）
    size_t origLine;    // 原始行号 (1-based)
} Token;

typedef struct {
    Token *tokens;
    size_t count, cap;
    // 字符串表、源位置映射表
} TokenStream;

int lex_eval(const char *src, TokenStream *out, char *errbuf, size_t errlen);
```

JS 实现等价：`export function lex(src): { tokens: Token[], strings: string[], errors: LexError[] }`。

### 3.4 容错策略

- 词法错误（未闭合字符串、非法字符）**不立即终止**；记录错误 token 并继续，让解析器有机会报告更上下文化的语法错误。
- `#opt(...)` 嵌套括号需正确计数（原版只扫到第一个 `)`，对嵌套 `#opt(f(a))` 会出错——现代化版本修正为括号配对）。

## 4. 与原版的差异

| 方面            | 原版                            | 现代化                           |
| --------------- | ------------------------------- | -------------------------------- |
| token 表示      | 无（字符流）                    | 显式 `Token` 结构                |
| 大小写归一      | 内联 `ch -= 32`                 | 词法阶段统一                     |
| 十六进制数字    | 解析时识别                      | 词法识别 `0x...`                 |
| 错误位置        | `texttrans` 位图 + `err0/err1` | `origOffset`/`origLine` 直接记录 |
| 注释嵌套        | 不支持                          | 不支持（保持兼容）               |

## 5. 测试用例（差分测试）

- 对每个 `ken/`、`tigrou/` 示例脚本：词法化后，把 token 流序列化为规范字符串，与原版 `kasm87_showdebug` 输出的规范化源对照。
- 边界：`"\"quoted\""`、`/* // not a line comment */`、`x=0xc8;`、`.5e-3`、`a/*comment*/b`（应识别为两个标识符）。
