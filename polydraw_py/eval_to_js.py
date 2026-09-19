"""EVAL AST → JavaScript 代码转换器

将eval_parser.py生成的AST(dict格式)转换为可执行的JavaScript代码。
用于WebGL渲染后端, 在浏览器中运行PSS效果。

转换策略:
- EVAL变量全部用JS let/number
- 数组用JS Array
- 内置数学函数映射到Math对象
- GL函数映射到WebGL封装对象
- shader块直接嵌入为GLSL字符串
"""
import os
import json
from typing import Dict, Any, List, Optional, Set


# ===== AST工具 =====
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
    # JS运算符映射
    op_map = {"^": "**", "==": "===", "!=": "!=="}
    return op_map.get(v, v)


# ===== 内置函数映射 =====
_JS_BUILTIN_MAP = {
    "abs": "Math.abs", "acos": "Math.acos", "asin": "Math.asin",
    "atan": "_atan", "atn": "_atan", "atan2": "Math.atan2",
    "ceil": "Math.ceil", "cos": "Math.cos", "exp": "Math.exp",
    "fabs": "Math.abs", "fact": "_fact", "floor": "Math.floor",
    "int": "Math.trunc", "log": "_log", "sgn": "_sgn",
    "sin": "Math.sin", "sqrt": "Math.sqrt", "tan": "Math.tan",
    "unit": "_unit", "fmod": "_fmod", "min": "Math.min", "max": "Math.max",
    "pow": "Math.pow", "noise": "_noise",
    "rgb": "_rgb", "rgba": "_rgba",
    "printf": "_printf", "printg": "_printg",
    "klock": "_klock", "glklockstart": "_glklockstart",
    "glklockelapsed": "_glklockelapsed", "srand": "_srand",
    # GL函数
    "glbegin": "GL.begin", "glend": "GL.end",
    "glvertex": "GL.vertex", "gltexcoord": "GL.texCoord",
    "glcolor": "GL.color", "glnormal": "GL.normal",
    "glpushmatrix": "GL.pushMatrix", "glpopmatrix": "GL.popMatrix",
    "gltranslate": "GL.translate", "glrotate": "GL.rotate",
    "glscale": "GL.scale", "glulookat": "GL.lookAt",
    "gluperspective": "GL.perspective", "setfov": "GL.setFov",
    "glsettex": "GL.setTex", "glgettex": "GL.getTex",
    "glactivetexture": "GL.activeTexture", "glbindtexture": "GL.bindTexture",
    "glsetshader": "GL.setShader", "glgetuniformloc": "GL.getUniformLoc",
    "gluniform1i": "GL.uniform1i", "gluniform1f": "GL.uniform1f",
    "gluniform": "GL.uniform", "glquad": "GL.quad",
    "glcullface": "GL.cullFace", "gllinewidth": "GL.lineWidth",
    "glalphaenable": "GL.alphaEnable", "glalphadisable": "GL.alphaDisable",
    "glblendfunc": "GL.blendFunc", "glenable": "GL.enable",
    "gldisable": "GL.disable", "glcapture": "GL.capture",
    "glcaptureend": "GL.captureEnd",
    "glswapinterval": "(() => 0)", "sleep": "(_x) => 0",
    "playnote": "(() => 0)", "mountzip": "(() => 0)",
    "glprogramlocalparam": "(() => 0)", "glprogramenvparam": "(() => 0)",
    "gltextdisable": "(() => 0)", "glgetattribloc": "(() => 0)",
    "glvertexattrib": "(() => 0)",
}

# 内置常量映射
_JS_CONST_MAP = {
    "pi": "Math.PI", "e": "Math.E",
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
    "xres": "SYS.xres", "yres": "SYS.yres",
    "mousx": "SYS.mousx", "mousy": "SYS.mousy",
    "bstatus": "SYS.bstatus", "numframes": "SYS.numframes",
}


