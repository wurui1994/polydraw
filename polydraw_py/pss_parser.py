"""PSS文件解析器 - 分离PolyDraw脚本中的Host/Shader代码块

PSS文件格式:
  - 默认代码为Host脚本(EVAL语言)
  - @v 标记Vertex Shader开始(GLSL)
  - @g 标记Geometry Shader开始(GLSL)
  - @f 标记Fragment Shader开始(GLSL)
  - @h 标记Host脚本(显式标记)
  - @(:name) 命名shader, @v:name, @f:name
  - @g,GL_TRIANGLES,GL_TRIANGLE_STRIP,1024:myname 几何shader带参数

示例:
  glquad(1);           // Host脚本
  @v                   // Vertex Shader
  void main() { ... }
  @f                   // Fragment Shader
  void main() { ... }
"""
import re
from dataclasses import dataclass, field
from typing import List, Optional, Dict


@dataclass
class ShaderBlock:
    """一个Shader代码块"""
    block_type: str          # 'host', 'vertex', 'geometry', 'fragment'
    name: str = ''           # shader名称(可选)
    code: str = ''           # 代码内容
    geo_input: str = ''      # 几何shader输入类型(仅geometry)
    geo_output: str = ''     # 几何shader输出类型(仅geometry)
    geo_max_vertices: int = 0  # 几何shader最大顶点数(仅geometry)
    line_number: int = 0     # 在源文件中的起始行号


@dataclass
class PSSFile:
    """解析后的PSS文件"""
    filename: str = ''
    host_blocks: List[ShaderBlock] = field(default_factory=list)
    vertex_blocks: List[ShaderBlock] = field(default_factory=list)
    geometry_blocks: List[ShaderBlock] = field(default_factory=list)
    fragment_blocks: List[ShaderBlock] = field(default_factory=list)

    @property
    def host_code(self) -> str:
        """合并所有host代码"""
        return '\n'.join(b.code for b in self.host_blocks)

    @property
    def all_blocks(self) -> List[ShaderBlock]:
        """按出现顺序返回所有块"""
        blocks = self.host_blocks + self.vertex_blocks + self.geometry_blocks + self.fragment_blocks
        blocks.sort(key=lambda b: b.line_number)
        return blocks


# @v / @g / @f / @h 标记行模式
# 格式: @type 或 @type:name 或 @type,geo_params:name
# 也支持长名称: @vertex_shader, @fragment_shader 等
# geo_params: GL_TRIANGLES,GL_TRIANGLE_STRIP,1024
# 注意: : 后面可以是任意字符(包括 === 分隔线), 也可以省略
_DIRECTIVE_RE = re.compile(
    r'^@([vhgf](?:ertex|ragment|eometry|ost)?_?(?:shader)?)'  # @v/@vertex_shader etc
    r'(?:,(\w+)'                         # 可选: geo input type
    r'(?:,(\w+))?'                       # 可选: geo output type
    r'(?:,(\d+))?)?'                     # 可选: geo max vertices
    r'(?::(.*))?'                        # 可选: :name (name可以为任意字符)
    r'\s*$'                              # 行尾
)

# @(:name) 格式 - 同类型续块
_DIRECTIVE_SHORT_RE = re.compile(
    r'^@\((\w+)\)\s*(?://.*)?$'
)


def parse_pss(source: str, filename: str = '') -> PSSFile:
    """解析PSS文件源码，返回结构化的PSSFile对象"""
    result = PSSFile(filename=filename)
    lines = source.split('\n')

    current_type = 'host'  # 默认为host
    current_name = ''
    current_start_line = 0
    current_lines: List[str] = []
    geo_input = ''
    geo_output = ''
    geo_max_vertices = 0

    def flush_block():
        """将当前收集的代码保存为一个block"""
        code = '\n'.join(current_lines).strip()
        if not code and current_type == 'host':
            return
        block = ShaderBlock(
            block_type=current_type,
            name=current_name,
            code=code,
            geo_input=geo_input,
            geo_output=geo_output,
            geo_max_vertices=geo_max_vertices,
            line_number=current_start_line,
        )
        if current_type == 'host':
            result.host_blocks.append(block)
        elif current_type == 'vertex':
            result.vertex_blocks.append(block)
        elif current_type == 'geometry':
            result.geometry_blocks.append(block)
        elif current_type == 'fragment':
            result.fragment_blocks.append(block)

    for line_num, line in enumerate(lines, 1):
        stripped = line.strip()

        # 检查是否是 @v/@g/@f/@h 指令行
        m = _DIRECTIVE_RE.match(stripped)
        if m:
            # 先保存之前的块
            flush_block()

            type_char = m.group(1)
            # 支持短名称和长名称
            type_map = {
                'v': 'vertex', 'vertex_shader': 'vertex', 'vert': 'vertex',
                'g': 'geometry', 'geometry_shader': 'geometry',
                'f': 'fragment', 'fragment_shader': 'fragment', 'frag': 'fragment',
                'h': 'host', 'host_shader': 'host',
            }
            # 先尝试完整匹配, 再尝试首字母
            current_type = type_map.get(type_char) or type_map.get(type_char[0], 'host')
            current_name = m.group(5) or ''
            current_start_line = line_num
            current_lines = []
            geo_input = m.group(2) or ''
            geo_output = m.group(3) or ''
            geo_max_vertices = int(m.group(4) or 0)
            continue

        # 检查是否是 @(:name) 短格式
        m2 = _DIRECTIVE_SHORT_RE.match(stripped)
        if m2:
            flush_block()
            current_name = m2.group(1)
            current_start_line = line_num
            current_lines = []
            # current_type 保持不变(同上一个类型)
            continue

        # 普通代码行
        current_lines.append(line)

    # 保存最后一个块
    flush_block()

    return result


def parse_pss_file(filepath: str) -> PSSFile:
    """从文件解析PSS"""
    with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
        source = f.read()
    import os
    return parse_pss(source, filename=os.path.basename(filepath))


# ===== 测试 =====
if __name__ == '__main__':
    import sys, os, glob

    pss_dir = os.path.join(os.path.dirname(__file__), '..')
    patterns = [
        os.path.join(pss_dir, 'ken', '*.pss'),
        os.path.join(pss_dir, 'tigrou', '*.pss'),
    ]

    total = 0
    ok = 0
    errors = []

    for pat in patterns:
        for f in sorted(glob.glob(pat)):
            total += 1
            name = os.path.relpath(f, pss_dir)
            try:
                pss = parse_pss_file(f)
                # 验证: 至少要有host和fragment
                if not pss.host_blocks and not pss.fragment_blocks:
                    errors.append((name, 'No host or fragment blocks'))
                    print(f'WARN: {name}: no host or fragment blocks')
                else:
                    ok += 1
            except Exception as e:
                errors.append((name, str(e)))
                print(f'FAIL: {name}: {e}')

    print(f'\n=== PSS解析: {ok}/{total} 通过, {len(errors)} 失败 ===')
    if errors:
        print('失败文件:')
        for name, err in errors:
            print(f'  {name}: {err}')
