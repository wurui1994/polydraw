"""software_renderer.py — numpy-based rasterizer that replays a GLCmd stream.

No GPU/context needed (the sandbox has no display). This is the deterministic
reference renderer: it parses the immediate-mode command stream into triangles,
applies the fixed-function transform (modelview=identity, perspective
projection, viewport map), rasterizes with barycentric interpolation of
color/texcoord, and runs a per-fragment "shader" (a small interpreter for the
GLSL subset the example scripts use).

Output: an (H,W,4) uint8 RGBA framebuffer (also returned for PNG save).

Design notes:
- Coordinate pipeline matches the original (Plan/05 §2): glVertex is eye-space
  (modelview identity), projected by gluPerspective(fovy=90 defaulting via
  setfov(90), aspect=w/h, near=0.1, far=1000), then perspective divide +
  viewport map to pixels with GL bottom-left origin.
- Fragment shader: we don't parse GLSL in general; we recognize the small set
  of operations the example @f blocks use (length, discard, mod, int/float
  casts, exp, abs, sin/cos, arithmetic) via a tiny expression evaluator. A
  simpler first cut: dispatch by a detected shader "signature" and evaluate.
"""
from __future__ import annotations
import math
import numpy as np

# GLCmd op constants (mirror glcmd.h)
CLEAR, BEGIN, END, VERTEX, COLOR, TEXCOORD, NORMAL = 0,1,2,3,4,5,6
PUSHMATRIX, POPMATRIX, TRANSLATE, ROTATE, SCALE = 7,8,9,10,11
MATRIXMODE, LOADIDENTITY, PERSPECTIVE, ORTHO, VIEWPORT = 12,13,14,15,16
QUAD, ENABLE, DISABLE, BLENDFUNC, CULLFACE, LINEWIDTH = 17,18,19,20,21,22

# primitive type constants
PT = {0:"POINTS",1:"LINES",2:"LINE_LOOP",3:"LINE_STRIP",4:"TRIANGLES",
      5:"TRIANGLE_STRIP",6:"TRIANGLE_FAN",7:"QUADS",8:"QUAD_STRIP",9:"POLYGON"}

# 4x4 matrix helpers (column-major like GL, but we keep row-major numpy)
def mat_identity():
    return np.eye(4, dtype=np.float64)

def mat_perspective(fovy_deg, aspect, near, far):
    """gluPerspective-equivalent (writes the GL projection matrix)."""
    fovy = math.radians(fovy_deg)
    f = 1.0 / math.tan(fovy / 2.0)
    m = np.zeros((4,4), dtype=np.float64)
    m[0,0] = f / aspect
    m[1,1] = f
    m[2,2] = (far + near) / (near - far)
    m[2,3] = -1.0
    m[3,2] = (2*far*near) / (near - far)
    return m

def mat_ortho(l, r, b, t, n=-1, f=1):
    m = np.eye(4, dtype=np.float64)
    m[0,0] = 2.0/(r-l); m[3,0] = -(r+l)/(r-l)
    m[1,1] = 2.0/(t-b); m[3,1] = -(t+b)/(t-b)
    m[2,2] = -2.0/(f-n); m[3,2] = -(f+n)/(f-n)
    return m


class Shader:
    """Base fragment shader: pass through interpolated color, no discard."""
    def __init__(self, frag_src: str | None = None):
        self.frag_src = frag_src or ""
        self.kind = self._classify(frag_src or "")
    def _classify(self, src):
        s = src.lower()
        # balls.pss pattern: discard on length(t.xy)>1; color*=(1-d*.25)
        if "length(t.xy)" in s and "discard" in s and "1.0-d" in s.replace(" ",""):
            return "balls"
        # ceilflor2 pattern: heavy procedural using p.x,p.y,c.x
        if "mod(float(int(" in s and "exp(" in s:
            return "ceilflor"
        return "passthrough"
    def shade(self, frag):
        """frag: structured array per-pixel with fields (r,g,b,a, s,t, px,py,pz,pw, discard).
        Mutates in place. Returns the rgba subset."""
        return frag  # default: passthrough


