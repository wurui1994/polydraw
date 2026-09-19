"""EVAL AST → Python 代码转换器

将eval_parser.py生成的AST(dict格式)转换为可执行的Python代码。
支持: 变量/数组、函数定义/调用、控制流、内置数学函数等。

转换策略:
- EVAL变量全部用Python float
- 数组用Python list
- 内置函数映射到Python math模块
- GL函数映射到runtime模块调用
- shader块原样保留为字符串
"""
import math
from typing import Dict, Any, List, Optional, Set


# ===== AST工具 (同eval_interpreter) =====
def is_tree(node): return isinstance(node, dict) and "children" in node
def is_token(node): return isinstance(node, dict) and "value" in node and "children" not in node
def get_type(node): return node.get("type", "") if isinstance(node, dict) else ""
def get_value(node): return node.get("value", "") if isinstance(node, dict) else str(node)
def children(node): return node.get("children", []) if isinstance(node, dict) else []

_OP_VALUES = {
    "+", "-", "*", "/", "%", "^",
    "==", "!=", "<", ">", "<=", ">=",
    "&&", "||",
    "+=", "-=", "*=", "/=", "%=",
    "=",
}

def is_op_token(node): return is_token(node) and get_value(node) in _OP_VALUES

def get_op_value(node):
    v = get_value(node)
    # Python运算符映射
    op_map = {"^": "**", "&&": " and ", "||": " or "}
    return op_map.get(v, v)


# ===== 内置函数映射 =====
_BUILTIN_MAP = {
    "abs": "abs", "acos": "math.acos", "asin": "math.asin",
    "atan": "_atan", "atn": "_atan", "atan2": "math.atan2",
    "ceil": "math.ceil", "cos": "math.cos", "exp": "math.exp",
    "fabs": "abs", "fact": "_fact", "floor": "math.floor",
    "int": "float(int({}))",
    "log": "_log", "sgn": "_sgn", "sin": "math.sin",
    "sqrt": "math.sqrt", "tan": "math.tan", "unit": "_unit",
    "fmod": "math.fmod", "min": "min", "max": "max",
    "pow": "_pow", "noise": "_noise",
    "rgb": "_rgb", "rgba": "_rgba",
    "printf": "_printf", "printg": "_printg",
    "klock": "_klock", "glklockstart": "_glklockstart",
    "glklockelapsed": "_glklockelapsed", "srand": "_srand",
    # GL函数
    "glbegin": "_gl.gl_begin", "glend": "_gl.gl_end",
    "glvertex": "_gl.gl_vertex", "gltexcoord": "_gl.gl_texcoord",
    "glcolor": "_gl.gl_color", "glnormal": "_gl.gl_normal",
    "glpushmatrix": "_gl.gl_push_matrix", "glpopmatrix": "_gl.gl_pop_matrix",
    "gltranslate": "_gl.gl_translate", "glrotate": "_gl.gl_rotate",
    "glscale": "_gl.gl_scale", "glulookat": "_gl.gl_look_at",
    "gluperspective": "_gl.gl_perspective", "setfov": "_setfov",
    "glsettex": "_gl.gl_set_tex", "glgettex": "_gl.gl_get_tex",
    "glactivetexture": "_gl.gl_active_texture", "glbindtexture": "_gl.gl_bind_texture",
    "glsetshader": "_gl.gl_set_shader", "glgetuniformloc": "_gl.gl_get_uniform_loc",
    "gluniform1i": "_gl.gl_uniform1i", "gluniform1f": "_gl.gl_uniform1f",
    "gluniform": "_gl.gl_uniform", "glquad": "_gl.gl_quad",
    "glcullface": "_gl.gl_cull_face", "gllinewidth": "_gl.gl_line_width",
    "glalphaenable": "_gl.gl_alpha_enable", "glalphadisable": "_gl.gl_alpha_disable",
    "glblendfunc": "_gl.gl_blend_func", "glenable": "_gl.gl_enable",
    "gldisable": "_gl.gl_disable", "glcapture": "_gl.gl_capture",
    "glcaptureend": "_gl.gl_capture_end",
    "glswapinterval": "lambda *a: 0.0", "sleep": "time.sleep",
    "playnote": "lambda *a: 0.0", "mountzip": "lambda *a: 0.0",
    "glprogramlocalparam": "lambda *a: 0.0", "glprogramenvparam": "lambda *a: 0.0",
    "gltextdisable": "lambda *a: 0.0", "glgetattribloc": "lambda *a: 0",
    "glvertexattrib": "lambda *a: 0.0",
}

