#!/usr/bin/env python3
"""Probe the C runlib via ctypes: load balls.pss, run a frame, dump the
recorded gl command stream. This validates the C→Python contract before
building the renderer."""
import ctypes, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, "..", "c_impl", "build", "libpolydraw_eval.dylib")

lib = ctypes.CDLL(LIB)

# struct GLCmd { int op; int mode; double a,b,c,d; } — 48 bytes
class GLCmd(ctypes.Structure):
    _fields_ = [
        ("op", ctypes.c_int),
        ("mode", ctypes.c_int),
        ("a", ctypes.c_double),
        ("b", ctypes.c_double),
        ("c", ctypes.c_double),
        ("d", ctypes.c_double),
    ]

class GLCmdBuf(ctypes.Structure):
    _fields_ = [
        ("cmds", ctypes.POINTER(GLCmd)),
        ("n", ctypes.c_size_t),
        ("cap", ctypes.c_size_t),
    ]

lib.pdrl_compile.restype = ctypes.c_void_p
lib.pdrl_compile.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
                             ctypes.c_char_p, ctypes.c_size_t]
lib.pdrl_run_frame.restype = ctypes.c_double
lib.pdrl_run_frame.argtypes = [ctypes.c_void_p, ctypes.c_double]
lib.pdrl_glbuf.restype = ctypes.POINTER(GLCmdBuf)
lib.pdrl_glbuf.argtypes = [ctypes.c_void_p]
lib.pdrl_free.argtypes = [ctypes.c_void_p]

def split_host(src):
    """Naive host-block extraction: everything before first @v/@g/@f/@h line."""
    lines = src.split("\n")
    out = []
    for ln in lines:
        if ln.lstrip().startswith("@"):
            break
        out.append(ln)
    return "\n".join(out)

def main():
    pss = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "ken", "balls.pss")
    pss = os.path.abspath(pss)
    with open(pss) as f:
        src = f.read()
    host = split_host(src)
    print(f"host block: {len(host)} chars, {host.count(chr(10))+1} lines")

    err = ctypes.create_string_buffer(256)
    ctx = lib.pdrl_compile(host.encode(), 640, 480, err, 256)
    if not ctx:
        print("compile error:", err.value.decode(errors="replace"))
        sys.exit(1)
    print("compiled OK")

    lib.pdrl_run_frame(ctx, 5.0)
    buf = lib.pdrl_glbuf(ctx).contents
    cmds = [(buf.cmds[i].op, buf.cmds[i].mode,
             buf.cmds[i].a, buf.cmds[i].b, buf.cmds[i].c, buf.cmds[i].d)
            for i in range(buf.n)]
    print(f"recorded {len(cmds)} commands")
    # summarize
    from collections import Counter
    c = Counter(cmd[0] for cmd in cmds)
    names = {0:"CLEAR",1:"BEGIN",2:"END",3:"VERTEX",4:"COLOR",5:"TEXCOORD",6:"NORMAL"}
    for op, cnt in sorted(c.items()):
        print(f"  {names.get(op, op)}: {cnt}")
    # show first few
    print("first 8:", cmds[:8])
    lib.pdrl_free(ctx)

if __name__ == "__main__":
    main()
