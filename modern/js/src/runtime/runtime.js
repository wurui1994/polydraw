export function canonical(name) {
  const text = String(name);
  const cached = CANONICAL_CACHE.get(text);
  if (cached) return cached;
  const value = text.toUpperCase();
  CANONICAL_CACHE.set(text, value);
  return value;
}

const CANONICAL_CACHE = new Map();

export class Cell {
  constructor(value = 0) {
    this.value = value;
  }
}

export class TraceLimitReached extends Error {
  constructor(limit) {
    super(`trace batch limit reached (${limit})`);
    this.limit = limit;
  }
}

export class PdArray {
  constructor(dimensions, initial = []) {
    this.dimensions = dimensions.map((n) => Math.max(1, Math.trunc(n)));
    this.size = this.dimensions.reduce((a, b) => a * b, 1);
    this.data = new Float64Array(this.size);
    initial.flat(Infinity).forEach((value, index) => {
      if (index < this.data.length) this.data[index] = Number(value);
    });
  }

  normalizeIndex(dim, index) {
    const size = this.dimensions[dim] ?? 1;
    let i = Math.trunc(index);
    if ((size & (size - 1)) === 0) return i & (size - 1);
    return i < 0 || i >= size ? 0 : i;
  }

  offset(indices) {
    if (indices.length === 1 && this.dimensions.length > 1) {
      let i = Math.trunc(indices[0] ?? 0);
      if ((this.size & (this.size - 1)) === 0) return i & (this.size - 1);
      return i < 0 || i >= this.size ? 0 : i;
    }
    let offset = 0;
    for (let dim = 0; dim < this.dimensions.length; dim++) {
      offset = offset * this.dimensions[dim] + this.normalizeIndex(dim, indices[dim] ?? 0);
    }
    return offset;
  }

  get(indices) {
    return this.data[this.offset(indices)];
  }

  set(indices, value) {
    this.data[this.offset(indices)] = Number(value);
    return Number(value);
  }

  setLinear(index, value) {
    if (index >= 0 && index < this.data.length) this.data[index] = Number(value);
    return Number(value);
  }

  snapshot(maxValues = 4096) {
    const count = Math.min(this.data.length, maxValues);
    return {
      dimensions: [...this.dimensions],
      truncated: this.data.length > count,
      values: Array.from(this.data.slice(0, count)),
    };
  }

  snapshotAll() {
    return {
      dimensions: [...this.dimensions],
      truncated: false,
      values: Array.from(this.data),
    };
  }
}

export class TraceRuntime {
  constructor({ xres = 640, yres = 480, loadTextureImage = null } = {}) {
    this.xres = xres;
    this.yres = yres;
    this.output = "";
    this.calls = [];
    this.recordCalls = true;
    this.batches = [];
    this.currentBatch = null;
    this.currentColor = [1, 1, 1, 1];
    this.currentTexCoord = [0, 0, 0, 1];
    this.currentNormal = [0, 0, 1];
    this.currentShader = { vertex: "-", geometry: "-", fragment: "0" };
    this.currentUniforms = new Map();
    this.programEnvParams = new Map();
    this.programLocalParams = new Map();
    this.currentState = { blend: false, depthTest: true };
    this.modelViewMatrix = mat4Identity();
    this.projectionMatrix = mat4PerspectiveDegrees(45, xres / yres, 0.1, 1000.0);
    this.matrixStack = [];
    this.activeTextureUnit = 0;
    this.textureBindings = new Map([[0, 0]]);
    this.textureMeta = new Map();
    this.textureSnapshots = new Map();
    this.loadTextureImage = loadTextureImage;
    this.captureTarget = null;
    this.uniformLocations = new Map();
    this.uniformNames = new Map();
    this.nextUniformLocation = 1;
    this.maxBatches = Infinity;
    this.startTime = Date.now();
    this.vars = new Map([
      ["XRES", new Cell(xres)],
      ["YRES", new Cell(yres)],
      ["MOUSX", new Cell(xres / 2)],
      ["MOUSY", new Cell(yres / 2)],
      ["BSTATUS", new Cell(0)],
      ["NUMFRAMES", new Cell(0)],
      ["KEYSTATUS", new PdArray([256])],
    ]);
    this.functions = new Map();
    this.installDefaults();
  }

  beginFrame() {
    this.batches = [];
    this.currentBatch = null;
    this.currentColor = [1, 1, 1, 1];
    this.currentTexCoord = [0, 0, 0, 1];
    this.currentNormal = [0, 0, 1];
    this.currentShader = { vertex: "-", geometry: "-", fragment: "0" };
    this.currentUniforms = new Map();
    this.currentState = { blend: false, depthTest: true };
    this.modelViewMatrix = mat4Identity();
    this.matrixStack = [];
    this.activeTextureUnit = 0;
    this.textureBindings = new Map([[0, 0]]);
    this.captureTarget = null;
  }

