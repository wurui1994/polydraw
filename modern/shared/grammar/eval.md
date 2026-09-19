# PolyDraw Host Grammar Notes

This grammar describes the EVAL-compatible host language targeted by the new
implementations. It is intentionally practical rather than a formal parser
generator source.

```text
program      := function_or_statement*
function     := ident? "(" params? ")" block_or_statement_list
statement    := block
             | if_stmt
             | while_stmt
             | do_stmt
             | for_stmt
             | goto_stmt
             | return_stmt
             | break_stmt
             | continue_stmt
             | enum_decl
             | static_decl
             | label
             | expr_stmt

block        := "{" statement* "}"
if_stmt      := "if" "(" expr ")" statement ("else" statement)?
while_stmt   := "while" "(" expr ")" statement
do_stmt      := "do" statement "while" "(" expr ")" ";"?
for_stmt     := "for" "(" expr_list? ";" expr? ";" expr_list? ")" statement
goto_stmt    := "goto" ident ";"
return_stmt  := "return" expr? ";"
label        := ident ":"
enum_decl    := "enum" "{" enum_item ("," enum_item)* ","? "}" ";"?
static_decl  := "static" static_item ("," static_item)* ";"?

expr         := Pratt expression parser
```

Names are case-insensitive. All numeric values are `double`. Strings are direct
call arguments. `^` is power, not XOR.
