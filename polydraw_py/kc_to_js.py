"""EVALDRAW .kc → JavaScript/HTML 转换器

将.kc文件转换为可在浏览器中运行的HTML页面。

KC文件格式:
  - 纯EVAL代码, 无shader块
  - 匿名函数签名决定渲染模式:
    (x,y,t,&r,&g,&b) → 逐像素2D/3D绘图模式 (Canvas2D ImageData)
    (x,y,t)          → 1D绘图模式 (Canvas2D折线图)
    ()               → 通用模式 (Canvas2D + WebGL 3D)
  - 支持 struct 语法: struct { x, y, z; } point3d;
  - 支持 evaldraw 特有API: setpix, getpix, setcol, cls, refresh, setcam, drawsph, drawcone, etc.
"""
import os
import json
import re
from typing import Dict, Any, List, Optional, Set, Tuple

from eval_to_js import (
    is_tree, is_token, get_type, get_value, children,
    is_op_token, get_op_value,
    _JS_BUILTIN_MAP as _PSS_BUILTIN_MAP,
    _JS_CONST_MAP as _PSS_CONST_MAP,
    EvalToJS,
)


# ===== evaldraw 扩展的内置函数映射 =====
_KC_BUILTIN_MAP = dict(_PSS_BUILTIN_MAP)
_KC_BUILTIN_MAP.update({
    # 2D绘图
    "setpix": "KC.setpix", "getpix": "KC.getpix",
    "setcol": "KC.setcol", "cls": "KC.cls",
    "clz": "KC.clz", "refresh": "KC.refresh",
    "setfont": "KC.setfont", "printf": "KC.printf",
    "printchar": "KC.printchar", "moveto": "KC.moveto",
    "drawline": "KC.drawline", "drawrect": "KC.drawrect",
    # 3D绘图
    "setcam": "KC.setcam",
    "drawsph": "KC.drawsph", "drawcone": "KC.drawcone",
    "drawspr": "KC.drawspr", "drawkv6": "KC.drawkv6",
    "drawline3d": "KC.drawline3d",
    # 输入
    "readmouse": "KC.readmouse",
    # 音频
    "playtext": "KC.playtext", "playnote": "KC.playnote",
    # 系统
    "sleep": "KC.sleep",
    "rgba": "KC.rgba", "getrgb": "KC.getrgb",
    "fadd": "KC.fadd",
    "frameinit": "KC.frameinit",
    # GL扩展
    "glsettex": "KC.glsettex", "glgettex": "KC.glgettex",
    "glenable": "KC.glenable", "gldisable": "KC.gldisable",
    "glcullface": "KC.glcullface",
    # 缓冲区
    "bufset": "KC.bufset", "bufcpy": "KC.bufcpy",
    "sethlin": "KC.sethlin", "gethlin": "KC.gethlin",
    # 文件
    "pic": "KC.pic", "fil": "KC.fil", "mountzip": "KC.mountzip",
    "getpicsiz": "KC.getpicsiz",
    # 数学扩展
    "normrand": "KC.normrand",
    "bouncevec": "KC.bouncevec",
    "getnorm": "KC.getnorm",
    "rotvex": "KC.rotvex",
})

# evaldraw 扩展常量映射
_KC_CONST_MAP = dict(_PSS_CONST_MAP)
_KC_CONST_MAP.update({
    "drawcone_flat": "0x1", "drawcone_cent": "0x2",
})


def detect_kc_mode(code: str) -> str:
    """从.kc代码中检测渲染模式

    Returns:
        'perpixel' - (x,y,t,&r,&g,&b) 逐像素模式
        'graph1d'  - (x,y,t) 1D绘图模式
        'general'  - () 通用模式
    """
    m = re.match(r'\s*\(([^)]*)\)', code)
    if m:
        params_str = m.group(1).strip()
        if not params_str:
            return 'general'
        params = [p.strip() for p in params_str.split(',')]
        has_rgb_refs = any(p.startswith('&') for p in params)
        if has_rgb_refs and len(params) >= 5:
            return 'perpixel'
        if len(params) == 3 and not has_rgb_refs:
            return 'graph1d'
    return 'general'


def parse_struct_defs(code: str) -> List[Dict[str, Any]]:
    """解析 struct 定义

    支持两种格式:
      struct { field1, field2, ...; } typename;   (C风格)
      struct typename { field1, field2, ...; };    (C++风格)
    
    字段可以带类型名: struct box_t {x,y,w,h; col_t col;};
    """
    structs = []
    # 格式1: struct { fields; } typename;
    pattern1 = re.compile(r'struct\s*\{([^}]*)\}\s*(\w+)\s*;', re.IGNORECASE)
    # 格式2: struct typename { fields; };
    pattern2 = re.compile(r'struct\s+(\w+)\s*\{([^}]*)\}\s*;', re.IGNORECASE)
    
    seen_names = set()
    for m in pattern1.finditer(code):
        fields_str = m.group(1)
        name = m.group(2)
        if name in seen_names:
            continue
        seen_names.add(name)
        fields = _parse_struct_fields(fields_str)
        structs.append({'name': name, 'fields': fields})
    
    for m in pattern2.finditer(code):
        name = m.group(1)
        fields_str = m.group(2)
        if name in seen_names:
            continue
        seen_names.add(name)
        fields = _parse_struct_fields(fields_str)
        structs.append({'name': name, 'fields': fields})
    
    return structs