  installDefaults() {
    this.addFunction("PRINTF", (args) => {
      const fmt = String(args[0] ?? "");
      let ai = 1;
      const text = fmt.replace(/%[-+ #0-9.]*[eEfgGdi]/g, (match) => {
        const value = Number(args[ai++] ?? 0);
        if (/[di]$/.test(match)) return String(Math.trunc(value));
        const precision = /\.(\d+)/.exec(match)?.[1];
        return value.toFixed(precision === undefined ? 6 : Number(precision));
      });
      this.output += text;
      this.recordCall({ name: "PRINTF", args, text });
      return 0;
    });
    this.addFunction("RGB", ([r, g, b]) => ((clip(r) << 16) + (clip(g) << 8) + clip(b)));
    this.addFunction("RGBA", ([r, g, b, a]) => ((clip(a) << 24) >>> 0) + (clip(r) << 16) + (clip(g) << 8) + clip(b));
    this.addFunction("NOISE", (args) => noise(args));
    this.addFunction("KLOCK", ([mode = 0] = []) => klock(mode, this.startTime));
    this.addFunction("SRAND", () => 0);
    this.addFunction("SLEEP", () => 0);
    this.glklockStart = null;
    this.addFunction("GLKLOCKSTART", (args) => {
      this.glklockStart = performanceNowMs();
      this.recordCall({ name: "GLKLOCKSTART", args });
      return 0;
    });
    this.addFunction("GLKLOCKELAPSED", (args) => {
      if (this.glklockStart === null) {
        this.recordCall({ name: "GLKLOCKELAPSED", args, elapsed: -2 });
        return -2;
      }
      const elapsed = (performanceNowMs() - this.glklockStart) / 1000;
      this.glklockStart = null;
      this.recordCall({ name: "GLKLOCKELAPSED", args, elapsed });
      return elapsed;
    });
    this.addFunction("GLCLEAR", (args) => {
      this.recordCall({ name: "GLCLEAR", args });
      return 0;
    });
    this.addFunction("GLBEGIN", ([mode]) => {
      this.currentBatch = { mode: Number(mode ?? 0), shader: { ...this.currentShader }, uniforms: this.snapshotUniforms(), textures: this.snapshotTextures(), state: this.snapshotState(), vertices: [] };
      this.recordCall({ name: "GLBEGIN", args: [mode] });
      return 0;
    });
    this.addFunction("GLEND", () => {
      if (this.currentBatch) {
        this.batches.push(this.currentBatch);
        this.currentBatch = null;
        this.checkBatchLimit();
      }
      this.recordCall({ name: "GLEND", args: [] });
      return 0;
    });
    this.addFunction("GLCOLOR", (args) => {
      this.currentColor = [Number(args[0] ?? 0), Number(args[1] ?? 0), Number(args[2] ?? 0), Number(args[3] ?? 1)];
      this.recordCall({ name: "GLCOLOR", args });
      return 0;
    });
    this.addFunction("GLTEXCOORD", (args) => {
      this.currentTexCoord = [Number(args[0] ?? 0), Number(args[1] ?? 0), Number(args[2] ?? 0), Number(args[3] ?? 1)];
      this.recordCall({ name: "GLTEXCOORD", args });
      return 0;
    });
    this.addFunction("GLVERTEX", (args) => {
      const vertex = {
        position: [Number(args[0] ?? 0), Number(args[1] ?? 0), Number(args[2] ?? 0), Number(args[3] ?? 1)],
        color: [...this.currentColor],
        texcoord: [...this.currentTexCoord],
        normal: [...this.currentNormal],
      };
      if (this.currentBatch) this.currentBatch.vertices.push(vertex);
      this.recordCall({ name: "GLVERTEX", args });
      return 0;
    });
    this.addFunction("GLQUAD", (args) => {
      this.batches.push({
        mode: 5,
        shader: { ...this.currentShader },
        uniforms: this.snapshotUniforms({ clipSpace: true }),
        textures: this.snapshotTextures(),
        state: this.snapshotState(),
        vertices: [
          { position: [-1, -1, 0, 1], color: [...this.currentColor], texcoord: [0, 0, 0, 1], normal: [...this.currentNormal] },
          { position: [ 1, -1, 0, 1], color: [...this.currentColor], texcoord: [1, 0, 0, 1], normal: [...this.currentNormal] },
          { position: [-1,  1, 0, 1], color: [...this.currentColor], texcoord: [0, 1, 0, 1], normal: [...this.currentNormal] },
          { position: [ 1,  1, 0, 1], color: [...this.currentColor], texcoord: [1, 1, 0, 1], normal: [...this.currentNormal] },
        ],
      });
      this.checkBatchLimit();
      this.recordCall({ name: "GLQUAD", args });
      return 0;
    });
    this.addFunction("GLPUSHMATRIX", () => {
      this.matrixStack.push([...this.modelViewMatrix]);
      this.recordCall({ name: "GLPUSHMATRIX", args: [] });
      return 0;
    });
    this.addFunction("GLPOPMATRIX", () => {
      if (this.matrixStack.length) this.modelViewMatrix = this.matrixStack.pop();
      this.recordCall({ name: "GLPOPMATRIX", args: [] });
      return 0;
    });
    this.addFunction("GLTRANSLATE", ([x, y, z]) => {
      this.modelViewMatrix = mat4Multiply(this.modelViewMatrix, mat4Translate(finiteOr(x, 0), finiteOr(y, 0), finiteOr(z, 0)));
      this.recordCall({ name: "GLTRANSLATE", args: [x, y, z] });
      return 0;
    });
    this.addFunction("GLSCALE", ([x, y, z]) => {
      const sx = finiteOr(x, 1);
      this.modelViewMatrix = mat4Multiply(this.modelViewMatrix, mat4Scale(sx, finiteOr(y, sx), finiteOr(z, sx)));
      this.recordCall({ name: "GLSCALE", args: [x, y, z] });
      return 0;
    });
    this.addFunction("GLROTATE", ([angle, x, y, z]) => {
      this.modelViewMatrix = mat4Multiply(this.modelViewMatrix, mat4Rotate(finiteOr(angle, 0), finiteOr(x, 0), finiteOr(y, 0), finiteOr(z, 1)));
      this.recordCall({ name: "GLROTATE", args: [angle, x, y, z] });
      return 0;
    });
    this.addFunction("GLUPERSPECTIVE", ([fovy, aspect, znear, zfar]) => {
      this.projectionMatrix = mat4PerspectiveDegrees(Number(fovy ?? 60), Number(aspect ?? 1), Number(znear ?? 0.1), Number(zfar ?? 1000));
      this.recordCall({ name: "GLUPERSPECTIVE", args: [fovy, aspect, znear, zfar] });
      return 0;
    });
    this.addFunction("SETFOV", ([fovy]) => {
      this.projectionMatrix = mat4PerspectiveDegrees(finiteOr(fovy, 60), this.xres / this.yres, 0.1, 1000);
      this.recordCall({ name: "SETFOV", args: [fovy] });
      return 0;
    });
    this.addFunction("GLULOOKAT", ([eyeX, eyeY, eyeZ, centerX, centerY, centerZ, upX, upY, upZ]) => {
      this.modelViewMatrix = mat4Multiply(this.modelViewMatrix, mat4LookAt(
        finiteOr(eyeX, 0), finiteOr(eyeY, 0), finiteOr(eyeZ, 1),
        finiteOr(centerX, 0), finiteOr(centerY, 0), finiteOr(centerZ, 0),
        finiteOr(upX, 0), finiteOr(upY, 1), finiteOr(upZ, 0),
      ));
      this.recordCall({ name: "GLULOOKAT", args: [eyeX, eyeY, eyeZ, centerX, centerY, centerZ, upX, upY, upZ] });
      return 0;
    });
    this.addFunction("GLGETUNIFORMLOC", (args) => {
      const name = String(args[0] ?? "");
      const key = `${this.currentShader.vertex}|${this.currentShader.geometry}|${this.currentShader.fragment}|${name}`;
      if (!this.uniformLocations.has(key)) {
        const loc = this.nextUniformLocation++;
        this.uniformLocations.set(key, loc);
        this.uniformNames.set(loc, name);
        for (let i = 1; i < 16; i++) this.uniformNames.set(loc + i, `${name}[${i}]`);
      }
      const loc = this.uniformLocations.get(key);
      this.recordCall({ name: "GLGETUNIFORMLOC", args, location: loc });
      return loc;
    });
    const setUniform = (kind, loc, values, count = 1) => {
      const name = this.uniformNames.get(Number(loc)) ?? String(loc);
      this.currentUniforms.set(name, { name, type: kind, count, values: values.map(Number) });
      this.recordCall({ name: `GLUNIFORM${kind.toUpperCase()}`, args: [loc, ...values], uniform: this.currentUniforms.get(name) });
      return 0;
    };
    this.addFunction("GLUNIFORM1F", ([loc, x]) => setUniform("1f", loc, [x]));
    this.addFunction("GLUNIFORM2F", ([loc, x, y]) => setUniform("2f", loc, [x, y]));
    this.addFunction("GLUNIFORM3F", ([loc, x, y, z]) => setUniform("3f", loc, [x, y, z]));
    this.addFunction("GLUNIFORM4F", ([loc, x, y, z, w]) => setUniform("4f", loc, [x, y, z, w]));
    this.addFunction("GLUNIFORM1I", ([loc, x]) => setUniform("1i", loc, [x]));
    this.addFunction("GLUNIFORM1FV", ([loc, count, ptr]) => setUniform("1fv", loc, this.readPointerValues(ptr, Number(count ?? 0)), Number(count ?? 0)));
    this.addFunction("GLUNIFORM2FV", ([loc, count, ptr]) => setUniform("2fv", loc, this.readPointerValues(ptr, Number(count ?? 0) * 2), Number(count ?? 0)));
    this.addFunction("GLUNIFORM3FV", ([loc, count, ptr]) => setUniform("3fv", loc, this.readPointerValues(ptr, Number(count ?? 0) * 3), Number(count ?? 0)));
    this.addFunction("GLUNIFORM4FV", ([loc, count, ptr]) => setUniform("4fv", loc, this.readPointerValues(ptr, Number(count ?? 0) * 4), Number(count ?? 0)));
    this.addFunction("GLUNIFORM1IV", ([loc, count, ptr]) => setUniform("1iv", loc, this.readPointerValues(ptr, Number(count ?? 0)), Number(count ?? 0)));
    this.addFunction("GLUNIFORM2IV", ([loc, count, ptr]) => setUniform("2iv", loc, this.readPointerValues(ptr, Number(count ?? 0) * 2), Number(count ?? 0)));
    this.addFunction("GLUNIFORM3IV", ([loc, count, ptr]) => setUniform("3iv", loc, this.readPointerValues(ptr, Number(count ?? 0) * 3), Number(count ?? 0)));
    this.addFunction("GLUNIFORM4IV", ([loc, count, ptr]) => setUniform("4iv", loc, this.readPointerValues(ptr, Number(count ?? 0) * 4), Number(count ?? 0)));
    const setProgramParam = (kind, args) => {
      const index = Math.max(0, Math.trunc(Number(args[0] ?? 0)));
      const values = [args[1], args[2], args[3], args[4]].map((v) => Number(v ?? 0));
      const name = kind === "env" ? `_pd_programEnv[${index}]` : `_pd_programLocal[${index}]`;
      const target = kind === "env" ? this.programEnvParams : this.programLocalParams;
      target.set(index, values);
      this.currentUniforms.set(name, { name, type: "4f", count: 1, values });
      this.recordCall({ name: kind === "env" ? "GLPROGRAMENVPARAM" : "GLPROGRAMLOCALPARAM", args, uniform: this.currentUniforms.get(name) });
      return 0;
    };
    this.addFunction("GLPROGRAMENVPARAM", (args) => setProgramParam("env", args));
    this.addFunction("GLPROGRAMLOCALPARAM", (args) => setProgramParam("local", args));
    this.addFunction("GLACTIVETEXTURE", ([unit]) => {
      const raw = Number(unit ?? 0);
      this.activeTextureUnit = raw >= 0x84c0 ? raw - 0x84c0 : raw;
      this.recordCall({ name: "GLACTIVETEXTURE", args: [unit], unit: this.activeTextureUnit });
      return 0;
    });
    this.addFunction("GLBINDTEXTURE", ([textureId]) => {
      this.textureBindings.set(this.activeTextureUnit, Math.trunc(Number(textureId ?? 0)));
      this.recordCall({ name: "GLBINDTEXTURE", args: [textureId], unit: this.activeTextureUnit });
      return 0;
    });
    this.addFunction("GLSETTEX", (args) => {
      const id = Math.trunc(Number(args[0] ?? 0));
      const source = args[1];
      const isFile = typeof source === "string";
      const format = isFile
        ? (Number(args[2] ?? 0) || 0)
        : (args.length >= 6 ? (Number(args[5] ?? 0) || 0) : (Number(args[4] ?? 0) || 0));
      const meta = {
        id,
        width: isFile ? 0 : (Number(args[2] ?? 1) || 1),
        height: isFile ? 0 : (Number(args[3] ?? 1) || 1),
        depth: isFile ? 1 : (args.length >= 6 ? (Number(args[4] ?? 1) || 1) : 1),
        format,
        source: isFile ? "file" : "array",
      };
      if (isFile) meta.path = source;
      this.textureMeta.set(id, meta);
      if (source instanceof PdArray) {
        const snapshot = source.snapshot(4096);
        this.textureSnapshots.set(id, { ...meta, ...snapshot, array: source });
      } else if (isFile && this.loadTextureImage) {
        const snapshot = this.loadTextureImage(source, { textureId: id, format });
        if (snapshot?.array instanceof PdArray) {
          const nextMeta = {
            ...meta,
            width: snapshot.width || meta.width,
            height: snapshot.height || meta.height,
            format: snapshot.format ?? meta.format,
            resolvedPath: snapshot.resolvedPath,
          };
          this.textureMeta.set(id, nextMeta);
          this.textureSnapshots.set(id, {
            ...nextMeta,
            truncated: false,
            values: snapshot.array.snapshot(4096).values,
            array: snapshot.array,
            rgba: snapshot.rgba,
          });
        }
      }
      this.recordCall({ name: "GLSETTEX", args, texture: meta });
      return 0;
    });
    this.addFunction("GLGETTEX", (args) => {
      const id = Math.trunc(Number(args[0] ?? 0));
      const target = args[1];
      if (target instanceof PdArray) {
        const snapshot = this.textureSnapshots.get(id);
        const readback = snapshot?.rgba
          ? makeTextureReadbackArray(snapshot.rgba, Number(args[2] ?? snapshot.width ?? 1), Number(args[3] ?? snapshot.height ?? 1), Number(args[4] ?? snapshot.format ?? 0))
          : snapshot?.array;
        const values = readback?.snapshotAll().values ?? snapshot?.values ?? [];
        const count = Math.min(target.data.length, values.length);
        for (let i = 0; i < count; i++) target.setLinear(i, values[i]);
      }
      this.recordCall({ name: "GLGETTEX", args });
      return 0;
    });
    this.addFunction("GLCAPTURE", (args) => {
      this.captureTarget = {
        id: args.length ? Math.trunc(Number(args[0] ?? 0)) : null,
        width: Number(args[1] ?? 0) || null,
        height: Number(args[2] ?? 0) || null,
        format: Number(args[3] ?? 0) || 0,
        startBatch: this.batches.length,
      };
      this.recordCall({ name: "GLCAPTURE", args, capture: this.captureTarget });
      return 0;
    });
    this.addFunction("GLCAPTUREEND", ([textureId]) => {
      const id = Math.trunc(Number(textureId ?? this.captureTarget?.id ?? 0));
      const width = this.captureTarget?.width ?? this.vars.get("XRES")?.value ?? 640;
      const height = this.captureTarget?.height ?? this.vars.get("YRES")?.value ?? 480;
      const format = this.captureTarget?.format ?? 0;
      const meta = {
        id,
        width,
        height,
        format,
        source: "capture",
        startBatch: this.captureTarget?.startBatch ?? 0,
        endBatch: this.batches.length,
      };
      const capture = this.makeCaptureArray(width, height, format, this.captureTarget?.startBatch ?? 0);
      this.textureMeta.set(id, meta);
      this.textureSnapshots.set(id, { ...meta, truncated: false, values: capture.snapshot(4096).values, array: capture });
      if (id >= 0 && id < 16) this.textureBindings.set(id, id);
      this.captureTarget = null;
      this.recordCall({ name: "GLCAPTUREEND", args: [textureId], textureId: id });
      return 0;
    });
    this.addFunction("GLSETSHADER", (args) => {
      if (args.length >= 3) {
        this.currentShader = { vertex: String(args[0]), geometry: String(args[1]), fragment: String(args[2]) };
      } else if (args.length >= 2) {
        this.currentShader = { vertex: String(args[0]), geometry: "-", fragment: String(args[1]) };
      } else if (args.length >= 1) {
        this.currentShader = { vertex: "-", geometry: "-", fragment: String(Math.trunc(Number(args[0] ?? 0))) };
      }
      this.recordCall({ name: "GLSETSHADER", args, shader: { ...this.currentShader } });
      return 0;
    });
    for (const name of [
      "GLBLENDFUNC", "GLENABLE", "GLDISABLE",
      "GLCULLFACE", "GLLINEWIDTH", "GLUPERSPECTIVE",
      "GLULOOKAT", "SETFOV", "GLGETATTRIBLOC", "GLVERTEXATTRIB1F", "GLVERTEXATTRIB2F",
      "GLVERTEXATTRIB3F", "GLVERTEXATTRIB4F", "GLSWAPINTERVAL",
    ]) {
      if (this.functions.has(canonical(name))) continue;
      this.addFunction(name, (args) => {
        this.recordCall({ name, args });
        return 0;
      });
    }
    this.addFunction("GLNORMAL", (args) => {
      this.currentNormal = [Number(args[0] ?? 0), Number(args[1] ?? 0), Number(args[2] ?? 1)];
      this.recordCall({ name: "GLNORMAL", args, normal: this.currentNormal });
      return 0;
    });
    this.addFunction("GLALPHAENABLE", (args) => {
      this.currentState.blend = true;
      this.currentState.depthTest = false;
      this.recordCall({ name: "GLALPHAENABLE", args, state: this.snapshotState() });
      return 0;
    });
    this.addFunction("GLALPHADISABLE", (args) => {
      this.currentState.blend = false;
      this.currentState.depthTest = true;
      this.recordCall({ name: "GLALPHADISABLE", args, state: this.snapshotState() });
      return 0;
    });
    const constants = {
      GL_POINTS: 0, GL_LINES: 1, GL_LINE_LOOP: 2, GL_LINE_STRIP: 3,
      GL_TRIANGLES: 4, GL_TRIANGLE_STRIP: 5, GL_TRIANGLE_FAN: 6, GL_QUADS: 7,
      GL_QUAD_STRIP: 8, GL_POLYGON: 9, GL_TEXTURE0: 0x84c0,
      GL_COLOR_BUFFER_BIT: 0x4000, GL_DEPTH_BUFFER_BIT: 0x0100, GL_STENCIL_BUFFER_BIT: 0x0400,
      GL_NONE: 0, GL_FRONT: 0x0404, GL_BACK: 0x0405, GL_FRONT_AND_BACK: 0x0408,
      GL_DEPTH_TEST: 0x0b71, KGL_BGRA32: 0, KGL_CHAR: 1, KGL_SHORT: 2, KGL_INT: 3,
      KGL_FLOAT: 4, KGL_VEC4: 5, KGL_LINEAR: 0, KGL_NEAREST: 16, KGL_MIPMAP: 32,
      KGL_MIPMAP0: 80, KGL_MIPMAP1: 64, KGL_MIPMAP2: 48, KGL_MIPMAP3: 32,
      KGL_REPEAT: 0, KGL_CLAMP: 512, KGL_CLAMP_TO_EDGE: 768,
    };
    for (const [name, value] of Object.entries(constants)) this.vars.set(name, new Cell(value));
  }

