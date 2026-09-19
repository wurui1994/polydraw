#!/usr/bin/env python3
"""全量PSS文件测试 - 解析、AST解释执行、Python代码转换

测试所有.pss文件:
1. PSS解析测试: parse_pss_file
2. EVAL解析测试: EvalParser.parse(host_code)
3. AST解释执行测试: EvalInterpreter (离线1帧)
4. Python代码转换测试: EvalToPython (离线1帧)

用法:
  python test_all_pss.py              # 仅解析测试
  python test_all_pss.py parse        # 仅解析测试
  python test_all_pss.py ast          # 解析+AST执行
  python test_all_pss.py python       # 解析+AST执行+Python代码转换
  python test_all_pss.py window       # 带窗口渲染测试(逐个)
"""
import sys
import os
import glob
import time
import traceback
import multiprocessing

# 添加项目路径
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from pss_parser import parse_pss_file
from eval_parser import EvalParser
from eval_interpreter import EvalInterpreter
from eval_to_python import EvalToPython
from runtime.pss_runner import PSSRunner


# ===== PSS文件列表 =====
PSS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PSS_PATTERNS = [
    os.path.join(PSS_DIR, "ken", "*.pss"),
    os.path.join(PSS_DIR, "tigrou", "*.pss"),
]


def get_all_pss_files():
    """获取所有PSS文件路径"""
    files = []
    for pat in PSS_PATTERNS:
        files.extend(sorted(glob.glob(pat)))
    return files


# ===== 测试函数 =====
def test_parse(filepath):
    """测试1: PSS文件解析"""
    try:
        pss = parse_pss_file(filepath)
        host_code = pss.host_code
        return True, f"OK (host:{len(host_code)}chars, v:{len(pss.vertex_blocks)}, f:{len(pss.fragment_blocks)}, g:{len(pss.geometry_blocks)})"
    except Exception as e:
        return False, f"PARSE FAIL: {str(e).split(chr(10))[0][:80]}"


def test_eval_parse(filepath):
    """测试2: EVAL语法解析(host代码)"""
    try:
        pss = parse_pss_file(filepath)
        host_code = pss.host_code
        if not host_code.strip():
            return True, "OK (no host code)"
        ast = EvalParser.parse(host_code)
        return True, f"OK (AST parsed)"
    except Exception as e:
        return False, f"EVAL PARSE FAIL: {str(e).split(chr(10))[0][:80]}"


# ===== 子进程执行函数(用于硬超时) =====
def _ast_interpret_worker(filepath, result_queue):
    """子进程: AST解释执行"""
    try:
        runner = PSSRunner(width=320, height=320, debug=False)
        runner.load_pss(filepath)
        t0 = time.time()
        runner.run(max_frames=1)
        elapsed = time.time() - t0
        result_queue.put(("ok", f"OK ({elapsed:.3f}s, {runner.frame_count} frames)"))
    except Exception as e:
        err = str(e).split('\n')[0][:100]
        result_queue.put(("fail", f"AST EXEC FAIL: {err}"))


def _py_convert_worker(filepath, result_queue):
    """子进程: Python代码转换执行"""
    try:
        runner = PSSRunner(width=320, height=320, debug=False)
        runner.load_pss(filepath)
        if runner._host_ast is None:
            result_queue.put(("ok", "OK (no host code)"))
            return
        t0 = time.time()
        runner.run_python_code(max_frames=1)
        elapsed = time.time() - t0
        result_queue.put(("ok", f"OK ({elapsed:.3f}s, {runner.frame_count} frames)"))
    except Exception as e:
        err = str(e).split('\n')[0][:100]
        result_queue.put(("fail", f"PY EXEC FAIL: {err}"))