def _parse_struct_fields(fields_str: str) -> List[str]:
    """解析struct字段列表, 处理带类型名的字段
    
    例如: "x,y,w,h; col_t col" → ["x", "y", "w", "h", "col"]
         "p3d_t r,d,f" → ["r", "d", "f"]
    """
    # 先按 ; 分割(处理分号分隔的字段组)
    parts = fields_str.replace(';', ',').split(',')
    fields = []
    for p in parts:
        p = p.strip()
        if not p:
            continue
        # 检查是否是 "typename fieldname" 格式
        tokens = p.split()
        if len(tokens) >= 2:
            # 最后一个token是字段名, 前面的是类型名
            fields.append(tokens[-1])
        else:
            fields.append(p)
    return fields


def strip_struct_defs(code: str) -> str:
    """移除 struct 定义行(已由parse_struct_defs处理)"""
    # 格式1: struct { fields; } typename;
    code = re.sub(r'struct\s*\{[^}]*\}\s*\w+\s*;', '', code)
    # 格式2: struct typename { fields; };
    code = re.sub(r'struct\s+\w+\s*\{[^}]*\}\s*;', '', code)
    return code


def preprocess_kc(code: str) -> str:
    """KC代码预处理: 处理C预处理器指令和字符常量"""
    # ===== C预处理器指令处理 =====
    # 策略: 对于 #if (expr), #elif (expr), #ifdef, #ifndef 等,
    # 我们无法在转译时求值, 所以默认保留最后一个 #else/#elif 1 块
    # (通常是默认/主要实现), 移除其他条件块

    # 1. 处理 #if 0 ... #else ... #endif (移除 #if 0 块, 保留 #else 块)
    code = re.sub(
        r'#if\s+0\b.*?(?:#elif\b.*?)*?(?:#else\b(.*?))?#endif\b',
        r'\1',
        code,
        flags=re.DOTALL
    )
    # 2. 处理 #if 1 ... #endif (保留内容, 移除 #else 块)
    code = re.sub(
        r'#if\s+1\b(.*?)(?:#else\b.*?)?#endif\b',
        r'\1',
        code,
        flags=re.DOTALL
    )
    # 3. 处理 #if (expr) ... #elif 1 ... #endif — 保留 #elif 1 块
    code = re.sub(
        r'#if\s+\(.*?\)\b.*?#elif\s+1\b(.*?)(?:#else\b.*?)?#endif\b',
        r'\1',
        code,
        flags=re.DOTALL
    )
    # 4. 处理 #if (expr) ... #else ... #endif — 保留 #else 块
    code = re.sub(
        r'#if\s+\(.*?\)\b.*?(?:#elif\b.*?)*?(?:#else\b(.*?))?#endif\b',
        r'\1',
        code,
        flags=re.DOTALL
    )
    # 5. 处理 #ifdef ... #else ... #endif — 保留 #else 块
    code = re.sub(
        r'#ifdef\b.*?(?:#elif\b.*?)*?(?:#else\b(.*?))?#endif\b',
        r'\1',
        code,
        flags=re.DOTALL
    )
    # 6. 处理 #ifndef ... #else ... #endif — 保留 #ifndef 块(第一个块)
    code = re.sub(
        r'#ifndef\b(\w+)(.*?)(?:#else\b.*?)?#endif\b',
        r'\2',
        code,
        flags=re.DOTALL
    )
    # 7. 移除剩余的 # 开头的预处理指令 (#include, #define 等)
    code = re.sub(r'^\s*#\s*(?:include|define|undef|ifdef|ifndef|endif|else|elif|if|pragma|error|warning)\b.*$', '', code, flags=re.MULTILINE)

    # ===== 块注释处理 =====
    # 移除 /* ... */ 块注释 (可能跨行)
    # 注意: 需要避免匹配 // 行注释中的 /*
    # 策略: 先移除行注释内容(保留换行), 再移除块注释, 最后恢复行注释
    # 更简单的策略: 逐字符扫描, 跟踪是否在行注释中
    def remove_comments(code):
        result = []
        i = 0
        while i < len(code):
            # 行注释: // 到行尾
            if code[i:i+2] == '//' and (i == 0 or code[i-1] != ':'):
                # 跳过到行尾
                while i < len(code) and code[i] != '\n':
                    i += 1
                continue
            # 块注释: /* 到 */
            if code[i:i+2] == '/*':
                i += 2
                while i < len(code) - 1 and code[i:i+2] != '*/':
                    if code[i] == '\n':
                        result.append('\n')  # 保留换行以维持行号
                    i += 1
                if i < len(code) - 1:
                    i += 2  # 跳过 */
                continue
            result.append(code[i])
            i += 1
        return ''.join(result)
    code = remove_comments(code)

    # ===== 字符常量处理 =====
    # 'A' → 65, '\n' → 10, 等
    # 先处理三重单引号 ''' → 39 (单引号的ASCII码)
    code = re.sub(r"'''", '39', code)

    def char_to_num(m):
        ch = m.group(1)
        if ch.startswith('\\'):
            escape_map = {'n': 10, 'r': 13, 't': 9, '\\': 92, "'": 39, '"': 34, '0': 0}
            return str(escape_map.get(ch[1], ord(ch[1])))
        return str(ord(ch))

    code = re.sub(r"'(\\.|[^'\\])'", char_to_num, code)

    # ===== 科学计数法处理 =====
    # 4.70935E-5 → 4.70935e-5 (E → e)
    # 但由于解析器的FLOAT_NUM词法优先级问题, \d+\.\d* 会先匹配 4.70935
    # 导致 e-5 被当作标识符。所以需要将科学计数法转换为普通小数
    # 注意: 不能匹配十六进制数中的e, 如 0xe0e0e0 中的 e0e0
    # 使用负向后瞻: 确保前面不是十六进制数字 (0-9, a-f, A-F, x, X)
    def sci_to_float(m):
        try:
            return str(float(m.group(0)))
        except ValueError:
            return m.group(0)
    code = re.sub(r'(?<![0-9a-fA-FxX])\d+\.?\d*[eE][+-]?\d+', sci_to_float, code)

    # ===== enum 中的分号处理 =====
    # enum {MAXDEP=8;} → enum {MAXDEP=8} (移除enum花括号内的分号)
    # enum { casehei =1.000, casethk = .100, casecurverad = .300; } → enum { casehei =1.000, casethk = .100, casecurverad = .300 }
    def fix_enum_semicolons(m):
        content = m.group(1)
        # 移除enum内容中的分号, 替换为逗号
        content = content.replace(';', ',')
        # 移除尾随逗号: , } → }
        content = re.sub(r',\s*$', '', content)
        return f'enum {{{content}}}'
    code = re.sub(r'enum\s*\{([^}]*)\}', fix_enum_semicolons, code, flags=re.IGNORECASE)

    # ===== 字符串中的转义反斜杠处理 =====
    # "Brake: Shift or R.MouseBut\\n" → "Brake: Shift or R.MouseBut\n"
    # evaldraw中 \\n 在字符串里表示换行, 但解析器可能把 \\n 当作字面反斜杠+n
    # 需要将字符串中的 \\n, \\t 等转换为 \n, \t
    def fix_string_escapes(m):
        s = m.group(0)
        # 将 \\n → \n, \\t → \t, \\\\ → \\ 等
        s = s.replace('\\\\n', '\\n')
        s = s.replace('\\\\t', '\\t')
        s = s.replace('\\\\r', '\\r')
        s = s.replace('\\\\\\', '\\')
        return s
    code = re.sub(r'"(?:[^"\\]|\\.)*"', fix_string_escapes, code)

    # ===== 相邻字符串拼接 =====
    # C语言中 "hello" "world" 等价于 "helloworld"
    # 需要将相邻的字符串字面量合并为一个
    # 反复替换直到没有变化 (处理多个相邻字符串)
    prev = None
    while prev != code:
        prev = code
        # 匹配 "str1" 紧跟 "str2" (中间只有空白)
        # 需要正确处理字符串内的转义引号
        code = re.sub(r'"((?:[^"\\]|\\.)*)"\s+"((?:[^"\\]|\\.)*)"', r'"\1\2"', code)

    # ===== 数组初始化中的空元素和尾随逗号 =====
    # 处理 {65,  ,  ,65, ...} 中的空元素 → 替换为 0
    # 处理 {0.000, 0.000, 0.000,,} 中的尾随双逗号
    # 需要多次替换, 因为 ,0,, 这样的中间结果可能还有连续逗号
    prev = None
    while prev != code:
        prev = code
        code = re.sub(r',\s*,', ',0,', code)  # ,, → ,0,
    code = re.sub(r'\{\s*,', '{0,', code)  # {, → {0,

    # ===== 函数调用中的尾随逗号 =====
    # drawcone(ox,oy,96,.25, cx,cy,96,.25,) → drawcone(ox,oy,96,.25, cx,cy,96,.25)
    # 只在函数调用参数中移除尾随逗号: ,) → )
    code = re.sub(r',\s*\)', ')', code)

    # ===== 冒号作为语句分隔符 =====
    # 在evaldraw中, )后跟: 可以作为语句分隔符 (类似;)
    # setcol(0xc0c0c0): drawsph(...) → setcol(0xc0c0c0); drawsph(...)
    # 但不能替换 ? : 三元运算符中的冒号
    # 只替换 ) 后紧跟的 : 且前面没有 ? 的情况
    # 使用负向前瞻: ): 后面不是 : (避免匹配 ::)
    # 使用负向后瞻: 前面不是 ? (避免匹配 ?:)
    code = re.sub(r'(?<!\?)\)\s*:(?!:)', ');', code)

    # ===== 空语句处理 =====
    # { ; } → { } (空语句块)
    code = re.sub(r'\{\s*;\s*\}', '{}', code)
    # while(cond); → while(cond) {} (空while体)
    # 注意: 1) 不匹配 do...while(cond); 中的while
    #       2) 需要处理嵌套括号 (如 while(net_recv(&a,&b) == 0);)
    code = _transform_empty_while(code)
    # else if (mode == 3) { ; } → else if (mode == 3) {} 
    # 单独的 ; 在某些上下文中也需要处理, 但不能全局移除
    # 因为 for(;;) 中的 ; 是合法的

    # ===== while(1) refresh() 模式转换 =====
    code = _transform_while_refresh(code)

    return code