class EvalToJS:
    """EVAL AST → JavaScript 转换器"""

    def __init__(self, for_webgl: bool = False):
        self._indent = 0
        self._lines: List[str] = []
        self._used_helpers: Set[str] = set()
        self._user_funcs: Set[str] = set()
        self._declared_vars: Set[str] = set()
        self._static_vars: List[str] = []  # static变量声明(提升到函数外)
        self._static_names: Set[str] = set()  # static/enum变量名集合(用于排除局部变量)
        self._for_webgl = for_webgl  # WebGL模式: 不生成GL/SYS定义

    def convert(self, ast: Dict[str, Any]) -> str:
        """转换AST为JavaScript代码"""
        self._indent = 0
        self._lines = []
        self._used_helpers = set()
        self._user_funcs = set()
        self._declared_vars = set()
        self._static_vars = []
        self._static_names = set()

        # 收集函数名
        self._collect_funcs(ast)

        # 收集所有赋值的变量名(用于自动let声明)
        self._collect_assigned_vars(ast)

        # WebGL模式: 先收集所有static/enum变量名(用于排除局部变量声明)
        if self._for_webgl:
            self._collect_static_names(ast)

        # 生成代码
        if not self._for_webgl:
            self._emit_header()
        self._convert_node(ast)

        # 添加辅助函数
        if self._used_helpers:
            self._emit_helpers()

        # WebGL模式: 在代码前面插入static/enum变量声明
        if self._for_webgl and self._static_vars:
            static_lines = []
            for sv in self._static_vars:
                static_lines.append(sv)
            self._lines = static_lines + [""] + self._lines

        return "\n".join(self._lines)

    def _collect_funcs(self, node):
        if not is_tree(node):
            return
        ntype = get_type(node)
        if ntype in ("named_func", "named_func_empty"):
            ch = children(node)
            if ch:
                name = self._extract_ident(ch[0])
                self._user_funcs.add(name.lower())
        for c in children(node):
            self._collect_funcs(c)

    def _collect_assigned_vars(self, node):
        """收集所有被赋值的变量名(非函数参数、非static、非enum)"""
        if not is_tree(node):
            return
        ntype = get_type(node)
        ch = children(node)

        if ntype == "assign_expr" and len(ch) == 3:
            left = ch[0]
            name = self._extract_ident(left)
            if name:
                key = name.lower()
                # 排除: 常量、内置变量、函数参数
                if key not in _JS_CONST_MAP and key not in ("pi", "e", "rnd", "nrnd"):
                    self._declared_vars.add(name)

        if ntype in ("post_inc", "post_dec"):
            name = self._extract_ident(ch[0]) if ch else ""
            if name:
                self._declared_vars.add(name)

        if ntype in ("pre_inc", "pre_dec"):
            name = get_value(ch[-1]) if ch and is_token(ch[-1]) else (self._extract_ident(ch[-1]) if ch else "")
            if name:
                self._declared_vars.add(name)

        for c in ch:
            self._collect_assigned_vars(c)

    def _collect_static_names(self, node):
        """预收集所有static和enum变量名(用于排除局部变量声明)"""
        if not is_tree(node):
            return
        ntype = get_type(node)
        ch = children(node)

        if ntype == "static_var":
            name = get_value(ch[0]) if ch else ""
            if name:
                self._static_names.add(name.lower())

        if ntype == "enum_entry":
            ech = children(node)
            name = get_value(ech[0]) if ech else ""
            if name:
                self._static_names.add(name.lower())
            # 处理逗号表达式中的多个enum项
            if len(ech) > 1:
                val_node = ech[1]
                if get_type(val_node) == "comma_expr":
                    for cc in children(val_node):
                        ename = self._extract_ident(cc)
                        if ename:
                            self._static_names.add(ename.lower())
                        else:
                            cch2 = children(cc)
                            if cch2:
                                ename = self._extract_ident(cch2[0])
                                if ename:
                                    self._static_names.add(ename.lower())

        for c in ch:
            self._collect_static_names(c)

    def _emit_header(self):
        self._lines.append("// Auto-generated JavaScript code from EVAL AST")
        self._lines.append("")
        self._lines.append("const SYS = { xres: 640, yres: 480, mousx: 0, mousy: 0, bstatus: 0, numframes: 0 };")
        self._lines.append("let _rndState = 0;")
        self._lines.append("const _startTime = performance.now() / 1000;")
        self._lines.append("")

    def _emit_helpers(self):
        self._lines.append("")
        self._lines.append("// ===== Helper functions =====")

        helpers = {
            "_atan": "function _atan(x, ...a) { return a.length ? Math.atan2(x, a[0]) : Math.atan(x); }",
            "_fact": "function _fact(x) { let r = 1; for (let i = 2; i <= x; i++) r *= i; return r; }",
            "_log": "function _log(x, ...a) { return a.length ? Math.log(x) / Math.log(a[0]) : Math.log(x); }",
            "_sgn": "function _sgn(x) { return x < 0 ? -1 : x > 0 ? 1 : 0; }",
            "_unit": "function _unit(x) { return x < 0 ? 0 : x > 0 ? 1 : 0.5; }",
            "_fmod": "function _fmod(x, y) { return x % y; }",
            "_noise": "function _noise(...a) {\n  const x = a[0] || 0, y = a[1] || 0, z = a[2] || 0;\n  let n = (x * 57 + y * 131 + z * 211) & 0x7fffffff;\n  n = (n >> 13) ^ n;\n  n = (n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff;\n  return n / 1073741824.0 - 1.0;\n}",
            "_rgb": "function _rgb(r, g, b) { return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF); }",
            "_rgba": "function _rgba(r, g, b, a) { return ((a & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF); }",
            "_printf": "function _printf(fmt, ...args) {\n  if (fmt.startsWith('\"') && fmt.endsWith('\"')) fmt = fmt.slice(1, -1);\n  fmt = fmt.replace(/\\\\n/g, '\\n').replace(/\\\\t/g, '\\t');\n  let argIdx = 0, out = '';\n  for (let i = 0; i < fmt.length; i++) {\n    if (fmt[i] === '%' && i + 1 < fmt.length) {\n      const spec = fmt[i + 1];\n      if (spec === 'd' || spec === 'i') { out += Math.trunc(args[argIdx++] || 0); i++; }\n      else if ('feEgG'.includes(spec)) { out += args[argIdx++] || 0; i++; }\n      else if (spec === 's') { out += args[argIdx++] || ''; i++; }\n      else if (spec === '%') { out += '%'; i++; }\n      else out += fmt[i];\n    } else out += fmt[i];\n  }\n  if (typeof console !== 'undefined') console.log(out); return 0;\n}",
            "_printg": "function _printg(...args) { console.log('[PRINTG]', ...args); return 0; }",
            "_klock": "function _klock(...a) {\n  if (!a.length || a[0] === 0) return performance.now() / 1000 - _startTime;\n  const d = new Date();\n  const opt = Math.trunc(a[0]);\n  if (opt === 2) return d.getFullYear();\n  if (opt === 3) return d.getMonth() + 1;\n  if (opt === 5) return d.getDate();\n  if (opt === 6) return d.getHours();\n  if (opt === 7) return d.getMinutes();\n  if (opt === 8) return d.getSeconds();\n  return 0;\n}",
            "_glklockstart": "let _glklockStart = 0;\nfunction _glklockstart() { _glklockStart = performance.now() / 1000; return 0; }",
            "_glklockelapsed": "function _glklockelapsed() { return performance.now() / 1000 - _glklockStart; }",
            "_srand": "function _srand(x) { _rndState = Math.trunc(x); return 0; }",
        }

        for name in sorted(self._used_helpers):
            if name in helpers:
                self._lines.append(helpers[name])

    def _line(self, code: str):
        self._lines.append("  " * self._indent + code)

    def _convert_node(self, node):
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
        if not is_tree(node):
            return
        ntype = get_type(node)
        ch = children(node)

        if ntype == "expr_stmt":
            expr = self._convert_expr(ch[0]) if ch else "0"
            self._line(f"{expr};")

        elif ntype in ("named_func", "named_func_empty"):
            self._convert_named_func(node)

        elif ntype == "if_stmt":
            cond = self._convert_expr(ch[0])
            self._line(f"if ({cond}) {{")
            self._indent += 1
            self._convert_stmt(ch[1])
            self._indent -= 1
            if len(ch) >= 3:
                self._line("} else {")
                self._indent += 1
                self._convert_stmt(ch[2])
                self._indent -= 1
            self._line("}")

        elif ntype == "while_stmt":
            cond = self._convert_expr(ch[0])
            self._line(f"while ({cond}) {{")
            self._indent += 1
            self._convert_stmt(ch[1])
            self._indent -= 1
            self._line("}")

        elif ntype == "do_while_stmt":
            self._line("do {")
            self._indent += 1
            self._convert_stmt(ch[0])
            self._indent -= 1
            cond = self._convert_expr(ch[1])
            self._line(f"}} while ({cond});")

        elif ntype == "for_stmt":
            init = self._convert_expr(ch[0]) if len(ch) > 0 and ch[0] else ""
            cond = self._convert_expr(ch[1]) if len(ch) > 1 and ch[1] else "true"
            update = self._convert_expr(ch[2]) if len(ch) > 2 and ch[2] else ""
            body = ch[3] if len(ch) > 3 else None

            self._line(f"for ({init}; {cond}; {update}) {{")
            self._indent += 1
            if body:
                self._convert_stmt(body)
            self._indent -= 1
            self._line("}")

        elif ntype == "block":
            self._line("{")
            self._indent += 1
            for c in ch:
                self._convert_stmt(c)
            self._indent -= 1
            self._line("}")

        elif ntype == "enum_decl":
            self._convert_enum(node)

        elif ntype == "static_decl":
            self._convert_static(node)

        elif ntype in ("anon_func", "anon_func_empty"):
            self._convert_anon_func(node)

        elif ntype == "return_stmt":
            val = self._convert_expr(ch[0]) if ch else "0"
            self._line(f"return {val};")

        elif ntype == "break_stmt":
            self._line("break;")

        elif ntype == "continue_stmt":
            self._line("continue;")

        elif ntype == "goto_stmt":
            label = get_value(ch[0])
            self._line(f"// goto {label} (not supported)")

        elif ntype == "label_stmt":
            label = get_value(ch[0])
            self._line(f"// {label}:")

    def _convert_named_func(self, node):
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
        self._line(f"function {name}({param_str}) {{")
        self._indent += 1

        # 收集函数体内赋值的局部变量
        local_vars = set()
        if body:
            self._collect_local_vars(body, local_vars, set(p.lower() for p in params))
        # 排除参数名和static/enum变量
        param_lower = set(p.lower() for p in params)
        local_vars = {v for v in local_vars if v.lower() not in param_lower and v.lower() not in self._static_names}
        if local_vars:
            self._line(f"let {', '.join(sorted(local_vars))};")

        if body:
            for stmt in children(body):
                self._convert_stmt(stmt)
        else:
            self._line("return 0;")
        self._indent -= 1
        self._line("}")
        self._line("")

    def _convert_anon_func(self, node):
        """转换匿名函数 - WebGL模式下生成hostFrame()"""
        ch = children(node)
        params = []
        body_stmts = []
        for c in ch:
            ntype2 = get_type(c)
            if ntype2 == "func_params":
                params = [self._extract_ident(pc) for pc in children(c)]
            elif ntype2 in ("block", "func_stmts", "func_body"):
                body_stmts = children(c)

        if self._for_webgl:
            # WebGL模式: 匿名函数体 → hostFrame()
            func_name = "hostFrame"
        else:
            func_name = ""  # IIFE

        param_str = ", ".join(params)

        if func_name:
            self._line(f"function {func_name}({param_str}) {{")
        else:
            self._line(f"({param_str}) => {{")
        self._indent += 1

        # 收集局部变量
        local_vars = set()
        for stmt in body_stmts:
            self._collect_local_vars(stmt, local_vars, set(p.lower() for p in params))
        # 排除参数和static/enum变量
        param_lower = set(p.lower() for p in params)
        local_vars = {v for v in local_vars if v.lower() not in param_lower and v.lower() not in self._static_names}
        if local_vars:
            self._line(f"let {', '.join(sorted(local_vars))};")

        for stmt in body_stmts:
            self._convert_stmt(stmt)

        self._indent -= 1
        if func_name:
            self._line("}")
        else:
            self._line("})();")
        self._line("")

    def _collect_local_vars(self, node, vars_set: set, params_lower: set):
        """收集节点内赋值的局部变量"""
        if not is_tree(node):
            return
        ntype = get_type(node)
        ch = children(node)

        if ntype == "assign_expr" and len(ch) == 3:
            name = self._extract_ident(ch[0])
            if name and name.lower() not in params_lower and name.lower() not in _JS_CONST_MAP:
                vars_set.add(name)

        if ntype in ("post_inc", "post_dec"):
            name = self._extract_ident(ch[0]) if ch else ""
            if name and name.lower() not in params_lower:
                vars_set.add(name)

        if ntype in ("pre_inc", "pre_dec"):
            name = get_value(ch[-1]) if ch and is_token(ch[-1]) else (self._extract_ident(ch[-1]) if ch else "")
            if name and name.lower() not in params_lower:
                vars_set.add(name)

        # 不递归进入嵌套函数定义(它们的变量是自己的)
        if ntype not in ("named_func", "named_func_empty", "anon_func", "anon_func_empty"):
            for c in ch:
                self._collect_local_vars(c, vars_set, params_lower)

    def _convert_enum(self, node):
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
                    first_val = self._convert_expr(cch[0])
                    decl = f"const {name} = {first_val};"
                    if self._for_webgl:
                        self._static_vars.append(decl)
                    else:
                        self._line(decl)
                    try:
                        next_val = int(float(first_val)) + 1
                    except:
                        next_val = 0
                    for cc in cch[1:]:
                        ename = self._extract_ident(cc)
                        if ename:
                            decl = f"const {ename} = {next_val};"
                            if self._for_webgl:
                                self._static_vars.append(decl)
                            else:
                                self._line(decl)
                            next_val += 1
                        else:
                            cch2 = children(cc)
                            if cch2:
                                ename = self._extract_ident(cch2[0])
                                if ename and len(cch2) >= 3:
                                    eval_val = self._convert_expr(cch2[2])
                                    decl = f"const {ename} = {eval_val};"
                                    if self._for_webgl:
                                        self._static_vars.append(decl)
                                    else:
                                        self._line(decl)
                                    try:
                                        next_val = int(float(eval_val)) + 1
                                    except:
                                        next_val = 0
                    return
                else:
                    val_expr = self._convert_expr(val_node)
                    decl = f"const {name} = {val_expr};"
                    if self._for_webgl:
                        self._static_vars.append(decl)
                    else:
                        self._line(decl)
                    try:
                        next_val = int(float(val_expr)) + 1
                    except:
                        next_val = 0
            else:
                decl = f"const {name} = {next_val};"
                if self._for_webgl:
                    self._static_vars.append(decl)
                else:
                    self._line(decl)
                next_val += 1

    def _convert_static(self, node):
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
                decl = f"let {name} = [{', '.join(inits)}]; {name}.length = {dim_str};"
            else:
                decl = f"let {name} = new Array({dim_str}).fill(0);"
        else:
            if init_node:
                ich = children(init_node)
                val = self._convert_expr(ich[0]) if ich else "0"
                decl = f"let {name} = {val};"
            else:
                decl = f"let {name} = 0;"

        if self._for_webgl:
            # WebGL模式: static变量提升到函数外(持久化)
            self._static_vars.append(decl)
        else:
            self._line(decl)

    def _convert_expr(self, node) -> str:
        if node is None or not isinstance(node, dict):
            return "0"

        ntype = get_type(node)
        ch = children(node)

        # Token
        if ntype == "NUMBER":
            val = get_value(node)
            if val.startswith("0x") or val.startswith("0X"):
                return val
            return val

        if ntype == "FLOAT_NUM":
            return get_value(node)

        if ntype == "STRING":
            return get_value(node)

        if ntype in ("IDENTIFIER", "PARAM_ID"):
            name = get_value(node)
            key = name.lower()
            if key in _JS_CONST_MAP:
                return _JS_CONST_MAP[key]
            if key == "rnd":
                return "Math.random()"
            if key == "nrnd":
                return "_nrnd()"
            return name

        if is_op_token(node):
            return get_op_value(node)

        if not is_tree(node):
            return "0"

        # 逗号表达式
        if ntype == "comma_expr":
            parts = [self._convert_expr(c) for c in ch]
            if len(parts) == 1:
                return parts[0]
            # JS逗号表达式
            return "(" + ", ".join(parts) + ")"

        # 赋值表达式
        if ntype == "assign_expr":
            if len(ch) == 1:
                return self._convert_expr(ch[0])
            if len(ch) == 3:
                left = self._convert_expr(ch[0])
                op = get_op_value(ch[1]) if is_op_token(ch[1]) else get_value(ch[1])
                right = self._convert_expr(ch[2])
                return f"({left} {op} {right})"

        # 二元运算
        if ntype in ("or_expr", "and_expr", "eq_expr", "cmp_expr",
                      "add_expr", "mul_expr", "pow_expr"):
            if len(ch) == 1:
                return self._convert_expr(ch[0])
            parts = []
            i = 0
            while i < len(ch):
                if is_op_token(ch[i]):
                    parts.append(get_op_value(ch[i]))
                else:
                    parts.append(self._convert_expr(ch[i]))
                i += 1
            return "(" + " ".join(parts) + ")"

        # 一元运算
        if ntype == "unary_expr":
            if len(ch) == 1:
                return self._convert_expr(ch[0])
            if len(ch) == 2:
                op = get_value(ch[0]) if is_token(ch[0]) else ""
                operand = self._convert_expr(ch[1])
                if op == "-":
                    return f"(-{operand})"
                elif op == "!":
                    return f"(!{operand} ? 1 : 0)"
                elif op == "+":
                    return f"(+{operand})"
                elif op == "&":
                    return operand

        # 后缀表达式
        if ntype == "postfix_expr":
            if ch:
                return self._convert_expr(ch[0])
            return "0"

        # 数组访问
        if ntype == "array_access":
            name = self._extract_ident(ch[0])
            index = "0"
            for c in ch[1:]:
                if is_token(c) and get_type(c) in ("LBRACK", "RBRACK"):
                    continue
                index = self._convert_expr(c)
                break
            return f"{name}[{index} & ({name}.length - 1)]"

        # 成员访问 (struct.field)
        if ntype == "member_access":
            obj = self._convert_expr(ch[0])
            field = get_value(ch[1]) if is_token(ch[1]) else self._extract_ident(ch[1])
            return f"{obj}.{field}"

        # 函数调用
        if ntype == "func_call":
            name = self._extract_ident(ch[0])
            args = []
            for c in ch[1:]:
                if get_type(c) == "args":
                    args = [self._convert_expr(ac) for ac in children(c)]
                    break
            key = name.lower()
            if key in _JS_BUILTIN_MAP:
                mapped = _JS_BUILTIN_MAP[key]
                self._used_helpers.add(mapped.split("(")[0].split(".")[0])
                args_str = ", ".join(args)
                return f"{mapped}({args_str})"
            if key in self._user_funcs:
                args_str = ", ".join(args)
                return f"{name}({args_str})"
            args_str = ", ".join(args)
            return f"/* unknown */ {name}({args_str})"

        # 后缀自增/自减
        if ntype == "post_inc":
            name = self._extract_ident(ch[0])
            return f"{name}++"
        if ntype == "post_dec":
            name = self._extract_ident(ch[0])
            return f"{name}--"

        # 前缀自增/自减
        if ntype == "pre_inc":
            name = get_value(ch[-1]) if is_token(ch[-1]) else self._extract_ident(ch[-1])
            return f"++{name}"
        if ntype == "pre_dec":
            name = get_value(ch[-1]) if is_token(ch[-1]) else self._extract_ident(ch[-1])
            return f"--{name}"

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
                body_lines.append(f"  {self._convert_expr(stmt)};")
            body_str = "\n".join(body_lines) if body_lines else "  return 0;"
            return f"({param_str}) => {{\n{body_str}\n}}"

        # 默认穿透
        if ch:
            return self._convert_expr(ch[0])
        return "0"

    def _extract_ident(self, node) -> str:
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
def eval_to_js(ast: Dict[str, Any]) -> str:
    converter = EvalToJS()
    return converter.convert(ast)