def _run_with_timeout(worker_func, filepath, timeout_sec=10.0):
    """用子进程+硬超时执行测试，防止卡死"""
    result_queue = multiprocessing.Queue()
    proc = multiprocessing.Process(target=worker_func, args=(filepath, result_queue))
    proc.start()
    proc.join(timeout=timeout_sec)

    if proc.is_alive():
        proc.terminate()
        proc.join(timeout=2)
        if proc.is_alive():
            proc.kill()
            proc.join()
        return True, f"TIMEOUT (>{timeout_sec:.0f}s, killed)"

    if not result_queue.empty():
        status, msg = result_queue.get_nowait()
        return (status == "ok"), msg

    # 进程退出但没结果
    if proc.exitcode != 0:
        return False, f"CRASH (exit code: {proc.exitcode})"
    return False, "CRASH (no result)"


def test_ast_interpret(filepath, timeout=10.0):
    """测试3: AST解释执行(离线1帧, 硬超时)"""
    return _run_with_timeout(_ast_interpret_worker, filepath, timeout)


def test_python_convert(filepath, timeout=10.0):
    """测试4: Python代码转换执行(离线1帧, 硬超时)"""
    return _run_with_timeout(_py_convert_worker, filepath, timeout)


def test_window_render(filepath, max_frames=300):
    """测试5: 窗口渲染(交互式)"""
    try:
        runner = PSSRunner(width=640, height=640, debug=True)
        runner.load_pss(filepath)
        name = os.path.basename(filepath)
        runner.run_with_window(title=f"PSS - {name}", max_frames=max_frames)
        return True, f"OK ({runner.frame_count} frames)"
    except Exception as e:
        err = str(e).split('\n')[0][:100]
        return False, f"WINDOW FAIL: {err}"


# ===== 批量测试 =====
def run_batch_tests(test_level="ast"):
    """运行批量测试

    test_level:
        "parse"  - 仅解析测试
        "ast"    - 解析 + AST执行
        "python" - 解析 + AST执行 + Python代码转换
    """
    files = get_all_pss_files()
    total = len(files)

    print(f"=== PSS全量测试 (level={test_level}) ===")
    print(f"共 {total} 个PSS文件\n")

    results = {
        "parse_ok": 0, "parse_fail": 0,
        "eval_parse_ok": 0, "eval_parse_fail": 0,
        "ast_ok": 0, "ast_fail": 0, "ast_timeout": 0,
        "py_ok": 0, "py_fail": 0, "py_timeout": 0,
    }
    failures = []

    for i, filepath in enumerate(files, 1):
        name = os.path.relpath(filepath, PSS_DIR)
        print(f"[{i:2d}/{total}] {name}", flush=True)

        # 测试1: PSS解析
        ok, msg = test_parse(filepath)
        if ok:
            results["parse_ok"] += 1
        else:
            results["parse_fail"] += 1
            failures.append(("PSS解析", name, msg))
            print(f"         {msg}", flush=True)
            continue
        print(f"         PSS解析: {msg}", flush=True)

        # 测试2: EVAL语法解析
        ok, msg = test_eval_parse(filepath)
        if ok:
            results["eval_parse_ok"] += 1
        else:
            results["eval_parse_fail"] += 1
            failures.append(("EVAL解析", name, msg))
            print(f"         {msg}", flush=True)
            continue
        print(f"         EVAL解析: {msg}", flush=True)

        if test_level == "parse":
            continue

        # 测试3: AST解释执行(硬超时)
        ok, msg = test_ast_interpret(filepath)
        if ok:
            if "TIMEOUT" in msg:
                results["ast_timeout"] += 1
                failures.append(("AST执行", name, msg))
            else:
                results["ast_ok"] += 1
        else:
            results["ast_fail"] += 1
            failures.append(("AST执行", name, msg))
        print(f"         AST执行: {msg}", flush=True)

        if test_level == "ast":
            continue

        # 测试4: Python代码转换执行(硬超时)
        ok, msg = test_python_convert(filepath)
        if ok:
            if "TIMEOUT" in msg:
                results["py_timeout"] += 1
                failures.append(("PY执行", name, msg))
            else:
                results["py_ok"] += 1
        else:
            results["py_fail"] += 1
            failures.append(("PY执行", name, msg))
        print(f"         PY执行: {msg}", flush=True)

    # 汇总
    print(f"\n{'='*60}")
    print(f"=== 测试结果汇总 ===")
    print(f"  PSS解析:  {results['parse_ok']}/{total} 通过, {results['parse_fail']} 失败")
    print(f"  EVAL解析: {results['eval_parse_ok']}/{total} 通过, {results['eval_parse_fail']} 失败")

    if test_level in ("ast", "python"):
        ast_total = results["ast_ok"] + results["ast_fail"] + results["ast_timeout"]
        print(f"  AST执行:  {results['ast_ok']}/{ast_total} 通过, {results['ast_fail']} 失败, {results['ast_timeout']} 超时")

    if test_level == "python":
        py_total = results["py_ok"] + results["py_fail"] + results["py_timeout"]
        print(f"  PY执行:   {results['py_ok']}/{py_total} 通过, {results['py_fail']} 失败, {results['py_timeout']} 超时")

    if failures:
        print(f"\n--- 失败/超时详情 ({len(failures)}) ---")
        for category, name, msg in failures:
            print(f"  [{category}] {name}: {msg}")

    return failures


