"""GL图形环境 - 矩阵栈、纹理、shader状态管理

模拟OpenGL固定管线状态机, 为EVAL解释器提供GL函数的实际实现。
支持: 矩阵变换、顶点提交、纹理管理、shader绑定、混合模式等。
"""
import math
import numpy as np
from typing import Dict, List, Any, Optional, Tuple
from enum import IntEnum


# ===== GL常量 =====
class GLConst(IntEnum):
    POINTS = 0x0000
    LINES = 0x0001
    LINE_LOOP = 0x0002
    LINE_STRIP = 0x0003
    TRIANGLES = 0x0004
    TRIANGLE_STRIP = 0x0005
    TRIANGLE_FAN = 0x0006
    QUADS = 0x0007
    QUAD_STRIP = 0x0008
    POLYGON = 0x0009

    DEPTH_TEST = 0x0B71
    NONE = 0
    FRONT = 0x0404
    BACK = 0x0405
    FRONT_AND_BACK = 0x0408

    TEXTURE0 = 0x84C0
    SRC_ALPHA = 0x0302
    ONE_MINUS_SRC_ALPHA = 0x0303

    # Ken's custom types
    KGL_BGRA32 = 1
    KGL_SHORT = 2
    KGL_INT = 3
    KGL_FLOAT = 4
    KGL_VEC4 = 5


# ===== 矩阵栈 =====
class MatrixStack:
    """4x4矩阵栈, 模拟OpenGL矩阵操作"""

    def __init__(self):
        self._stack: List[np.ndarray] = [np.eye(4, dtype=np.float64)]
        self._mode: str = "modelview"  # 'modelview' | 'projection'

    @property
    def current(self) -> np.ndarray:
        return self._stack[-1]

    def push(self):
        self._stack.append(self._stack[-1].copy())

    def pop(self):
        if len(self._stack) > 1:
            self._stack.pop()

    def multiply(self, m: np.ndarray):
        self._stack[-1] = self._stack[-1] @ m

    def load_identity(self):
        self._stack[-1] = np.eye(4, dtype=np.float64)

    def translate(self, x: float, y: float, z: float):
        m = np.eye(4, dtype=np.float64)
        m[0, 3] = x
        m[1, 3] = y
        m[2, 3] = z
        self.multiply(m)

    def rotate(self, angle_deg: float, x: float, y: float, z: float):
        rad = math.radians(angle_deg)
        c, s = math.cos(rad), math.sin(rad)
        length = math.sqrt(x * x + y * y + z * z)
        if length < 1e-10:
            return
        x, y, z = x / length, y / length, z / length
        m = np.eye(4, dtype=np.float64)
        m[0, 0] = x * x * (1 - c) + c
        m[0, 1] = x * y * (1 - c) - z * s
        m[0, 2] = x * z * (1 - c) + y * s
        m[1, 0] = y * x * (1 - c) + z * s
        m[1, 1] = y * y * (1 - c) + c
        m[1, 2] = y * z * (1 - c) - x * s
        m[2, 0] = z * x * (1 - c) - y * s
        m[2, 1] = z * y * (1 - c) + x * s
        m[2, 2] = z * z * (1 - c) + c
        self.multiply(m)

    def scale(self, x: float, y: float, z: float):
        m = np.eye(4, dtype=np.float64)
        m[0, 0] = x
        m[1, 1] = y
        m[2, 2] = z
        self.multiply(m)

    def perspective(self, fovy: float, aspect: float, znear: float, zfar: float):
        f = 1.0 / math.tan(math.radians(fovy) / 2.0)
        m = np.zeros((4, 4), dtype=np.float64)
        m[0, 0] = f / aspect
        m[1, 1] = f
        m[2, 2] = (zfar + znear) / (znear - zfar)
        m[2, 3] = 2 * zfar * znear / (znear - zfar)
        m[3, 2] = -1.0
        self.multiply(m)

    def frustum(self, left: float, right: float, bottom: float, top: float, znear: float, zfar: float):
        """设置透视投影矩阵 (glFrustum)"""
        m = np.zeros((4, 4), dtype=np.float64)
        m[0, 0] = 2.0 * znear / (right - left)
        m[0, 2] = (right + left) / (right - left)
        m[1, 1] = 2.0 * znear / (top - bottom)
        m[1, 2] = (top + bottom) / (top - bottom)
        m[2, 2] = (zfar + znear) / (znear - zfar)
        m[2, 3] = 2.0 * zfar * znear / (znear - zfar)
        m[3, 2] = -1.0
        self.multiply(m)

    def ortho(self, left: float, right: float, bottom: float, top: float, znear: float = -1.0, zfar: float = 1.0):
        """设置正交投影矩阵 (glOrtho)"""
        m = np.eye(4, dtype=np.float64)
        m[0, 0] = 2.0 / (right - left)
        m[0, 3] = -(right + left) / (right - left)
        m[1, 1] = 2.0 / (top - bottom)
        m[1, 3] = -(top + bottom) / (top - bottom)
        m[2, 2] = -2.0 / (zfar - znear)
        m[2, 3] = -(zfar + znear) / (zfar - znear)
        self.multiply(m)

    def look_at(self, eye, center, up):
        f = np.array(center) - np.array(eye)
        f = f / np.linalg.norm(f)
        u = np.array(up) / np.linalg.norm(up)
        s = np.cross(f, u)
        s = s / np.linalg.norm(s)
        u = np.cross(s, f)
        m = np.eye(4, dtype=np.float64)
        m[0, :3] = s
        m[1, :3] = u
        m[2, :3] = -f
        m[0, 3] = -np.dot(s, eye)
        m[1, 3] = -np.dot(u, eye)
        m[2, 3] = np.dot(f, eye)
        self.multiply(m)

    def transform_point(self, p: Tuple[float, float, float, float]) -> np.ndarray:
        """变换一个4D点"""
        return self.current @ np.array(p)