def _transform_empty_while(code: str) -> str:
    """将空语句体转换为空块: while(cond); → while(cond) {}, if(cond); → if(cond) {}, for(;;); → for(;;) {}

    注意:
    - 不匹配 do { ... } while(cond); 中的while (do-while的;是合法的)
    - 需要处理嵌套括号
    - 也处理 else ; → else {} 和 else if(cond); → else if(cond) {}
    """
    result = []
    i = 0
    while i < len(code):
        # 检查关键字: while, if, for, else if, else
        matched = False

        # 检查 else if / else
        if code[i:i+4] == 'else' and (i == 0 or not code[i-1].isalnum() and code[i-1] != '_'):
            j = i + 4
            while j < len(code) and code[j] in ' \t':
                j += 1
            if code[j:j+2] == 'if' and (j+2 >= len(code) or not code[j+2].isalnum() and code[j+2] != '_'):
                # else if(cond); → else if(cond) {}
                k = j + 2
                while k < len(code) and code[k] in ' \t':
                    k += 1
                if k < len(code) and code[k] == '(':
                    end_paren = _find_matching_paren(code, k)
                    if end_paren is not None:
                        m = end_paren + 1
                        while m < len(code) and code[m] in ' \t':
                            m += 1
                        if m < len(code) and code[m] == ';':
                            result.append(code[i:m])
                            result.append(' {}')
                            i = m + 1
                            matched = True
            elif not code[j:j+2] == 'if' or (j+2 < len(code) and code[j+2].isalnum() or code[j+2] == '_'):
                # else ; → else {}
                m = j
                while m < len(code) and code[m] in ' \t':
                    m += 1
                if m < len(code) and code[m] == ';':
                    result.append(code[i:m])
                    result.append(' {}')
                    i = m + 1
                    matched = True

        if not matched and code[i:i+5] == 'while' and (i == 0 or not code[i-1].isalnum() and code[i-1] != '_'):
            # 检查前面是否是 do...while
            j = i - 1
            while j >= 0 and code[j] in ' \t\n\r':
                j -= 1
            is_do_while = (j >= 0 and code[j] == '}')

            if not is_do_while:
                k = i + 5
                while k < len(code) and code[k] in ' \t':
                    k += 1
                if k < len(code) and code[k] == '(':
                    end_paren = _find_matching_paren(code, k)
                    if end_paren is not None:
                        m = end_paren + 1
                        while m < len(code) and code[m] in ' \t':
                            m += 1
                        if m < len(code) and code[m] == ';':
                            result.append(code[i:m])
                            result.append(' {}')
                            i = m + 1
                            matched = True

        if not matched and code[i:i+2] == 'if' and (i == 0 or not code[i-1].isalnum() and code[i-1] != '_'):
            # 确保不是 else if (已处理)
            j = i - 1
            while j >= 0 and code[j] in ' \t':
                j -= 1
            is_else_if = (j >= 1 and code[j-1:j+1] == 'el')
            if not is_else_if:
                k = i + 2
                while k < len(code) and code[k] in ' \t':
                    k += 1
                if k < len(code) and code[k] == '(':
                    end_paren = _find_matching_paren(code, k)
                    if end_paren is not None:
                        m = end_paren + 1
                        while m < len(code) and code[m] in ' \t':
                            m += 1
                        if m < len(code) and code[m] == ';':
                            result.append(code[i:m])
                            result.append(' {}')
                            i = m + 1
                            matched = True

        if not matched and code[i:i+3] == 'for' and (i == 0 or not code[i-1].isalnum() and code[i-1] != '_'):
            k = i + 3
            while k < len(code) and code[k] in ' \t':
                k += 1
            if k < len(code) and code[k] == '(':
                end_paren = _find_matching_paren(code, k)
                if end_paren is not None:
                    m = end_paren + 1
                    while m < len(code) and code[m] in ' \t':
                        m += 1
                    if m < len(code) and code[m] == ';':
                        result.append(code[i:m])
                        result.append(' {}')
                        i = m + 1
                        matched = True

        if not matched:
            result.append(code[i])
            i += 1
    return ''.join(result)


