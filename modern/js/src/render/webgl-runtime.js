import { TraceRuntime } from "../runtime/runtime.js";
import { SectionType } from "../parser/sections.js";

const DEFAULT_VS = `
attribute vec4 position;
attribute vec4 color;
attribute vec4 texcoord;
varying vec4 v_color;
varying vec4 v_texcoord;
void main() {
  v_color = color;
  v_texcoord = texcoord;
  gl_Position = position;
}
`;

const DEFAULT_FS = `
precision mediump float;
varying vec4 v_color;
varying vec4 v_texcoord;
void main() {
  gl_FragColor = v_color;
}
`;

export class WebGLRuntime extends TraceRuntime {
  constructor(canvas, options = {}) {
    super(options);
    this.canvas = canvas;
    this.sections = options.sections ?? [];
    this.gl = canvas.getContext("webgl", { antialias: true, depth: true }) || canvas.getContext("experimental-webgl");
    if (!this.gl) throw new Error("WebGL is not available");
    this.program = createProgram(this.gl, DEFAULT_VS, DEFAULT_FS);
    this.programCache = new Map();
    this.buffer = this.gl.createBuffer();
    this.drawnBatchCount = 0;
    this.installWebGLFunctions();
  }

  beginFrame() {
    super.beginFrame();
    this.drawnBatchCount = 0;
  }

  installWebGLFunctions() {
    this.addFunction("GLCLEAR", () => {
      const gl = this.gl;
      gl.clearColor(0, 0, 0, 1);
      gl.clearDepth(1);
      gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
      return 0;
    });
  }

  checkBatchLimit() {
    super.checkBatchLimit();
    while (this.drawnBatchCount < this.batches.length) {
      this.drawBatch(this.batches[this.drawnBatchCount]);
      this.drawnBatchCount++;
    }
  }

  drawBatch(batch) {
    if (!batch?.vertices?.length) return;
    const gl = this.gl;
    const vertices = batch.mode === 7 ? expandQuads(batch.vertices) : batch.vertices;
    if (!vertices.length) return;
    const data = new Float32Array(vertices.length * 12);
    vertices.forEach((vertex, index) => {
      data.set(vertex.position, index * 12);
      data.set(vertex.color, index * 12 + 4);
      data.set(vertex.texcoord, index * 12 + 8);
    });

    gl.viewport(0, 0, this.canvas.width, this.canvas.height);
    if (batch.state?.depthTest === false) gl.disable(gl.DEPTH_TEST);
    else gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    if (batch.state?.blend) {
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    } else {
      gl.disable(gl.BLEND);
    }
    const program = this.programForBatch(batch);
    gl.useProgram(program);
    uploadUniforms(gl, program, batch.uniforms ?? []);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
    gl.bufferData(gl.ARRAY_BUFFER, data, gl.STREAM_DRAW);
    bindAttribAny(gl, program, ["_pd_position", "position"], 4, 12, 0);
    bindAttribAny(gl, program, ["_pd_color", "color"], 4, 12, 4);
    bindAttribAny(gl, program, ["_pd_texcoord", "texcoord"], 4, 12, 8);
    gl.drawArrays(mapMode(gl, batch.mode), 0, vertices.length);
  }

  programForBatch(batch) {
    const shader = batch?.shader ?? {};
    if (shader.geometry && shader.geometry !== "-") return this.program;
    const vertex = findShaderSection(this.sections, SectionType.VERTEX, shader.vertex);
    const fragment = findShaderSection(this.sections, SectionType.FRAGMENT, shader.fragment);
    if (!vertex || !fragment) return this.program;
    const key = `${vertex.line}:${fragment.line}:${shader.vertex ?? ""}:${shader.fragment ?? ""}`;
    const cached = this.programCache.get(key);
    if (cached) return cached;
    const program = createProgram(
      this.gl,
      modernizeGlsl(vertex.text, "vertex"),
      modernizeGlsl(fragment.text, "fragment"),
    );
    this.programCache.set(key, program);
    return program;
  }
}

function createProgram(gl, vsSource, fsSource) {
  const vs = compileShader(gl, gl.VERTEX_SHADER, vsSource);
  const fs = compileShader(gl, gl.FRAGMENT_SHADER, fsSource);
  const program = gl.createProgram();
  gl.attachShader(program, vs);
  gl.attachShader(program, fs);
  gl.linkProgram(program);
  gl.deleteShader(vs);
  gl.deleteShader(fs);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    const log = gl.getProgramInfoLog(program);
    gl.deleteProgram(program);
    throw new Error(`WebGL program link failed: ${log}`);
  }
  return program;
}

function compileShader(gl, type, source) {
  const shader = gl.createShader(type);
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(shader);
    gl.deleteShader(shader);
    throw new Error(`WebGL shader compile failed: ${log}`);
  }
  return shader;
}

function bindAttrib(gl, program, name, size, strideFloats, offsetFloats) {
  const loc = gl.getAttribLocation(program, name);
  if (loc < 0) return;
  gl.enableVertexAttribArray(loc);
  gl.vertexAttribPointer(loc, size, gl.FLOAT, false, strideFloats * 4, offsetFloats * 4);
}

