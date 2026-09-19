import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import { fileURLToPath } from "node:url";
import { splitSections, getHostSection } from "../src/parser/sections.js";
import { runHost } from "../src/runtime/interpreter.js";
import { compileHost } from "../src/runtime/interpreter.js";
import { TraceRuntime } from "../src/runtime/runtime.js";
import { makeTextureImageLoader } from "../src/cli/image-loader.js";

function runFixture(name) {
  const text = fs.readFileSync(new URL(`../../shared/fixtures/${name}`, import.meta.url), "utf8");
  const host = getHostSection(splitSections(text));
  const runtime = new TraceRuntime();
  return runHost(host.text, runtime);
}

test("runs static arrays and loops", () => {
  const { value, runtime } = runFixture("host_basic.pss");
  assert.equal(value, 14);
  assert.equal(runtime.output, "14.000000\n");
});

test("runs user functions and goto", () => {
  const { value, runtime } = runFixture("functions_goto.pss");
  assert.equal(value, 15);
  assert.equal(runtime.output, "15.000000\n");
});

test("reads updated reference parameters inside user functions", () => {
  const program = compileHost("mut(&x){ x = 2; y = x + 3; return y; } a = 0; return mut(&a) + a;");
  const runtime = new TraceRuntime();
  assert.equal(program.run(runtime).value, 7);
});

test("preserves statics across frames", () => {
  const program = compileHost("static x; x += 1; return x;");
  const runtime = new TraceRuntime();
  assert.equal(program.run(runtime).value, 1);
  assert.equal(program.run(runtime).value, 2);
});

test("preserves global enums across frames", () => {
  const program = compileHost("enum { N = 2 } static x[N]; x[0] = x[0] + 1; return N + x[0];");
  const runtime = new TraceRuntime();
  assert.equal(program.run(runtime).value, 3);
  assert.equal(program.run(runtime).value, 4);
});

test("scopes function statics separately from main statics", () => {
  const program = compileHost("(){ static px=2; return f()+px; } f(){ static px[4]; px[1]=5; return px[1]; }");
  const runtime = new TraceRuntime();
  assert.equal(program.run(runtime).value, 7);
});

test("assignments can shadow enum constants with locals", () => {
  const program = compileHost("enum {N=65536}; f(){ n=min(13,N); return n; } return f();");
  const runtime = new TraceRuntime();
  assert.equal(program.run(runtime).value, 13);
});

test("single index accesses multidimensional arrays linearly", () => {
  const program = compileHost("static a[2][3]; a[4]=9; return a[1][1];");
  const runtime = new TraceRuntime();
  assert.equal(program.run(runtime).value, 9);
});

test("records modernizable GL batches", () => {
  const program = compileHost("glsetshader(\"v\",\"f\"); glcolor(0.25,0.5,0.75); glquad(1);");
  const runtime = new TraceRuntime();
  program.run(runtime);
  assert.equal(runtime.batches.length, 1);
  assert.equal(runtime.batches[0].vertices.length, 4);
  assert.deepEqual(runtime.batches[0].shader, { vertex: "v", geometry: "-", fragment: "f" });
  assert.deepEqual(runtime.batches[0].vertices[0].color, [0.25, 0.5, 0.75, 1]);
});

test("records uniforms on GL batches", () => {
  const program = compileHost('glsetshader("v","f"); glUniform3f(glgetuniformloc("tint"),0.125,0.5,0.875); glquad(1);');
  const runtime = new TraceRuntime();
  program.run(runtime);
  assert.equal(runtime.batches.length, 1);
  assert.deepEqual(runtime.batches[0].uniforms.find((u) => u.name === "tint"), {
    name: "tint",
    type: "3f",
    count: 1,
    values: [0.125, 0.5, 0.875],
  });
});

test("records array uniforms addressed by location offsets", () => {
  const program = compileHost('loc=glgetuniformloc("env"); glUniform4f(loc,1,2,3,4); glUniform4f(loc+1,5,6,7,8); glquad(1);');
  const runtime = new TraceRuntime();
  program.run(runtime);
  assert.deepEqual(runtime.batches[0].uniforms.find((u) => u.name === "env[1]"), {
    name: "env[1]",
    type: "4f",
    count: 1,
    values: [5, 6, 7, 8],
  });
});