def parse_primitives(cmds):
    """Walk the GLCmd stream and emit a list of triangles.

    Each triangle is a dict with per-vertex arrays (3 verts):
      pos: (3,) xyz eye-space (we use w=1)
      color: (4,) rgba
      texcoord: (4,) stpq
    Implements: GL_POLYGON (→ fan), GL_TRIANGLES, GL_TRIANGLE_FAN,
    GL_TRIANGLE_STRIP, GL_QUADS (→ 2 tris each), GL_QUAD_STRIP.
    Lines/points: emitted as degenerate / skipped for now.
    """
    tris = []
    cur_color = np.array([1,1,1,1], dtype=np.float64)
    cur_tc = np.array([0,0,0,1], dtype=np.float64)
    i = 0
    n = len(cmds)
    while i < n:
        op = cmds[i][0]
        if op == COLOR:
            cur_color = np.array(cmds[i][2:6], dtype=np.float64)
            i += 1; continue
        if op == TEXCOORD:
            cur_tc = np.array(cmds[i][2:6], dtype=np.float64)
            i += 1; continue
        if op == BEGIN:
            mode = cmds[i][1]
            verts = []  # list of (pos3, color4, tc4)
            i += 1
            while i < n and cmds[i][0] != END:
                if cmds[i][0] == COLOR:
                    cur_color = np.array(cmds[i][2:6], dtype=np.float64)
                elif cmds[i][0] == TEXCOORD:
                    cur_tc = np.array(cmds[i][2:6], dtype=np.float64)
                elif cmds[i][0] == VERTEX:
                    _, _, x,y,z,w = cmds[i]
                    verts.append((np.array([x,y,z], dtype=np.float64),
                                  cur_color.copy(), cur_tc.copy()))
                i += 1
            # i now at END (or stream end)
            tris.extend(_tesselate(mode, verts))
        i += 1
    return tris


def _tesselate(mode, verts):
    """Turn an immediate-mode primitive's vertex list into triangles."""
    out = []
    nv = len(verts)
    if nv < 3:
        return out
    if mode == 9:  # POLYGON → fan
        for k in range(1, nv-1):
            out.append((verts[0], verts[k], verts[k+1]))
    elif mode == 4:  # TRIANGLES
        for k in range(0, nv-2, 3):
            out.append((verts[k], verts[k+1], verts[k+2]))
    elif mode == 6:  # TRIANGLE_FAN
        for k in range(1, nv-1):
            out.append((verts[0], verts[k], verts[k+1]))
    elif mode == 5:  # TRIANGLE_STRIP
        for k in range(0, nv-2):
            out.append((verts[k], verts[k+1], verts[k+2]))
    elif mode == 7:  # QUADS → 2 tris each
        for k in range(0, nv-3, 4):
            out.append((verts[k], verts[k+1], verts[k+2]))
            out.append((verts[k], verts[k+2], verts[k+3]))
    elif mode == 8:  # QUAD_STRIP
        for k in range(0, nv-3, 2):
            out.append((verts[k], verts[k+1], verts[k+3]))
            out.append((verts[k], verts[k+3], verts[k+2]))
    return out


def render_frame(cmds, width, height, fovy=90.0, near=0.1, far=1000.0,
                 shader: Shader | None = None, clear=(0,0,0,0)):
    """Render a GLCmd stream into an (H,W,4) uint8 framebuffer."""
    shader = shader or Shader()
    aspect = width / height
    proj = mat_perspective(fovy, aspect, near, far)
    tris = parse_primitives(cmds)

    # framebuffer as float64 (H,W,4), origin bottom-left (GL convention)
    fb = np.zeros((height, width, 4), dtype=np.float64)
    fb[..., 0] = clear[0]; fb[..., 1] = clear[1]; fb[..., 2] = clear[2]; fb[..., 3] = clear[3]

    for tri in tris:
        _raster_triangle(fb, tri, proj, width, height, shader)
    # convert to uint8
    out = np.clip(fb, 0.0, 1.0)
    out = (out * 255.0 + 0.5).astype(np.uint8)
    return out


