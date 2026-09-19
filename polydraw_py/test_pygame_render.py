#!/usr/bin/env python3
"""测试PSS pygame渲染 - 用driftbox.pss验证窗口显示

测试两种执行路径:
1. AST解释执行 + pygame窗口
2. Python代码转换执行 + pygame窗口
"""
import sys
import os

# 添加项目路径
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from runtime.pss_runner import PSSRunner


def test_ast_interpret_with_window():
    """测试1: AST解释执行 + pygame窗口"""
    print("=== 测试1: AST解释执行 + pygame窗口 ===")

    # 使用driftbox.pss的简化代码
    host_code = """
glBegin(GL_QUADS);
glColor(klock(), 0, 0);
x = 2; y = 2; z = 1;
glVertex(-x, +y, -z);
glVertex(+x, +y, -z);
glVertex(+x, -y, -z);
glVertex(-x, -y, -z);
glEnd();
"""

    runner = PSSRunner(width=640, height=640, debug=True)
    runner.load_code(host_code)
    runner.run_with_window(title="AST Interpret - driftbox", max_frames=300)
    print(f"  帧数: {runner.frame_count}")


def test_python_code_with_window():
    """测试2: Python代码转换执行 + pygame窗口"""
    print("\n=== 测试2: Python代码转换执行 + pygame窗口 ===")

    host_code = """
glBegin(GL_QUADS);
glColor(klock(), 0, 0);
x = 2; y = 2; z = 1;
glVertex(-x, +y, -z);
glVertex(+x, +y, -z);
glVertex(+x, -y, -z);
glVertex(-x, -y, -z);
glEnd();
"""

    runner = PSSRunner(width=640, height=640, debug=True)
    runner.load_code(host_code)
    runner.run_python_code_with_window(title="Python Code - driftbox", max_frames=300)
    print(f"  帧数: {runner.frame_count}")


def test_pss_file_with_window():
    """测试3: 加载实际PSS文件 + pygame窗口"""
    print("\n=== 测试3: 加载实际PSS文件 + pygame窗口 ===")

    pss_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ken", "driftbox.pss")
    if not os.path.exists(pss_path):
        print(f"  PSS文件不存在: {pss_path}")
        return

    runner = PSSRunner(width=640, height=640, debug=True)
    runner.load_pss(pss_path)
    runner.run_with_window(title=f"AST - {os.path.basename(pss_path)}", max_frames=300)
    print(f"  帧数: {runner.frame_count}")


def test_simple_triangle():
    """测试4: 简单三角形渲染"""
    print("\n=== 测试4: 简单三角形渲染 ===")

    host_code = """
glBegin(GL_TRIANGLES);
glColor(1, 0, 0);
glVertex(0, 3, 0);
glColor(0, 1, 0);
glVertex(-3, -3, 0);
glColor(0, 0, 1);
glVertex(3, -3, 0);
glEnd();
"""

    runner = PSSRunner(width=640, height=640, debug=True)
    runner.load_code(host_code)
    runner.run_with_window(title="Simple Triangle", max_frames=300)
    print(f"  帧数: {runner.frame_count}")


def test_animated_quad():
    """测试5: 动画四边形 (验证klock时间函数)"""
    print("\n=== 测试5: 动画四边形 ===")

    host_code = """
t = klock();
x = sin(t) * 3;
y = cos(t) * 3;
glBegin(GL_QUADS);
glColor(1, 0.5, 0);
glVertex(x-1, y-1, 0);
glVertex(x+1, y-1, 0);
glVertex(x+1, y+1, 0);
glVertex(x-1, y+1, 0);
glEnd();
"""

    runner = PSSRunner(width=640, height=640, debug=True)
    runner.load_code(host_code)
    runner.run_with_window(title="Animated Quad", max_frames=600)
    print(f"  帧数: {runner.frame_count}")


if __name__ == "__main__":
    if len(sys.argv) > 1:
        test_id = int(sys.argv[1])
    else:
        print("选择测试:")
        print("  1: AST解释执行 + pygame窗口 (driftbox简化)")
        print("  2: Python代码转换执行 + pygame窗口 (driftbox简化)")
        print("  3: 加载实际PSS文件 + pygame窗口")
        print("  4: 简单三角形渲染")
        print("  5: 动画四边形")
        print("  0: 全部测试")
        test_id = int(input("输入测试编号: "))

    if test_id == 1:
        test_ast_interpret_with_window()
    elif test_id == 2:
        test_python_code_with_window()
    elif test_id == 3:
        test_pss_file_with_window()
    elif test_id == 4:
        test_simple_triangle()
    elif test_id == 5:
        test_animated_quad()
    elif test_id == 0:
        test_simple_triangle()
        test_animated_quad()
        test_ast_interpret_with_window()
        test_python_code_with_window()
        test_pss_file_with_window()