def eval_code_to_js(code: str) -> str:
    from eval_parser import EvalParser
    ast = EvalParser.parse(code)
    return eval_to_js(ast)


def pss_to_html(pss_file: str, output_file: str = None) -> str:
    """将PSS文件转换为完整的HTML+WebGL页面

    Args:
        pss_file: PSS文件路径
        output_file: 输出HTML文件路径(可选)
    """
    from pss_parser import parse_pss_file
    from eval_parser import EvalParser

    pss = parse_pss_file(pss_file)
    host_code = pss.host_code

    # 转换host代码 (WebGL模式)
    js_host = ""
    if host_code.strip():
        ast = EvalParser.parse(host_code)
        converter = EvalToJS(for_webgl=True)
        js_host = converter.convert(ast)

    # 收集shader代码
    vertex_shaders = []
    fragment_shaders = []
    for b in pss.vertex_blocks:
        vertex_shaders.append(b.code)
    for b in pss.fragment_blocks:
        fragment_shaders.append(b.code)

    # 生成shader源码字符串
    vs_src = json.dumps(vertex_shaders) if vertex_shaders else "[]"
    fs_src = json.dumps(fragment_shaders) if fragment_shaders else "[]"

    # 生成HTML
    html = f"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>PSS Viewer - {os.path.basename(pss_file)}</title>