  addFunction(name, fn) {
    this.functions.set(canonical(name), fn);
  }

  getExternal(name) {
    return this.vars.get(canonical(name));
  }

  callExternal(name, args) {
    const fn = this.functions.get(canonical(name));
    if (!fn) throw new Error(`unknown external function ${name}`);
    return Number(fn(args, this));
  }

  recordCall(call) {
    if (this.recordCalls) this.calls.push(call);
  }

  snapshotUniforms(options = {}) {
    const mvp = options.clipSpace
      ? [...this.modelViewMatrix]
      : mat4Multiply(this.projectionMatrix, this.modelViewMatrix);
    const uniforms = Array.from(this.currentUniforms.values()).map((uniform) => ({
      name: uniform.name,
      type: uniform.type,
      count: uniform.count,
      values: [...uniform.values],
    }));
    uniforms.push({
      name: "_pd_mvp",
      type: "mat4",
      count: 1,
      values: mvp,
    });
    uniforms.push({
      name: "_pd_modelview",
      type: "mat4",
      count: 1,
      values: [...this.modelViewMatrix],
    });
    uniforms.push({
      name: "_pd_normalMatrix",
      type: "mat3",
      count: 1,
      values: mat3NormalFromMat4(this.modelViewMatrix),
    });
    return uniforms;
  }

