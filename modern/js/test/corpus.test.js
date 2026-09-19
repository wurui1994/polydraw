import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { splitSections, getHostSection } from "../src/parser/sections.js";
import { parseProgram } from "../src/parser/parser.js";

const root = path.resolve(new URL("../../..", import.meta.url).pathname);
const samples = [
  "ken/balls.pss",
  "ken/texture.pss",
  "ken/gpgpu.pss",
  "ken/geo_test.pss",
  "ken/drawcone2.pss",
  "tigrou/fractal.pss",
];

for (const sample of samples) {
  test(`parses host corpus ${sample}`, () => {
    const text = fs.readFileSync(path.join(root, sample), "utf8");
    const host = getHostSection(splitSections(text));
    assert.ok(host);
    const ast = parseProgram(host.text);
    assert.equal(ast.kind, "Program");
  });
}