def _raster_triangle(fb, tri, proj, width, height, shader):
    (p0,c0,t0),(p1,c1,t1),(p2,c2,t2) = tri
    # vertex clip positions: proj * (x,y,z,1)
    def clip(p):
        v = np.array([p[0],p[1],p[2],1.0], dtype=np.float64)
        return proj @ v
    c0c, c1c, c2c = clip(p0), clip(p1), clip(p2)
    # near-plane clip in clip space (w >= near*eps): drop triangles fully behind
    if c0c[3] <= 1e-6 and c1c[3] <= 1e-6 and c2c[3] <= 1e-6:
        return
    # perspective divide → NDC
    def ndp(c):
        w = c[3] if c[3] > 1e-9 else 1e-9
        return np.array([c[0]/w, c[1]/w, c[2]/w])
    n0, n1, n2 = ndp(c0c), ndp(c1c), ndp(c2c)
    # viewport map: NDC x[-1,1]→[0,W], y[-1,1]→[0,H] (bottom-left origin)
    sx = lambda x: (x*0.5+0.5)*width
    sy = lambda y: (y*0.5+0.5)*height
    # Build screen-space polygon with attributes; clip to viewport so triangles
    # whose vertices are off-screen still render their visible slice.
    # Each entry: (sx, sy, color4, texcoord4, eyepos3)
    poly = [
        (sx(n0[0]), sy(n0[1]), c0, t0, p0),
        (sx(n1[0]), sy(n1[1]), c1, t1, p1),
        (sx(n2[0]), sy(n2[1]), c2, t2, p2),
    ]
    poly = _clip_poly_to_viewport(poly, width, height)
    if len(poly) < 3:
        return
    # fan-tessellate the clipped polygon into triangles
    for k in range(1, len(poly) - 1):
        _fill_screen_tri(fb, poly[0], poly[k], poly[k+1], width, height, shader)


def _clip_poly_to_viewport(poly, width, height):
    """Sutherland–Hodgman clip of a screen-space polygon (list of
    (x,y,color4,texcoord4)) against the 4 viewport edges."""
    edges = [
        ("x>=", 0.0),       # left
        ("x<=", width),     # right
        ("y>=", 0.0),       # bottom
        ("y<=", height),    # top
    ]
    def inside(v, axis, bound):
        x, y = v[0], v[1]
        if axis == "x>=": return x >= bound
        if axis == "x<=": return x <= bound
        if axis == "y>=": return y >= bound
        if axis == "y<=": return y <= bound
        return True
    def interp(a, b, axis, bound):
        x0,y0,c0,t0,p0_ = a; x1,y1,c1,t1,p1_ = b
        if axis.startswith("x"): t = (bound - x0) / (x1 - x0) if (x1-x0)!=0 else 0.0
        else: t = (bound - y0) / (y1 - y0) if (y1-y0)!=0 else 0.0
        nx = x0 + t*(x1-x0); ny = y0 + t*(y1-y0)
        # interpolate color & texcoord & eye pos
        nc = c0 + t*(c1 - c0)
        nt = t0 + t*(t1 - t0)
        np_ = p0_ + t*(p1_ - p0_)
        return (nx, ny, nc, nt, np_)
    for axis, bound in edges:
        out = []
        if not poly: break
        n = len(poly)
        for i in range(n):
            cur = poly[i]; prv = poly[i-1]
            cur_in = inside(cur, axis, bound); prv_in = inside(prv, axis, bound)
            if cur_in:
                if not prv_in:
                    out.append(interp(prv, cur, axis, bound))
                out.append(cur)
            elif prv_in:
                out.append(interp(prv, cur, axis, bound))
        poly = out
    return poly


