"""PSS Runner - 整合解析+解释执行+渲染的完整运行器

PSS执行流程:
1. 解析PSS文件 -> PSSFile (host/shader块)
2. 创建GLContext + ShaderManager
3. 编译shader块 -> shader程序
4. 进入绘图循环:
   a. 执行host代码 (EVAL解释器)
   b. host代码调用GL函数 -> 写入GLContext
   c. 渲染GLContext中的顶点数据
   d. 交换缓冲区
5. 循环直到退出

支持两种执行路径:
- AST解释执行: EvalInterpreter直接遍历AST执行
- Python代码执行: EvalToPython转换后exec()执行
"""
import time
from typing import Optional, Callable, Any

from .gl_context import GLContext
from .gl_builtins import GLBuiltins
from .shader_manager import ShaderManager


class PSSRunner:
    """PSS运行器"""

    def __init__(self, width: int = 640, height: int = 640, debug: bool = False):
        self.ctx = GLContext(width, height)
        self.builtins = GLBuiltins(self.ctx, debug)
        self.shaders = ShaderManager()
        self.debug = debug

        # 解释器 (延迟导入, 避免循环依赖)
        self._interpreter = None

        # PSS数据
        self._pss = None
        self._host_ast = None

        # 渲染后端
        self.renderer = None  # PygameRenderer实例, 由外部设置或run_with_window()创建

        # 渲染回调
        self.on_render: Optional[Callable] = None  # (ctx, shaders) -> None

        # 运行状态
        self._running = False
        self._frame_count = 0
        self._target_fps = 60

    def load_pss(self, filepath: str):
        """加载PSS文件"""
        from pss_parser import parse_pss_file
        from eval_parser import EvalParser

        self._pss = parse_pss_file(filepath)

        # 编译shader
        self.shaders = ShaderManager()
        self.shaders.create_from_pss_blocks(
            self._pss.vertex_blocks,
            self._pss.fragment_blocks,
            self._pss.geometry_blocks,
        )

        # 解析host代码
        host_code = self._pss.host_code
        if host_code.strip():
            self._host_ast = EvalParser.parse(host_code)
        else:
            self._host_ast = None

        if self.debug:
            print(f"加载PSS: {filepath}")
            print(f"  Host代码: {len(host_code)} 字符")
            print(f"  Vertex shaders: {len(self._pss.vertex_blocks)}")
            print(f"  Fragment shaders: {len(self._pss.fragment_blocks)}")
            print(f"  Geometry shaders: {len(self._pss.geometry_blocks)}")
            print(f"  Shader程序: {self.shaders.program_count}")

    def load_code(self, host_code: str, vertex_code: str = "", fragment_code: str = ""):
        """直接加载代码"""
        from eval_parser import EvalParser
        from pss_parser import ShaderBlock, PSSFile

        # 构造PSSFile
        pss = PSSFile()
        if host_code.strip():
            pss.host_blocks.append(ShaderBlock(block_type="host", code=host_code))
        if vertex_code.strip():
            pss.vertex_blocks.append(ShaderBlock(block_type="vertex", code=vertex_code))
        if fragment_code.strip():
            pss.fragment_blocks.append(ShaderBlock(block_type="fragment", code=fragment_code))

        self._pss = pss

        # 编译shader
        self.shaders = ShaderManager()
        self.shaders.create_from_pss_blocks(
            pss.vertex_blocks, pss.fragment_blocks, pss.geometry_blocks
        )

        # 解析host代码
        if host_code.strip():
            self._host_ast = EvalParser.parse(host_code)
        else:
            self._host_ast = None

    def _get_interpreter(self):
        """获取EVAL解释器实例"""
        if self._interpreter is None:
            from eval_interpreter import EvalInterpreter
            self._interpreter = EvalInterpreter(debug=self.debug)
            # 注入GL内置函数和常量
            self._setup_interpreter(self._interpreter)
        return self._interpreter

    def _setup_interpreter(self, interp):
        """将GL内置函数和常量注入解释器"""
        # 注入常量
        for name, val in self.builtins.get_all_consts().items():
            interp.builtin_vars[name] = val

        # 注入函数
        for name, func in self.builtins.get_all_functions().items():
            interp.builtin_functions[name] = func

        # 注入系统变量访问
        interp.builtin_vars.update(self.builtins.sys_vars)

    def execute_frame(self) -> Any:
        """执行一帧host代码"""
        if self._host_ast is None:
            return 0.0

        interp = self._get_interpreter()

        # 更新系统变量
        self.builtins.sys_vars["numframes"] = float(self._frame_count)

        # 每帧重置模型视图矩阵
        # 投影矩阵不重置 - 让host代码的setfov/gluperspective生效
        # (原始引擎在host代码执行前设置gfov默认值, host代码可覆盖)
        self.ctx.modelview.load_identity()

        # 执行host代码
        try:
            result = interp.interpret(self._host_ast)
        except Exception as e:
            if self.debug:
                print(f"执行错误: {e}")
                import traceback
                traceback.print_exc()
            result = 0.0

        # 推进帧
        self._frame_count += 1
        self.builtins.advance_frame()

        return result

    def run(self, max_frames: int = 0, callback: Optional[Callable] = None):
        """运行绘图循环 (无窗口, 离线模式)

        Args:
            max_frames: 最大帧数, 0=无限
            callback: 每帧回调 (runner, frame) -> bool, 返回False停止
        """
        self._running = True
        self._frame_count = 0
        frame_time = 1.0 / self._target_fps

        while self._running:
            t0 = time.time()

            # 执行host代码
            self.execute_frame()

            # 渲染回调
            if self.on_render:
                self.on_render(self.ctx, self.shaders)

            # 用户回调
            if callback and not callback(self, self._frame_count):
                break

            # 帧数限制
            if max_frames > 0 and self._frame_count >= max_frames:
                break

            # 帧率控制
            elapsed = time.time() - t0
            if elapsed < frame_time:
                time.sleep(frame_time - elapsed)

        self._running = False

    def run_with_window(self, title: str = "PolyDraw PSS", max_frames: int = 0,
                        callback: Optional[Callable] = None):
        """运行绘图循环 (带pygame窗口显示)

        Args:
            title: 窗口标题
            max_frames: 最大帧数, 0=无限
            callback: 每帧回调 (runner, frame) -> bool, 返回False停止
        """
        from .pygame_renderer import PygameRenderer

        # 创建渲染器
        self.renderer = PygameRenderer(self.ctx, title)
        if not self.renderer.open():
            print("无法打开pygame窗口")
            return

        self._running = True
        self._frame_count = 0

        try:
            while self._running:
                # 处理pygame事件
                if not self.renderer.process_events():
                    break

                # 更新鼠标变量
                self.renderer.update_mouse_vars(self.builtins.sys_vars)

                # 开始新帧
                self.renderer.begin_frame()

                # 执行host代码
                self.execute_frame()

                # 渲染回调
                if self.on_render:
                    self.on_render(self.ctx, self.shaders)

                # 结束帧 (光栅化+显示)
                self.renderer.end_frame()

                # 用户回调
                if callback and not callback(self, self._frame_count):
                    break

                # 帧数限制
                if max_frames > 0 and self._frame_count >= max_frames:
                    break

        finally:
            self.renderer.close()
            self._running = False

    def run_python_code(self, max_frames: int = 0, callback: Optional[Callable] = None):
        """使用转换后的Python代码执行 (无窗口, 离线模式)

        将EVAL AST转换为Python代码, 然后exec()执行。
        """
        if self._host_ast is None:
            return

        from eval_to_python import EvalToPython

        converter = EvalToPython()
        py_code = converter.convert(self._host_ast)

        # 准备执行环境
        exec_globals = {
            "math": __import__("math"),
            "time": __import__("time"),
            "random": __import__("random"),
            "_gl": self.ctx,
            "_sys": self.builtins.sys_vars,
        }

        self._running = True
        self._frame_count = 0
        frame_time = 1.0 / self._target_fps

        while self._running:
            t0 = time.time()

            # 更新系统变量
            self.builtins.sys_vars["numframes"] = float(self._frame_count)

            # 执行转换后的Python代码
            try:
                exec(py_code, exec_globals)
            except Exception as e:
                if self.debug:
                    print(f"Python代码执行错误: {e}")
                    import traceback
                    traceback.print_exc()

            # 渲染回调
            if self.on_render:
                self.on_render(self.ctx, self.shaders)

            # 推进帧
            self._frame_count += 1
            self.builtins.advance_frame()

            # 用户回调
            if callback and not callback(self, self._frame_count):
                break

            # 帧数限制
            if max_frames > 0 and self._frame_count >= max_frames:
                break

            # 帧率控制
            elapsed = time.time() - t0
            if elapsed < frame_time:
                time.sleep(frame_time - elapsed)

        self._running = False

    def run_python_code_with_window(self, title: str = "PolyDraw PSS (Python)",
                                     max_frames: int = 0,
                                     callback: Optional[Callable] = None):
        """使用转换后的Python代码执行 (带pygame窗口显示)

        将EVAL AST转换为Python代码, 然后exec()执行, 同时通过pygame渲染。
        """
        from .pygame_renderer import PygameRenderer
        from eval_to_python import EvalToPython

        if self._host_ast is None:
            return

        # 转换代码
        converter = EvalToPython()
        py_code = converter.convert(self._host_ast)

        # 创建渲染器
        self.renderer = PygameRenderer(self.ctx, title)
        if not self.renderer.open():
            print("无法打开pygame窗口")
            return

        # 准备执行环境
        exec_globals = {
            "math": __import__("math"),
            "time": __import__("time"),
            "random": __import__("random"),
            "_gl": self.ctx,
            "_sys": self.builtins.sys_vars,
        }

        self._running = True
        self._frame_count = 0

        try:
            while self._running:
                # 处理pygame事件
                if not self.renderer.process_events():
                    break

                # 更新鼠标变量
                self.renderer.update_mouse_vars(self.builtins.sys_vars)

                # 更新系统变量
                self.builtins.sys_vars["numframes"] = float(self._frame_count)

                # 开始新帧
                self.renderer.begin_frame()

                # 执行转换后的Python代码
                try:
                    exec(py_code, exec_globals)
                except Exception as e:
                    if self.debug:
                        print(f"Python代码执行错误: {e}")
                        import traceback
                        traceback.print_exc()

                # 结束帧
                self.renderer.end_frame()

                # 推进帧
                self._frame_count += 1
                self.builtins.advance_frame()

                # 用户回调
                if callback and not callback(self, self._frame_count):
                    break

                # 帧数限制
                if max_frames > 0 and self._frame_count >= max_frames:
                    break

        finally:
            self.renderer.close()
            self._running = False

    def stop(self):
        """停止运行"""
        self._running = False

    @property
    def frame_count(self) -> int:
        return self._frame_count

    @property
    def is_running(self) -> bool:
        return self._running