# ===== 纹理管理 =====
class TextureManager:
    """纹理管理器"""

    def __init__(self):
        self._textures: Dict[int, Any] = {}  # id -> texture data
        self._next_id = 0
        self._active_unit = 0  # 当前活动纹理单元
        self._bindings: Dict[int, int] = {}  # unit -> texture_id

    def create(self, data=None, width=0, height=0, fmt=0) -> int:
        """创建纹理, 返回ID"""
        tid = self._next_id
        self._next_id += 1
        self._textures[tid] = {
            "data": data,
            "width": width,
            "height": height,
            "format": fmt,
        }
        return tid

    def load_file(self, filepath: str) -> int:
        """从文件加载纹理"""
        tid = self._next_id
        self._next_id += 1
        self._textures[tid] = {
            "data": filepath,  # 延迟加载
            "width": 0,
            "height": 0,
            "format": 0,
            "file": filepath,
        }
        return tid

    def bind(self, unit: int, texture_id: int):
        """绑定纹理到指定单元"""
        self._bindings[unit] = texture_id

    def active_texture(self, unit: int):
        """设置活动纹理单元"""
        self._active_unit = unit

    def get(self, texture_id: int) -> Optional[dict]:
        return self._textures.get(texture_id)


# ===== Shader管理 =====
class ShaderState:
    """Shader状态"""

    def __init__(self):
        self._programs: Dict[int, dict] = {}  # id -> {vertex, fragment, geometry, uniforms}
        self._next_id = 0
        self._current = -1
        self._uniforms: Dict[str, Any] = {}  # name -> value
        self._uniform_locs: Dict[str, int] = {}  # name -> location

    def create_program(self, vertex_code: str = "", fragment_code: str = "",
                       geometry_code: str = "", name: str = "") -> int:
        """创建shader程序, 返回ID"""
        pid = self._next_id
        self._next_id += 1
        self._programs[pid] = {
            "vertex": vertex_code,
            "fragment": fragment_code,
            "geometry": geometry_code,
            "name": name,
            "uniforms": {},
        }
        return pid

    def use(self, program_id: int):
        """使用shader程序"""
        self._current = program_id

    def get_uniform_loc(self, name: str) -> int:
        """获取uniform位置"""
        if name not in self._uniform_locs:
            self._uniform_locs[name] = len(self._uniform_locs)
        return self._uniform_locs[name]

    def set_uniform(self, loc: int, value: Any):
        """设置uniform值"""
        self._uniforms[loc] = value

    @property
    def current_program(self) -> Optional[dict]:
        if self._current >= 0:
            return self._programs.get(self._current)
        return None