<style>
  body {{ margin: 0; overflow: hidden; background: #000; }}
  canvas {{ display: block; }}
  #info {{ position: absolute; top: 8px; left: 8px; color: #0f0; font: 12px monospace; }}
</style>
</head>
<body>
<canvas id="c"></canvas>
<div id="info"></div>
<script>
// ===== Shader Sources =====
const VERTEX_SHADERS = {vs_src};
const FRAGMENT_SHADERS = {fs_src};

// ===== WebGL Setup =====
const canvas = document.getElementById('c');
const gl = canvas.getContext('webgl2') || canvas.getContext('webgl') || canvas.getContext('experimental-webgl');
const isWebGL2 = (gl instanceof WebGL2RenderingContext);
const W = 640, H = 640;
canvas.width = W; canvas.height = H;
gl.viewport(0, 0, W, H);
gl.enable(gl.DEPTH_TEST);
gl.clearColor(0, 0, 0, 1);

// ===== Matrix Library =====
const M4 = {{
  create() {{ return new Float32Array(16); }},
  identity() {{ const m = M4.create(); m[0]=m[5]=m[10]=m[15]=1; return m; }},
  multiply(a, b) {{
    const o = M4.create();
    for (let i=0;i<4;i++) for (let j=0;j<4;j++) {{
      o[j*4+i] = a[i]*b[j*4] + a[4+i]*b[j*4+1] + a[8+i]*b[j*4+2] + a[12+i]*b[j*4+3];
    }}
    return o;
  }},
  perspective(fovy, aspect, zn, zf) {{
    const f = 1.0 / Math.tan(fovy * Math.PI / 360);
    const m = M4.create();
    m[0] = f / aspect; m[5] = f;
    m[10] = (zf+zn)/(zn-zf); m[11] = -1;
    m[14] = 2*zf*zn/(zn-zf);
    return m;
  }},
  lookAt(eye, center, up) {{
    let fx=center[0]-eye[0], fy=center[1]-eye[1], fz=center[2]-eye[2];
    let fl=Math.sqrt(fx*fx+fy*fy+fz*fz); fx/=fl; fy/=fl; fz/=fl;
    let sx=fy*up[2]-fz*up[1], sy=fz*up[0]-fx*up[2], sz=fx*up[1]-fy*up[0];
    let sl=Math.sqrt(sx*sx+sy*sy+sz*sz); sx/=sl; sy/=sl; sz/=sl;
    let ux=sy*fz-sz*fy, uy=sz*fx-sx*fz, uz=sx*fy-sy*fx;
    const m = M4.identity();
    m[0]=sx; m[1]=ux; m[2]=-fx;
    m[4]=sy; m[5]=uy; m[6]=-fy;
    m[8]=sz; m[9]=uz; m[10]=-fz;
    m[12]=-(sx*eye[0]+sy*eye[1]+sz*eye[2]);
    m[13]=-(ux*eye[0]+uy*eye[1]+uz*eye[2]);
    m[14]= (fx*eye[0]+fy*eye[1]+fz*eye[2]);
    return m;
  }},
  translate(x, y, z) {{
    const m = M4.identity(); m[12]=x; m[13]=y; m[14]=z; return m;
  }},
  rotate(angle, x, y, z) {{
    const r = angle * Math.PI / 180;
    const c = Math.cos(r), s = Math.sin(r);
    let l = Math.sqrt(x*x+y*y+z*z); x/=l; y/=l; z/=l;
    const m = M4.identity();
    m[0]=x*x*(1-c)+c;   m[1]=y*x*(1-c)+z*s; m[2]=x*z*(1-c)-y*s;
    m[4]=x*y*(1-c)-z*s; m[5]=y*y*(1-c)+c;   m[6]=y*z*(1-c)+x*s;
    m[8]=x*z*(1-c)+y*s; m[9]=y*z*(1-c)-x*s; m[10]=z*z*(1-c)+c;
    return m;
  }},
  scale(x, y, z) {{
    const m = M4.create(); m[0]=x; m[5]=y; m[10]=z; m[15]=1; return m;
  }},
  transformVec4(m, v) {{
    return [
      m[0]*v[0]+m[4]*v[1]+m[8]*v[2]+m[12]*v[3],
      m[1]*v[0]+m[5]*v[1]+m[9]*v[2]+m[13]*v[3],
      m[2]*v[0]+m[6]*v[1]+m[10]*v[2]+m[14]*v[3],
      m[3]*v[0]+m[7]*v[1]+m[11]*v[2]+m[15]*v[3]
    ];
  }}
}};

// ===== GL State =====
const GL = {{
  _modelStack: [M4.identity()],
  _projStack: [M4.identity()],
  _mode: 'modelview',
  _curColor: [0.5, 0.5, 0.5, 1.0],
  _curNormal: [0, 0, 1],
  _curTexCoord: [0, 0, 0],
  _verts: [],
  _drawMode: 0,
  _inBegin: false,
  _program: null,
  _uniforms: {{}},
  _uniformLocs: {{}},
  _gfov: 90.0,

  get _model() {{ return this._modelStack[this._modelStack.length-1]; }},
  get _proj() {{ return this._projStack[this._projStack.length-1]; }},

  begin(mode) {{ this._inBegin = true; this._drawMode = mode; this._verts = []; }},
  end() {{
    if (!this._inBegin) return;
    this._inBegin = false;
    if (this._verts.length === 0) return;
    this._flush();
  }},
  vertex(...a) {{
    const x=a[0]||0, y=a[1]||0, z=a[2]||0, w=a[3]||1;
    // Store raw vertex position (gl_Vertex = untransformed)
    this._verts.push({{
      pos: [x, y, z, w],
      color: [...this._curColor],
      normal: [...this._curNormal],
      texcoord: [...this._curTexCoord]
    }});
  }},
  color(...a) {{
    this._curColor = [a[0]||0, a[1]||0, a[2]||0, a[3]!==undefined?a[3]:1.0];
  }},
  normal(...a) {{ this._curNormal = [a[0]||0, a[1]||0, a[2]||0]; }},
  texCoord(...a) {{ this._curTexCoord = [a[0]||0, a[1]||0, a[2]||0]; }},
  pushMatrix() {{
    if (this._mode === 'modelview') this._modelStack.push([...this._model]);
    else this._projStack.push([...this._proj]);
  }},
  popMatrix() {{
    if (this._mode === 'modelview') {{ if (this._modelStack.length>1) this._modelStack.pop(); }}
    else {{ if (this._projStack.length>1) this._projStack.pop(); }}
  }},
  translate(x, y, z) {{
    if (this._mode === 'modelview')
      this._modelStack[this._modelStack.length-1] = M4.multiply(this._model, M4.translate(x,y,z));
    else
      this._projStack[this._projStack.length-1] = M4.multiply(this._proj, M4.translate(x,y,z));
  }},
  rotate(a, x, y, z) {{
    if (this._mode === 'modelview')
      this._modelStack[this._modelStack.length-1] = M4.multiply(this._model, M4.rotate(a,x,y,z));
    else
      this._projStack[this._projStack.length-1] = M4.multiply(this._proj, M4.rotate(a,x,y,z));
  }},
  scale(x, y, z) {{
    if (this._mode === 'modelview')
      this._modelStack[this._modelStack.length-1] = M4.multiply(this._model, M4.scale(x,y,z));
    else
      this._projStack[this._projStack.length-1] = M4.multiply(this._proj, M4.scale(x,y,z));
  }},
  lookAt(...a) {{
    this._modelStack[this._modelStack.length-1] = M4.multiply(
      this._model, M4.lookAt([a[0],a[1],a[2]], [a[3],a[4],a[5]], [a[6],a[7],a[8]]));
  }},
  perspective(fovy, aspect, zn, zf) {{
    this._projStack[this._projStack.length-1] = M4.perspective(fovy, aspect, zn, zf);
  }},
  setFov(fovy) {{
    this._gfov = fovy;
    this._projStack[this._projStack.length-1] = M4.perspective(fovy, W/H, 0.1, 1000.0);
  }},
  setShader(id) {{ /* shader already compiled, just track current */ }},
  getUniformLoc(name) {{
    // Store name for later lookup in _flush
    if (!(name in this._uniformLocs)) this._uniformLocs[name] = Object.keys(this._uniformLocs).length;
    return this._uniformLocs[name];
  }},
  uniform1i(loc, val) {{ this._uniforms[loc] = val; }},
  uniform1f(loc, val) {{ this._uniforms[loc] = val; }},
  uniform(...a) {{ /* TODO */ }},
  quad(size) {{
    this.begin(0x0007);
    this.vertex(-size,-size,0); this.vertex(+size,-size,0);
    this.vertex(+size,+size,0); this.vertex(-size,+size,0);
    this.end();
  }},
  cullFace(mode) {{ if(mode) gl.enable(gl.CULL_FACE); else gl.disable(gl.CULL_FACE); }},
  lineWidth(w) {{ gl.lineWidth(w); }},
  alphaEnable() {{ gl.enable(gl.BLEND); gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA); }},
  alphaDisable() {{ gl.disable(gl.BLEND); }},
  blendFunc(s, d) {{ gl.blendFunc(s, d); }},
  enable(cap) {{ if(cap===0x0B71) gl.enable(gl.DEPTH_TEST); }},
  disable(cap) {{ if(cap===0x0B71) gl.disable(gl.DEPTH_TEST); }},
  setTex(...a) {{ return 0; }},
  getTex(id) {{ return null; }},
  activeTexture(unit) {{}},
  bindTexture(id) {{}},
  capture() {{}},
  captureEnd(id) {{}},

  _flush() {{
    if (!this._program || this._verts.length === 0) return;
    const prog = this._program;
    const mvp = M4.multiply(this._proj, this._model);

    // Build vertex data
    const n = this._verts.length;
    const posData = new Float32Array(n * 4);
    const colData = new Float32Array(n * 4);
    const nrmData = new Float32Array(n * 3);
    const texData = new Float32Array(n * 3);

    for (let i = 0; i < n; i++) {{
      const v = this._verts[i];
      // Transform position by modelview for shader (v = gl_Vertex in eye space)
      posData[i*4]   = v.pos[0];
      posData[i*4+1] = v.pos[1];
      posData[i*4+2] = v.pos[2];
      posData[i*4+3] = v.pos[3];
      colData[i*4]   = v.color[0];
      colData[i*4+1] = v.color[1];
      colData[i*4+2] = v.color[2];
      colData[i*4+3] = v.color[3];
      nrmData[i*3]   = v.normal[0];
      nrmData[i*3+1] = v.normal[1];
      nrmData[i*3+2] = v.normal[2];
      texData[i*3]   = v.texcoord[0];
      texData[i*3+1] = v.texcoord[1];
      texData[i*3+2] = v.texcoord[2];
    }}

    // Upload buffers
    function uploadBuf(loc, data, size) {{
      if (loc < 0) return;
      const buf = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, buf);
      gl.bufferData(gl.ARRAY_BUFFER, data, gl.DYNAMIC_DRAW);
      gl.enableVertexAttribArray(loc);
      gl.vertexAttribPointer(loc, size, gl.FLOAT, false, 0, 0);
    }}

    gl.useProgram(prog);
    uploadBuf(prog.aPosition, posData, 4);
    uploadBuf(prog.aColor, colData, 4);
    uploadBuf(prog.aNormal, nrmData, 3);
    uploadBuf(prog.aTexCoord, texData, 3);

    // Set uniforms
    const mvLoc = gl.getUniformLocation(prog, 'uModelView');
    const pLoc = gl.getUniformLocation(prog, 'uProjection');
    const mvpLoc = gl.getUniformLocation(prog, 'uMVP');
    if (mvLoc) gl.uniformMatrix4fv(mvLoc, false, this._model);
    if (pLoc) gl.uniformMatrix4fv(pLoc, false, this._proj);
    if (mvpLoc) gl.uniformMatrix4fv(mvpLoc, false, mvp);

    // Set custom uniforms (by name, respecting int vs float type)
    // Build a name->type map from active uniforms
    const _uTypes = {{}};
    const _numU = gl.getProgramParameter(prog, gl.ACTIVE_UNIFORMS);
    for (let _ui = 0; _ui < _numU; _ui++) {{
      const _uInfo = gl.getActiveUniform(prog, _ui);
      if (_uInfo) _uTypes[_uInfo.name] = _uInfo.type;
    }}
    for (const [name, idx] of Object.entries(this._uniformLocs)) {{
      if (idx in this._uniforms) {{
        const uLoc = gl.getUniformLocation(prog, name);
        if (uLoc) {{
          const val = this._uniforms[idx];
          if (_uTypes[name] === gl.INT) {{
            gl.uniform1i(uLoc, Math.trunc(val));
          }} else {{
            gl.uniform1f(uLoc, val);
          }}
        }}
      }}
    }}

    // Draw
    const mode = this._drawMode;
    let glMode;
    switch(mode) {{
      case 0x0000: glMode = gl.POINTS; break;
      case 0x0001: glMode = gl.LINES; break;
      case 0x0002: glMode = gl.LINE_LOOP; break;
      case 0x0003: glMode = gl.LINE_STRIP; break;
      case 0x0004: glMode = gl.TRIANGLES; break;
      case 0x0005: glMode = gl.TRIANGLE_STRIP; break;
      case 0x0006: glMode = gl.TRIANGLE_FAN; break;
      case 0x0007: glMode = gl.TRIANGLE_FAN; break; // QUADS as FAN
      default: glMode = gl.TRIANGLES;
    }}
    gl.drawArrays(glMode, 0, n);
  }}
}};