def _find_matching_paren(code: str, start: int) -> Optional[int]:
    """找到与start位置的(匹配的)的位置, 返回None如果不匹配"""
    if start >= len(code) or code[start] != '(':
        return None
    depth = 1
    k = start + 1
    while k < len(code) and depth > 0:
        if code[k] == '(':
            depth += 1
        elif code[k] == ')':
            depth -= 1
        k += 1
    if depth == 0:
        return k - 1  # 返回)的位置
    return None


def _transform_while_refresh(code: str) -> str:
    """将 while(1) { ... refresh(); Sleep(N); } 转换为每帧执行模式

    在原始evaldraw中, refresh()会显示画面并等待下一帧。
    在JS中, 我们需要让动画循环来处理帧迭代。

    策略:
    - 对于 while(1) refresh(); (纯显示循环): 移除, 因为动画循环会处理
    - 对于 while(1) { body; refresh(); Sleep(N); }: 将body作为每帧执行的代码
      移除while(1)和refresh/Sleep, 让动画循环每帧调用hostFrame
    """
    # 处理 while(1) refresh(); (无花括号, 纯显示循环)
    code = re.sub(r'while\s*\(\s*1\s*\)\s*refresh\s*\([^)]*\)\s*;', '', code)
    # 处理 while(1) { refresh(); } (花括号, 纯显示循环)
    code = re.sub(r'while\s*\(\s*1\s*\)\s*\{\s*refresh\s*\([^)]*\)\s*;\s*\}', '', code)

    # 处理 while(1) { body; refresh(); Sleep(N); } (有实际逻辑的循环)
    # 使用从后向前替换避免位置偏移
    pattern = re.compile(r'while\s*\(\s*1\s*\)\s*\{', re.IGNORECASE)
    matches = list(pattern.finditer(code))

    # 从后向前处理, 避免位置偏移
    for m in reversed(matches):
        start = m.start()
        brace_start = m.end() - 1  # { 的位置
        # 找到匹配的 }
        depth = 1
        pos = brace_start + 1
        while pos < len(code) and depth > 0:
            if code[pos] == '{':
                depth += 1
            elif code[pos] == '}':
                depth -= 1
            pos += 1
        if depth != 0:
            continue

        body = code[brace_start + 1:pos - 1]

        # 检查body中是否包含 refresh()
        has_refresh = bool(re.search(r'\brefresh\s*\(', body))

        if has_refresh:
            # 移除 refresh() 和 Sleep() 调用
            new_body = re.sub(r';\s*refresh\s*\([^)]*\)\s*;', ';', body)
            new_body = re.sub(r';\s*Sleep\s*\([^)]*\)\s*;', ';', new_body)
            new_body = re.sub(r'refresh\s*\([^)]*\)\s*;', '', new_body)
            new_body = re.sub(r'Sleep\s*\([^)]*\)\s*;', '', new_body)
            # 替换 while(1) { body } 为 { body }
            replacement = '{' + new_body + '}'
            code = code[:start] + replacement + code[pos:]

    return code


