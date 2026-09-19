"""Shader管理器 - GLSL编译、uniform绑定、多shader切换

PSS文件中的 @v/@f/@g 块是GLSL代码, 需要编译为shader程序。
Shader管理器负责:
- 从PSS的shader块创建GLSL程序
- 管理uniform变量位置
- 处理shader切换
- 生成WebGL兼容的GLSL
"""
import re
from typing import Dict, List, Optional, Tuple
from dataclasses import dataclass, field


@dataclass
class ShaderProgram:
    """一个shader程序"""
    id: int = 0
    name: str = ""
    vertex_code: str = ""
    fragment_code: str = ""
    geometry_code: str = ""
    geo_input: str = ""
    geo_output: str = ""
    geo_max_vertices: int = 0
    uniforms: Dict[str, int] = field(default_factory=dict)  # name -> location


class ShaderManager:
    """Shader管理器"""

    def __init__(self):
        self._programs: Dict[int, ShaderProgram] = {}
        self._next_id = 0
        self._current_id = -1
        self._uniform_values: Dict[int, Dict[int, any]] = {}  # prog_id -> {loc -> value}

    def create_from_pss_blocks(self, vertex_blocks: list, fragment_blocks: list,
                                geometry_blocks: list = None) -> List[int]:
        """从PSS解析结果创建shader程序

        PSS可以有多个shader程序:
        - 第一个 @v + @f 组成程序0 (默认)
        - 后续 @(:name) 块组成额外程序
        - glsetshader(0/1/...) 切换

        Returns: 程序ID列表
        """
        ids = []

        # 收集所有shader块, 按出现顺序配对
        # 简单策略: 每遇到一个 @v 或 @f 就开始新程序
        # 更复杂的策略需要看 PSS 文件的实际用法

        # 按名称分组
        v_by_name: Dict[str, str] = {}
        f_by_name: Dict[str, str] = {}
        g_by_name: Dict[str, str] = {}

        for b in vertex_blocks:
            name = b.name or ""
            v_by_name[name] = b.code
        for b in fragment_blocks:
            name = b.name or ""
            f_by_name[name] = b.code
        if geometry_blocks:
            for b in geometry_blocks:
                name = b.name or ""
                g_by_name[name] = b.code

        # 创建程序
        all_names = set(list(v_by_name.keys()) + list(f_by_name.keys()) + list(g_by_name.keys()))
        if not all_names:
            all_names = {""}

        for name in sorted(all_names):
            vcode = v_by_name.get(name, "")
            fcode = f_by_name.get(name, "")
            gcode = g_by_name.get(name, "")

            if not vcode and not fcode:
                continue

            pid = self._create_program(vcode, fcode, gcode, name)
            ids.append(pid)

        return ids

    def _create_program(self, vertex_code: str, fragment_code: str,
                        geometry_code: str = "", name: str = "") -> int:
        """创建shader程序"""
        pid = self._next_id
        self._next_id += 1

        prog = ShaderProgram(
            id=pid,
            name=name,
            vertex_code=vertex_code,
            fragment_code=fragment_code,
            geometry_code=geometry_code,
        )

        # 扫描uniform变量
        prog.uniforms = self._scan_uniforms(vertex_code, fragment_code, geometry_code)

        self._programs[pid] = prog
        self._uniform_values[pid] = {}
        return pid

    def _scan_uniforms(self, *codes: str) -> Dict[str, int]:
        """扫描GLSL代码中的uniform声明"""
        uniforms = {}
        loc = 0
        for code in codes:
            if not code:
                continue
            # 匹配 uniform type name; 或 uniform type name[N];
            for m in re.finditer(r'uniform\s+\w+\s+(\w+)(?:\[(\d+)\])?\s*;', code):
                name = m.group(1)
                if name not in uniforms:
                    uniforms[name] = loc
                    loc += 1
        return uniforms

    def use(self, program_id: int):
        """切换当前shader程序"""
        self._current_id = program_id

    @property
    def current(self) -> Optional[ShaderProgram]:
        if self._current_id >= 0:
            return self._programs.get(self._current_id)
        return None

    def get_uniform_loc(self, name: str) -> int:
        """获取uniform位置"""
        prog = self.current
        if prog:
            return prog.uniforms.get(name, -1)
        return -1

    def set_uniform(self, loc: int, value: any):
        """设置uniform值"""
        if self._current_id >= 0:
            self._uniform_values.setdefault(self._current_id, {})[loc] = value

    def get_uniform_value(self, program_id: int, loc: int) -> any:
        """获取uniform值"""
        return self._uniform_values.get(program_id, {}).get(loc)

    def get_program(self, pid: int) -> Optional[ShaderProgram]:
        return self._programs.get(pid)

    @property
    def program_count(self) -> int:
        return len(self._programs)

    def generate_glsl_header(self, target: str = "gl") -> str:
        """生成GLSL头部, 适配不同目标

        Args:
            target: 'gl' (OpenGL), 'gles' (OpenGL ES), 'webgl' (WebGL)
        """
        if target == "webgl":
            return "#version 100\nprecision mediump float;\n"
        elif target == "gles":
            return "#version 100\nprecision highp float;\n"
        else:
            return "#version 120\n"

    def get_vertex_glsl(self, program_id: int, target: str = "gl") -> str:
        """获取适配目标平台的vertex shader代码"""
        prog = self._programs.get(program_id)
        if not prog:
            return ""
        code = prog.vertex_code
        if target in ("webgl", "gles"):
            code = self._adapt_to_gles(code, "vertex")
        return code

    def get_fragment_glsl(self, program_id: int, target: str = "gl") -> str:
        """获取适配目标平台的fragment shader代码"""
        prog = self._programs.get(program_id)
        if not prog:
            return ""
        code = prog.fragment_code
        if target in ("webgl", "gles"):
            code = self._adapt_to_gles(code, "fragment")
        return code

    def _adapt_to_gles(self, code: str, stage: str) -> str:
        """将桌面GLSL适配为GLES/WebGL兼容

        主要修改:
        - ftransform() -> 手动计算
        - gl_MultiTexCoord0 -> texcoord attribute
        - varying -> in/out (GLES 3.0)
        - texture2D -> texture (GLES 3.0)
        """
        # 添加精度声明
        if "precision" not in code:
            code = "precision mediump float;\n" + code

        # ftransform替换
        if "ftransform()" in code:
            code = code.replace("ftransform()", "(gl_ProjectionMatrix * gl_ModelViewMatrix * gl_Vertex)")

        return code