const SYS = {{ xres: W, yres: H, mousx: 0, mousy: 0, bstatus: 0, numframes: 0 }};
const _startTime = performance.now() / 1000;

// ===== Mouse =====
canvas.addEventListener('mousemove', e => {{
  const r = canvas.getBoundingClientRect();
  SYS.mousx = (e.clientX - r.left) * W / r.width;
  SYS.mousy = (e.clientY - r.top) * H / r.height;
}});
canvas.addEventListener('mousedown', e => {{ SYS.bstatus = e.buttons; }});
canvas.addEventListener('mouseup', e => {{ SYS.bstatus = e.buttons; }});

// ===== Compile Shaders =====
function compileShader(src, type) {{
  const s = gl.createShader(type);
  gl.shaderSource(s, src);
  gl.compileShader(s);
  if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {{
    const err = gl.getShaderInfoLog(s);
    console.error('Shader error:', err);
    document.getElementById('info').textContent = 'Shader error: ' + err;
    return null;
  }}
  return s;
}}

function createProgram(vsSrc, fsSrc) {{
  const vs = compileShader(vsSrc, gl.VERTEX_SHADER);
  const fs = compileShader(fsSrc, gl.FRAGMENT_SHADER);
  if (!vs || !fs) return null;
  const prog = gl.createProgram();
  gl.attachShader(prog, vs);
  gl.attachShader(prog, fs);
  gl.linkProgram(prog);
  if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {{
    const err = gl.getProgramInfoLog(prog);
    console.error('Program link error:', err);
    document.getElementById('info').textContent = 'Link error: ' + err;
    return null;
  }}
  // Cache attribute locations
  prog.aPosition = gl.getAttribLocation(prog, 'aPosition');
  prog.aColor = gl.getAttribLocation(prog, 'aColor');
  prog.aNormal = gl.getAttribLocation(prog, 'aNormal');
  prog.aTexCoord = gl.getAttribLocation(prog, 'aTexCoord');
  return prog;
}}

