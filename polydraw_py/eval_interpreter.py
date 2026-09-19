"""EVAL语言AST解释器 - 参照cust/interpreter.py模式

遍历eval_parser.py生成的AST(dict格式), 解释执行EVAL代码。
支持: 变量/数组、函数定义/调用、控制流、内置数学函数等。

GL绘图函数和常量由外部注入(通过 builtin_functions / builtin_vars),
不再内置stub实现, 便于对接不同渲染后端。
"""
import math
from typing import Dict, List, Any, Optional, Callable


# ===== 异常类 =====
class EvalException(Exception):
    pass

class ReturnException(EvalException):
    def __init__(self, value: Any = 0.0):
        self.value = value

class BreakException(EvalException):
    pass

class ContinueException(EvalException):
    pass

class GotoException(EvalException):
    def __init__(self, label: str):
        self.label = label


# ===== 环境管理 =====
class Environment:
    def __init__(self, parent: Optional["Environment"] = None):
        self.parent = parent
        self.variables: Dict[str, Any] = {}

    def define(self, name: str, value: Any) -> None:
        self.variables[name.lower()] = value

    def get(self, name: str) -> Any:
        key = name.lower()
        if key in self.variables:
            return self.variables[key]
        if self.parent:
            return self.parent.get(name)
        raise EvalException(f"未定义的变量: {name}")

    def set(self, name: str, value: Any) -> None:
        key = name.lower()
        if key in self.variables:
            self.variables[key] = value
            return
        if self.parent:
            self.parent.set(name, value)
            return
        raise EvalException(f"未定义的变量: {name}")

    def has(self, name: str) -> bool:
        key = name.lower()
        if key in self.variables:
            return True
        if self.parent:
            return self.parent.has(name)
        return False


# ===== 函数对象 =====
class EvalFunction:
    def __init__(self, name: str, params: list, body, interpreter, is_anon: bool = False):
        self.name = name
        self.params = params  # [(name, type), ...] type: 'value','ptr','str','array','func'
        self.body = body
        self.interpreter = interpreter
        self.is_anon = is_anon


# ===== AST工具 =====
def is_tree(node: Any) -> bool:
    return isinstance(node, dict) and "children" in node

def is_token(node: Any) -> bool:
    return isinstance(node, dict) and "value" in node and "children" not in node

def get_type(node: Any) -> str:
    if isinstance(node, dict):
        return node.get("type", "")
    return ""

def get_value(node: Any) -> str:
    if isinstance(node, dict):
        return node.get("value", "")
    return str(node)

def children(node: Any) -> list:
    if isinstance(node, dict):
        return node.get("children", [])
    return []

# 操作符值集合 (通过value判断, 不依赖Lark生成的type名)
_OP_VALUES = {
    "+", "-", "*", "/", "%", "^",
    "==", "!=", "<", ">", "<=", ">=",
    "&&", "||",
    "+=", "-=", "*=", "/=", "%=",
    "=",
}

def is_op_token(node: Any) -> bool:
    """判断是否是操作符token (如 +, -, *, == 等)"""
    return is_token(node) and get_value(node) in _OP_VALUES

def get_op_value(node: Any) -> str:
    """获取操作符的字符串表示"""
    return get_value(node)