# 内置常量映射
_CONST_MAP = {
    "pi": "math.pi", "e": "math.e",
    "gl_points": "0x0000", "gl_lines": "0x0001", "gl_line_loop": "0x0002",
    "gl_line_strip": "0x0003", "gl_triangles": "0x0004",
    "gl_triangle_strip": "0x0005", "gl_triangle_fan": "0x0006",
    "gl_quads": "0x0007", "gl_quad_strip": "0x0008", "gl_polygon": "0x0009",
    "gl_depth_test": "0x0B71", "gl_none": "0", "gl_front": "0x0404",
    "gl_back": "0x0405", "gl_front_and_back": "0x0408",
    "gl_texture0": "0x84C0", "gl_src_alpha": "0x0302",
    "gl_one_minus_src_alpha": "0x0303",
    "kg_bgra32": "1", "kg_short": "2", "kg_int": "3",
    "kg_float": "4", "kg_vec4": "5",
    "xres": "_sys['xres']", "yres": "_sys['yres']",
    "mousx": "_sys['mousx']", "mousy": "_sys['mousy']",
    "bstatus": "_sys['bstatus']", "numframes": "_sys['numframes']",
}


class EvalToPython:
    """EVAL AST → Python 转换器"""

    def __init__(self):
        self._indent = 0
        self._lines: List[str] = []
        self._used_helpers: Set[str] = set()
        self._user_funcs: Set[str] = set()

    def convert(self, ast: Dict[str, Any]) -> str:
        """转换AST为Python代码"""
        self._indent = 0
        self._lines = []
        self._used_helpers = set()
        self._user_funcs = set()

        # 第一遍: 收集用户函数名
        self._collect_funcs(ast)

        # 生成代码
        self._emit_header()

        # 先收集主代码到临时列表, 以便在头部之后插入辅助函数
        main_lines = []
        saved_lines = self._lines
        self._lines = main_lines
        self._convert_node(ast)
        self._lines = saved_lines

        # 添加辅助函数 (在主代码之前)
        if self._used_helpers:
            self._emit_helpers()

        # 添加主代码
        self._lines.extend(main_lines)

        return "\n".join(self._lines)

    def _collect_funcs(self, node):
        """收集用户定义的函数名"""
        if not is_tree(node):
            return
        ntype = get_type(node)
        if ntype in ("named_func", "named_func_empty"):
            ch = children(node)
            if ch:
                name = self._extract_ident(ch[0]).lower()
                self._user_funcs.add(name)
        for c in children(node):
            self._collect_funcs(c)

    def _emit_header(self):
        """生成文件头"""
        self._lines.append('"""Auto-generated Python code from EVAL AST"""')
        self._lines.append("import math")
        self._lines.append("import time")
        self._lines.append("import random")
        self._lines.append("")
        self._lines.append("# Runtime objects (must be injected before execution)")
        self._lines.append("# _gl = GLContext instance")
        self._lines.append("# _sys = system variables dict")
        self._lines.append("_sys = {'xres': 640.0, 'yres': 480.0, 'mousx': 0.0, 'mousy': 0.0, 'bstatus': 0.0, 'numframes': 0.0}")
        self._lines.append("_rnd = random.Random(0)")
        self._lines.append("_start_time = time.time()")
        self._lines.append("")

    def _emit_helpers(self):
        """生成辅助函数"""
        self._lines.append("")
        self._lines.append("# ===== Helper functions =====")

        helpers = {
            "_atan": "def _atan(x, *a): return math.atan2(x, a[0]) if a else math.atan(x)",
            "_fact": "def _fact(x): return float(math.gamma(x + 1))",
            "_log": "def _log(x, *a): return math.log(x, a[0]) if a else math.log(x)",
            "_sgn": "def _sgn(x): return -1.0 if x < 0 else (1.0 if x > 0 else 0.0)",
            "_unit": "def _unit(x): return 0.0 if x < 0 else (1.0 if x > 0 else 0.5)",
            "_pow": "def _pow(x, y): return x ** y",
            "_noise": "def _noise(*a):\n    x = a[0] if len(a) > 0 else 0\n    y = a[1] if len(a) > 1 else 0\n    z = a[2] if len(a) > 2 else 0\n    n = int(x * 57 + y * 131 + z * 211) & 0x7fffffff\n    n = (n >> 13) ^ n\n    n = (n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff\n    return float(n) / 1073741824.0 - 1.0",
            "_rgb": "def _rgb(r, g, b): return float((int(r) & 0xFF) << 16 | (int(g) & 0xFF) << 8 | (int(b) & 0xFF))",
            "_rgba": "def _rgba(r, g, b, a): return float((int(a) & 0xFF) << 24 | (int(r) & 0xFF) << 16 | (int(g) & 0xFF) << 8 | (int(b) & 0xFF))",
            "_printf": "def _printf(fmt, *args):\n    if isinstance(fmt, str) and fmt.startswith('\"') and fmt.endswith('\"'): fmt = fmt[1:-1]\n    fmt = fmt.replace('\\\\n', '\\n').replace('\\\\t', '\\t')\n    arg_idx = 0; i = 0; out = []\n    while i < len(fmt):\n        if i + 1 < len(fmt) and fmt[i] == '%':\n            spec = fmt[i + 1]\n            if spec in ('d', 'i'): out.append(str(int(args[arg_idx])) if arg_idx < len(args) else '0'); arg_idx += 1; i += 2\n            elif spec in ('f', 'e', 'E', 'g', 'G'): out.append(f'%{spec}' % (args[arg_idx] if arg_idx < len(args) else 0.0)); arg_idx += 1; i += 2\n            elif spec == 's': out.append(str(args[arg_idx]) if arg_idx < len(args) else ''); arg_idx += 1; i += 2\n            elif spec == '%': out.append('%'); i += 2\n            else: out.append(fmt[i]); i += 1\n        else: out.append(fmt[i]); i += 1\n    print(''.join(out), end=''); return 0.0",
            "_printg": "def _printg(*args): print('[PRINTG]', *args); return 0.0",
            "_klock": "def _klock(*a):\n    if not a or a[0] == 0: return time.time() - _start_time\n    opt = int(a[0]); t = time.localtime()\n    if opt == 1: return float(f'{t.tm_year}{t.tm_mon:02d}{t.tm_mday:02d}{t.tm_hour:02d}{t.tm_min:02d}{t.tm_sec:02d}')\n    elif opt == 2: return float(t.tm_year)\n    elif opt == 3: return float(t.tm_mon)\n    elif opt == 5: return float(t.tm_mday)\n    elif opt == 6: return float(t.tm_hour)\n    elif opt == 7: return float(t.tm_min)\n    elif opt == 8: return float(t.tm_sec)\n    return 0.0",
            "_glklockstart": "def _glklockstart(): global _glklock_start; _glklock_start = time.time(); return 0.0",
            "_glklockelapsed": "def _glklockelapsed(): return time.time() - _glklock_start",
            "_srand": "def _srand(x): _rnd.seed(int(x)); return 0.0",
            "_setfov": "def _setfov(fovy): _gl.gl_perspective(float(fovy), _sys['xres'] / _sys['yres'], 0.1, 100.0)",
        }

        for name in sorted(self._used_helpers):
            if name in helpers:
                self._lines.append(helpers[name])

    def _line(self, code: str):
        self._lines.append("    " * self._indent + code)

    def _convert_node(self, node):
        """转换AST节点"""
        if not is_tree(node):
            return
        ntype = get_type(node)
        ch = children(node)

        if ntype == "start":
            for c in ch:
                self._convert_stmt(c)
        else:
            self._convert_stmt(node)

    def _convert_stmt(self, node):
        """转换单个语句"""
        if not is_tree(node):
            return
        ntype = get_type(node)
        ch = children(node)

        if ntype == "expr_stmt":
            if ch:
                self._convert_expr_as_stmt(ch[0])
            else:
                self._line("0.0")

        elif ntype in ("named_func", "named_func_empty"):
            self._convert_named_func(node)

        elif ntype == "if_stmt":
            cond = self._convert_expr(ch[0])
            # Python中非零即真, 但EVAL用1.0/0.0, 需要显式bool转换
            self._line(f"if {cond}:")
            self._indent += 1
            self._convert_stmt(ch[1])
            self._indent -= 1
            if len(ch) >= 3:
                self._line("else:")
                self._indent += 1
                self._convert_stmt(ch[2])
                self._indent -= 1

        elif ntype == "while_stmt":
            cond = self._convert_expr(ch[0])
            self._line(f"while {cond}:")
            self._indent += 1
            self._convert_stmt(ch[1])
            self._indent -= 1

        elif ntype == "do_while_stmt":
            self._line("while True:")
            self._indent += 1
            self._convert_stmt(ch[0])
            cond = self._convert_expr(ch[1])
            self._line(f"if not ({cond}): break")
            self._indent -= 1

        elif ntype == "for_stmt":
            init = ch[0] if len(ch) > 0 and ch[0] else None
            cond = ch[1] if len(ch) > 1 and ch[1] else None
            update = ch[2] if len(ch) > 2 and ch[2] else None
            body = ch[3] if len(ch) > 3 else None

            if init:
                self._convert_expr_as_stmt(init)
            cond_str = self._convert_expr(cond) if cond else "True"
            self._line(f"while {cond_str}:")
            self._indent += 1
            if body:
                self._convert_stmt(body)
            if update:
                self._convert_expr_as_stmt(update)
            self._indent -= 1

        elif ntype == "block":
            for c in ch:
                self._convert_stmt(c)

        elif ntype == "enum_decl":
            self._convert_enum(node)

        elif ntype == "static_decl":
            self._convert_static(node)

        elif ntype == "return_stmt":
            val = self._convert_expr(ch[0]) if ch else "0.0"
            self._line(f"return {val}")

        elif ntype == "break_stmt":
            self._line("break")

        elif ntype == "continue_stmt":
            self._line("continue")

        elif ntype == "goto_stmt":
            label = get_value(ch[0])
            self._line(f"# goto {label} (not supported in Python)")

        elif ntype == "label_stmt":
            label = get_value(ch[0])
            self._line(f"# label {label}:")

    def _convert_named_func(self, node):
        """转换命名函数定义"""
        ch = children(node)
        name = self._extract_ident(ch[0])
        params = []
        body = None
        for c in ch[1:]:
            ntype = get_type(c)
            if ntype == "args":
                params = [self._extract_ident(ac) for ac in children(c)]
            elif ntype == "block":
                body = c

        param_str = ", ".join(params)
        self._line(f"def {name}({param_str}):")
        self._indent += 1
        if body:
            for stmt in children(body):
                self._convert_stmt(stmt)
        else:
            self._line("pass")
        self._indent -= 1
        self._line("")

    def _convert_enum(self, node):
        """转换enum声明"""
        ch = children(node)
        next_val = 0
        for c in ch:
            if get_type(c) != "enum_entry":
                continue
            ech = children(c)
            name = get_value(ech[0])

            if len(ech) > 1:
                val_node = ech[1]
                ntype = get_type(val_node)
                if ntype == "comma_expr":
                    # enum {A=1, B, C} 被解析为单个enum_entry包含comma_expr
                    cch = children(val_node)
                    # 第一个子节点是A=1的值
                    first_val = self._convert_expr(cch[0])
                    self._line(f"{name} = {first_val}")
                    try:
                        next_val = int(float(first_val.replace("float(", "").replace(")", ""))) + 1
                    except:
                        next_val = 0
                    # 后续子节点是B, C等
                    for cc in cch[1:]:
                        ename = self._extract_ident(cc)
                        if ename:
                            self._line(f"{ename} = {next_val}.0")
                            next_val += 1
                        else:
                            # 可能是 D=5 形式
                            cch2 = children(cc)
                            if cch2:
                                ename = self._extract_ident(cch2[0])
                                if ename and len(cch2) >= 3:
                                    eval_val = self._convert_expr(cch2[2])
                                    self._line(f"{ename} = {eval_val}")
                                    try:
                                        next_val = int(float(eval_val.replace("float(", "").replace(")", ""))) + 1
                                    except:
                                        next_val = 0
                    return
                else:
                    val_expr = self._convert_expr(val_node)
                    self._line(f"{name} = {val_expr}")
                    try:
                        next_val = int(float(val_expr.replace("float(", "").replace(")", ""))) + 1
                    except:
                        next_val = 0
            else:
                self._line(f"{name} = {next_val}.0")
                next_val += 1

    def _convert_static(self, node):
        """转换static声明"""
        ch = children(node)
        for c in ch:
            if get_type(c) == "static_var":
                self._convert_static_var(c)

    def _convert_static_var(self, node):
        ch = children(node)
        name = get_value(ch[0])
        dims = []
        init_node = None
        for c in ch[1:]:
            if get_type(c) == "static_init":
                init_node = c
            elif is_tree(c):
                dims.append(self._convert_expr(c))

        if dims:
            dim_str = " * ".join(dims)
            if init_node:
                inits = [self._convert_expr(ic) for ic in children(init_node)]
                self._line(f"{name} = [{', '.join(inits)}] + [0.0] * ({dim_str} - {len(inits)})")
            else:
                self._line(f"{name} = [0.0] * {dim_str}")
        else:
            if init_node:
                ich = children(init_node)
                val = self._convert_expr(ich[0]) if ich else "0.0"
                self._line(f"{name} = {val}")
            else:
                self._line(f"{name} = 0.0")

    def _convert_expr_as_stmt(self, node):
        """将表达式作为语句转换 - 赋值等需要拆分为多行"""
        if not is_tree(node):
            self._line(self._convert_expr(node))
            return

        ntype = get_type(node)
        ch = children(node)

        # 逗号表达式: 逐个作为语句执行
        if ntype == "comma_expr":
            for c in ch:
                self._convert_expr_as_stmt(c)
            return

        # 赋值表达式: 拆分为赋值语句 (不调用_convert_expr避免重复输出)
        if ntype == "assign_expr" and len(ch) == 3:
            op = get_value(ch[1]) if is_op_token(ch[1]) else get_value(ch[1])
            left = self._convert_expr_pure(ch[0])
            right = self._convert_expr_pure(ch[2])
            if op == "=":
                self._line(f"{left} = {right}")
            else:
                py_op = {"^": "**", "&&": " and ", "||": " or "}.get(op, op)
                self._line(f"{left} {py_op} {right}")
            return

        if ntype == "assign_expr" and len(ch) == 1:
            self._convert_expr_as_stmt(ch[0])
            return

        # 后缀自增/自减: 拆分为多行
        if ntype == "post_inc":
            name = self._extract_ident(ch[0])
            self._line(f"_old = {name}")
            self._line(f"{name} = {name} + 1.0")
            return
        if ntype == "post_dec":
            name = self._extract_ident(ch[0])
            self._line(f"_old = {name}")
            self._line(f"{name} = {name} - 1.0")
            return

        # 前缀自增/自减
        if ntype == "pre_inc":
            name = get_value(ch[-1]) if is_token(ch[-1]) else self._extract_ident(ch[-1])
            self._line(f"{name} = {name} + 1.0")
            return
        if ntype == "pre_dec":
            name = get_value(ch[-1]) if is_token(ch[-1]) else self._extract_ident(ch[-1])
            self._line(f"{name} = {name} - 1.0")
            return

        # 其他表达式: 直接输出
        self._line(self._convert_expr(node))

    def _convert_expr(self, node) -> str:
        """转换表达式为Python表达式字符串 (可能产生副作用行如赋值)"""
        return self._convert_expr_impl(node, pure=False)

    def _convert_expr_pure(self, node) -> str:
        """转换表达式为Python表达式字符串 (不产生副作用行, 用于语句上下文)"""
        return self._convert_expr_impl(node, pure=True)

    def _convert_expr_impl(self, node, pure: bool = False) -> str:
        """转换表达式为Python表达式字符串"""
        if node is None or not isinstance(node, dict):
            return "0.0"

        ntype = get_type(node)
        ch = children(node)

        # Token
        if ntype == "NUMBER":
            val = get_value(node)
            if val.startswith("0x") or val.startswith("0X"):
                return f"float({val})"
            return f"float({val})"

        if ntype == "FLOAT_NUM":
            return f"float({get_value(node)})"

        if ntype == "STRING":
            return get_value(node)

        if ntype in ("IDENTIFIER", "PARAM_ID"):
            name = get_value(node)
            key = name.lower()
            if key in _CONST_MAP:
                return _CONST_MAP[key]
            if key == "rnd":
                return "_rnd.random()"
            if key == "nrnd":
                return "_rnd.gauss(0, 1)"
            return name

        if is_op_token(node):
            op = get_op_value(node)
            # Python运算符映射
            op_map = {"^": "**", "&&": " and ", "||": " or "}
            return op_map.get(op, op)

        if not is_tree(node):
            return "0.0"

        # 逗号表达式
        if ntype == "comma_expr":
            parts = [self._convert_expr_impl(c, pure) for c in ch]
            # 逗号表达式: 依次执行, 返回最后一个
            if len(parts) == 1:
                return parts[0]
            return "(" + ", ".join(parts) + ")"

        # 赋值表达式
        if ntype == "assign_expr":
            if len(ch) == 1:
                return self._convert_expr_impl(ch[0], pure)
            if len(ch) == 3:
                left = self._convert_expr_impl(ch[0], pure)
                op = get_value(ch[1]) if is_op_token(ch[1]) else get_value(ch[1])
                right = self._convert_expr_impl(ch[2], pure)
                py_op = {"^": "**", "&&": " and ", "||": " or "}.get(op, op)
                if pure:
                    # 纯模式: 不生成副作用行, 返回赋值表达式
                    return f"({left} {py_op} {right})"
                else:
                    # 非纯模式: 生成赋值语句行+返回变量名
                    self._line(f"{left} {py_op} {right}")
                    return left

        # 二元运算
        if ntype in ("or_expr", "and_expr", "eq_expr", "cmp_expr",
                      "add_expr", "mul_expr", "pow_expr"):
            if len(ch) == 1:
                return self._convert_expr_impl(ch[0], pure)
            parts = []
            i = 0
            while i < len(ch):
                if is_op_token(ch[i]):
                    op = get_op_value(ch[i])
                    op_map = {"^": "**", "&&": " and ", "||": " or "}
                    parts.append(op_map.get(op, op))
                else:
                    parts.append(self._convert_expr_impl(ch[i], pure))
                i += 1
            return "(" + " ".join(parts) + ")"

        # 一元运算
        if ntype == "unary_expr":
            if len(ch) == 1:
                return self._convert_expr_impl(ch[0], pure)
            if len(ch) == 2:
                op = get_value(ch[0]) if is_token(ch[0]) else ""
                operand = self._convert_expr_impl(ch[1], pure)
                if op == "-":
                    return f"(-{operand})"
                elif op == "!":
                    return f"(1.0 if not {operand} else 0.0)"
                elif op == "+":
                    return operand
                elif op == "&":
                    return operand

        # 后缀表达式
        if ntype == "postfix_expr":
            if ch:
                return self._convert_expr_impl(ch[0], pure)
            return "0.0"

        # 数组访问
        if ntype == "array_access":
            name = self._extract_ident(ch[0])
            index = "0.0"
            for c in ch[1:]:
                if is_token(c) and get_type(c) in ("LBRACK", "RBRACK"):
                    continue
                index = self._convert_expr_impl(c, pure)
                break
            return f"{name}[int({index}) % len({name})]"

        # 函数调用
        if ntype == "func_call":
            name = self._extract_ident(ch[0])
            args = []
            for c in ch[1:]:
                if get_type(c) == "args":
                    args = [self._convert_expr_impl(ac, pure) for ac in children(c)]
                    break
            key = name.lower()
            if key in _BUILTIN_MAP:
                mapped = _BUILTIN_MAP[key]
                self._used_helpers.add(mapped.split("(")[0].split(".")[0])
                args_str = ", ".join(args)
                if mapped.startswith("lambda"):
                    return f"({mapped})({args_str})"
                if "{}" in mapped:
                    return mapped.format(args_str)
                return f"{mapped}({args_str})"
            if key in self._user_funcs:
                args_str = ", ".join(args)
                return f"{name}({args_str})"
            # 未知函数
            args_str = ", ".join(args)
            return f"# unknown: {name}({args_str})"

        # 后缀自增/自减 - 需要拆分为多行
        if ntype == "post_inc":
            name = self._extract_ident(ch[0])
            self._line(f"_old = {name}")
            self._line(f"{name} = {name} + 1.0")
            return "_old"
        if ntype == "post_dec":
            name = self._extract_ident(ch[0])
            self._line(f"_old = {name}")
            self._line(f"{name} = {name} - 1.0")
            return "_old"

        # 前缀自增/自减
        if ntype == "pre_inc":
            name = get_value(ch[-1]) if is_token(ch[-1]) else self._extract_ident(ch[-1])
            self._line(f"{name} = {name} + 1.0")
            return name
        if ntype == "pre_dec":
            name = get_value(ch[-1]) if is_token(ch[-1]) else self._extract_ident(ch[-1])
            self._line(f"{name} = {name} - 1.0")
            return name

        # 匿名函数
        if ntype in ("anon_func", "anon_func_empty"):
            params = []
            body_stmts = []
            for c in ch:
                ntype2 = get_type(c)
                if ntype2 == "func_params":
                    params = [self._extract_ident(pc) for pc in children(c)]
                elif ntype2 in ("block", "func_stmts", "func_body"):
                    body_stmts = children(c)

            param_str = ", ".join(params)
            body_lines = []
            for stmt in body_stmts:
                body_lines.append(self._convert_expr_impl(stmt, pure))
            body_str = "; ".join(body_lines) if body_lines else "0.0"
            return f"(lambda {param_str}: {body_str})"

        # 默认穿透
        if ch:
            return self._convert_expr_impl(ch[0], pure)
        return "0.0"

    def _extract_ident(self, node) -> str:
        """提取标识符名称"""
        if is_token(node) and get_type(node) in ("IDENTIFIER", "PARAM_ID"):
            return get_value(node)
        if is_tree(node):
            ch = children(node)
            if len(ch) == 1:
                return self._extract_ident(ch[0])
            if ch:
                name = self._extract_ident(ch[0])
                if name:
                    return name
        return ""


