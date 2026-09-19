export const SectionType = Object.freeze({
  HOST: "host",
  VERTEX: "vertex",
  GEOMETRY: "geometry",
  FRAGMENT: "fragment",
});

function isLineStart(text, index) {
  return index === 0 || text[index - 1] === "\n" || text[index - 1] === "\r";
}

function skipLine(text, index) {
  while (index < text.length && text[index] !== "\n") index++;
  return index;
}

function parseDirective(line, previousType) {
  let body = line.trim();
  if (!body.startsWith("@")) return null;
  body = body.slice(1).trim();

  let type = previousType ?? SectionType.HOST;
  if (body.startsWith("vertex_shader")) {
    type = SectionType.VERTEX;
    body = body.slice("vertex_shader".length);
  } else if (body.startsWith("fragment_shader")) {
    type = SectionType.FRAGMENT;
    body = body.slice("fragment_shader".length);
  } else if (body[0] === "h") {
    type = SectionType.HOST;
    body = body.slice(1);
  } else if (body[0] === "v") {
    type = SectionType.VERTEX;
    body = body.slice(1);
  } else if (body[0] === "g") {
    type = SectionType.GEOMETRY;
    body = body.slice(1);
  } else if (body[0] === "f") {
    type = SectionType.FRAGMENT;
    body = body.slice(1);
  }

  const geometry = {};
  if (type === SectionType.GEOMETRY && body.startsWith(",")) {
    const nameStart = body.indexOf(":");
    const meta = (nameStart >= 0 ? body.slice(1, nameStart) : body.slice(1)).split(",");
    geometry.input = meta[0]?.trim() || "GL_TRIANGLES";
    geometry.output = meta[1]?.trim() || "GL_TRIANGLE_STRIP";
    geometry.maxVertices = meta[2] ? Number.parseInt(meta[2], 10) : 8;
    body = nameStart >= 0 ? body.slice(nameStart) : "";
  }

  let name = "";
  const colon = body.indexOf(":");
  if (colon >= 0) {
    name = body.slice(colon + 1).split("//")[0].trim();
  } else {
    const compact = body.trim();
    if (compact && !compact.startsWith("//") && !compact.startsWith("-") && !compact.startsWith("=")) {
      name = compact.split(/\s+/)[0].trim();
    }
  }

  return { type, name, geometry };
}

export function splitSections(text) {
  const sections = [];
  let currentStart = 0;
  let currentType = SectionType.HOST;
  let currentName = "";
  let currentGeometry = {};
  let currentLine = 1;
  let line = 1;
  let inBlockComment = false;

  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    const next = text[i + 1];

    if (inBlockComment) {
      if (ch === "*" && next === "/") {
        inBlockComment = false;
        i++;
      } else if (ch === "\n") {
        line++;
      }
      continue;
    }

    if (ch === "/" && next === "*") {
      inBlockComment = true;
      i++;
      continue;
    }

    if (ch === "/" && next === "/") {
      i = skipLine(text, i + 2);
      if (text[i] === "\n") line++;
      continue;
    }

    if (ch === "\n") {
      line++;
      continue;
    }

    if (ch === "@" && isLineStart(text, i)) {
      const lineEnd = skipLine(text, i);
      const directive = parseDirective(text.slice(i, lineEnd), currentType);
      if (!directive) continue;
      sections.push({
        type: currentType,
        name: currentName,
        start: currentStart,
        end: i,
        line: currentLine,
        text: text.slice(currentStart, i),
        geometry: currentGeometry,
      });
      currentType = directive.type;
      currentName = directive.name;
      currentGeometry = directive.geometry;
      currentStart = lineEnd + (text[lineEnd] === "\n" ? 1 : 0);
      currentLine = line + 1;
      i = lineEnd;
      if (text[i] === "\n") line++;
    }
  }

  sections.push({
    type: currentType,
    name: currentName,
    start: currentStart,
    end: text.length,
    line: currentLine,
    text: text.slice(currentStart),
    geometry: currentGeometry,
  });
  return sections.filter((section, index) => index === 0 || section.text.length || section.name);
}

export function getHostSection(sections) {
  const hosts = sections.filter((section) => section.type === SectionType.HOST);
  return hosts[hosts.length - 1] ?? null;
}
