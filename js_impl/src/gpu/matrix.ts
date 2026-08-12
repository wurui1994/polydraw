// matrix.ts — 4x4 column-major matrix stack for the JS fixed-function layer.
// Faithful port of c_impl/src/render/gl_renderer.c mat4_* helpers + matrix
// stack (GLPushMatrix/GLPopMatrix/GLTranslate/GLRotate/GLScale/GLLoadIdentity/
// GLPerspective/GLOrtho/GLMultMatrix). Mirrors OpenGL column-major storage.

export type Mat4 = Float64Array; // length 16, column-major

export function mat4Identity(): Mat4 {
  const m = new Float64Array(16);
  m[0] = m[5] = m[10] = m[15] = 1;
  return m;
}

// m = m * t   (post-multiply by translation)
export function mat4Translate(m: Mat4, x: number, y: number, z: number): void {
  m[12] = m[0] * x + m[4] * y + m[8] * z + m[12];
  m[13] = m[1] * x + m[5] * y + m[9] * z + m[13];
  m[14] = m[2] * x + m[6] * y + m[10] * z + m[14];
  m[15] = m[3] * x + m[7] * y + m[11] * z + m[15];
}

export function mat4Scale(m: Mat4, x: number, y: number, z: number): void {
  m[0] *= x; m[4] *= y; m[8] *= z;
  m[1] *= x; m[5] *= y; m[9] *= z;
  m[2] *= x; m[6] *= y; m[10] *= z;
  m[3] *= x; m[7] *= y; m[11] *= z;
}

export function mat4Rotate(m: Mat4, angDeg: number, x: number, y: number, z: number): void {
  let len = Math.hypot(x, y, z);
  if (len < 1e-12) return;
  x /= len; y /= len; z /= len;
  const a = (angDeg * Math.PI) / 180;
  const s = Math.sin(a), c = Math.cos(a), t = 1 - c;
  // rotation matrix (column-major)
  const r = new Float64Array(16);
  r[0] = x * x * t + c;       r[1] = y * x * t + z * s;   r[2] = z * x * t - y * s;   r[3] = 0;
  r[4] = x * y * t - z * s;   r[5] = y * y * t + c;       r[6] = z * y * t + x * s;   r[7] = 0;
  r[8] = x * z * t + y * s;   r[9] = y * z * t - x * s;   r[10] = z * z * t + c;      r[11] = 0;
  r[12] = 0; r[13] = 0; r[14] = 0; r[15] = 1;
  mat4Mul(m, m, r);
}

// out = a * b   (column-major)
export function mat4Mul(out: Mat4, a: Mat4, b: Mat4): void {
  const o = new Float64Array(16);
  for (let c = 0; c < 4; c++) {
    for (let r = 0; r < 4; r++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
      o[c * 4 + r] = s;
    }
  }
  out.set(o);
}

export function mat4Perspective(out: Mat4, fovyDeg: number, aspect: number, near: number, far: number): void {
  const f = 1 / Math.tan((fovyDeg * Math.PI) / 360);
  out.fill(0);
  out[0] = f / aspect;
  out[5] = f;
  out[10] = (far + near) / (near - far);
  out[11] = -1;
  out[14] = (2 * far * near) / (near - far);
  out[15] = 0;
}

export function mat4Ortho(out: Mat4, l: number, r: number, b: number, t: number): void {
  out.fill(0);
  out[0] = 2 / (r - l);
  out[5] = 2 / (t - b);
  out[10] = -1;
  out[12] = -(r + l) / (r - l);
  out[13] = -(t + b) / (t - b);
  out[15] = 1;
}

// Simple matrix stack with two modes (modelview / projection).
export class MatrixStack {
  mode: 0 | 1 = 0; // 0 = modelview, 1 = projection
  private model = mat4Identity();
  private proj = mat4Identity();
  private mvStack: Mat4[] = [];
  private projStack: Mat4[] = [];

  get current(): Mat4 {
    return this.mode === 0 ? this.model : this.proj;
  }

  loadIdentity(): void {
    const m = this.current;
    m.fill(0); m[0] = m[5] = m[10] = m[15] = 1;
  }

  matrixMode(mode: number): void {
    this.mode = mode === 1 ? 1 : 0;
  }

  translate(x: number, y: number, z: number): void {
    mat4Translate(this.current, x, y, z);
  }
  scale(x: number, y: number, z: number): void {
    mat4Scale(this.current, x, y, z);
  }
  rotate(ang: number, x: number, y: number, z: number): void {
    mat4Rotate(this.current, ang, x, y, z);
  }
  mult(b: Mat4): void {
    mat4Mul(this.current, this.current, b);
  }
  perspective(fovy: number, aspect: number, near: number, far: number): void {
    mat4Perspective(this.current, fovy, aspect, near, far);
  }
  ortho(l: number, r: number, b: number, t: number): void {
    mat4Ortho(this.current, l, r, b, t);
  }

  push(): void {
    if (this.mode === 0) this.mvStack.push(this.model.slice());
    else this.projStack.push(this.proj.slice());
  }
  pop(): void {
    if (this.mode === 0) { const m = this.mvStack.pop(); if (m) this.model = m; }
    else { const m = this.projStack.pop(); if (m) this.proj = m; }
  }

  // combined modelview * projection (column-major) for vertex transform
  combined(): Mat4 {
    const o = mat4Identity();
    mat4Mul(o, this.proj, this.model);
    return o;
  }

  get modelView(): Mat4 { return this.model; }
  get projection(): Mat4 { return this.proj; }
}