# ===== 接口函数 =====
def eval_to_python(ast: Dict[str, Any]) -> str:
    """将EVAL AST转换为Python代码"""
    converter = EvalToPython()
    return converter.convert(ast)


def eval_code_to_python(code: str) -> str:
    """将EVAL代码转换为Python代码"""
    from eval_parser import EvalParser
    ast = EvalParser.parse(code)
    return eval_to_python(ast)


def pss_file_to_python(filepath: str) -> str:
    """将PSS文件转换为Python代码"""
    from pss_parser import parse_pss_file
    from eval_parser import EvalParser

    pss = parse_pss_file(filepath)
    host_code = pss.host_code

    result_lines = []
    result_lines.append('"""Auto-generated from PSS file"""')
    result_lines.append("import math, time, random")
    result_lines.append("")

    # Shader代码作为注释保留
    for b in pss.vertex_blocks:
        result_lines.append(f"# === Vertex Shader{': ' + b.name if b.name else ''} ===")
        for line in b.code.split("\n"):
            result_lines.append(f"# {line}")
    for b in pss.fragment_blocks:
        result_lines.append(f"# === Fragment Shader{': ' + b.name if b.name else ''} ===")
        for line in b.code.split("\n"):
            result_lines.append(f"# {line}")

    result_lines.append("")

    # Host代码
    if host_code.strip():
        ast = EvalParser.parse(host_code)
        py_code = eval_to_python(ast)
        result_lines.append(py_code)

    return "\n".join(result_lines)


# ===== 测试 =====
if __name__ == "__main__":
    import sys

    tests = [
        ("1 + 2;", "简单加法"),
        ("x = 3; y = 4; x * y;", "变量赋值"),
        ("printf(\"hello %d\\n\", 42);", "printf"),
        ("for(i=0;i<5;i++) printf(\"%d \", i);", "for循环"),
        ("if (1) printf(\"yes\"); else printf(\"no\");", "if-else"),
        ("x = sqrt(2); printf(\"%g\", x);", "内置函数"),
        ("static buf[4] = {1,2,3,4}; printf(\"%g\", buf[2]);", "static数组"),
        ("enum {A=1, B, C}; printf(\"%d %d %d\", A, B, C);", "enum"),
        ("cube(x) { return x*x*x; }", "命名函数"),
        ("(x) x+1", "匿名函数"),
    ]

    print("=== EVAL → Python 转换测试 ===")
    for code, desc in tests:
        try:
            py = eval_code_to_python(code)
            print(f"\n--- {desc} ---")
            print(py)
        except Exception as e:
            print(f"FAIL: {desc}: {e}")