  snapshotTextures() {
    return Array.from(this.textureBindings.entries())
      .filter(([unit]) => unit >= 0 && unit < 16)
      .sort((a, b) => a[0] - b[0])
      .map(([unit, textureId]) => ({ unit, textureId }));
  }

  snapshotState() {
    return { ...this.currentState };
  }

  readPointerValues(ptr, count) {
    if (ptr instanceof PdArray) {
      const values = [];
      for (let i = 0; i < count; i++) values.push(ptr.data[i] ?? 0);
      return values;
    }
    if (ptr && typeof ptr.get === "function") {
      const values = [];
      for (let i = 0; i < count; i++) values.push(ptr.get([i]));
      return values;
    }
    return [Number(ptr ?? 0)];
  }

  checkBatchLimit() {
    if (this.batches.length >= this.maxBatches) throw new TraceLimitReached(this.maxBatches);
  }

  makeCaptureArray(width, height, format, startBatch) {
    const w = Math.max(1, Math.trunc(width || 1));
    const h = Math.max(1, Math.trunc(height || 1));
    const channels = format === 5 ? 4 : 1;
    const array = new PdArray(channels === 4 ? [h, w, 4] : [h * w]);
    const captured = this.batches.slice(startBatch);
    const seedBatch = captured[captured.length - 1];
    const color = seedBatch?.vertices?.[0]?.color ?? this.currentColor;
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const u = w > 1 ? x / (w - 1) : 0;
        const v = h > 1 ? y / (h - 1) : 0;
        const pulse = (Number(color[0] ?? 0) + u * 0.5 + v * 0.25) % 1;
        if (channels === 4) {
          const base = (y * w + x) * 4;
          array.setLinear(base + 0, pulse);
          array.setLinear(base + 1, (Number(color[1] ?? 0) + u) % 1);
          array.setLinear(base + 2, (Number(color[2] ?? 0) + v) % 1);
          array.setLinear(base + 3, 1);
        } else {
          array.setLinear(y * w + x, pulse);
        }
      }
    }
    return array;
  }
}