# ===== 顶点缓冲 =====
class VertexBuffer:
    """当前glBegin/glEnd之间的顶点缓冲"""

    def __init__(self):
        self.mode: int = 0
        self.vertices: List[List[float]] = []
        self.colors: List[List[float]] = []
        self.normals: List[List[float]] = []
        self.texcoords: List[List[float]] = []
        self._current_color = [1.0, 1.0, 1.0, 1.0]
        self._current_normal = [0.0, 0.0, 1.0]
        self._current_texcoord = [0.0, 0.0]

    def reset(self, mode: int):
        self.mode = mode
        self.vertices = []
        self.colors = []
        self.normals = []
        self.texcoords = []
        self._current_color = [1.0, 1.0, 1.0, 1.0]
        self._current_normal = [0.0, 0.0, 1.0]
        self._current_texcoord = [0.0, 0.0]

    def add_vertex(self, x: float, y: float, z: float = 0.0, w: float = 1.0):
        self.vertices.append([x, y, z, w])
        self.colors.append(self._current_color[:])
        self.normals.append(self._current_normal[:])
        self.texcoords.append(self._current_texcoord[:])

    def set_color(self, r: float, g: float, b: float, a: float = 1.0):
        self._current_color = [r, g, b, a]

    def set_normal(self, x: float, y: float, z: float):
        self._current_normal = [x, y, z]

    def set_texcoord(self, s: float, t: float):
        self._current_texcoord = [s, t]


