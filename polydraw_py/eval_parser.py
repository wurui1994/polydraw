"""EVAL语言解析器 - LALR, 统一语法

函数名可选: name(params) body 或 (params) body
函数体可选{}: (x)x+1 或 (x){return x+1;}

消除歧义: 用 PARAM_ID.2 优先级让 contextual lexer 在参数列表中优先匹配 PARAM_ID
命名函数定义: 在 expr_stmt 中用 func_params, 与 func_call 通过 contextual lexer 区分
"""
from lark import Lark, Tree, Token
from typing import Dict, Any

EVAL_GRAMMAR = r"""
    start: stmt*

    ?stmt: expr_stmt
         | if_stmt
         | while_stmt
         | do_while_stmt
         | for_stmt
         | return_stmt
         | break_stmt
         | continue_stmt
         | goto_stmt
         | label_stmt
         | block
         | enum_decl
         | static_decl
         | func_def

    expr_stmt: expr ";"?
             | postfix_expr "(" args ")" block   -> named_func
             | postfix_expr "(" ")" block         -> named_func_empty

    if_stmt: "if" "(" expr ")" stmt ("else" stmt)?

    while_stmt: "while" "(" expr ")" stmt

    do_while_stmt: "do" stmt "while" "(" expr ")" ";"

    for_stmt: "for" "(" for_init? ";" expr? ";" expr? ")" stmt

    for_init: expr | static_decl

    return_stmt: "return" expr? ";"

    break_stmt: "break" ";"

    continue_stmt: "continue" ";"

    goto_stmt: "goto" IDENTIFIER ";"

    label_stmt: IDENTIFIER ":" ";"?

    block: "{" stmt* "}"

    enum_decl: "enum" "{" enum_entry ("," enum_entry)* ","? "}" ";"?

    enum_entry: IDENTIFIER
             | IDENTIFIER "=" expr

    static_decl: "static" static_var ("," static_var)* ";"

    static_var: IDENTIFIER ("[" expr "]")* ("=" static_init)?

    static_init: assign_expr
              | "{" assign_expr ("," assign_expr)* ","? "}"

    // 匿名函数定义: (params) body
    func_def: "(" func_params ")" func_body  -> anon_func
            | "(" ")" func_body              -> anon_func_empty

    // 函数体: block 或 语句序列+可选末尾表达式
    func_body: block | func_stmts

    func_stmts: func_inner+

    func_inner: expr_stmt
              | if_stmt
              | while_stmt
              | do_while_stmt
              | for_stmt
              | return_stmt
              | break_stmt
              | continue_stmt
              | goto_stmt
              | label_stmt
              | enum_decl
              | static_decl

    // 函数参数: 用 PARAM_ID 避免和 primary_expr 中的 IDENTIFIER 冲突
    func_params: func_param ("," func_param)*

    func_param: PARAM_ID               -> param_value
             | "&" PARAM_ID            -> param_ptr
             | "$" PARAM_ID            -> param_str
             | PARAM_ID "[" expr "]"   -> param_array
             | PARAM_ID "(" fptr_params? ")" -> param_func

    fptr_params: fptr_param ("," fptr_param)*

    fptr_param: AMP   -> fptr_val
             | DOLLAR -> fptr_str
             | DOT    -> fptr_varargs
             | LBRACK expr RBRACK -> fptr_array

    // ===== 表达式 =====
    // 用 ! 前缀保留操作符token, 确保解释器能获取运算符
    ?expr: comma_expr

    comma_expr: assign_expr ("," assign_expr)*

    !assign_expr: or_expr
               | or_expr ("=" | "*=" | "/=" | "%=" | "+=" | "-=") assign_expr

    !or_expr: and_expr
            | or_expr "||" and_expr

    !and_expr: eq_expr
             | and_expr "&&" eq_expr

    !eq_expr: cmp_expr
            | eq_expr ("==" | "!=") cmp_expr

    !cmp_expr: add_expr
             | cmp_expr ("<=" | ">=" | "<" | ">") add_expr

    !add_expr: mul_expr
             | add_expr ("+" | "-") mul_expr

    !mul_expr: pow_expr
             | mul_expr ("*" | "/" | "%") pow_expr

    !pow_expr: unary_expr
             | pow_expr "^" unary_expr

    !unary_expr: postfix_expr
               | ("-" | "+" | "!" | "&") unary_expr

    !postfix_expr: primary_expr
                 | postfix_expr "[" expr "]"           -> array_access
                 | postfix_expr "(" args? ")"          -> func_call
                 | postfix_expr "." IDENTIFIER         -> member_access
                 | postfix_expr "++"                   -> post_inc
                 | postfix_expr "--"                   -> post_dec

    ?primary_expr: NUMBER
                 | FLOAT_NUM
                 | STRING
                 | IDENTIFIER
                 | "++" IDENTIFIER                     -> pre_inc
                 | "--" IDENTIFIER                     -> pre_dec
                 | "(" expr ")"

    args: assign_expr ("," assign_expr)*

    // ===== 终结符 =====
    AMP: "&"
    DOLLAR: "$"
    DOT: "."
    LBRACK: "["
    RBRACK: "]"

    // PARAM_ID 优先级高于 IDENTIFIER
    // contextual lexer 在 func_param 上下文优先匹配 PARAM_ID
    // 在 expr 上下文优先匹配 IDENTIFIER
    PARAM_ID.2: /[a-zA-Z_][a-zA-Z0-9_]*/
    IDENTIFIER: /[a-zA-Z_][a-zA-Z0-9_]*/
    FLOAT_NUM: /\d+\.\d*[eE][+-]?\d+|\.\d+[eE][+-]?\d+|\d+[eE][+-]?\d+|\d+\.\d*|\.\d+/
    NUMBER: /0[xX][0-9a-fA-F]+|\d+/
    STRING: /"([^"\\]|\\.)*"/

    %ignore /\/\/[^\n]*/
    %ignore /\/\*[\s\S]*?\*\//
    %ignore /\s+/
"""