function clip(v) {
  return Math.max(0, Math.min(255, Math.trunc(Number(v) || 0)));
}

function noise(args) {
  const x = Number(args[0] ?? 0);
  const y = Number(args[1] ?? 0);
  const z = Number(args[2] ?? 0);
  const n = Math.sin(x * 12.9898 + y * 78.233 + z * 37.719) * 43758.5453;
  return n - Math.floor(n);
}

function finiteOr(value, fallback) {
  const n = Number(value ?? fallback);
  return Number.isFinite(n) ? n : fallback;
}

function makeTextureReadbackArray(rgba, width, height, format) {
  const sourceWidth = Math.max(1, Math.trunc(rgba.width || 1));
  const sourceHeight = Math.max(1, Math.trunc(rgba.height || 1));
  const targetWidth = Math.max(1, Math.trunc(width || sourceWidth));
  const targetHeight = Math.max(1, Math.trunc(height || sourceHeight));
  const baseFormat = Math.trunc(Number(format) || 0) & 0x0f;
  const array = new PdArray(baseFormat === 5 ? [targetHeight, targetWidth, 4] : [targetHeight, targetWidth]);
  for (let y = 0; y < targetHeight; y++) {
    const sy = Math.min(sourceHeight - 1, Math.floor((y * sourceHeight) / targetHeight));
    for (let x = 0; x < targetWidth; x++) {
      const sx = Math.min(sourceWidth - 1, Math.floor((x * sourceWidth) / targetWidth));
      const src = (sy * sourceWidth + sx) * 4;
      const r = Number(rgba.data[src + 0] ?? 0);
      const g = Number(rgba.data[src + 1] ?? 0);
      const b = Number(rgba.data[src + 2] ?? 0);
      const a = Number(rgba.data[src + 3] ?? 255);
      const dst = y * targetWidth + x;
      if (baseFormat === 4) {
        array.setLinear(dst, (r + g + b) / (255 * 3));
      } else if (baseFormat === 5) {
        const base = dst * 4;
        array.setLinear(base + 0, r / 255);
        array.setLinear(base + 1, g / 255);
        array.setLinear(base + 2, b / 255);
        array.setLinear(base + 3, a / 255);
      } else {
        array.setLinear(dst, (((a & 255) << 24) >>> 0) + ((r & 255) << 16) + ((g & 255) << 8) + (b & 255));
      }
    }
  }
  return array;
}