def run_window_tests():
    """交互式窗口渲染测试 - 逐个显示"""
    files = get_all_pss_files()
    total = len(files)

    print(f"=== PSS窗口渲染测试 ===")
    print(f"共 {total} 个PSS文件")
    print(f"操作: ESC=退出当前, 关闭窗口=退出全部\n")

    for i, filepath in enumerate(files, 1):
        name = os.path.relpath(filepath, PSS_DIR)
        print(f"\n[{i}/{total}] {name}")

        try:
            runner = PSSRunner(width=640, height=640, debug=True)
            runner.load_pss(filepath)
            runner.run_with_window(title=f"[{i}/{total}] {name}", max_frames=600)
            print(f"  完成: {runner.frame_count} 帧")
        except Exception as e:
            print(f"  错误: {e}")
            traceback.print_exc()

    print("\n全部测试完成!")


def run_single_window(filepath):
    """单个文件窗口渲染测试"""
    if not os.path.exists(filepath):
        print(f"文件不存在: {filepath}")
        return

    print(f"=== 渲染: {filepath} ===")
    try:
        runner = PSSRunner(width=640, height=640, debug=True)
        runner.load_pss(filepath)
        runner.run_with_window(title=os.path.basename(filepath))
        print(f"完成: {runner.frame_count} 帧")
    except Exception as e:
        print(f"错误: {e}")
        traceback.print_exc()


# ===== 主入口 =====
if __name__ == "__main__":
    # macOS multiprocessing 需要这个
    multiprocessing.set_start_method("spawn", force=True)

    if len(sys.argv) <= 1:
        print("用法:")
        print("  python test_all_pss.py parse       # 仅解析测试")
        print("  python test_all_pss.py ast         # 解析+AST执行")
        print("  python test_all_pss.py python      # 解析+AST+Python代码转换")
        print("  python test_all_pss.py window      # 交互式窗口渲染(逐个)")
        print("  python test_all_pss.py <file.pss>  # 单个文件窗口渲染")
        print("  python test_all_pss.py full        # 全部测试(解析+AST+PY)")
        sys.exit(0)

    mode = sys.argv[1]

    if mode == "parse":
        run_batch_tests("parse")
    elif mode == "ast":
        run_batch_tests("ast")
    elif mode == "python":
        run_batch_tests("python")
    elif mode == "full":
        run_batch_tests("python")
    elif mode == "window":
        run_window_tests()
    elif os.path.exists(mode):
        run_single_window(mode)
    else:
        print(f"未知模式: {mode}")