// Build shader program from PSS vertex+fragment blocks
let shaderProgram = null;
if (VERTEX_SHADERS.length > 0 && FRAGMENT_SHADERS.length > 0) {{
  let vsCode = VERTEX_SHADERS.join('\\n');
  let fsCode = FRAGMENT_SHADERS.join('\\n');

  if (isWebGL2) {{
    // WebGL2: GLSL 300 es - full support for gl_FragDepth, uniform int, etc.
    vsCode = vsCode.replace(/ftransform\\(\\)/g, 'uMVP * aPosition');
    vsCode = vsCode.replace(/gl_Vertex/g, 'aPosition');
    vsCode = vsCode.replace(/gl_Normal/g, 'aNormal');
    vsCode = vsCode.replace(/gl_Color/g, 'aColor');
    vsCode = vsCode.replace(/gl_MultiTexCoord0/g, 'vec4(aTexCoord, 1.0)');
    vsCode = vsCode.replace(/varying\\s+/g, 'out ');
    vsCode = '#version 300 es\\nin vec4 aPosition;\\nin vec4 aColor;\\nin vec3 aNormal;\\nin vec3 aTexCoord;\\nuniform mat4 uModelView;\\nuniform mat4 uProjection;\\nuniform mat4 uMVP;\\n' + vsCode;

    fsCode = fsCode.replace(/varying\\s+/g, 'in ');
    fsCode = fsCode.replace(/gl_FragColor/g, 'fragColor');
    fsCode = '#version 300 es\\nprecision mediump float;\\nout vec4 fragColor;\\n' + fsCode;
  }} else {{
    // WebGL1: GLSL 100
    vsCode = 'attribute vec4 aPosition;\\nattribute vec4 aColor;\\nattribute vec3 aNormal;\\nattribute vec3 aTexCoord;\\nuniform mat4 uModelView;\\nuniform mat4 uProjection;\\nuniform mat4 uMVP;\\n' + vsCode;
    vsCode = vsCode.replace(/ftransform\\(\\)/g, 'uMVP * aPosition');
    vsCode = vsCode.replace(/gl_Vertex/g, 'aPosition');
    vsCode = vsCode.replace(/gl_Normal/g, 'aNormal');
    vsCode = vsCode.replace(/gl_Color/g, 'aColor');
    vsCode = vsCode.replace(/gl_MultiTexCoord0/g, 'vec4(aTexCoord, 1.0)');

    const hasFragDepth = !!gl.getExtension('EXT_frag_depth');
    let fsHeader = '';
    if (hasFragDepth) fsHeader += '#extension GL_EXT_frag_depth : enable\\n';
    fsHeader += 'precision mediump float;\\n';
    fsCode = fsHeader + fsCode;
    fsCode = fsCode.replace(/uniform\\s+int\\s+/g, 'uniform float ');
    fsCode = fsCode.replace(/(bstatus\\s*!=\\s*)(\\d+)(?!\\.)/g, '$1$2.0');
    fsCode = fsCode.replace(/(bstatus\\s*==\\s*)(\\d+)(?!\\.)/g, '$1$2.0');
    if (!hasFragDepth) {{
      fsCode = fsCode.replace(/gl_FragDepth\\s*=\\s*[^;]+;/g, '/* gl_FragDepth not supported */');
    }}
  }}

  shaderProgram = createProgram(vsCode, fsCode);
  if (shaderProgram) {{
    GL._program = shaderProgram;
    console.log('Shader compiled (' + (isWebGL2 ? 'WebGL2/GLSL300' : 'WebGL1/GLSL100') + ')');
  }}
}}

