"""KC HTML生成器 - 为不同渲染模式生成HTML页面"""
import os


def _js_helpers():
    """通用JS辅助函数"""
    return r"""
function _atan(x, ...a) { return a.length ? Math.atan2(x, a[0]) : Math.atan(x); }
function _fact(x) { let r = 1; for (let i = 2; i <= x; i++) r *= i; return r; }
function _log(x, ...a) { return a.length ? Math.log(x) / Math.log(a[0]) : Math.log(x); }
function _sgn(x) { return x < 0 ? -1 : x > 0 ? 1 : 0; }
function _unit(x) { return x < 0 ? 0 : x > 0 ? 1 : 0.5; }
function _fmod(x, y) { return x % y; }
function _noise(...a) {
  const x = a[0] || 0, y = a[1] || 0, z = a[2] || 0;
  let n = (x * 57 + y * 131 + z * 211) & 0x7fffffff;
  n = (n >> 13) ^ n;
  n = (n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff;
  return n / 1073741824.0 - 1.0;
}
function _rgb(r, g, b) { return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF); }
function _printf(fmt, ...args) {
  if (typeof fmt === 'string' && fmt.startsWith('"') && fmt.endsWith('"'))
    fmt = fmt.slice(1, -1);
  fmt = String(fmt).replace(/\\n/g, '\n').replace(/\\t/g, '\t');
  let argIdx = 0, out = '';
  for (let i = 0; i < fmt.length; i++) {
    if (fmt[i] === '%' && i + 1 < fmt.length) {
      const spec = fmt[i + 1];
      if (spec === 'd' || spec === 'i') { out += Math.trunc(args[argIdx++] || 0); i++; }
      else if ('feEgG'.includes(spec)) { out += (args[argIdx++] || 0); i++; }
      else if (spec === 's') { out += (args[argIdx++] || ''); i++; }
      else if (spec === '%') { out += '%'; i++; }
      else out += fmt[i];
    } else out += fmt[i];
  }
  if (typeof console !== 'undefined') console.log(out); return 0;
}
function _klock(...a) {
  if (!a.length || a[0] === 0) return performance.now() / 1000 - _startTime;
  const d = new Date();
  const opt = Math.trunc(a[0]);
  if (opt === 2) return d.getFullYear();
  if (opt === 3) return d.getMonth() + 1;
  if (opt === 5) return d.getDate();
  if (opt === 6) return d.getHours();
  if (opt === 7) return d.getMinutes();
  if (opt === 8) return d.getSeconds();
  return 0;
}
let _glklockStart = 0;
function _glklockstart() { _glklockStart = performance.now() / 1000; return 0; }
function _glklockelapsed() { return performance.now() / 1000 - _glklockStart; }
function _srand(x) { return 0; }
"""


def _js_mouse():
    return r"""
canvas.addEventListener('mousemove', e => {
  const r = canvas.getBoundingClientRect();
  SYS.mousx = (e.clientX - r.left) * W / r.width;
  SYS.mousy = (e.clientY - r.top) * H / r.height;
});
canvas.addEventListener('mousedown', e => { SYS.bstatus = e.buttons; });
canvas.addEventListener('mouseup', e => { SYS.bstatus = e.buttons; });
"""


def _js_keyboard():
    return r"""
const KC_KEYS = new Uint8Array(256);
document.addEventListener('keydown', e => {
  if (e.keyCode < 256) KC_KEYS[e.keyCode] = 1;
  if ([37,38,39,40,32,16,17,18].includes(e.keyCode)) e.preventDefault();
});
document.addEventListener('keyup', e => {
  if (e.keyCode < 256) KC_KEYS[e.keyCode] = 0;
});
"""