def strip_typed_statics(code: str, struct_defs: List[Dict]) -> str:
    """移除 static 声明中的自定义类型名

    例如: static part_t part[PARTMAX]; → static part[PARTMAX];
         static point3d pts[100]; → static pts[100];
    """
    struct_names = set(s['name'] for s in struct_defs)
    # 匹配: static typename varname ... ;
    # typename 是已知的struct名
    def replacer(m):
        typename = m.group(1)
        if typename in struct_names:
            return f"static {m.group(2)}"
        return m.group(0)

    return re.sub(
        r'\bstatic\s+(' + '|'.join(struct_names) + r')\s+(\w+)',
        replacer,
        code
    )


def strip_typed_locals(code: str, struct_defs: List[Dict]) -> str:
    """移除局部变量声明中的自定义类型名，并转换花括号初始化为字段赋值

    例如: point3d pp = {0,0,-4}; → pp = _new_point3d(0,0,-4);
         point3d pol[4] = {-1,-1,0, 1,-1,0, 1,1,0, -1,1,0}; → pol = _arr_point3d(-1,-1,0, 1,-1,0, 1,1,0, -1,1,0);
         point3d norm, vin, vout; → norm = _new_point3d(); vin = _new_point3d(); vout = _new_point3d();

    _new_point3d() 和 _arr_point3d() 是在JS运行时中定义的辅助函数
    """
    struct_names = set(s['name'] for s in struct_defs)
    struct_field_count = {s['name']: len(s['fields']) for s in struct_defs}
    if not struct_names:
        return code

    # 匹配: typename varname ... ; (局部变量声明)
    pattern = re.compile(
        r'(?<![a-zA-Z0-9_])(' + '|'.join(struct_names) + r')\s+'
        r'(\w+(?:\s*\[[^\]]*\])*(?:\s*=\s*\{[^}]*\})?'
        r'(?:\s*,\s*\w+(?:\s*\[[^\]]*\])*(?:\s*=\s*\{[^}]*\})?)*)\s*;'
    )

    def replacer(m):
        typename = m.group(1)
        var_part = m.group(2).strip()
        nfields = struct_field_count.get(typename, 0)
        tl = typename.lower()

        # 处理逗号分隔的多个变量
        vars_list = []
        current = ''
        depth = 0
        for ch in var_part:
            if ch == '{': depth += 1
            elif ch == '}': depth -= 1
            elif ch == ',' and depth == 0:
                vars_list.append(current.strip())
                current = ''
                continue
            current += ch
        if current.strip():
            vars_list.append(current.strip())

        result_parts = []
        for v in vars_list:
            v = v.strip()
            # 检查是否有数组维度 [N]
            arr_match = re.match(r'(\w+)\s*\[(\d+)\]\s*(?:=\s*(\{[^}]*\}))?', v)
            simple_match = re.match(r'(\w+)\s*(?:=\s*(\{[^}]*\}))?', v)

            if arr_match:
                varname = arr_match.group(1)
                arr_size = int(arr_match.group(2))
                brace_init = arr_match.group(3)
                if brace_init:
                    inner = brace_init.strip().lstrip('{').rstrip('}').strip()
                    result_parts.append(f"{varname} = _arr_{tl}({inner})")
                else:
                    # 无初始化的数组
                    result_parts.append(f"{varname} = _arr_{tl}({', '.join(['0'] * (arr_size * nfields))})")
            elif simple_match:
                varname = simple_match.group(1)
                brace_init = simple_match.group(2)
                if brace_init:
                    inner = brace_init.strip().lstrip('{').rstrip('}').strip()
                    result_parts.append(f"{varname} = _new_{tl}({inner})")
                else:
                    result_parts.append(f"{varname} = _new_{tl}()")

        return '; '.join(result_parts) + ';'

    return pattern.sub(replacer, code)