// ===== Host Code =====
{js_host}

// ===== Animation Loop =====
function animate() {{
  // Reset modelview each frame
  GL._modelStack = [M4.identity()];
  // Reset projection each frame (like original engine: gluPerspective(gfov, aspect, 0.1, 1000))
  GL._projStack = [M4.perspective(GL._gfov, W/H, 0.1, 1000.0)];

  gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

  // Execute host frame code (numframes incremented AFTER, so hostFrame sees 0 on first call)
  if (typeof hostFrame === 'function') hostFrame();
  SYS.numframes++;

  requestAnimationFrame(animate);
}}

// Start
animate();
</script>
</body>
</html>"""

    if output_file:
        with open(output_file, 'w') as f:
            f.write(html)

    return html


# ===== 测试 =====
if __name__ == "__main__":
    import sys

    tests = [
        ("1 + 2;", "简单加法"),
        ("x = 3; y = 4; x * y;", "变量赋值"),
        ("for(i=0;i<5;i++) printf(\"%d \", i);", "for循环"),
        ("if (1) printf(\"yes\"); else printf(\"no\");", "if-else"),
        ("x = sqrt(2); printf(\"%g\", x);", "内置函数"),
        ("static buf[4] = {1,2,3,4};", "static数组"),
        ("enum {A=1, B, C};", "enum"),
        ("cube(x) { return x*x*x; }", "命名函数"),
        ("(x) x+1", "匿名函数"),
    ]

    print("=== EVAL → JavaScript 转换测试 ===")
    for code, desc in tests:
        try:
            js = eval_code_to_js(code)
            print(f"\n--- {desc} ---")
            print(js)
        except Exception as e:
            print(f"FAIL: {desc}: {e}")