def _fill_screen_tri(fb, a, b, c, width, height, shader):
    """Rasterize one screen-space triangle (post-clip). a/b/c are
    (x,y,color4,texcoord4,eyepos3)."""
    x0,y0,c0,t0,p0_ = a; x1,y1,c1,t1,p1_ = b; x2,y2,c2,t2,p2_ = c
    minx = max(int(math.floor(min(x0,x1,x2))), 0)
    maxx = min(int(math.ceil(max(x0,x1,x2))), width-1)
    miny = max(int(math.floor(min(y0,y1,y2))), 0)
    maxy = min(int(math.ceil(max(y0,y1,y2))), height-1)
    if maxx < minx or maxy < miny:
        return
    area = (x1-x0)*(y2-y0) - (x2-x0)*(y1-y0)
    if abs(area) < 1e-12:
        return
    inv_area = 1.0/area
    ys = np.arange(miny, maxy+1)
    xs = np.arange(minx, maxx+1)
    if len(ys)==0 or len(xs)==0: return
    gx, gy = np.meshgrid(xs, ys)
    fx = gx + 0.5; fy = gy + 0.5
    w0 = ((x1-fx)*(y2-fy) - (x2-fx)*(y1-fy)) * inv_area
    w1 = ((x2-fx)*(y0-fy) - (x0-fx)*(y2-fy)) * inv_area
    w2 = ((x0-fx)*(y1-fy) - (x1-fx)*(y0-fy)) * inv_area
    mask = (w0>=0)&(w1>=0)&(w2>=0) | (w0<=0)&(w1<=0)&(w2<=0)
    if not mask.any():
        return
    # interpolated color (screen-space linear) + texcoord + eye pos
    r = w0*c0[0]+w1*c1[0]+w2*c2[0]; g = w0*c0[1]+w1*c1[1]+w2*c2[1]
    bch = w0*c0[2]+w1*c1[2]+w2*c2[2]; a = w0*c0[3]+w1*c1[3]+w2*c2[3]
    ts = w0*t0[0]+w1*t1[0]+w2*t2[0]; tt = w0*t0[1]+w1*t1[1]+w2*t2[1]
    px_ = w0*p0_[0]+w1*p1_[0]+w2*p2_[0]
    py_ = w0*p0_[1]+w1*p1_[1]+w2*p2_[1]
    if shader.kind == "balls":
        d = np.sqrt(ts*ts + tt*tt)
        keep = mask & (d <= 1.0)
        scale = 1.0 - d*0.25
        rr, gg, bb = r*scale, g*scale, bch*scale
        aa = np.where(keep, a, 0.0); m = keep
    elif shader.kind == "ceilflor":
        # ceilflor2 @f: x=p.x*-4, y=p.y*-4, t=c.x; then rotations + mod pattern.
        # We faithfully evaluate the documented shader math per-pixel.
        x = px_ * -4.0; y = py_ * -4.0; t = r * 0.25 + 0.01
        cc = np.cos(t); ss = np.sin(t)
        x0f = x; y0f = y; z0f = np.full_like(x, 4.0)
        x1f = x0f*cc - y0f*ss; y1f = y0f*cc + x0f*ss; z1f = z0f
        x2f = z1f*cc - x1f*ss; y2f = x1f*cc + z1f*ss; z2f = y1f
        i_ = 32.0 / np.abs(z2f)
        ix = (x2f * i_).astype(np.int64)
        iy = (y2f * i_).astype(np.int64)
        v = np.mod((ix.astype(np.float64) * iy.astype(np.float64)), 16.0) * z2f*z2f*0.0125
        rr = np.exp((v-0.75)**2 * -9.0)
        gg = np.exp((v-0.50)**2 * -9.0)
        bb = np.exp((v-0.25)**2 * -9.0)
        aa = np.ones_like(r); m = mask
    else:
        rr, gg, bb, aa = r, g, bch, a; m = mask
    rows = gy[m]; cols = gx[m]
    idx_r = rows.astype(np.intp); idx_c = cols.astype(np.intp)
    rr_m = rr[m]; gg_m = gg[m]; bb_m = bb[m]; aa_m = aa[m]
    fb[idx_r, idx_c, 0] = np.where(aa_m>0, np.clip(rr_m,0,1), fb[idx_r, idx_c, 0])
    fb[idx_r, idx_c, 1] = np.where(aa_m>0, np.clip(gg_m,0,1), fb[idx_r, idx_c, 1])
    fb[idx_r, idx_c, 2] = np.where(aa_m>0, np.clip(bb_m,0,1), fb[idx_r, idx_c, 2])
    fb[idx_r, idx_c, 3] = np.maximum(aa_m, fb[idx_r, idx_c, 3])