# ===== 解释器核心 =====
class EvalInterpreter:
    def __init__(self, debug: bool = False):
        self.global_env = Environment()
        self.current_env = self.global_env
        self.functions: Dict[str, EvalFunction] = {}
        self.builtin_functions: Dict[str, Callable] = {}
        self.builtin_vars: Dict[str, Any] = {}
        self.enum_values: Dict[str, float] = {}
        self.labels: Dict[str, Any] = {}
        self.debug = debug
        self.loop_counter = 0
        self.MAX_LOOP_ITER = 100000

        self._init_builtins()

    def _init_builtins(self):
        """初始化基础内置函数和变量 (仅数学/系统, 不含GL)"""
        self.builtin_vars["pi"] = math.pi
        self.builtin_vars["e"] = math.e

        # 数学函数
        self.builtin_functions["abs"] = lambda x: abs(x)
        self.builtin_functions["acos"] = lambda x: math.acos(x)
        self.builtin_functions["asin"] = lambda x: math.asin(x)
        self.builtin_functions["atan"] = lambda x, *a: math.atan2(x, a[0]) if a else math.atan(x)
        self.builtin_functions["atn"] = self.builtin_functions["atan"]
        self.builtin_functions["atan2"] = lambda y, x: math.atan2(y, x)
        self.builtin_functions["ceil"] = lambda x: float(math.ceil(x))
        self.builtin_functions["cos"] = lambda x: math.cos(x)
        self.builtin_functions["exp"] = lambda x: math.exp(x)
        self.builtin_functions["fabs"] = lambda x: abs(x)
        self.builtin_functions["fact"] = lambda x: float(math.gamma(x + 1))
        self.builtin_functions["floor"] = lambda x: float(math.floor(x))
        self.builtin_functions["int"] = lambda x: float(int(x))
        self.builtin_functions["log"] = lambda x, *a: math.log(x, a[0]) if a else math.log(x)
        self.builtin_functions["sgn"] = lambda x: -1.0 if x < 0 else (1.0 if x > 0 else 0.0)
        self.builtin_functions["sin"] = lambda x: math.sin(x)
        self.builtin_functions["sqrt"] = lambda x: math.sqrt(x)
        self.builtin_functions["tan"] = lambda x: math.tan(x)
        self.builtin_functions["unit"] = lambda x: 0.0 if x < 0 else (1.0 if x > 0 else 0.5)
        self.builtin_functions["fmod"] = lambda x, y: math.fmod(x, y)
        self.builtin_functions["min"] = lambda x, y: min(x, y)
        self.builtin_functions["max"] = lambda x, y: max(x, y)
        self.builtin_functions["pow"] = lambda x, y: x ** y
        self.builtin_functions["rgb"] = lambda r, g, b: float((int(r) & 0xFF) << 16 | (int(g) & 0xFF) << 8 | (int(b) & 0xFF))
        self.builtin_functions["rgba"] = lambda r, g, b, a: float((int(a) & 0xFF) << 24 | (int(r) & 0xFF) << 16 | (int(g) & 0xFF) << 8 | (int(b) & 0xFF))
        self.builtin_functions["printf"] = lambda *a: self._builtin_printf(*a)

    def _builtin_printf(self, fmt, *args):
        if isinstance(fmt, str) and fmt.startswith('"') and fmt.endswith('"'):
            fmt = fmt[1:-1]
        fmt = fmt.replace("\\n", "\n").replace("\\t", "\t").replace("\\r", "\r")
        arg_idx = 0
        i = 0
        out = []
        while i < len(fmt):
            if i + 1 < len(fmt) and fmt[i] == '%':
                spec = fmt[i + 1]
                if spec in ('d', 'i'):
                    val = int(args[arg_idx]) if arg_idx < len(args) else 0
                    out.append(str(val))
                    arg_idx += 1
                    i += 2
                elif spec in ('f', 'e', 'E', 'g', 'G'):
                    val = args[arg_idx] if arg_idx < len(args) else 0.0
                    try:
                        out.append(f"%{spec}" % val)
                    except:
                        out.append(str(val))
                    arg_idx += 1
                    i += 2
                elif spec == 's':
                    val = args[arg_idx] if arg_idx < len(args) else ""
                    out.append(str(val))
                    arg_idx += 1
                    i += 2
                elif spec == '%':
                    out.append('%')
                    i += 2
                else:
                    out.append(fmt[i])
                    i += 1
            else:
                out.append(fmt[i])
                i += 1
        print("".join(out), end="")
        return 0.0

    # ===== 主入口 =====
    def interpret(self, ast: Dict[str, Any]) -> Any:
        self._first_pass(ast)
        return self._exec_stmts_from_node(ast)

    def _first_pass(self, ast: Dict[str, Any]):
        """第一遍: 收集函数、static、enum、label"""
        self._scan_node(ast)

    def _scan_node(self, node: Any):
        """递归扫描AST, 收集定义"""
        if not is_tree(node):
            return
        ntype = get_type(node)
        ch = children(node)

        if ntype in ("named_func", "named_func_empty"):
            self._register_named_func(node)
        elif ntype == "enum_decl":
            self._register_enum(node)
        elif ntype == "static_decl":
            self._register_static(node)
        elif ntype == "label_stmt":
            name = get_value(ch[0])
            self.labels[name.lower()] = node

        for c in ch:
            self._scan_node(c)

    def _register_named_func(self, node: Dict[str, Any]):
        ch = children(node)
        name = self._extract_ident_name(ch[0])
        params = []
        body = None
        for c in ch[1:]:
            ntype = get_type(c)
            if ntype == "args":
                for ac in children(c):
                    pname = self._extract_ident_name(ac)
                    if pname:
                        params.append((pname, "value"))
            elif ntype == "block":
                body = c
        if body is None:
            for c in ch:
                if get_type(c) == "block":
                    body = c
        if body is None:
            raise EvalException(f"函数 {name} 没有函数体")
        self.functions[name.lower()] = EvalFunction(name, params, body, self)

    def _register_enum(self, node: Dict[str, Any]):
        ch = children(node)
        next_val = 0.0
        for c in ch:
            if get_type(c) != "enum_entry":
                continue
            ech = children(c)
            name = get_value(ech[0])

            if len(ech) > 1:
                val_node = ech[1]
                ntype = get_type(val_node)
                if ntype == "comma_expr":
                    cch = children(val_node)
                    if cch:
                        next_val = self.evaluate_expression(cch[0])
                    self.enum_values[name.lower()] = next_val
                    self.builtin_vars[name.lower()] = next_val
                    next_val += 1.0
                    for cc in cch[1:]:
                        ename = self._extract_ident_name(cc)
                        if ename:
                            self.enum_values[ename.lower()] = next_val
                            self.builtin_vars[ename.lower()] = next_val
                            next_val += 1.0
                        else:
                            cch2 = children(cc)
                            if cch2:
                                ename = self._extract_ident_name(cch2[0])
                                if ename and len(cch2) >= 3:
                                    eval_val = self.evaluate_expression(cch2[2])
                                    self.enum_values[ename.lower()] = eval_val
                                    self.builtin_vars[ename.lower()] = eval_val
                                    next_val = eval_val + 1.0
                    return
                else:
                    next_val = self.evaluate_expression(val_node)

            self.enum_values[name.lower()] = next_val
            self.builtin_vars[name.lower()] = next_val
            next_val += 1.0

    def _register_static(self, node: Dict[str, Any]):
        ch = children(node)
        for c in ch:
            if get_type(c) == "static_var":
                self._init_static_var(c)

    def _init_static_var(self, node: Dict[str, Any]):
        ch = children(node)
        name = get_value(ch[0])

        dims = []
        init_node = None
        for c in ch[1:]:
            ntype = get_type(c)
            if ntype == "static_init":
                init_node = c
            elif is_tree(c):
                dims.append(int(self.evaluate_expression(c)))

        if dims:
            total_size = 1
            for d in dims:
                total_size *= d
            arr = [0.0] * total_size
            if init_node:
                ich = children(init_node)
                for j, ic in enumerate(ich):
                    if j < len(arr):
                        arr[j] = self.evaluate_expression(ic)
            self.global_env.define(name, arr)
        else:
            val = 0.0
            if init_node:
                ich = children(init_node)
                if ich:
                    val = self.evaluate_expression(ich[0])
            self.global_env.define(name, val)

    # ===== 语句执行 =====
    def _exec_stmts_from_node(self, node: Any) -> Any:
        result = 0.0
        ch = children(node)
        i = 0
        while i < len(ch):
            c = ch[i]
            if is_tree(c):
                ntype = get_type(c)
                if ntype in ("named_func", "named_func_empty"):
                    i += 1
                    continue
                if ntype in ("anon_func", "anon_func_empty"):
                    # () { ... } 是主函数入口, 立即执行其body
                    body = self._find_func_body(c)
                    if body is not None:
                        try:
                            result = self._exec_stmts_from_node(body)
                        except GotoException as g:
                            target = self.labels.get(g.label.lower())
                            if target is None:
                                raise EvalException(f"未定义的label: {g.label}")
                            i = 0
                            while i < len(ch):
                                if ch[i] is target:
                                    break
                                i += 1
                            continue
                    i += 1
                    continue
                try:
                    result = self.execute_statement(c)
                except GotoException as g:
                    target = self.labels.get(g.label.lower())
                    if target is None:
                        raise EvalException(f"未定义的label: {g.label}")
                    i = 0
                    while i < len(ch):
                        if ch[i] is target:
                            break
                        i += 1
                    continue
            i += 1
        return result

    def execute_statement(self, stmt: Any) -> Any:
        if not is_tree(stmt):
            return 0.0
        ntype = get_type(stmt)
        handlers = {
            "expr_stmt": self._exec_expr_stmt,
            "if_stmt": self._exec_if,
            "while_stmt": self._exec_while,
            "do_while_stmt": self._exec_do_while,
            "for_stmt": self._exec_for,
            "return_stmt": self._exec_return,
            "break_stmt": self._exec_break,
            "continue_stmt": self._exec_continue,
            "goto_stmt": self._exec_goto,
            "label_stmt": lambda s: 0.0,
            "block": self._exec_block,
            "enum_decl": lambda s: 0.0,
            "static_decl": lambda s: 0.0,
            "named_func": lambda s: 0.0,
            "named_func_empty": lambda s: 0.0,
            "anon_func": lambda s: 0.0,
            "anon_func_empty": lambda s: 0.0,
        }
        handler = handlers.get(ntype)
        if handler:
            return handler(stmt)
        return self.evaluate_expression(stmt)

    def _exec_expr_stmt(self, stmt: Any) -> Any:
        ch = children(stmt)
        if ch:
            return self.evaluate_expression(ch[0])
        return 0.0

    def _exec_if(self, stmt: Any) -> Any:
        ch = children(stmt)
        if len(ch) < 2:
            return 0.0
        cond = self.evaluate_expression(ch[0])
        if cond:
            return self.execute_statement(ch[1])
        elif len(ch) >= 3:
            return self.execute_statement(ch[2])
        return 0.0

    def _exec_while(self, stmt: Any) -> Any:
        ch = children(stmt)
        if len(ch) < 2:
            return 0.0
        result = 0.0
        self.loop_counter = 0
        while self.evaluate_expression(ch[0]):
            self.loop_counter += 1
            if self.loop_counter > self.MAX_LOOP_ITER:
                raise EvalException(f"循环超过最大迭代次数 ({self.MAX_LOOP_ITER})")
            try:
                result = self.execute_statement(ch[1])
            except BreakException:
                break
            except ContinueException:
                continue
        return result

    def _exec_do_while(self, stmt: Any) -> Any:
        ch = children(stmt)
        if len(ch) < 2:
            return 0.0
        result = 0.0
        self.loop_counter = 0
        while True:
            self.loop_counter += 1
            if self.loop_counter > self.MAX_LOOP_ITER:
                raise EvalException(f"循环超过最大迭代次数 ({self.MAX_LOOP_ITER})")
            try:
                result = self.execute_statement(ch[0])
            except BreakException:
                break
            except ContinueException:
                pass
            if not self.evaluate_expression(ch[1]):
                break
        return result

    def _exec_for(self, stmt: Any) -> Any:
        ch = children(stmt)
        init_node = ch[0] if len(ch) > 0 else None
        cond_node = ch[1] if len(ch) > 1 else None
        update_node = ch[2] if len(ch) > 2 else None
        body_node = ch[3] if len(ch) > 3 else None

        # EVAL/C语言中 for 不创建新作用域, 只有函数才创建
        self.loop_counter = 0
        if init_node:
            ntype = get_type(init_node)
            if ntype == "for_init":
                fic = children(init_node)
                if fic:
                    self.execute_statement(fic[0])
            else:
                self.execute_statement(init_node)
        result = 0.0
        while True:
            self.loop_counter += 1
            if self.loop_counter > self.MAX_LOOP_ITER:
                raise EvalException(f"循环超过最大迭代次数 ({self.MAX_LOOP_ITER})")
            if cond_node and not self.evaluate_expression(cond_node):
                break
            try:
                if body_node:
                    result = self.execute_statement(body_node)
            except BreakException:
                break
            except ContinueException:
                pass
            if update_node:
                self.evaluate_expression(update_node)
        return result

    def _exec_return(self, stmt: Any) -> Any:
        ch = children(stmt)
        value = 0.0
        if ch:
            value = self.evaluate_expression(ch[0])
        raise ReturnException(value)

    def _exec_break(self, stmt: Any) -> Any:
        raise BreakException()

    def _exec_continue(self, stmt: Any) -> Any:
        raise ContinueException()

    def _exec_goto(self, stmt: Any) -> Any:
        ch = children(stmt)
        label = get_value(ch[0])
        raise GotoException(label)

    def _exec_block(self, stmt: Any) -> Any:
        # EVAL/C语言中 {} 不创建新作用域, 只有函数才创建
        # 直接在当前环境中执行block内的语句
        result = 0.0
        for c in children(stmt):
            result = self.execute_statement(c)
        return result

    # ===== 表达式求值 =====
    def evaluate_expression(self, expr: Any) -> Any:
        if expr is None:
            return 0.0
        if not isinstance(expr, dict):
            return 0.0

        ntype = get_type(expr)
        ch = children(expr)

        # --- Token ---
        if ntype == "NUMBER":
            val = get_value(expr)
            try:
                if val.startswith("0x") or val.startswith("0X"):
                    return float(int(val, 16))
                return float(val)
            except:
                return 0.0

        if ntype == "FLOAT_NUM":
            try:
                return float(get_value(expr))
            except:
                return 0.0

        if ntype == "STRING":
            s = get_value(expr)
            if s.startswith('"') and s.endswith('"'):
                s = s[1:-1]
            return s

        if ntype in ("IDENTIFIER", "PARAM_ID"):
            return self._get_variable(get_value(expr))

        if is_op_token(expr):
            return get_op_value(expr)

        if not is_tree(expr):
            return 0.0

        # --- 逗号表达式 ---
        if ntype == "comma_expr":
            result = 0.0
            for c in ch:
                result = self.evaluate_expression(c)
            return result

        # --- 赋值表达式 ---
        if ntype == "assign_expr":
            return self._eval_assign(expr)

        # --- 二元运算 ---
        if ntype in ("or_expr", "and_expr", "eq_expr", "cmp_expr",
                      "add_expr", "mul_expr", "pow_expr"):
            return self._eval_binary_chain(expr)

        # --- 一元运算 ---
        if ntype == "unary_expr":
            return self._eval_unary(expr)

        # --- 后缀表达式 ---
        if ntype == "postfix_expr":
            if ch:
                return self.evaluate_expression(ch[0])
            return 0.0

        # --- 数组访问 ---
        if ntype == "array_access":
            return self._eval_array_access(expr)

        # --- 函数调用 ---
        if ntype == "func_call":
            return self._eval_func_call(expr)

        # --- 后缀自增/自减 ---
        if ntype == "post_inc":
            return self._eval_post_inc(expr)
        if ntype == "post_dec":
            return self._eval_post_dec(expr)

        # --- 前缀自增/自减 ---
        if ntype == "pre_inc":
            return self._eval_pre_inc(expr)
        if ntype == "pre_dec":
            return self._eval_pre_dec(expr)

        # --- 匿名函数 ---
        if ntype in ("anon_func", "anon_func_empty"):
            return self._eval_anon_func_def(expr)

        # --- 命名函数定义(表达式上下文) ---
        if ntype in ("named_func", "named_func_empty"):
            self._register_named_func(expr)
            return 0.0

        # --- 默认: 递归穿透 ---
        if ch:
            return self.evaluate_expression(ch[0])
        return 0.0

    def _get_variable(self, name: str) -> Any:
        key = name.lower()
        if key in self.enum_values:
            return self.enum_values[key]
        if self.current_env.has(name):
            return self.current_env.get(name)
        if key in self.builtin_vars:
            return self.builtin_vars[key]
        # 自动定义新变量
        self.current_env.define(name, 0.0)
        return 0.0

    def _set_variable(self, name: str, value: Any) -> None:
        key = name.lower()
        if self.current_env.has(name):
            self.current_env.set(name, value)
        elif key in self.builtin_vars:
            self.builtin_vars[key] = value
        else:
            self.current_env.define(name, value)

    def _eval_assign(self, expr: Any) -> Any:
        ch = children(expr)
        if len(ch) == 1:
            return self.evaluate_expression(ch[0])

        if len(ch) == 3:
            left, op_node, right = ch[0], ch[1], ch[2]
            op = get_op_value(op_node) if is_op_token(op_node) else get_value(op_node)

            # 数组元素赋值
            if is_tree(left) and get_type(left) == "array_access":
                ach = children(left)
                arr_name = self._extract_ident_name(ach[0])
                # 跳过LBRACK/RBRACK找索引
                index = 0.0
                for ac in ach[1:]:
                    if is_token(ac) and get_type(ac) in ("LBRACK", "RBRACK"):
                        continue
                    index = self.evaluate_expression(ac)
                    break
                val = self.evaluate_expression(right)
                arr = self._get_variable(arr_name)
                if isinstance(arr, list):
                    idx = int(index)
                    size = len(arr)
                    if size > 0:
                        if size & (size - 1) == 0:
                            idx = idx & (size - 1)
                        elif idx < 0 or idx >= size:
                            idx = 0
                        if op == "=":
                            arr[idx] = val
                        else:
                            arr[idx] = self._compound_op(arr[idx], op, val)
                return val

            # 普通变量赋值
            name = self._extract_ident_name(left)
            val = self.evaluate_expression(right)
            if op == "=":
                self._set_variable(name, val)
            else:
                current = self._get_variable(name)
                val = self._compound_op(current, op, val)
                self._set_variable(name, val)
            return val

        return 0.0

    def _compound_op(self, left: Any, op: str, right: Any) -> Any:
        ops = {
            "+=": lambda x, y: x + y,
            "-=": lambda x, y: x - y,
            "*=": lambda x, y: x * y,
            "/=": lambda x, y: x / y if y != 0 else 0,
            "%=": lambda x, y: math.fmod(x, y) if y != 0 else 0,
        }
        return ops.get(op, lambda x, y: y)(left, right)

    def _eval_binary_chain(self, expr: Any) -> Any:
        ch = children(expr)
        if not ch:
            return 0.0
        if len(ch) == 1:
            return self.evaluate_expression(ch[0])

        result = self.evaluate_expression(ch[0])
        i = 1
        while i + 1 < len(ch):
            op = get_op_value(ch[i]) if is_op_token(ch[i]) else get_value(ch[i])
            right = self.evaluate_expression(ch[i + 1])
            result = self._binary_op(result, op, right)
            i += 2
        return result

    def _binary_op(self, left: Any, op: str, right: Any) -> Any:
        ops = {
            "+": lambda x, y: x + y,
            "-": lambda x, y: x - y,
            "*": lambda x, y: x * y,
            "/": lambda x, y: x / y if y != 0 else 0.0,
            "%": lambda x, y: math.fmod(x, y) if y != 0 else 0.0,
            "^": lambda x, y: x ** y,
            "==": lambda x, y: 1.0 if x == y else 0.0,
            "!=": lambda x, y: 1.0 if x != y else 0.0,
            "<": lambda x, y: 1.0 if x < y else 0.0,
            ">": lambda x, y: 1.0 if x > y else 0.0,
            "<=": lambda x, y: 1.0 if x <= y else 0.0,
            ">=": lambda x, y: 1.0 if x >= y else 0.0,
            "&&": lambda x, y: 1.0 if x and y else 0.0,
            "||": lambda x, y: 1.0 if x or y else 0.0,
        }
        handler = ops.get(op)
        if handler:
            return handler(left, right)
        return right

    def _eval_unary(self, expr: Any) -> Any:
        ch = children(expr)
        if not ch:
            return 0.0
        if len(ch) == 1:
            return self.evaluate_expression(ch[0])
        if len(ch) == 2:
            op = get_value(ch[0]) if is_token(ch[0]) else get_op_value(ch[0])
            operand = self.evaluate_expression(ch[1])
            if op == "-":
                return -operand
            elif op == "+":
                return operand
            elif op == "!":
                return 1.0 if not operand else 0.0
            elif op == "&":
                return operand
        return 0.0

    def _eval_array_access(self, expr: Any) -> Any:
        ch = children(expr)
        if len(ch) < 4:
            return 0.0
        name = self._extract_ident_name(ch[0])
        index = 0.0
        for c in ch[1:]:
            if is_token(c) and get_type(c) in ("LBRACK", "RBRACK"):
                continue
            index = self.evaluate_expression(c)
            break
        arr = self._get_variable(name)
        if isinstance(arr, list):
            idx = int(index)
            size = len(arr)
            if size == 0:
                return 0.0
            if size & (size - 1) == 0:
                idx = idx & (size - 1)
            elif idx < 0 or idx >= size:
                idx = 0
            return arr[idx]
        if int(index) == 0:
            return arr
        return 0.0

    def _eval_func_call(self, expr: Any) -> Any:
        ch = children(expr)
        if not ch:
            return 0.0
        name = self._extract_ident_name(ch[0])
        args = []
        for c in ch[1:]:
            if get_type(c) == "args":
                args = [self.evaluate_expression(ac) for ac in children(c)]
                break
        return self._call_function(name, args)

    def _call_function(self, name: str, args: list) -> Any:
        key = name.lower()
        if key in self.builtin_functions:
            try:
                return self.builtin_functions[key](*args)
            except Exception as e:
                if self.debug:
                    print(f"内置函数 {name} 错误: {e}")
                return 0.0
        if key in self.functions:
            return self._call_user_function(self.functions[key], args)
        if self.debug:
            print(f"未定义的函数: {name}")
        return 0.0

    def _call_user_function(self, func: EvalFunction, args: list) -> Any:
        old_env = self.current_env
        self.current_env = Environment(self.global_env)
        try:
            for i, (pname, ptype) in enumerate(func.params):
                val = args[i] if i < len(args) else 0.0
                if ptype == "array":
                    if isinstance(val, list):
                        self.current_env.define(pname, val)
                    else:
                        self.current_env.define(pname, [val])
                else:
                    self.current_env.define(pname, val)
            try:
                result = 0.0
                for stmt in children(func.body):
                    result = self.execute_statement(stmt)
                return result
            except ReturnException as ret:
                return ret.value
        finally:
            self.current_env = old_env

    def _eval_post_inc(self, expr: Any) -> Any:
        ch = children(expr)
        name = self._extract_ident_name(ch[0])
        current = self._get_variable(name)
        self._set_variable(name, current + 1)
        return current

    def _eval_post_dec(self, expr: Any) -> Any:
        ch = children(expr)
        name = self._extract_ident_name(ch[0])
        current = self._get_variable(name)
        self._set_variable(name, current - 1)
        return current

    def _eval_pre_inc(self, expr: Any) -> Any:
        ch = children(expr)
        name = get_value(ch[-1]) if is_token(ch[-1]) else self._extract_ident_name(ch[-1])
        current = self._get_variable(name)
        new_val = current + 1
        self._set_variable(name, new_val)
        return new_val

    def _eval_pre_dec(self, expr: Any) -> Any:
        ch = children(expr)
        name = get_value(ch[-1]) if is_token(ch[-1]) else self._extract_ident_name(ch[-1])
        current = self._get_variable(name)
        new_val = current - 1
        self._set_variable(name, new_val)
        return new_val

    def _find_func_body(self, node: Any) -> Any:
        """查找函数节点的body (block/func_stmts/func_body)"""
        ch = children(node)
        for c in ch:
            if is_tree(c) and get_type(c) in ("block", "func_stmts", "func_body"):
                return c
        return None

    def _eval_anon_func_def(self, expr: Any) -> Any:
        ch = children(expr)
        params = []
        body = None
        for c in ch:
            ntype = get_type(c)
            if ntype == "func_params":
                params = self._parse_func_def_params(c)
            elif ntype in ("block", "func_stmts", "func_body"):
                body = c
        if body is None:
            for c in ch:
                if is_tree(c) and get_type(c) in ("block", "func_stmts", "func_body"):
                    body = c
                    break
        return EvalFunction("<anon>", params, body, self, is_anon=True)

    def _parse_func_def_params(self, params_node: Any) -> list:
        params = []
        for c in children(params_node):
            ntype = get_type(c)
            pch = children(c)
            if ntype == "param_value":
                name = get_value(pch[0])
                params.append((name, "value"))
            elif ntype == "param_ptr":
                name = get_value(pch[1])
                params.append((name, "ptr"))
            elif ntype == "param_str":
                name = get_value(pch[1])
                params.append((name, "str"))
            elif ntype == "param_array":
                name = get_value(pch[0])
                params.append((name, "array"))
            elif ntype == "param_func":
                name = get_value(pch[0])
                params.append((name, "func"))
        return params

    def _extract_ident_name(self, node: Any) -> str:
        """从节点提取标识符名称 - 递归穿透所有单子节点包装"""
        if is_token(node) and get_type(node) in ("IDENTIFIER", "PARAM_ID"):
            return get_value(node)
        if is_tree(node):
            ch = children(node)
            if len(ch) == 1:
                return self._extract_ident_name(ch[0])
            if ch:
                name = self._extract_ident_name(ch[0])
                if name:
                    return name
        return ""


