#!/usr/bin/env python3
"""verify.py — regenerate golden reference renders and check for regressions.

Renders a small set of example scripts to a fixed frame at a fixed resolution
and compares against pyref/golden/*.png (pixel-identical, since the software
renderer is deterministic). Use --update to regenerate the goldens.

Usage:
  python verify.py                # render + diff against golden
  python verify.py --update       # overwrite golden with current output
"""
from __future__ import annotations
import argparse, os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN = os.path.join(HERE, "golden")
ROOT = os.path.join(HERE, "..")

# (script rel-path, frame, w, h, golden filename, fovy)
CASES = [
    ("ken/balls.pss",     5, 320, 240, "balls_f5.png",     73.74),
    ("ken/ceilflor2.pss", 2, 320, 240, "ceilflor2_f2.png", 73.74),
]

def render_case(lib_api, case):
    import render as R  # noqa
    script, frame, w, h, _, fovy = case
    path = os.path.join(ROOT, script)
    with open(path) as f:
        src = f.read()
    host, vert, frag = R.split_sections(src)
    err = __import__("ctypes").create_string_buffer(256)
    ctx = R.lib.pdrl_compile(host.encode(), w, h, err, 256)
    if not ctx:
        return None, f"compile error: {err.value.decode(errors='replace')}"
    R.lib.pdrl_set_clock_scale(ctx, 1.0/60.0)  # deterministic
    cmds = None
    for fr in range(0, frame + 1):
        R.lib.pdrl_run_frame(ctx, float(fr))
        buf = R.lib.pdrl_glbuf(ctx).contents
        cmds = [(buf.cmds[i].op, buf.cmds[i].mode,
                 buf.cmds[i].a, buf.cmds[i].b, buf.cmds[i].c, buf.cmds[i].d)
                for i in range(buf.n)]
    from software_renderer import render_frame, Shader
    out = render_frame(cmds, w, h, fovy=fovy, shader=Shader(frag or ""))
    R.lib.pdrl_free(ctx)
    return np.flipud(out[:, :, :3]), None  # top-left origin RGB

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true", help="regenerate goldens")
    args = ap.parse_args()

    sys.path.insert(0, HERE)
    import render as R
    R.lib = R.load_lib()

    os.makedirs(GOLDEN, exist_ok=True)
    allok = True
    for case in CASES:
        script, frame, w, h, gname, _ = case
        arr, err = render_case(R, case)
        if err:
            print(f"FAIL {script}: {err}")
            allok = False; continue
        gpath = os.path.join(GOLDEN, gname)
        if args.update or not os.path.exists(gpath):
            Image.fromarray(arr.astype(np.uint8), "RGB").save(gpath)
            print(f"{'wrote' if args.update else 'created'} golden {gname}")
            continue
        golden = np.asarray(Image.open(gpath).convert("RGB"))
        if golden.shape != arr.shape:
            print(f"FAIL {script}: shape {arr.shape} != golden {golden.shape}")
            allok = False; continue
        diff = np.abs(arr.astype(np.int16) - golden.astype(np.int16))
        maxd = int(diff.max())
        npix_diff = int(np.count_nonzero(diff.any(axis=2)))
        if maxd == 0:
            print(f"PASS {script} (identical)")
        else:
            print(f"FAIL {script}: {npix_diff}/{arr.shape[0]*arr.shape[1]} pixels differ, max delta {maxd}")
            allok = False
    sys.exit(0 if allok else 1)

if __name__ == "__main__":
    main()