def _kc_runtime_minimal():
    """perpixel/graph1d模式的最小运行时"""
    return r"""
const KC = {
  _frameinit: true,
  frameinit() { const v = this._frameinit; return v ? 1 : 0; },
  fadd(x, y) { return Math.fround(x + y); },
  rgba(r, g, b, a) { return ((a & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF); },
  getrgb(c, rgb) {
    if (Array.isArray(rgb)) { rgb[0] = (c >> 16) & 0xFF; rgb[1] = (c >> 8) & 0xFF; rgb[2] = c & 0xFF; }
    return 0;
  },
  normrand() { return (Math.random()+Math.random()+Math.random()+Math.random()+Math.random()+Math.random()+Math.random()+Math.random()+Math.random()+Math.random()+Math.random()+Math.random()-6); },
  playtext() { return 0; }, playnote() { return 0; }, sleep() { return 0; },
  pic() { return 0; }, fil() { return 0; }, mountzip() { return 0; }, getpicsiz() { return 0; },
  bufset() { return 0; }, bufcpy() { return 0; }, sethlin() { return 0; }, gethlin() { return 0; },
  keystatus: new Uint8Array(256),
};
"""


def generate_perpixel_html(kc_file: str, js_code: str, struct_js: str) -> str:
    """生成逐像素模式的HTML"""
    basename = os.path.basename(kc_file)
    return f"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>EvalDraw - {basename}</title>
