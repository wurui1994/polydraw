"""Pygame渲染后端 - 将GLContext的顶点数据光栅化到pygame窗口

实现软件光栅化, 将GLContext中的顶点缓冲区数据绘制到pygame Surface。
支持: POINTS, LINES, LINE_LOOP, LINE_STRIP, TRIANGLES, TRIANGLE_STRIP,
      TRIANGLE_FAN, QUADS, QUAD_STRIP, POLYGON

两种使用方式:
1. 与PSSRunner集成: runner.renderer = PygameRenderer(runner.ctx)
2. 独立使用: renderer = PygameRenderer(); renderer.open(); ...
"""
import math
import sys
from typing import Optional, List, Tuple

try:
    import pygame
    HAS_PYGAME = True
except ImportError:
    HAS_PYGAME = False

from .gl_context import GLContext, GLConst, VertexBuffer


class PygameRenderer:
    """Pygame软件渲染器"""

    def __init__(self, ctx: GLContext, title: str = "PolyDraw PSS"):
        if not HAS_PYGAME:
            raise RuntimeError("pygame未安装, 请运行: python3 -m pip install pygame")

        self.ctx = ctx
        self.title = title
        self.screen: Optional[pygame.Surface] = None
        self.clock = pygame.time.Clock()
        self._running = False
        self._fps = 60
        self._clear_color = (0, 0, 0)

        # 连接GLContext的flush回调
        self.ctx.on_flush = self._on_flush

        # 帧缓冲: 累积所有glBegin/glEnd提交的图元
        self._primitives: List[dict] = []

        # 鼠标状态
        self._mouse_x = 0
        self._mouse_y = 0
        self._mouse_buttons = 0

    def open(self) -> bool:
        """打开渲染窗口"""
        if not HAS_PYGAME:
            print("错误: pygame未安装")
            return False

        pygame.init()
        self.screen = pygame.display.set_mode(
            (self.ctx.width, self.ctx.height),
            pygame.DOUBLEBUF | pygame.HWSURFACE
        )
        pygame.display.set_caption(self.title)
        self._running = True
        return True

    def close(self):
        """关闭渲染窗口"""
        self._running = False
        pygame.quit()

    @property
    def is_open(self) -> bool:
        return self._running and self.screen is not None

    def process_events(self) -> bool:
        """处理pygame事件, 返回False表示应退出"""
        if not self._running:
            return False

        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                self._running = False
                return False
            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_ESCAPE:
                    self._running = False
                    return False
            elif event.type == pygame.MOUSEMOTION:
                self._mouse_x, self._mouse_y = event.pos
            elif event.type == pygame.MOUSEBUTTONDOWN:
                self._mouse_buttons = 1
            elif event.type == pygame.MOUSEBUTTONUP:
                self._mouse_buttons = 0

        # 更新系统变量
        self.ctx  # 确保ctx存在
        return True

    def update_mouse_vars(self, builtins_sys_vars: dict):
        """更新鼠标系统变量"""
        builtins_sys_vars["mousx"] = float(self._mouse_x)
        builtins_sys_vars["mousy"] = float(self._mouse_y)
        builtins_sys_vars["bstatus"] = float(self._mouse_buttons)

    def begin_frame(self):
        """开始新帧: 清空帧缓冲"""
        self._primitives = []

    def end_frame(self):
        """结束帧: 光栅化所有图元并显示"""
        if self.screen is None:
            return

        # 清屏
        self.screen.fill(self._clear_color)

        # 光栅化所有图元
        for prim in self._primitives:
            self._rasterize_primitive(prim)

        # 交换缓冲区
        pygame.display.flip()
        self.clock.tick(self._fps)

    def _on_flush(self, vbuf: VertexBuffer, ctx: GLContext):
        """GLContext.on_flush回调: 收集图元数据"""
        if not vbuf.vertices:
            return

        prim = {
            "mode": vbuf.mode,
            "vertices": [v[:] for v in vbuf.vertices],
            "colors": [c[:] for c in vbuf.colors],
            "texcoords": [t[:] for t in vbuf.texcoords],
            "normals": [n[:] for n in vbuf.normals],
        }
        self._primitives.append(prim)

    def _rasterize_primitive(self, prim: dict):
        """光栅化单个图元"""
        mode = prim["mode"]
        verts = prim["vertices"]
        colors = prim["colors"]

        if not verts:
            return

        # 投影变换: 将3D坐标映射到屏幕坐标
        screen_pts = []
        for v in verts:
            sx, sy = self._project(v[0], v[1], v[2], v[3])
            screen_pts.append((sx, sy))

        if mode == GLConst.POINTS:
            for i, pt in enumerate(screen_pts):
                c = self._color_to_pygame(colors[i])
                pygame.draw.circle(self.screen, c, (int(pt[0]), int(pt[1])), 3)

        elif mode == GLConst.LINES:
            for i in range(0, len(screen_pts) - 1, 2):
                c = self._color_to_pygame(colors[i])
                pygame.draw.line(self.screen, c,
                                 (int(screen_pts[i][0]), int(screen_pts[i][1])),
                                 (int(screen_pts[i+1][0]), int(screen_pts[i+1][1])),
                                 max(1, int(self.ctx.line_width)))

        elif mode == GLConst.LINE_LOOP:
            if len(screen_pts) >= 2:
                c = self._color_to_pygame(colors[0])
                pts = [(int(p[0]), int(p[1])) for p in screen_pts]
                pygame.draw.lines(self.screen, c, True, pts, max(1, int(self.ctx.line_width)))

        elif mode == GLConst.LINE_STRIP:
            if len(screen_pts) >= 2:
                c = self._color_to_pygame(colors[0])
                pts = [(int(p[0]), int(p[1])) for p in screen_pts]
                pygame.draw.lines(self.screen, c, False, pts, max(1, int(self.ctx.line_width)))

        elif mode == GLConst.TRIANGLES:
            for i in range(0, len(screen_pts) - 2, 3):
                c = self._color_to_pygame(colors[i])
                pts = [
                    (int(screen_pts[i][0]), int(screen_pts[i][1])),
                    (int(screen_pts[i+1][0]), int(screen_pts[i+1][1])),
                    (int(screen_pts[i+2][0]), int(screen_pts[i+2][1])),
                ]
                pygame.draw.polygon(self.screen, c, pts)

        elif mode == GLConst.TRIANGLE_STRIP:
            for i in range(len(screen_pts) - 2):
                c = self._color_to_pygame(colors[i])
                pts = [
                    (int(screen_pts[i][0]), int(screen_pts[i][1])),
                    (int(screen_pts[i+1][0]), int(screen_pts[i+1][1])),
                    (int(screen_pts[i+2][0]), int(screen_pts[i+2][1])),
                ]
                pygame.draw.polygon(self.screen, c, pts)

        elif mode == GLConst.TRIANGLE_FAN:
            if len(screen_pts) >= 3:
                center = screen_pts[0]
                for i in range(1, len(screen_pts) - 1):
                    c = self._color_to_pygame(colors[0])
                    pts = [
                        (int(center[0]), int(center[1])),
                        (int(screen_pts[i][0]), int(screen_pts[i][1])),
                        (int(screen_pts[i+1][0]), int(screen_pts[i+1][1])),
                    ]
                    pygame.draw.polygon(self.screen, c, pts)

        elif mode == GLConst.QUADS:
            for i in range(0, len(screen_pts) - 3, 4):
                c = self._color_to_pygame(colors[i])
                pts = [
                    (int(screen_pts[i][0]), int(screen_pts[i][1])),
                    (int(screen_pts[i+1][0]), int(screen_pts[i+1][1])),
                    (int(screen_pts[i+2][0]), int(screen_pts[i+2][1])),
                    (int(screen_pts[i+3][0]), int(screen_pts[i+3][1])),
                ]
                pygame.draw.polygon(self.screen, c, pts)

        elif mode == GLConst.QUAD_STRIP:
            for i in range(0, len(screen_pts) - 3, 2):
                c = self._color_to_pygame(colors[i])
                pts = [
                    (int(screen_pts[i][0]), int(screen_pts[i][1])),
                    (int(screen_pts[i+1][0]), int(screen_pts[i+1][1])),
                    (int(screen_pts[i+3][0]), int(screen_pts[i+3][1])),
                    (int(screen_pts[i+2][0]), int(screen_pts[i+2][1])),
                ]
                pygame.draw.polygon(self.screen, c, pts)

        elif mode == GLConst.POLYGON:
            if len(screen_pts) >= 3:
                c = self._color_to_pygame(colors[0])
                pts = [(int(p[0]), int(p[1])) for p in screen_pts]
                pygame.draw.polygon(self.screen, c, pts)

    def _project(self, x: float, y: float, z: float, w: float) -> Tuple[float, float]:
        """将3D坐标投影到屏幕坐标

        应用投影矩阵和视口变换。
        如果投影矩阵是单位矩阵(未设置), 使用默认正交映射:
        坐标范围 [-5, 5] 映射到屏幕, 原点在屏幕中心。
        """
        # 应用投影矩阵
        proj = self.ctx.projection.current
        p = proj @ np.array([x, y, z, w])

        # 透视除法
        if abs(p[3]) > 1e-10:
            px = p[0] / p[3]
            py = p[1] / p[3]
        else:
            px, py = p[0], p[1]

        # 检查投影矩阵是否为单位矩阵(未设置)
        is_identity = np.allclose(proj, np.eye(4), atol=1e-6)

        if is_identity:
            # 默认正交映射: 坐标直接映射到屏幕
            # 范围 [-5, 5] -> 屏幕, 原点在中心, Y轴翻转
            scale = min(self.ctx.width, self.ctx.height) / 10.0
            sx = self.ctx.width * 0.5 + px * scale
            sy = self.ctx.height * 0.5 - py * scale  # Y轴翻转
        else:
            # NDC -> 屏幕坐标
            # NDC范围 [-1, 1] -> 屏幕 [0, width/height]
            sx = (px + 1.0) * 0.5 * self.ctx.width
            sy = (1.0 - py) * 0.5 * self.ctx.height  # Y轴翻转

        return sx, sy

    def _color_to_pygame(self, rgba: List[float]) -> Tuple[int, int, int]:
        """将RGBA浮点颜色转换为pygame RGB元组

        支持两种颜色范围:
        - 0.0-1.0 (OpenGL标准): 值<=1.0时按此范围处理
        - 0-255 (Ken风格): 值>1.0时按此范围处理
        """
        def to_byte(v):
            if v <= 1.0:
                return max(0, min(255, int(v * 255)))
            else:
                return max(0, min(255, int(v)))

        r = to_byte(rgba[0])
        g = to_byte(rgba[1])
        b = to_byte(rgba[2])
        return (r, g, b)

    def set_clear_color(self, r: float, g: float, b: float):
        """设置清屏颜色"""
        self._clear_color = (
            max(0, min(255, int(r * 255))),
            max(0, min(255, int(g * 255))),
            max(0, min(255, int(b * 255))),
        )

    def set_fps(self, fps: int):
        """设置目标帧率"""
        self._fps = fps

    def get_fps(self) -> float:
        """获取实际帧率"""
        return self.clock.get_fps()


# numpy延迟导入
import numpy as np