function mat4Identity() {
  return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
}

function mat4Multiply(a, b) {
  const out = new Array(16).fill(0);
  for (let col = 0; col < 4; col++) {
    for (let row = 0; row < 4; row++) {
      out[col * 4 + row] =
        a[0 * 4 + row] * b[col * 4 + 0] +
        a[1 * 4 + row] * b[col * 4 + 1] +
        a[2 * 4 + row] * b[col * 4 + 2] +
        a[3 * 4 + row] * b[col * 4 + 3];
    }
  }
  return out;
}

function mat4Translate(x, y, z) {
  return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1];
}

function mat4Scale(x, y, z) {
  return [x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1];
}

function mat4Rotate(angleDegrees, x, y, z) {
  const len = Math.hypot(x, y, z);
  if (!len) return mat4Identity();
  x /= len; y /= len; z /= len;
  const a = angleDegrees * Math.PI / 180;
  const c = Math.cos(a);
  const s = Math.sin(a);
  const t = 1 - c;
  return [
    t * x * x + c, t * x * y + s * z, t * x * z - s * y, 0,
    t * x * y - s * z, t * y * y + c, t * y * z + s * x, 0,
    t * x * z + s * y, t * y * z - s * x, t * z * z + c, 0,
    0, 0, 0, 1,
  ];
}