def strip_auto_keyword(code: str) -> str:
    """移除 auto 关键字 (evaldraw中auto等价于局部变量声明)

    例如: auto perim, grab=-1, grabx, graby; → perim, grab=-1, grabx, graby;
         auto ndice[4]; → ndice[4];
    """
    return re.sub(r'\bauto\s+', '', code)


def strip_typed_func_params(code: str, struct_defs: List[Dict]) -> str:
    """移除命名函数参数中的自定义类型名

    例如: bouncevec (point3d vin, point3d vout, point3d norm) → bouncevec (vin, vout, norm)
         getnorm (point3d pol[4], point3d norm) → getnorm (pol, norm)
         inssort (a[NMAX], n) → inssort (a, n)
    """
    struct_names = set(s['name'] for s in struct_defs)

    # 匹配命名函数定义: funcname ( ... ) { 或 funcname ( ... ) // comment {
    # 要求 funcname 在行首(可能有缩进), 排除关键字
    keywords = {'if', 'while', 'for', 'switch', 'do', 'return', 'enum', 'static'}
    func_pattern = re.compile(
        r'^(\s*)(\w+)\s*(\([^)]*\))\s*(?://[^\n]*)?\s*\{',
        re.MULTILINE
    )

    def replacer(m):
        indent = m.group(1)
        func_name = m.group(2)
        if func_name.lower() in keywords:
            return m.group(0)  # 不处理关键字
        params = m.group(3)
        # 在参数列表中移除类型名
        if struct_names:
            type_pattern = re.compile(
                r'\b(' + '|'.join(struct_names) + r')\s+'
            )
            params = type_pattern.sub('', params)
        # 移除数组参数中的维度: a[NMAX] → a, pol[4] → pol
        params = re.sub(r'(\w+)\[[^\]]*\]', r'\1', params)
        return f"{indent}{func_name} {params} {{"

    return func_pattern.sub(replacer, code)