test("records texture bindings on GL batches", () => {
  const program = compileHost("glactivetexture(GL_TEXTURE0+1); glbindtexture(3); glquad(1);");
  const runtime = new TraceRuntime();
  program.run(runtime);
  assert.equal(runtime.batches.length, 1);
  assert.deepEqual(runtime.batches[0].textures, [
    { unit: 0, textureId: 0 },
    { unit: 1, textureId: 3 },
  ]);
});

test("glgettex copies texture snapshots back to arrays", () => {
  const program = compileHost("static src[4]={0,0.25,0.5,1}, dst[4]; glsettex(7,src,2,2,KGL_FLOAT); glgettex(7,dst,2,2,KGL_FLOAT); return dst[2];");
  const runtime = new TraceRuntime();
  assert.equal(program.run(runtime).value, 0.5);
});

test("glgettex can read file texture snapshots", () => {
  const fixtureDir = new URL("../../shared/fixtures/", import.meta.url);
  const loader = makeTextureImageLoader((texturePath) => fileURLToPath(new URL(texturePath, fixtureDir)));
  const program = compileHost('static dst[4]; glsettex(8,"checker.ppm",KGL_NEAREST); glgettex(8,dst,2,2,KGL_BGRA32+KGL_NEAREST); return dst[0];');
  const runtime = new TraceRuntime({ loadTextureImage: loader });
  assert.notEqual(program.run(runtime).value, 0);
  assert.equal(runtime.textureSnapshots.get(8).width, 2);
  assert.equal(runtime.textureSnapshots.get(8).height, 2);
});

test("captureend creates a texture snapshot", () => {
  const program = compileHost("glcapture(9,4,4,KGL_FLOAT); glcolor(0.25,0,0); glquad(1); glcaptureend(9); glactivetexture(GL_TEXTURE0); glbindtexture(9); glquad(1);");
  const runtime = new TraceRuntime();
  program.run(runtime);
  assert.equal(runtime.textureSnapshots.has(9), true);
  assert.equal(runtime.textureSnapshots.get(9).width, 4);
  assert.equal(runtime.batches.at(-1).textures[0].textureId, 9);
});

test("captureend binds low texture ids to matching sampler units", () => {
  const program = compileHost("glcapture(); glquad(1); glcaptureend(0); glcapture(); glquad(1); glcaptureend(1); glquad(1);");
  const runtime = new TraceRuntime();
  program.run(runtime);
  assert.deepEqual(runtime.batches.at(-1).textures.map((t) => [t.unit, t.textureId]), [[0, 0], [1, 1]]);
});

test("records MVP uniforms on batches", () => {
  const program = compileHost("glscale(0.5,0.5,1); glquad(1);");
  const runtime = new TraceRuntime();
  program.run(runtime);
  const mvp = runtime.batches[0].uniforms.find((u) => u.name === "_pd_mvp");
  assert.equal(mvp.type, "mat4");
  assert.equal(mvp.values[0], 0.5);
  assert.equal(mvp.values[5], 0.5);
});

test("matrix compatibility calls clamp non-finite inputs", () => {
  const program = compileHost("glscale(1/0,1/0,1); gltranslate(0/0,2,3); glrotate(0/0,1,0,0); glquad(1);");
  const runtime = new TraceRuntime();
  program.run(runtime);
  const mvp = runtime.batches[0].uniforms.find((u) => u.name === "_pd_mvp");
  assert.equal(mvp.type, "mat4");
  assert.equal(mvp.values.every(Number.isFinite), true);
});

test("beginFrame resets transient GL matrix state", () => {
  const program = compileHost("glscale(0.5,0.5,1); glquad(1);");
  const runtime = new TraceRuntime();
  program.run(runtime);
  runtime.beginFrame();
  program.run(runtime);
  const mvp = runtime.batches[0].uniforms.find((u) => u.name === "_pd_mvp");
  assert.equal(mvp.values[0], 0.5);
  assert.equal(mvp.values[5], 0.5);
});

test("setfov and glulookat update compatibility matrices", () => {
  const program = compileHost("setfov(60); glulookat(0,0,10, 0,0,0, 0,1,0); glquad(1);");
  const runtime = new TraceRuntime({ xres: 640, yres: 480 });
  program.run(runtime);
  const mvp = runtime.batches[0].uniforms.find((u) => u.name === "_pd_mvp");
  const modelview = runtime.batches[0].uniforms.find((u) => u.name === "_pd_modelview");
  assert.equal(mvp.type, "mat4");
  assert.equal(mvp.values.every(Number.isFinite), true);
  assert.equal(modelview.values[14], -10);
});