def _transform(node) -> Dict[str, Any]:
    if isinstance(node, Tree):
        return {
            "type": node.data,
            "children": [_transform(c) for c in node.children],
        }
    elif isinstance(node, Token):
        return {"type": node.type, "value": str(node)}
    return str(node)


class EvalParser:
    _parser = None

    @classmethod
    def _get_parser(cls):
        if cls._parser is None:
            cls._parser = Lark(EVAL_GRAMMAR, parser="lalr", propagate_positions=True)
        return cls._parser

    @classmethod
    def parse(cls, code: str) -> Dict[str, Any]:
        tree = cls._get_parser().parse(code)
        return _transform(tree)

    @classmethod
    def parse_file(cls, filepath: str) -> Dict[str, Any]:
        with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
            code = f.read()
        return cls.parse(code)


if __name__ == '__main__':
    import sys, os, glob

    try:
        p = EvalParser._get_parser()
        print("语法编译OK")
    except Exception as e:
        print(f"语法编译失败: {e}")
        exit(1)

    # evaltest测试
    eval_tests = [
        ('(x)x+1', 'simple'),
        ('(x)(x*(9/5))+32', 'C2F'),
        ('(x,y)cos(max(x,y)*PI)^2+sin(max(y,x)*PI)^2', 'cos2+sin2'),
        ('()42', 'no params'),
        ('(x){return x*x;}', 'block body'),
        ('(x,y){z=x+y;return z;}', 'block multi stmt'),
        ('(cat,dog)cat*=cat;bozo=dog^2;sqrt(cat+bozo)', 'hypot'),
        ('(ang,&x,&y)ang*=PI/180;x=cos(ang);y=sin(ang);', 'ptr params'),
        ('(a[3])a[0]+=a[1];a[1]+=a[2];', 'array param'),
        ('(x,pifunc())pifunc(x)*10+pifunc(x+1)', 'func ptr'),
        ('(sc,buf[8])for(i=0;i<8;i++)buf[i]=i*i*sc;0', 'write buffer'),
        ('(dum) static buf[28]; srand(0); for(i=0;i<28;i++) buf[i] = i; 0', 'static buf'),
    ]

    print("\n=== evaltest ===")
    eok = 0
    for code, desc in eval_tests:
        try:
            EvalParser.parse(code)
            eok += 1
            print(f'OK: {desc}')
        except Exception as e:
            print(f'FAIL: {desc}: {str(e).split(chr(10))[0][:80]}')
    print(f'{eok}/{len(eval_tests)}')

    # PSS测试
    pss_tests = [
        ('1 + 2;', 'simple expr'),
        ('x = 1 + 2;', 'assignment'),
        ('printf("%g", x);', 'func call'),
        ('if (x > 0) { y = 1; } else { y = 0; }', 'if-else'),
        ('for (i = 0; i < 10; i++) { x += i; }', 'for loop'),
        ('() { return 42; }', 'anon func block'),
        ('(x, y) { return x + y; }', 'anon func params block'),
        ('static buf[256];', 'static array'),
        ('enum { A = 1, B = 2, C };', 'enum'),
        ('foo(y) { return y * 2; }', 'named func'),
        ('enum {N=16384,NS=3}; static px[N], py[N];', 'enum+static'),
        ('buf[y][x] = 0;', 'multi-dim array'),
        ('(x, pifunc()) { return pifunc(x); }', 'func ptr param'),
        ('(a[3]) { a[0] += a[1]; }', 'array param'),
        ('($st) { printf(st); }', 'string param'),
        ('cube(x) { return x*x*x; }', 'named func def'),
        ('getperpvec(nx, ny, nz, &ax, &ay, &az, &bx, &by, &bz) { ax = 0; }', 'named func ptr params'),
    ]

    print("\n=== PSS ===")
    pok = 0
    for code, desc in pss_tests:
        try:
            EvalParser.parse(code)
            pok += 1
            print(f'OK: {desc}')
        except Exception as e:
            print(f'FAIL: {desc}: {str(e).split(chr(10))[0][:80]}')
    print(f'{pok}/{len(pss_tests)}')

    # PSS文件测试
    pss_dir = os.path.join(os.path.dirname(__file__), '..')
    sys.path.insert(0, os.path.dirname(__file__))
    from pss_parser import parse_pss_file

    patterns = [
        os.path.join(pss_dir, 'ken', '*.pss'),
        os.path.join(pss_dir, 'tigrou', '*.pss'),
    ]

    total = 0
    fok = 0
    ferr = []
    for pat in patterns:
        for f in sorted(glob.glob(pat)):
            total += 1
            name = os.path.relpath(f, pss_dir)
            try:
                pss = parse_pss_file(f)
                host_code = pss.host_code
                if not host_code.strip():
                    fok += 1
                    continue
                EvalParser.parse(host_code)
                fok += 1
            except Exception as e:
                err = str(e).split('\n')[0][:80]
                ferr.append((name, err))
                print(f'FAIL: {name}: {err}')

    print(f'\n=== PSS文件: {fok}/{total} ===')
    if ferr:
        print('失败:')
        for n, e in ferr:
            print(f'  {n}: {e}')

    print(f'\n总结: evaltest {eok}/{len(eval_tests)}, PSS {pok}/{len(pss_tests)}, 文件 {fok}/{total}')
