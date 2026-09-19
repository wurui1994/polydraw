"""GL内置函数绑定 - 将EVAL解释器的GL调用映射到GLContext

提供EVAL解释器所需的GL函数实现, 包括:
- 绘图函数 (glBegin/glEnd/glVertex/glColor/...)
- 矩阵操作 (glPushMatrix/glTranslate/glRotate/...)
- 纹理管理 (glSetTex/glGetTex/...)
- Shader管理 (glSetShader/glUniform/...)
- 系统函数 (klock/srand/noise/printf/...)
"""
import math
import random
import time
from typing import Dict, Any, Callable, Optional

from .gl_context import GLContext, GLConst


class GLBuiltins:
    """GL内置函数注册表"""

    def __init__(self, ctx: GLContext, debug: bool = False):
        self.ctx = ctx
        self.debug = debug
        self._start_time = time.time()
        self._glklock_start = 0.0
        self._random = random.Random(0)
        self._numframes = 0

        # 系统变量
        self.sys_vars: Dict[str, Any] = {
            "xres": float(ctx.width),
            "yres": float(ctx.height),
            "mousx": 0.0,
            "mousy": 0.0,
            "bstatus": 0.0,
            "numframes": 0.0,
        }

        # GL常量
        self.gl_consts: Dict[str, float] = {
            "gl_points": float(GLConst.POINTS),
            "gl_lines": float(GLConst.LINES),
            "gl_line_loop": float(GLConst.LINE_LOOP),
            "gl_line_strip": float(GLConst.LINE_STRIP),
            "gl_triangles": float(GLConst.TRIANGLES),
            "gl_triangle_strip": float(GLConst.TRIANGLE_STRIP),
            "gl_triangle_fan": float(GLConst.TRIANGLE_FAN),
            "gl_quads": float(GLConst.QUADS),
            "gl_quad_strip": float(GLConst.QUAD_STRIP),
            "gl_polygon": float(GLConst.POLYGON),
            "gl_depth_test": float(GLConst.DEPTH_TEST),
            "gl_none": float(GLConst.NONE),
            "gl_front": float(GLConst.FRONT),
            "gl_back": float(GLConst.BACK),
            "gl_front_and_back": float(GLConst.FRONT_AND_BACK),
            "gl_texture0": float(GLConst.TEXTURE0),
            "gl_src_alpha": float(GLConst.SRC_ALPHA),
            "gl_one_minus_src_alpha": float(GLConst.ONE_MINUS_SRC_ALPHA),
            "kg_bgra32": float(GLConst.KGL_BGRA32),
            "kg_short": float(GLConst.KGL_SHORT),
            "kg_int": float(GLConst.KGL_INT),
            "kg_float": float(GLConst.KGL_FLOAT),
            "kg_vec4": float(GLConst.KGL_VEC4),
        }

        # 数学常量
        self.math_consts: Dict[str, float] = {
            "pi": math.pi,
            "e": math.e,
        }

    def get_all_consts(self) -> Dict[str, float]:
        """获取所有常量"""
        result = {}
        result.update(self.math_consts)
        result.update(self.gl_consts)
        return result

    def get_all_functions(self) -> Dict[str, Callable]:
        """获取所有内置函数"""
        funcs = {}

        # ===== 数学函数 =====
        funcs["abs"] = lambda x: abs(x)
        funcs["acos"] = lambda x: math.acos(x)
        funcs["asin"] = lambda x: math.asin(x)
        funcs["atan"] = lambda x, *a: math.atan2(x, a[0]) if a else math.atan(x)
        funcs["atn"] = funcs["atan"]
        funcs["atan2"] = lambda y, x: math.atan2(y, x)
        funcs["ceil"] = lambda x: float(math.ceil(x))
        funcs["cos"] = lambda x: math.cos(x)
        funcs["exp"] = lambda x: math.exp(x)
        funcs["fabs"] = lambda x: abs(x)
        funcs["fact"] = lambda x: float(math.gamma(x + 1))
        funcs["floor"] = lambda x: float(math.floor(x))
        funcs["int"] = lambda x: float(int(x))
        funcs["log"] = lambda x, *a: math.log(x, a[0]) if a else math.log(x)
        funcs["sgn"] = lambda x: -1.0 if x < 0 else (1.0 if x > 0 else 0.0)
        funcs["sin"] = lambda x: math.sin(x)
        funcs["sqrt"] = lambda x: math.sqrt(x)
        funcs["tan"] = lambda x: math.tan(x)
        funcs["unit"] = lambda x: 0.0 if x < 0 else (1.0 if x > 0 else 0.5)
        funcs["fmod"] = lambda x, y: math.fmod(x, y)
        funcs["min"] = lambda x, y: min(x, y)
        funcs["max"] = lambda x, y: max(x, y)
        funcs["pow"] = lambda x, y: x ** y
        funcs["noise"] = lambda *a: self._builtin_noise(*a)
        funcs["rgb"] = lambda r, g, b: float((int(r) & 0xFF) << 16 | (int(g) & 0xFF) << 8 | (int(b) & 0xFF))
        funcs["rgba"] = lambda r, g, b, a: float((int(a) & 0xFF) << 24 | (int(r) & 0xFF) << 16 | (int(g) & 0xFF) << 8 | (int(b) & 0xFF))

        # ===== 系统函数 =====
        funcs["klock"] = lambda *a: self._builtin_klock(*a)
        funcs["glklockstart"] = lambda: self._builtin_glklockstart()
        funcs["glklockelapsed"] = lambda: self._builtin_glklockelapsed()
        funcs["srand"] = lambda x: self._builtin_srand(x)
        funcs["printf"] = lambda *a: self._builtin_printf(*a)
        funcs["printg"] = lambda *a: self._builtin_printg(*a)
        funcs["sleep"] = lambda x: time.sleep(x) or 0.0

        # ===== GL绘图函数 =====
        funcs["glbegin"] = lambda mode: self.ctx.gl_begin(int(mode))
        funcs["glend"] = lambda: self.ctx.gl_end()
        funcs["glvertex"] = lambda *a: self.ctx.gl_vertex(*[float(x) for x in a])
        funcs["gltexcoord"] = lambda *a: self.ctx.gl_texcoord(*[float(x) for x in a])
        funcs["glcolor"] = lambda *a: self.ctx.gl_color(*[float(x) for x in a])
        funcs["glnormal"] = lambda *a: self.ctx.gl_normal(*[float(x) for x in a])

        # ===== GL矩阵函数 =====
        funcs["glpushmatrix"] = lambda: self.ctx.gl_push_matrix()
        funcs["glpopmatrix"] = lambda: self.ctx.gl_pop_matrix()
        funcs["gltranslate"] = lambda x, y, z=0: self.ctx.gl_translate(float(x), float(y), float(z))
        funcs["glrotate"] = lambda a, x, y, z: self.ctx.gl_rotate(float(a), float(x), float(y), float(z))
        funcs["glscale"] = lambda x, y, z=1: self.ctx.gl_scale(float(x), float(y), float(z))
        funcs["glulookat"] = lambda *a: self.ctx.gl_look_at(*[float(x) for x in a])
        funcs["gluperspective"] = lambda fovy, asp, zn, zf: self.ctx.gl_perspective(float(fovy), float(asp), float(zn), float(zf))
        funcs["glfrustum"] = lambda l, r, b, t, zn, zf: self.ctx.gl_frustum(float(l), float(r), float(b), float(t), float(zn), float(zf))
        funcs["glortho"] = lambda l, r, b, t, zn=-1, zf=1: self.ctx.gl_ortho(float(l), float(r), float(b), float(t), float(zn), float(zf))
        funcs["setfov"] = lambda fovy: self.ctx.gl_perspective(float(fovy), self.ctx.width / self.ctx.height, 0.1, 100.0)

        # ===== GL纹理函数 =====
        funcs["glsettex"] = lambda *a: self._gl_settex(*a)
        funcs["glgettex"] = lambda tid: self.ctx.gl_get_tex(int(tid))
        funcs["glactivetexture"] = lambda unit: self.ctx.gl_active_texture(int(unit))
        funcs["glbindtexture"] = lambda tid: self.ctx.gl_bind_texture(int(tid))

        # ===== GL Shader函数 =====
        funcs["glsetshader"] = lambda pid: self.ctx.gl_set_shader(int(pid))
        funcs["glgetuniformloc"] = lambda name: self.ctx.gl_get_uniform_loc(str(name))
        funcs["gluniform1i"] = lambda loc, val: self.ctx.gl_uniform1i(int(loc), int(val))
        funcs["gluniform1f"] = lambda loc, val: self.ctx.gl_uniform1f(int(loc), float(val))
        funcs["gluniform"] = lambda *a: self.ctx.gl_uniform(*[int(a[0])] + [float(x) for x in a[1:]])
        funcs["glgetattribloc"] = lambda *a: 0
        funcs["glvertexattrib"] = lambda *a: 0.0

        # ===== GL状态函数 =====
        funcs["glquad"] = lambda size=1.0: self.ctx.gl_quad(float(size))
        funcs["gltextdisable"] = lambda: 0.0
        funcs["glcullface"] = lambda mode: self.ctx.gl_cull_face(int(mode))
        funcs["gllinewidth"] = lambda w: setattr(self.ctx, 'line_width', float(w)) or 0.0
        funcs["glalphaenable"] = lambda: self.ctx.gl_alpha_enable()
        funcs["glalphadisable"] = lambda: self.ctx.gl_alpha_disable()
        funcs["glblendfunc"] = lambda s, d: self.ctx.gl_blend_func(int(s), int(d))
        funcs["glenable"] = lambda cap: self.ctx.gl_enable(int(cap))
        funcs["gldisable"] = lambda cap: self.ctx.gl_disable(int(cap))

        # ===== GL Capture =====
        funcs["glcapture"] = lambda: self.ctx.gl_capture()
        funcs["glcaptureend"] = lambda tid: self.ctx.gl_capture_end(int(tid))

        # ===== 其他 =====
        funcs["glswapinterval"] = lambda x: 0.0
        funcs["playnote"] = lambda *a: 0.0
        funcs["mountzip"] = lambda *a: 0.0
        funcs["glprogramlocalparam"] = lambda *a: 0.0
        funcs["glprogramenvparam"] = lambda *a: 0.0

        return funcs

    # ===== 内置函数实现 =====

    def _builtin_klock(self, *args):
        if not args or args[0] == 0:
            return time.time() - self._start_time
        opt = int(args[0])
        t = time.localtime()
        if opt == 1:
            return float(f"{t.tm_year}{t.tm_mon:02d}{t.tm_mday:02d}{t.tm_hour:02d}{t.tm_min:02d}{t.tm_sec:02d}")
        elif opt == 2: return float(t.tm_year)
        elif opt == 3: return float(t.tm_mon)
        elif opt == 4: return float(t.tm_wday)
        elif opt == 5: return float(t.tm_mday)
        elif opt == 6: return float(t.tm_hour)
        elif opt == 7: return float(t.tm_min)
        elif opt == 8: return float(t.tm_sec)
        elif opt == 9: return float(int((time.time() % 1) * 1000))
        return 0.0

    def _builtin_glklockstart(self):
        self._glklock_start = time.time()
        return 0.0

    def _builtin_glklockelapsed(self):
        return time.time() - self._glklock_start

    def _builtin_srand(self, seed):
        self._random.seed(int(seed))
        return 0.0

    def _builtin_noise(self, *args):
        x = args[0] if len(args) > 0 else 0
        y = args[1] if len(args) > 1 else 0
        z = args[2] if len(args) > 2 else 0
        n = int(x * 57 + y * 131 + z * 211) & 0x7fffffff
        n = (n >> 13) ^ n
        n = (n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff
        return float(n) / 1073741824.0 - 1.0

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

    def _builtin_printg(self, *args):
        """printg: 输出到图形overlay"""
        text = " ".join(str(a) for a in args)
        if self.debug:
            print(f"[PRINTG] {text}")
        return 0.0

    def _gl_settex(self, *args):
        """glSetTex: 设置纹理"""
        if len(args) == 1:
            # glsettex(texid) - 无操作
            return 0.0
        if len(args) == 2:
            # glsettex(texid, "filename")
            tex_id = int(args[0])
            if isinstance(args[1], str):
                return float(self.ctx.gl_set_tex(tex_id, args[1]))
            # glsettex(texid, data_array)
            return float(self.ctx.gl_set_tex(tex_id, args[1]))
        if len(args) >= 5:
            # glsettex(texid, data, w, h, fmt)
            tex_id = int(args[0])
            return float(self.ctx.gl_set_tex(tex_id, args[1], int(args[2]), int(args[3]), int(args[4])))
        return 0.0

    def get_rnd(self) -> float:
        """获取随机数 (rnd变量)"""
        return self._random.random()

    def get_nrnd(self) -> float:
        """获取正态随机数 (nrnd变量)"""
        return self._random.gauss(0, 1)

    def advance_frame(self):
        """推进帧计数"""
        self._numframes += 1
        self.sys_vars["numframes"] = float(self._numframes)