function mat4LookAt(eyeX, eyeY, eyeZ, centerX, centerY, centerZ, upX, upY, upZ) {
  let fx = centerX - eyeX;
  let fy = centerY - eyeY;
  let fz = centerZ - eyeZ;
  let fl = Math.hypot(fx, fy, fz);
  if (!fl) return mat4Identity();
  fx /= fl; fy /= fl; fz /= fl;
  let ul = Math.hypot(upX, upY, upZ);
  if (!ul) {
    upX = 0; upY = 1; upZ = 0; ul = 1;
  }
  upX /= ul; upY /= ul; upZ /= ul;
  let sx = fy * upZ - fz * upY;
  let sy = fz * upX - fx * upZ;
  let sz = fx * upY - fy * upX;
  let sl = Math.hypot(sx, sy, sz);
  if (!sl) return mat4Translate(-eyeX, -eyeY, -eyeZ);
  sx /= sl; sy /= sl; sz /= sl;
  const ux = sy * fz - sz * fy;
  const uy = sz * fx - sx * fz;
  const uz = sx * fy - sy * fx;
  return mat4Multiply([
    sx, ux, -fx, 0,
    sy, uy, -fy, 0,
    sz, uz, -fz, 0,
    0, 0, 0, 1,
  ], mat4Translate(-eyeX, -eyeY, -eyeZ));
}