# ===== 接口函数 =====
def interpret_eval_code(code: str, debug: bool = False) -> Any:
    from eval_parser import EvalParser
    ast = EvalParser.parse(code)
    interpreter = EvalInterpreter(debug=debug)
    return interpreter.interpret(ast)


def interpret_eval_file(filepath: str, debug: bool = False) -> Any:
    from eval_parser import EvalParser
    ast = EvalParser.parse_file(filepath)
    interpreter = EvalInterpreter(debug=debug)
    return interpreter.interpret(ast)


def interpret_pss_file(filepath: str, debug: bool = False) -> Any:
    from pss_parser import parse_pss_file
    from eval_parser import EvalParser
    pss = parse_pss_file(filepath)
    host_code = pss.host_code
    if not host_code.strip():
        return 0.0
    ast = EvalParser.parse(host_code)
    interpreter = EvalInterpreter(debug=debug)
    return interpreter.interpret(ast)


# ===== 测试 =====
if __name__ == "__main__":
    import sys

    tests = [
        ("1 + 2;", "简单加法", 3.0),
        ("x = 3; y = 4; x * y;", "变量赋值", 12.0),
        ("printf(\"hello %d\\n\", 42);", "printf", 0.0),
        ("for(i=0;i<5;i++) printf(\"%d \", i);", "for循环", 0.0),
        ("if (1) printf(\"yes\"); else printf(\"no\");", "if-else", 0.0),
        ("x = sqrt(2); printf(\"%g\", x);", "内置函数", 0.0),
        ("static buf[4] = {1,2,3,4}; printf(\"%g\", buf[2]);", "static数组", 0.0),
        ("enum {A=1, B, C}; printf(\"%d %d %d\", A, B, C);", "enum", 0.0),
        ("1 + 2 * 3;", "运算优先级", 7.0),
        ("(1 + 2) * 3;", "括号", 9.0),
        ("x = 10; x++; printf(\"%d\", x);", "后缀++", 0.0),
        ("x = 10; ++x; printf(\"%d\", x);", "前缀++", 0.0),
    ]

    print("=== EVAL解释器测试 ===")
    ok = 0
    for code, desc, expected in tests:
        try:
            result = interpret_eval_code(code, debug=False)
            ok += 1
            status = "OK" if expected is None or result == expected else f"VAL({result}!={expected})"
            print(f"OK: {desc} => {result}")
        except Exception as e:
            print(f"FAIL: {desc}: {e}")

    print(f"\n{ok}/{len(tests)} 通过")

    if len(sys.argv) > 1:
        filepath = sys.argv[1]
        print(f"\n=== 执行PSS文件: {filepath} ===")
        try:
            result = interpret_pss_file(filepath, debug=True)
            print(f"结果: {result}")
        except Exception as e:
            print(f"错误: {e}")
            import traceback
            traceback.print_exc()
