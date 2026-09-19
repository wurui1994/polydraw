"""测试: PARAM_ID vs IDENTIFIER 在 LALR 中是否可行"""
from lark import Lark

grammar = r"""
start: stmt*

?stmt: expr_stmt | func_def | block | static_decl | enum_decl
     | if_stmt | while_stmt | do_while_stmt | for_stmt
     | return_stmt | break_stmt | continue_stmt | goto_stmt | label_stmt

expr_stmt: expr ";"

if_stmt: "if" "(" expr ")" stmt ("else" stmt)?
while_stmt: "while" "(" expr ")" stmt
do_while_stmt: "do" stmt "while" "(" expr ")" ";"
for_stmt: "for" "(" for_init? ";" expr? ";" expr? ")" stmt
for_init: expr | static_decl
return_stmt: "return" expr? ";"
break_stmt: "break" ";"
continue_stmt: "continue" ";"
goto_stmt: "goto" IDENTIFIER ";"
label_stmt: IDENTIFIER ":"
block: "{" stmt* "}"

enum_decl: "enum" "{" enum_entry ("," enum_entry)* ","? "}" ";"?
enum_entry: IDENTIFIER | IDENTIFIER "=" expr

static_decl: "static" static_var ("," static_var)* ";"
static_var: IDENTIFIER ("[" expr "]")* ("=" static_init)?
static_init: expr | "{" expr ("," expr)* ","? "}"

func_def: IDENTIFIER "(" func_params ")" func_body  -> named_func
        | IDENTIFIER "(" ")" func_body              -> named_func_empty
        | "(" func_params ")" func_body             -> anon_func
        | "(" ")" func_body                         -> anon_func_empty

func_body: block | expr

func_params: func_param ("," func_param)*

func_param: PARAM_ID               -> param_value
         | "&" PARAM_ID            -> param_ptr
         | "$" PARAM_ID            -> param_str
         | PARAM_ID "[" expr "]"   -> param_array
         | PARAM_ID "(" fptr_params? ")" -> param_func

fptr_params: fptr_param ("," fptr_param)*
fptr_param: AMP -> fptr_val | DOLLAR -> fptr_str | DOT -> fptr_varargs | LBRACK expr RBRACK -> fptr_array

?expr: assign_expr

assign_expr: or_expr
           | or_expr ("=" | "*=" | "/=" | "%=" | "+=" | "-=") assign_expr

?or_expr: and_expr | or_expr "||" and_expr
?and_expr: eq_expr | and_expr "&&" eq_expr
?eq_expr: cmp_expr | eq_expr ("==" | "!=") cmp_expr
?cmp_expr: add_expr | cmp_expr ("<=" | ">=" | "<" | ">") add_expr
?add_expr: mul_expr | add_expr ("+" | "-") mul_expr
?mul_expr: pow_expr | mul_expr ("*" | "/" | "%") pow_expr
?pow_expr: unary_expr | pow_expr "^" unary_expr
?unary_expr: postfix_expr | ("-" | "+" | "!") unary_expr

?postfix_expr: primary_expr
             | postfix_expr "[" expr "]"           -> array_access
             | postfix_expr "(" args? ")"          -> func_call
             | postfix_expr "++"                   -> post_inc
             | postfix_expr "--"                   -> post_dec

?primary_expr: NUMBER | FLOAT_NUM | STRING | IDENTIFIER
             | "++" IDENTIFIER                     -> pre_inc
             | "--" IDENTIFIER                     -> pre_dec
             | "(" expr ")"

args: expr ("," expr)*

AMP: "&"
DOLLAR: "$"
DOT: "."
LBRACK: "["
RBRACK: "]"

PARAM_ID: /[a-zA-Z_][a-zA-Z0-9_]*/
IDENTIFIER: /[a-zA-Z_][a-zA-Z0-9_]*/
NUMBER: /0[xX][0-9a-fA-F]+|\d+/
FLOAT_NUM: /\d+\.\d*|\.\d+/
STRING: /"([^"\\]|\\.)*"/

%ignore /\/\/[^\n]*/
%ignore /\/\*[\s\S]*?\*\//
%ignore /\s+/
"""

try:
    p = Lark(grammar, parser="lalr")
    print("LALR 编译OK")
    tree = p.parse("(x)x+1")
    print(tree.pretty()[:300])
except Exception as e:
    print(f"失败: {e}")