<style>
  body {{ margin: 0; overflow: hidden; background: #000; display: flex; justify-content: center; align-items: center; height: 100vh; }}
  canvas {{ image-rendering: pixelated; }}
  #info {{ position: absolute; top: 8px; left: 8px; color: #0f0; font: 12px monospace; }}
</style>
</head>
<body>
<canvas id="c"></canvas>
<div id="info"></div>
<script>
const W = 640, H = 480;
const canvas = document.getElementById('c');
canvas.width = W; canvas.height = H;
const ctx = canvas.getContext('2d');
const imgData = ctx.createImageData(W, H);
const pixels = imgData.data;

const SYS = {{ xres: W, yres: H, mousx: 0, mousy: 0, bstatus: 0, numframes: 0 }};
const _startTime = performance.now() / 1000;

{_js_mouse()}
{_js_keyboard()}
{_kc_runtime_minimal()}
{_js_helpers()}

// ===== Struct definitions =====
{struct_js}

// ===== Host Code =====
{js_code}

// ===== Animation Loop =====
const _pixelColor = [0, 0, 0]; // reusable color array for &r,&g,&b
function animate() {{
  const t = performance.now() / 1000 - _startTime;

  for (let y = 0; y < H; y++) {{
    for (let x = 0; x < W; x++) {{
      const nx = (x / W - 0.5) * 2;
      const ny = (y / H - 0.5) * 2;
      _pixelColor[0] = 0; _pixelColor[1] = 0; _pixelColor[2] = 0;
      hostPixel(nx, ny, t, _pixelColor);
      const idx = (y * W + x) * 4;
      pixels[idx]   = Math.min(255, Math.max(0, Math.trunc(_pixelColor[0])));
      pixels[idx+1] = Math.min(255, Math.max(0, Math.trunc(_pixelColor[1])));
      pixels[idx+2] = Math.min(255, Math.max(0, Math.trunc(_pixelColor[2])));
      pixels[idx+3] = 255;
    }}
  }}

  ctx.putImageData(imgData, 0, 0);
  KC._frameinit = false;
  SYS.numframes++;
  requestAnimationFrame(animate);
}}

animate();
</script>
</body>
</html>"""


def generate_graph1d_html(kc_file: str, js_code: str, struct_js: str) -> str:
    """生成1D绘图模式的HTML"""
    basename = os.path.basename(kc_file)
    return f"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>EvalDraw - {basename}</title>
<style>
  body {{ margin: 0; overflow: hidden; background: #000; display: flex; justify-content: center; align-items: center; height: 100vh; }}
  canvas {{ image-rendering: pixelated; }}
  #info {{ position: absolute; top: 8px; left: 8px; color: #0f0; font: 12px monospace; }}
</style>
</head>
<body>
<canvas id="c"></canvas>
<div id="info"></div>
<script>
const W = 640, H = 480;
const canvas = document.getElementById('c');
canvas.width = W; canvas.height = H;
const ctx = canvas.getContext('2d');

const SYS = {{ xres: W, yres: H, mousx: 0, mousy: 0, bstatus: 0, numframes: 0 }};
const _startTime = performance.now() / 1000;

{_js_mouse()}
{_js_keyboard()}
{_kc_runtime_minimal()}
{_js_helpers()}

// ===== Struct definitions =====
{struct_js}

// ===== Host Code =====
{js_code}

// ===== Animation Loop =====
function animate() {{
  const t = performance.now() / 1000 - _startTime;

  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, W, H);

  ctx.strokeStyle = '#0f0';
  ctx.lineWidth = 2;
  ctx.beginPath();
  for (let px = 0; px < W; px++) {{
    const x = (px / W - 0.5) * 2;
    const y = hostGraph(x, 0, t);
    const sy = H / 2 - y * H / 4;
    if (px === 0) ctx.moveTo(px, sy);
    else ctx.lineTo(px, sy);
  }}
  ctx.stroke();

  KC._frameinit = false;
  SYS.numframes++;
  requestAnimationFrame(animate);
}}

animate();
</script>
</body>
</html>"""


def generate_general_html(kc_file: str, js_code: str, struct_js: str) -> str:
    """生成通用模式的HTML (Canvas2D像素缓冲区 + 3D投影)"""
    basename = os.path.basename(kc_file)
    return f"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>EvalDraw - {basename}</title>
<style>
  body {{ margin: 0; overflow: hidden; background: #000; }}
  canvas {{ display: block; }}
  #info {{ position: absolute; top: 8px; left: 8px; color: #0f0; font: 12px monospace; }}
</style>
</head>
<body>
<canvas id="c"></canvas>
<div id="info"></div>
<script>
const W = 640, H = 480;
const canvas = document.getElementById('c');
canvas.width = W; canvas.height = H;
const ctx = canvas.getContext('2d');
const imgData = ctx.createImageData(W, H);
const pixels = imgData.data;

const SYS = {{ xres: W, yres: H, mousx: 0, mousy: 0, bstatus: 0, numframes: 0 }};
const _startTime = performance.now() / 1000;

{_js_mouse()}
{_js_keyboard()}

// ===== 2D Pixel Buffer =====
const _pixBuf = new Int32Array(W * H);
const _zBuf = new Float64Array(W * H);
let _curColor = 0xFFFFFFFF;
let _curFontW = 8, _curFontH = 16, _curFontMono = 0;
let _curX = 0, _curY = 0;
let _needRefresh = false;
let _gfov = 90;

// ===== 3D Camera State =====
let _camPos = [0,0,0], _camR = [1,0,0], _camD = [0,1,0], _camF = [0,0,1];
let _camYaw = 0, _camPitch = 0;

// ===== KC Runtime (general) =====
const KC = {{
  keystatus: KC_KEYS,
  _frameinit: true,

  // ---- 2D Drawing ----
  setpix(x, y) {{
    const ix = Math.trunc(x), iy = Math.trunc(y);
    if (ix >= 0 && ix < W && iy >= 0 && iy < H) _pixBuf[iy * W + ix] = _curColor;
    return 0;
  }},

  getpix(x, y, r, g, b) {{
    const ix = Math.trunc(x), iy = Math.trunc(y);
    if (ix >= 0 && ix < W && iy >= 0 && iy < H) {{
      const c = _pixBuf[iy * W + ix];
      if (Array.isArray(r)) r[0] = (c >> 16) & 0xFF;
      if (Array.isArray(g)) g[0] = (c >> 8) & 0xFF;
      if (Array.isArray(b)) b[0] = c & 0xFF;
    }}
    return 0;
  }},

  setcol(...a) {{
    if (a.length === 1) {{ _curColor = a[0] & 0xFFFFFFFF; }}
    else if (a.length >= 3) {{
      const r = Math.trunc(a[0]) & 0xFF, g = Math.trunc(a[1]) & 0xFF, b = Math.trunc(a[2]) & 0xFF;
      const al = a.length >= 4 ? (Math.trunc(a[3]) & 0xFF) : 0xFF;
      _curColor = (al << 24) | (r << 16) | (g << 8) | b;
    }}
    return 0;
  }},

  cls(color) {{ _pixBuf.fill((color !== undefined ? color : 0) & 0xFFFFFFFF); return 0; }},
  clz(z) {{ _zBuf.fill(z || 1e32); return 0; }},
  refresh() {{
    _needRefresh = true;
    blitPixels();
    return 0;
  }},

  setfont(w, h, mono) {{ _curFontW = w || 8; _curFontH = h || 16; _curFontMono = mono || 0; return 0; }},

  printf(fmt, ...args) {{
    if (typeof fmt === 'string' && fmt.startsWith('"') && fmt.endsWith('"')) fmt = fmt.slice(1, -1);
    fmt = String(fmt).replace(/\\\\n/g, '\\n').replace(/\\\\t/g, '\\t');
    let argIdx = 0, out = '';
    for (let i = 0; i < fmt.length; i++) {{
      if (fmt[i] === '%' && i + 1 < fmt.length) {{
        const spec = fmt[i + 1];
        if (spec === 'd' || spec === 'i') {{ out += Math.trunc(args[argIdx++] || 0); i++; }}
        else if ('feEgG'.includes(spec)) {{ out += (args[argIdx++] || 0); i++; }}
        else if (spec === 's') {{ out += (args[argIdx++] || ''); i++; }}
        else if (spec === '%') {{ out += '%'; i++; }}
        else out += fmt[i];
      }} else out += fmt[i];
    }}
    this._drawText(out, _curX, _curY);
    _curX += out.length * _curFontW;
    console.log(out); return 0;
  }},

  printchar(ch) {{ this._drawText(String.fromCharCode(ch), _curX, _curY); _curX += _curFontW; return 0; }},
  moveto(x, y) {{ _curX = x; _curY = y; return 0; }},

  _drawText(text, x, y) {{
    const fw = _curFontW, fh = _curFontH;
    for (let i = 0; i < text.length; i++) {{
      const cx = Math.trunc(x) + i * fw, cy = Math.trunc(y);
      for (let py = 0; py < fh && cy + py < H; py++)
        for (let px = 0; px < fw && cx + px < W; px++)
          if (cx + px >= 0 && cy + py >= 0) _pixBuf[(cy + py) * W + (cx + px)] = _curColor;
    }}
  }},

  drawline(x0, y0, x1, y1) {{
    x0 = Math.trunc(x0); y0 = Math.trunc(y0); x1 = Math.trunc(x1); y1 = Math.trunc(y1);
    let dx = Math.abs(x1-x0), dy = Math.abs(y1-y0);
    let sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
    while (true) {{
      if (x0 >= 0 && x0 < W && y0 >= 0 && y0 < H) _pixBuf[y0 * W + x0] = _curColor;
      if (x0 === x1 && y0 === y1) break;
      let e2 = 2 * err;
      if (e2 > -dy) {{ err -= dy; x0 += sx; }}
      if (e2 < dx) {{ err += dx; y0 += sy; }}
    }}
    return 0;
  }},

  drawrect(x, y, w, h) {{
    this.drawline(x,y,x+w,y); this.drawline(x+w,y,x+w,y+h);
    this.drawline(x+w,y+h,x,y+h); this.drawline(x,y+h,x,y);
    return 0;
  }},

  // ---- 3D Drawing ----
  setcam(...a) {{
    if (a.length >= 5) {{
      _camPos = [a[0]||0, a[1]||0, a[2]||0];
      if (a.length >= 12) {{
        _camR = [a[3]||1, a[4]||0, a[5]||0];
        _camD = [a[6]||0, a[7]||1, a[8]||0];
        _camF = [a[9]||0, a[10]||0, a[11]||1];
      }} else {{
        _camYaw = a[3] || 0; _camPitch = a[4] || 0;
        const cy = Math.cos(_camYaw), sy = Math.sin(_camYaw);
        const cp = Math.cos(_camPitch), sp = Math.sin(_camPitch);
        _camF = [cp*cy, sp, cp*sy]; _camR = [cy, 0, sy]; _camD = [-sp*cy, cp, -sp*sy];
      }}
    }}
    return 0;
  }},

  _project(x, y, z) {{
    const dx = x-_camPos[0], dy = y-_camPos[1], dz = z-_camPos[2];
    const fx = dx*_camR[0]+dy*_camR[1]+dz*_camR[2];
    const fy = dx*_camD[0]+dy*_camD[1]+dz*_camD[2];
    const fz = dx*_camF[0]+dy*_camF[1]+dz*_camF[2];
    if (fz <= 0.01) return null;
    const fovScale = 1.0 / Math.tan(_gfov * Math.PI / 360);
    const aspect = W / H;
    return [(fx * fovScale / fz / aspect + 0.5) * W, (0.5 - fy * fovScale / fz) * H, fz];
  }},

  drawsph(x, y, r, ...a) {{
    if (a.length > 0) {{
      const z = r, rad = a[0] || 1;
      const p = this._project(x, y, z);
      if (!p) return 0;
      const screenR = rad * (1.0 / Math.tan(_gfov * Math.PI / 360)) / p[2] * W * 0.5;
      this._fillCircle(p[0], p[1], screenR, _curColor);
    }} else {{
      this._fillCircle(x, y, r, _curColor);
    }}
    return 0;
  }},

  _fillCircle(cx, cy, r, color) {{
    const ix = Math.trunc(cx), iy = Math.trunc(cy), ir = Math.max(1, Math.trunc(r));
    for (let dy = -ir; dy <= ir; dy++)
      for (let dx = -ir; dx <= ir; dx++)
        if (dx*dx + dy*dy <= ir*ir) {{
          const px = ix+dx, py = iy+dy;
          if (px >= 0 && px < W && py >= 0 && py < H) _pixBuf[py * W + px] = color;
        }}
  }},

  drawcone(x0,y0,z0,r0, x1,y1,z1,r1, ...flags) {{
    const p0 = this._project(x0, y0, z0), p1 = this._project(x1, y1, z1);
    if (!p0 || !p1) return 0;
    const fovScale = 1.0 / Math.tan(_gfov * Math.PI / 360);
    const sr0 = r0 * fovScale / p0[2] * W * 0.5;
    const sr1 = r1 * fovScale / p1[2] * W * 0.5;
    this._drawLine2D(p0[0], p0[1], p1[0], p1[1], _curColor);
    if (sr0 > 1) this._fillCircle(p0[0], p0[1], sr0, _curColor);
    if (sr1 > 1) this._fillCircle(p1[0], p1[1], sr1, _curColor);
    return 0;
  }},

  _drawLine2D(x0, y0, x1, y1, color) {{
    x0 = Math.trunc(x0); y0 = Math.trunc(y0); x1 = Math.trunc(x1); y1 = Math.trunc(y1);
    let dx = Math.abs(x1-x0), dy = Math.abs(y1-y0);
    let sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
    while (true) {{
      if (x0 >= 0 && x0 < W && y0 >= 0 && y0 < H) _pixBuf[y0 * W + x0] = color;
      if (x0 === x1 && y0 === y1) break;
      let e2 = 2 * err;
      if (e2 > -dy) {{ err -= dy; x0 += sx; }}
      if (e2 < dx) {{ err += dx; y0 += sy; }}
    }}
  }},

  drawspr(x, y, size, angle) {{ this._fillCircle(x, y, size/2, _curColor); return 0; }},
  drawkv6() {{ return 0; }},
  drawline3d(x0,y0,z0, x1,y1,z1) {{
    const p0 = this._project(x0,y0,z0), p1 = this._project(x1,y1,z1);
    if (!p0 || !p1) return 0;
    this._drawLine2D(p0[0], p0[1], p1[0], p1[1], _curColor);
    return 0;
  }},

  // ---- Input ----
  readmouse(mx, my) {{
    if (Array.isArray(mx)) mx[0] = SYS.mousx;
    if (Array.isArray(my)) my[0] = SYS.mousy;
    return 0;
  }},

  // ---- Audio/System ----
  playtext() {{ return 0; }}, playnote() {{ return 0; }}, sleep() {{ return 0; }},
  rgba(r, g, b, a) {{ return ((a & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF); }},
  getrgb(c, rgb) {{ if (Array.isArray(rgb)) {{ rgb[0]=(c>>16)&0xFF; rgb[1]=(c>>8)&0xFF; rgb[2]=c&0xFF; }} return 0; }},
  fadd(x, y) {{ return Math.fround(x + y); }},
  frameinit() {{ return this._frameinit ? 1 : 0; }},

  // ---- GL stubs ----
  glsettex(...a) {{ return 0; }}, glgettex() {{ return null; }},
  glenable() {{ return 0; }}, gldisable() {{ return 0; }}, glcullface() {{ return 0; }},
  bufset() {{ return 0; }}, bufcpy() {{ return 0; }}, sethlin() {{ return 0; }}, gethlin() {{ return 0; }},
  pic() {{ return 0; }}, fil() {{ return 0; }}, mountzip() {{ return 0; }}, getpicsiz() {{ return 0; }},

  // ---- Math ----
  normrand() {{ return (Math.random()+Math.random()+Math.random()+Math.random()+Math.random()+Math.random()+Math.random()+Math.random()+Math.random()+Math.random()+Math.random()+Math.random()-6); }},
  bouncevec(vin, vout, norm) {{
    const f = (vin.x*norm.x+vin.y*norm.y+vin.z*norm.z)*2;
    vout.x=vin.x-norm.x*f; vout.y=vin.y-norm.y*f; vout.z=vin.z-norm.z*f; return 0;
  }},
  getnorm(pol, norm) {{
    const ax=pol[1].x-pol[0].x, bx=pol[3].x-pol[0].x;
    const ay=pol[1].y-pol[0].y, by=pol[3].y-pol[0].y;
    const az=pol[1].z-pol[0].z, bz=pol[3].z-pol[0].z;
    norm.x=ay*bz-az*by; norm.y=az*bx-ax*bz; norm.z=ax*by-ay*bx;
    const fl=1/Math.sqrt(norm.x**2+norm.y**2+norm.z**2);
    norm.x*=fl; norm.y*=fl; norm.z*=fl; return 0;
  }},
  rotvex(ang, a, b) {{
    const c=Math.cos(ang), s=Math.sin(ang);
    let f=a.x; a.x=f*c+b.x*s; b.x=b.x*c-f*s;
    f=a.y; a.y=f*c+b.y*s; b.y=b.y*c-f*s;
    f=a.z; a.z=f*c+b.z*s; b.z=b.z*c-f*s; return 0;
  }},

  // ---- GL immediate mode (simplified) ----
  _glMode: 0, _glVerts: [], _glInBegin: false,
  _glColor: [1,1,1,1], _glNormal: [0,0,1], _glTexCoord: [0,0],

  glbegin(mode) {{ this._glInBegin = true; this._glMode = mode; this._glVerts = []; }},
  glend() {{
    this._glInBegin = false;
    const verts = this._glVerts;
    if (verts.length < 3) return 0;
    if (this._glMode === 0x0007) {{ // GL_QUADS
      for (let i = 0; i + 3 < verts.length; i += 4) {{
        this._drawTri(verts[i], verts[i+1], verts[i+2]);
        this._drawTri(verts[i], verts[i+2], verts[i+3]);
      }}
    }} else if (this._glMode === 0x0005) {{ // GL_TRIANGLE_STRIP
      for (let i = 0; i + 2 < verts.length; i++)
        this._drawTri(verts[i], verts[i+1], verts[i+2]);
    }} else if (this._glMode === 0x0004) {{ // GL_TRIANGLES
      for (let i = 0; i + 2 < verts.length; i += 3)
        this._drawTri(verts[i], verts[i+1], verts[i+2]);
    }}
    return 0;
  }},

  _drawTri(v0, v1, v2) {{
    const p0 = this._project(v0.x, v0.y, v0.z);
    const p1 = this._project(v1.x, v1.y, v1.z);
    const p2 = this._project(v2.x, v2.y, v2.z);
    if (!p0 || !p1 || !p2) return;
    this._drawLine2D(p0[0],p0[1],p1[0],p1[1],_curColor);
    this._drawLine2D(p1[0],p1[1],p2[0],p2[1],_curColor);
    this._drawLine2D(p2[0],p2[1],p0[0],p0[1],_curColor);
  }},

  glvertex(x, y, z, w) {{ this._glVerts.push({{x:x||0,y:y||0,z:z||0}}); }},
  gltexcoord(u, v, w) {{ this._glTexCoord = [u||0, v||0, w||0]; }},
  glcolor(r, g, b, a) {{ this._glColor = [r||0, g||0, b||0, a !== undefined ? a : 1]; }},
  glnormal(x, y, z) {{ this._glNormal = [x||0, y||0, z||0]; }},
  glpushmatrix() {{}}, glpopmatrix() {{}}, gltranslate() {{}}, glrotate() {{}}, glscale() {{}},
  glulookat() {{}}, gluperspective() {{}},
  setfov(fovy) {{ _gfov = fovy; }},
  glquad(size) {{
    this.glbegin(0x0007);
    this.glvertex(-size,-size,0); this.glvertex(+size,-size,0);
    this.glvertex(+size,+size,0); this.glvertex(-size,+size,0);
    this.glend();
  }},
  glalphaenable() {{}}, glalphadisable() {{}}, glblendfunc() {{}},
  glcapture() {{}}, glcaptureend() {{}}, glswapinterval() {{ return 0; }},
  glsetshader() {{}}, glgetuniformloc() {{ return 0; }},
  gluniform1i() {{}}, gluniform1f() {{}}, gluniform() {{}}, gllinewidth() {{}},
  glactivetexture() {{}}, glbindtexture() {{}},
  glprogramlocalparam() {{ return 0; }}, glprogramenvparam() {{ return 0; }},
  gltextdisable() {{ return 0; }}, glgetattribloc() {{ return 0; }}, glvertexattrib() {{ return 0; }},
}};

{_js_helpers()}

// ===== Struct definitions =====
{struct_js}

// ===== Host Code =====
{js_code}

// ===== Pixel Buffer → Canvas =====
function blitPixels() {{
  for (let i = 0; i < W * H; i++) {{
    const c = _pixBuf[i], idx = i * 4;
    pixels[idx]   = (c >> 16) & 0xFF;
    pixels[idx+1] = (c >> 8) & 0xFF;
    pixels[idx+2] = c & 0xFF;
    pixels[idx+3] = 255;
  }}
  ctx.putImageData(imgData, 0, 0);
}}

// ===== Animation Loop =====
function animate() {{
  _needRefresh = false;
  KC._frameinit = true;

  if (typeof hostFrame === 'function') hostFrame();

  blitPixels();

  KC._frameinit = false;
  SYS.numframes++;
  requestAnimationFrame(animate);
}}

animate();
</script>
</body>
</html>"""