class KCToJS(EvalToJS):
    """EVALDRAW .kc → JavaScript 转换器"""

    def __init__(self, mode: str = 'general', struct_defs: List[Dict] = None):
        super().__init__(for_webgl=True)
        self._mode = mode
        self._struct_defs = struct_defs or []
        self._struct_names: Set[str] = set()
        self._struct_field_map: Dict[str, List[str]] = {}
        for s in self._struct_defs:
            self._struct_names.add(s['name'].lower())
            self._struct_field_map[s['name']] = s['fields']

    def _convert_expr(self, node) -> str:
        """重写表达式转换, 使用KC内置函数映射"""
        if node is None or not isinstance(node, dict):
            return "0"

        ntype = get_type(node)
        ch = children(node)

        if ntype == "NUMBER":
            return get_value(node)
        if ntype == "FLOAT_NUM":
            return get_value(node)
        if ntype == "STRING":
            return get_value(node)

        if ntype in ("IDENTIFIER", "PARAM_ID"):
            name = get_value(node)
            key = name.lower()
            if key in _KC_CONST_MAP:
                return _KC_CONST_MAP[key]
            if key == "rnd":
                return "Math.random()"
            if key == "nrnd":
                return "KC.normrand()"
            if key == "keystatus":
                return "KC.keystatus"
            if key in ("mousx", "mousy", "bstatus", "numframes", "xres", "yres"):
                return f"SYS.{key}"
            if key == "frameinit":
                return "KC._frameinit ? 1 : 0"
            return name

        if is_op_token(node):
            return get_op_value(node)

        if not is_tree(node):
            return "0"

        if ntype == "comma_expr":
            parts = [self._convert_expr(c) for c in ch]
            return parts[0] if len(parts) == 1 else "(" + ", ".join(parts) + ")"

        if ntype == "assign_expr":
            if len(ch) == 1:
                return self._convert_expr(ch[0])
            if len(ch) == 3:
                left = self._convert_expr(ch[0])
                op = get_op_value(ch[1]) if is_op_token(ch[1]) else get_value(ch[1])
                right = self._convert_expr(ch[2])
                return f"({left} {op} {right})"

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

        if ntype == "unary_expr":
            if len(ch) == 1:
                return self._convert_expr(ch[0])
            if len(ch) == 2:
                op = get_value(ch[0]) if is_token(ch[0]) else ""
                operand = self._convert_expr(ch[1])
                if op == "-": return f"(-{operand})"
                elif op == "!": return f"(!{operand} ? 1 : 0)"
                elif op == "+": return f"(+{operand})"
                elif op == "&": return operand

        if ntype == "postfix_expr":
            return self._convert_expr(ch[0]) if ch else "0"

        if ntype == "array_access":
            name = self._extract_ident(ch[0])
            index = "0"
            for c in ch[1:]:
                if is_token(c) and get_type(c) in ("LBRACK", "RBRACK"):
                    continue
                index = self._convert_expr(c)
                break
            return f"{name}[{index} & ({name}.length - 1)]"

        if ntype == "member_access":
            obj = self._convert_expr(ch[0])
            field = get_value(ch[1]) if is_token(ch[1]) else self._extract_ident(ch[1])
            return f"{obj}.{field}"

        if ntype == "func_call":
            name = self._extract_ident(ch[0])
            args = []
            for c in ch[1:]:
                if get_type(c) == "args":
                    args = [self._convert_expr(ac) for ac in children(c)]
                    break
            key = name.lower()
            if key in _KC_BUILTIN_MAP:
                mapped = _KC_BUILTIN_MAP[key]
                self._used_helpers.add(mapped.split("(")[0].split(".")[0])
                return f"{mapped}({', '.join(args)})"
            if key in self._user_funcs:
                return f"{name}({', '.join(args)})"
            return f"/* unknown */ {name}({', '.join(args)})"

        if ntype == "post_inc":
            return f"{self._extract_ident(ch[0])}++"
        if ntype == "post_dec":
            return f"{self._extract_ident(ch[0])}--"
        if ntype == "pre_inc":
            name = get_value(ch[-1]) if is_token(ch[-1]) else self._extract_ident(ch[-1])
            return f"++{name}"
        if ntype == "pre_dec":
            name = get_value(ch[-1]) if is_token(ch[-1]) else self._extract_ident(ch[-1])
            return f"--{name}"

        if ntype in ("anon_func", "anon_func_empty"):
            params = []
            body_stmts = []
            for c in ch:
                ntype2 = get_type(c)
                if ntype2 == "func_params":
                    params = [self._extract_ident(pc) for pc in children(c)]
                elif ntype2 in ("block", "func_stmts", "func_body"):
                    body_stmts = self._extract_body_stmts(c)
            body_lines = [f"  {self._convert_expr(s)};" for s in body_stmts]
            body_str = "\n".join(body_lines) if body_lines else "  return 0;"
            return f"({', '.join(params)}) => {{\n{body_str}\n}}"

        if ch:
            return self._convert_expr(ch[0])
        return "0"

    def _extract_body_stmts(self, node):
        """从func_body/func_stmts/func_inner中递归提取实际语句列表"""
        if not is_tree(node):
            return []
        ntype = get_type(node)
        ch = children(node)
        if ntype == "block":
            return ch
        if ntype == "func_body":
            # func_body → [func_stmts] 或 [block]
            stmts = []
            for c in ch:
                stmts.extend(self._extract_body_stmts(c))
            return stmts
        if ntype == "func_stmts":
            # func_stmts → [func_inner, ...]
            stmts = []
            for c in ch:
                stmts.extend(self._extract_body_stmts(c))
            return stmts
        if ntype == "func_inner":
            # func_inner → [expr_stmt / if_stmt / ...]
            return ch
        return ch

    def _convert_anon_func(self, node):
        """转换匿名函数 - 根据模式生成不同的入口函数"""
        ch = children(node)
        raw_params = []  # (name, is_ref) 元组
        body_stmts = []
        for c in ch:
            ntype2 = get_type(c)
            if ntype2 == "func_params":
                for pc in children(c):
                    pname = self._extract_ident(pc)
                    # 检查是否是引用参数 (&r, &g, &b)
                    is_ref = get_type(pc) == "param_ptr"
                    raw_params.append((pname, is_ref))
            elif ntype2 in ("block", "func_stmts", "func_body"):
                body_stmts = self._extract_body_stmts(c)

        if self._mode == 'perpixel':
            func_name = "hostPixel"
        elif self._mode == 'graph1d':
            func_name = "hostGraph"
        else:
            func_name = "hostFrame"

        # perpixel模式: &r,&g,&b 引用参数合并为 _color 数组
        ref_params = [p for p, is_ref in raw_params if is_ref]
        val_params = [p for p, is_ref in raw_params if not is_ref]

        if self._mode == 'perpixel' and ref_params:
            # 函数签名: hostPixel(x, y, t, _color)
            param_str = ", ".join(val_params + ["_color"])
            self._line(f"function {func_name}({param_str}) {{")
            self._indent += 1
            # 解构: let r = _color[0], g = _color[1], b = _color[2];
            destructure = ", ".join(ref_params)
            indices = ", ".join(str(i) for i in range(len(ref_params)))
            self._line(f"let {destructure} = [_color[{indices.replace(', ', '], _color[')}]];")
            # 修正: 用更清晰的方式
            self._indent -= 1
            # 重新生成
            self._lines.pop()  # 移除最后的 }

        # 通用路径
        if not (self._mode == 'perpixel' and ref_params):
            param_str = ", ".join(p for p, _ in raw_params)
            self._line(f"function {func_name}({param_str}) {{")
            self._indent += 1

        # 收集局部变量
        all_param_names = [p for p, _ in raw_params]
        local_vars = set()
        for stmt in body_stmts:
            self._collect_local_vars(stmt, local_vars, set(p.lower() for p in all_param_names))
        param_lower = set(p.lower() for p in all_param_names)
        local_vars = {v for v in local_vars if v.lower() not in param_lower and v.lower() not in self._static_names}
        if local_vars:
            self._line(f"let {', '.join(sorted(local_vars))};")

        # perpixel模式: 解构引用参数
        if self._mode == 'perpixel' and ref_params:
            for i, pname in enumerate(ref_params):
                self._line(f"let {pname} = _color[{i}];")

        for stmt in body_stmts:
            self._convert_stmt(stmt)

        # perpixel模式: 写回引用参数
        if self._mode == 'perpixel' and ref_params:
            for i, pname in enumerate(ref_params):
                self._line(f"_color[{i}] = {pname};")

        self._indent -= 1
        self._line("}")
        self._line("")