# ===== GL上下文 =====
class GLContext:
    """完整的OpenGL状态上下文"""

    def __init__(self, width: int = 640, height: int = 480):
        self.width = width
        self.height = height

        # 矩阵栈
        self.modelview = MatrixStack()
        self.projection = MatrixStack()
        self._matrix_mode = "modelview"

        # 纹理
        self.textures = TextureManager()

        # Shader
        self.shaders = ShaderState()

        # 顶点缓冲
        self.vertex_buf = VertexBuffer()
        self._in_begin = False

        # 状态标志
        self.depth_test = False
        self.alpha_blend = False
        self.cull_face = 0  # 0=off, FRONT, BACK
        self.line_width = 1.0
        self.blend_src = GLConst.SRC_ALPHA
        self.blend_dst = GLConst.ONE_MINUS_SRC_ALPHA

        # 帧缓冲 (用于capture)
        self._capture_mode = False
        self._capture_tex_id = -1

        # 渲染回调 - 由具体渲染后端设置
        self.on_flush: Optional[callable] = None  # (vertex_buf, context) -> None

    @property
    def active_matrix(self) -> MatrixStack:
        return self.modelview if self._matrix_mode == "modelview" else self.projection

    # ===== GL操作 =====
    def gl_begin(self, mode: int):
        self._in_begin = True
        self.vertex_buf.reset(mode)

    def gl_end(self):
        self._in_begin = False
        if self.on_flush:
            self.on_flush(self.vertex_buf, self)

    def gl_vertex(self, x: float, y: float, z: float = 0.0, w: float = 1.0):
        # 应用当前模型视图矩阵变换
        p = self.modelview.transform_point((x, y, z, w))
        self.vertex_buf.add_vertex(p[0], p[1], p[2], p[3])

    def gl_color(self, r: float, g: float, b: float, a: float = 1.0):
        self.vertex_buf.set_color(r, g, b, a)

    def gl_normal(self, x: float, y: float, z: float):
        self.vertex_buf.set_normal(x, y, z)

    def gl_texcoord(self, s: float, t: float, r: float = 0.0):
        self.vertex_buf.set_texcoord(s, t)

    def gl_push_matrix(self):
        self.active_matrix.push()

    def gl_pop_matrix(self):
        self.active_matrix.pop()

    def gl_translate(self, x: float, y: float, z: float):
        self.active_matrix.translate(x, y, z)

    def gl_rotate(self, angle: float, x: float, y: float, z: float):
        self.active_matrix.rotate(angle, x, y, z)

    def gl_scale(self, x: float, y: float, z: float):
        self.active_matrix.scale(x, y, z)

    def gl_load_identity(self):
        self.active_matrix.load_identity()

    def gl_perspective(self, fovy: float, aspect: float, znear: float, zfar: float):
        self.projection.perspective(fovy, aspect, znear, zfar)

    def gl_frustum(self, left: float, right: float, bottom: float, top: float, znear: float, zfar: float):
        self.projection.frustum(left, right, bottom, top, znear, zfar)

    def gl_ortho(self, left: float, right: float, bottom: float, top: float, znear: float = -1.0, zfar: float = 1.0):
        self.projection.ortho(left, right, bottom, top, znear, zfar)

    def gl_look_at(self, eyex, eyey, eyez, cx, cy, cz, upx, upy, upz):
        self.modelview.look_at((eyex, eyey, eyez), (cx, cy, cz), (upx, upy, upz))

    def gl_set_tex(self, tex_id: int, data=None, width: int = 0, height: int = 0, fmt: int = 0):
        """设置纹理数据"""
        if isinstance(data, str):
            # 文件路径
            tid = self.textures.load_file(data)
            return tid
        elif isinstance(data, list):
            # 数组数据
            tid = self.textures.create(data, width, height, fmt)
            return tid
        return tex_id

    def gl_get_tex(self, tex_id: int):
        """获取纹理数据"""
        return self.textures.get(tex_id)

    def gl_active_texture(self, unit: int):
        self.textures.active_texture(unit - GLConst.TEXTURE0)

    def gl_bind_texture(self, tex_id: int):
        self.textures.bind(self.textures._active_unit, tex_id)

    def gl_set_shader(self, program_id: int):
        self.shaders.use(program_id)

    def gl_get_uniform_loc(self, name: str) -> int:
        return self.shaders.get_uniform_loc(name)

    def gl_uniform1i(self, loc: int, val: int):
        self.shaders.set_uniform(loc, val)

    def gl_uniform1f(self, loc: int, val: float):
        self.shaders.set_uniform(loc, val)

    def gl_uniform(self, loc: int, *vals):
        """通用uniform设置"""
        self.shaders.set_uniform(loc, list(vals))

    def gl_enable(self, cap: int):
        if cap == GLConst.DEPTH_TEST:
            self.depth_test = True

    def gl_disable(self, cap: int):
        if cap == GLConst.DEPTH_TEST:
            self.depth_test = False

    def gl_alpha_enable(self):
        self.alpha_blend = True

    def gl_alpha_disable(self):
        self.alpha_blend = False

    def gl_blend_func(self, src: int, dst: int):
        self.blend_src = src
        self.blend_dst = dst

    def gl_cull_face(self, mode: int):
        self.cull_face = mode

    def gl_line_width(self, width: float):
        self.line_width = width

    def gl_quad(self, size: float = 1.0):
        """快捷绘制全屏四边形"""
        self.gl_begin(GLConst.QUADS)
        self.gl_vertex(-size, -size, 0)
        self.gl_vertex(+size, -size, 0)
        self.gl_vertex(+size, +size, 0)
        self.gl_vertex(-size, +size, 0)
        self.gl_end()

    def gl_capture(self):
        """开始capture渲染到纹理"""
        self._capture_mode = True

    def gl_capture_end(self, tex_id: int):
        """结束capture"""
        self._capture_mode = False
        self._capture_tex_id = tex_id
