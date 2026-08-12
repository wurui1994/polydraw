#!/usr/bin/env python3
"""render.py — end-to-end: .pss → C EVAL (ctypes) → GLCmd stream → software raster → PNG.

Usage:
  python render.py <script.pss> [--frame N] [--w W] [--h H] [--out out.png]

This is the Python reference renderer. It loads the C EVAL shared library
(libpolydraw_eval), runs the host block for the requested frame, replays the
recorded gl* commands through a numpy software rasterizer, and writes a PNG.
"""
from __future__ import annotations
import argparse, ctypes, os, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from software_renderer import render_frame, Shader  # noqa: E402


# ---- ctypes binding to libpolydraw_eval ----
class GLCmd(ctypes.Structure):
    _fields_ = [
        ("op", ctypes.c_int), ("mode", ctypes.c_int),
        ("a", ctypes.c_double), ("b", ctypes.c_double),
        ("c", ctypes.c_double), ("d", ctypes.c_double),
        ("s", ctypes.c_char_p),
    ]

class GLCmdBuf(ctypes.Structure):
    _fields_ = [("cmds", ctypes.POINTER(GLCmd)),
                ("n", ctypes.c_size_t), ("cap", ctypes.c_size_t)]

def load_lib():
    cand = [
        os.path.join(HERE, "..", "c_impl", "build", "libpolydraw_eval.dylib"),
        os.path.join(HERE, "..", "c_impl", "build", "libpolydraw_eval.so"),
    ]
    for p in cand:
        if os.path.exists(p):
            lib = ctypes.CDLL(os.path.abspath(p))
            _bind(lib)
            return lib
    raise SystemExit("libpolydraw_eval not built. Run `make` in c_impl/.")

def _bind(lib):
    lib.pdrl_compile.restype = ctypes.c_void_p
    lib.pdrl_compile.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
                                 ctypes.c_char_p, ctypes.c_size_t]
    lib.pdrl_run_frame.restype = ctypes.c_double
    lib.pdrl_run_frame.argtypes = [ctypes.c_void_p, ctypes.c_double]
    lib.pdrl_glbuf.restype = ctypes.POINTER(GLCmdBuf)
    lib.pdrl_glbuf.argtypes = [ctypes.c_void_p]
    lib.pdrl_set_resolution.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    lib.pdrl_set_clock_scale.restype = None
    lib.pdrl_set_clock_scale.argtypes = [ctypes.c_void_p, ctypes.c_double]
    lib.pdrl_free.argtypes = [ctypes.c_void_p]


# ---- .pss section split (host + shader blocks) ----
def split_sections(src: str):
    """Return (host_src, vert_src, frag_src). Naive: @v/@f lines start blocks."""
    lines = src.split("\n")
    blocks = {"host": [], "@v": None, "@f": None}
    cur = "host"
    buf: list[str] = []
    def flush():
        nonlocal buf
        if cur in ("@v","@f","@g") and buf:
            blocks[cur] = "\n".join(buf)
        elif cur == "host":
            blocks["host"].extend(buf)
        buf = []
    for ln in lines:
        st = ln.lstrip()
        if st.startswith("@"):
            # marker like '@v:' or '@f:' or '@v name'
            head = st.split(":",1)[0].split(None,1)[0].rstrip()
            if head in ("@v","@g","@f","@h"):
                flush()
                cur = head if head != "@h" else "host"
                continue
        buf.append(ln)
    flush()
    return "\n".join(blocks["host"]), blocks["@v"], blocks["@f"]


def save_png(arr_uint8: np.ndarray, path: str):
    """Save (H,W,4) uint8 to PNG via Pillow. Flip Y (fb is bottom-left)."""
    from PIL import Image
    flipped = np.flipud(arr_uint8[:, :, :3])  # GL bottom-left → image top-left
    Image.fromarray(flipped.astype(np.uint8), "RGB").save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("script")
    ap.add_argument("--frame", type=int, default=30)
    ap.add_argument("--w", type=int, default=640)
    ap.add_argument("--h", type=int, default=480)
    ap.add_argument("--out", default=None)
    ap.add_argument("--fovy", type=float, default=73.74)  # setfov(90) effective
    args = ap.parse_args()

    with open(args.script) as f:
        src = f.read()
    host, vert, frag = split_sections(src)
    print(f"host: {len(host)} chars; vert={'yes' if vert else 'no'}; frag={'yes' if frag else 'no'}")

    lib = load_lib()
    err = ctypes.create_string_buffer(256)
    ctx = lib.pdrl_compile(host.encode(), args.w, args.h, err, 256)
    if not ctx:
        print("compile error:", err.value.decode(errors="replace"))
        sys.exit(1)
    print("compiled OK")
    # Deterministic clock: klock() = numframes/60 (so renders are reproducible).
    lib.pdrl_set_clock_scale(ctx, 1.0/60.0)

    # Run frames sequentially from 0: .pss scripts accumulate state in static
    # arrays (init only runs at numframes==0), so we must warm up.
    target = args.frame
    cmds = None
    for f in range(0, target + 1):
        lib.pdrl_run_frame(ctx, float(f))
        buf = lib.pdrl_glbuf(ctx).contents
        cmds = [(buf.cmds[i].op, buf.cmds[i].mode,
                 buf.cmds[i].a, buf.cmds[i].b, buf.cmds[i].c, buf.cmds[i].d)
                for i in range(buf.n)]
    print(f"after {target+1} frames, last frame recorded {len(cmds)} gl commands")

    shader = Shader(frag or "")
    print(f"shader kind: {shader.kind}")
    out = render_frame(cmds, args.w, args.h, fovy=args.fovy, shader=shader)

    outpath = args.out or (os.path.splitext(args.script)[0] + f"_f{args.frame}.png")
    outpath = os.path.abspath(outpath)
    save_png(out, outpath)
    nonblack = int(np.count_nonzero(out[..., :3].sum(axis=2)))
    total = args.w * args.h
    print(f"wrote {outpath}  (non-black: {nonblack}/{total} = {100*nonblack/total:.1f}%)")
    lib.pdrl_free(ctx)

if __name__ == "__main__":
    main()