function bindAttribAny(gl, program, names, size, strideFloats, offsetFloats) {
  for (const name of names) bindAttrib(gl, program, name, size, strideFloats, offsetFloats);
}

function uploadUniforms(gl, program, uniforms) {
  for (const uniform of uniforms) {
    const name = uniform.name ?? "";
    let loc = gl.getUniformLocation(program, name);
    if (loc === null && Number(uniform.count ?? 1) > 1) loc = gl.getUniformLocation(program, `${name}[0]`);
    if (loc === null) continue;
    const values = uniform.values ?? [];
    const count = Number(uniform.count ?? 1);
    switch (uniform.type) {
      case "1f":
        gl.uniform1f(loc, Number(values[0] ?? 0));
        break;
      case "2f":
        gl.uniform2f(loc, Number(values[0] ?? 0), Number(values[1] ?? 0));
        break;
      case "3f":
        gl.uniform3f(loc, Number(values[0] ?? 0), Number(values[1] ?? 0), Number(values[2] ?? 0));
        break;
      case "4f":
        gl.uniform4f(loc, Number(values[0] ?? 0), Number(values[1] ?? 0), Number(values[2] ?? 0), Number(values[3] ?? 0));
        break;
      case "1i":
        gl.uniform1i(loc, Number(values[0] ?? 0));
        break;
      case "1fv":
        gl.uniform1fv(loc, new Float32Array(values.slice(0, count)));
        break;
      case "2fv":
        gl.uniform2fv(loc, new Float32Array(values.slice(0, count * 2)));
        break;
      case "3fv":
        gl.uniform3fv(loc, new Float32Array(values.slice(0, count * 3)));
        break;
      case "4fv":
        gl.uniform4fv(loc, new Float32Array(values.slice(0, count * 4)));
        break;
      case "mat3":
        gl.uniformMatrix3fv(loc, false, new Float32Array(values.slice(0, 9)));
        break;
      case "mat4":
        gl.uniformMatrix4fv(loc, false, new Float32Array(values.slice(0, 16)));
        break;
      default:
        break;
    }
  }
}

function mapMode(gl, mode) {
  switch (Number(mode)) {
    case 0: return gl.POINTS;
    case 1: return gl.LINES;
    case 2: return gl.LINE_LOOP;
    case 3: return gl.LINE_STRIP;
    case 4: return gl.TRIANGLES;
    case 5: return gl.TRIANGLE_STRIP;
    case 6: return gl.TRIANGLE_FAN;
    case 7: return gl.TRIANGLES;
    case 9: return gl.TRIANGLE_FAN;
    default: return gl.TRIANGLE_STRIP;
  }
}

function findShaderSection(sections, type, key) {
  const matching = sections.filter((section) => section.type === type);
  if (!matching.length) return null;
  if (key === undefined || key === null || key === "" || key === "-") return matching[0];
  const text = String(key);
  if (/^\d+$/.test(text)) return matching[Number(text)] ?? null;
  return matching.find((section) => section.name === text) ?? null;
}

function modernizeGlsl(source, stage) {
  let body = String(source ?? "");
  body = body.replace(/^\s*#version[^\n]*(\n|$)/, "");
  if (stage === "vertex") {
    body = body.replaceAll("ftransform()", "(_pd_mvp * _pd_position)");
    body = body.replaceAll("gl_Vertex", "_pd_position");
    body = body.replaceAll("gl_Color", "_pd_color");
    body = body.replaceAll("gl_MultiTexCoord0", "_pd_texcoord");
    body = body.replaceAll("gl_FrontColor", "_pd_front_color");
    body = body.replaceAll("gl_TexCoord[0]", "_pd_texcoord0");
    body = body.replaceAll("gl_ModelViewProjectionMatrix", "_pd_mvp");
    body = body.replaceAll("gl_ModelViewMatrix", "_pd_modelview");
    body = body.replaceAll("gl_NormalMatrix", "_pd_normalMatrix");
    body = body.replaceAll("gl_Normal", "_pd_normal");
    return [
      "attribute vec4 _pd_position;",
      "attribute vec4 _pd_color;",
      "attribute vec4 _pd_texcoord;",
      "attribute vec3 _pd_normal;",
      "uniform mat4 _pd_mvp;",
      "uniform mat4 _pd_modelview;",
      "uniform mat3 _pd_normalMatrix;",
      "varying vec4 _pd_front_color;",
      "varying vec4 _pd_texcoord0;",
      body,
    ].join("\n");
  }
  body = body.replaceAll("gl_Color", "_pd_front_color");
  body = body.replaceAll("gl_TexCoord[0]", "_pd_texcoord0");
  return [
    "precision mediump float;",
    "varying vec4 _pd_front_color;",
    "varying vec4 _pd_texcoord0;",
    body,
  ].join("\n");
}

function expandQuads(vertices) {
  const out = [];
  for (let i = 0; i + 3 < vertices.length; i += 4) {
    out.push(vertices[i], vertices[i + 1], vertices[i + 2], vertices[i + 2], vertices[i + 1], vertices[i + 3]);
  }
  return out;
}