def _generate_struct_js(struct_defs: List[Dict]) -> str:
    """生成struct相关的JS代码"""
    if not struct_defs:
        return ""
    lines = []
    for s in struct_defs:
        name = s['name']
        fields = s['fields']
        args = ", ".join(fields)
        assigns = "; ".join(f"this.{f} = {f} || 0" for f in fields)
        lines.append(f"function {name}({args}) {{ {assigns}; }}")
        # _new_typename() 构造函数
        lines.append(f"function _new_{name.lower()}(...a) {{ return new {name}(...a); }}")
        # _arr_typename() 数组构造函数 - 将扁平值列表转为struct数组
        nfields = len(fields)
        lines.append(f"function _arr_{name.lower()}(...vals) {{")
        lines.append(f"  const arr = [];")
        lines.append(f"  for (let i = 0; i < vals.length; i += {nfields}) {{")
        line_args = ", ".join(f"vals[i + {j}]" for j in range(nfields))
        lines.append(f"    arr.push(new {name}({line_args}));")
        lines.append(f"  }}")
        lines.append(f"  return arr;")
        lines.append(f"}}")
    return "\n".join(lines)


def strip_multidim_arrays(code: str) -> str:
    """将static声明中的多维数组转换为一维数组

    例如: static ramp[8][4] → static ramp[32]
         static music[MUSICN][3] → static music[MUSICN*3]
         static grid[16][16] → static grid[256]
    
    注意: 只处理static声明中的多维数组, 不处理表达式中的多维数组访问
    """
    def replacer(m):
        prefix = m.group(1)  # "static varname"
        dims_str = m.group(2)  # "[dim1][dim2]..."
        rest = m.group(3)  # 剩余部分 (= init, , next var, etc.)
        
        dims = re.findall(r'\[([^\]]*)\]', dims_str)
        if len(dims) < 2:
            return m.group(0)
        # 计算总大小
        total = ''
        for i, d in enumerate(dims):
            d = d.strip()
            if not d:
                return m.group(0)  # 空维度, 不处理
            if i == 0:
                total = d
            else:
                total = f'({total})*({d})'
        return f'{prefix}[{total}]{rest}'

    # 只匹配static声明中的多维数组: static varname[dim1][dim2]...
    return re.sub(
        r'(static\s+\w+)((?:\[[^\]]*\]){2,})((?:\s*=|,|;).*)',
        replacer,
        code
    )


def kc_to_html(kc_file: str, output_file: str = None) -> str:
    """将.kc文件转换为完整的HTML页面"""
    from eval_parser import EvalParser
    from kc_html import generate_perpixel_html, generate_graph1d_html, generate_general_html

    with open(kc_file, 'r', encoding='utf-8', errors='replace') as f:
        code = f.read()

    # 预处理: C预处理器指令、字符常量
    code = preprocess_kc(code)

    mode = detect_kc_mode(code)
    struct_defs = parse_struct_defs(code)
    code_clean = strip_struct_defs(code)
    code_clean = strip_typed_statics(code_clean, struct_defs)
    code_clean = strip_typed_locals(code_clean, struct_defs)
    code_clean = strip_typed_func_params(code_clean, struct_defs)
    code_clean = strip_auto_keyword(code_clean)
    code_clean = strip_multidim_arrays(code_clean)

    ast = EvalParser.parse(code_clean)
    converter = KCToJS(mode=mode, struct_defs=struct_defs)
    js_code = converter.convert(ast)
    struct_js = _generate_struct_js(struct_defs)

    if mode == 'perpixel':
        html = generate_perpixel_html(kc_file, js_code, struct_js)
    elif mode == 'graph1d':
        html = generate_graph1d_html(kc_file, js_code, struct_js)
    else:
        html = generate_general_html(kc_file, js_code, struct_js)

    if output_file:
        with open(output_file, 'w') as f:
            f.write(html)

    return html