# ===== 便捷函数 =====
def run_pss_file(filepath: str, max_frames: int = 1, debug: bool = False) -> PSSRunner:
    """运行PSS文件(指定帧数, 离线模式)"""
    runner = PSSRunner(debug=debug)
    runner.load_pss(filepath)
    runner.run(max_frames=max_frames)
    return runner


def run_pss_code(host_code: str, vertex_code: str = "", fragment_code: str = "",
                 max_frames: int = 1, debug: bool = False) -> PSSRunner:
    """运行PSS代码(离线模式)"""
    runner = PSSRunner(debug=debug)
    runner.load_code(host_code, vertex_code, fragment_code)
    runner.run(max_frames=max_frames)
    return runner


def run_pss_file_with_window(filepath: str, title: str = "", debug: bool = False) -> PSSRunner:
    """运行PSS文件(带pygame窗口显示)"""
    runner = PSSRunner(debug=debug)
    runner.load_pss(filepath)
    t = title or f"PolyDraw - {filepath}"
    runner.run_with_window(title=t)
    return runner


def run_pss_code_with_window(host_code: str, vertex_code: str = "", fragment_code: str = "",
                              title: str = "PolyDraw PSS", debug: bool = False) -> PSSRunner:
    """运行PSS代码(带pygame窗口显示)"""
    runner = PSSRunner(debug=debug)
    runner.load_code(host_code, vertex_code, fragment_code)
    runner.run_with_window(title=title)
    return runner