function mat3NormalFromMat4(m) {
  const a00 = m[0], a01 = m[4], a02 = m[8];
  const a10 = m[1], a11 = m[5], a12 = m[9];
  const a20 = m[2], a21 = m[6], a22 = m[10];
  const b01 = a22 * a11 - a12 * a21;
  const b11 = -a22 * a10 + a12 * a20;
  const b21 = a21 * a10 - a11 * a20;
  let det = a00 * b01 + a01 * b11 + a02 * b21;
  if (Math.abs(det) < 1e-12) return [1, 0, 0, 0, 1, 0, 0, 0, 1];
  det = 1 / det;
  const inv = [
    b01 * det,
    (-a22 * a01 + a02 * a21) * det,
    (a12 * a01 - a02 * a11) * det,
    b11 * det,
    (a22 * a00 - a02 * a20) * det,
    (-a12 * a00 + a02 * a10) * det,
    b21 * det,
    (-a21 * a00 + a01 * a20) * det,
    (a11 * a00 - a01 * a10) * det,
  ];
  return [
    inv[0], inv[3], inv[6],
    inv[1], inv[4], inv[7],
    inv[2], inv[5], inv[8],
  ];
}

function mat4PerspectiveDegrees(fovy, aspect, znear, zfar) {
  const f = 1 / Math.tan((fovy * Math.PI / 180) / 2);
  const nf = 1 / (znear - zfar);
  return [
    f / aspect, 0, 0, 0,
    0, f, 0, 0,
    0, 0, (zfar + znear) * nf, -1,
    0, 0, (2 * zfar * znear) * nf, 0,
  ];
}

function klock(mode, startTime) {
  const i = Math.trunc(Number(mode ?? 0));
  if (!i) return (Date.now() - startTime) / 1000;
  if (Math.abs(i) >= 10) return 0;
  const d = new Date();
  const utc = i < 0;
  const field = Math.abs(i);
  const year = utc ? d.getUTCFullYear() : d.getFullYear();
  const month = (utc ? d.getUTCMonth() : d.getMonth()) + 1;
  const weekday = utc ? d.getUTCDay() : d.getDay();
  const day = utc ? d.getUTCDate() : d.getDate();
  const hour = utc ? d.getUTCHours() : d.getHours();
  const minute = utc ? d.getUTCMinutes() : d.getMinutes();
  const second = utc ? d.getUTCSeconds() : d.getSeconds();
  const millisecond = utc ? d.getUTCMilliseconds() : d.getMilliseconds();
  switch (field) {
    case 1:
      return (year * 10000000000000 + month * 100000000000 + day * 1000000000 +
        hour * 10000000 + minute * 100000 + second * 1000 + millisecond) * 0.001;
    case 2: return year;
    case 3: return month;
    case 4: return weekday;
    case 5: return day;
    case 6: return hour;
    case 7: return minute;
    case 8: return second;
    case 9: return millisecond;
    default: return 0;
  }
}

function performanceNowMs() {
  if (typeof performance !== "undefined" && typeof performance.now === "function") return performance.now();
  return Date.now();
}
