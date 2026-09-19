import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import { splitSections, SectionType, getHostSection } from "../src/parser/sections.js";

test("splits basic pss sections", () => {
  const text = fs.readFileSync(new URL("../../shared/fixtures/host_basic.pss", import.meta.url), "utf8");
  const sections = splitSections(text);
  assert.equal(sections.length, 3);
  assert.equal(sections[0].type, SectionType.HOST);
  assert.equal(sections[1].type, SectionType.VERTEX);
  assert.equal(sections[1].name, "basic");
  assert.equal(sections[2].type, SectionType.FRAGMENT);
  assert.equal(getHostSection(sections), sections[0]);
});

test("supports geometry metadata", () => {
  const sections = splitSections("@v:v\nv\n@g,GL_TRIANGLES,GL_TRIANGLE_STRIP,12:g\ng\n@f:f\nf\n");
  const geometry = sections.find((section) => section.type === SectionType.GEOMETRY);
  assert.equal(geometry.name, "g");
  assert.equal(geometry.geometry.input, "GL_TRIANGLES");
  assert.equal(geometry.geometry.output, "GL_TRIANGLE_STRIP");
  assert.equal(geometry.geometry.maxVertices, 12);
});
