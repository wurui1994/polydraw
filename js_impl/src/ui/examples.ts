// Inlined example pss sources for the UI playground (browser cannot read the
// filesystem, so these are bundled). Mirrors examples/opengl/*.pss.

export const EXAMPLES: Record<string, string> = {
  '03_point.pss': `// Translated from basic/point.c — a single point at the origin.
glColor(1, 0, 0);
glPointSize(9);
glBegin(GL_POINTS);
glVertex(0, 0, -3);
glEnd();
`,

  '12_mesh_surface.pss': `// Wireframe saddle z = x*x - y*y (mesh.c). Each cell is a closed GL_LINE_STRIP
// because pss has no GL_LINE_LOOP / glPolygonMode (see NOT_SUPPORTED.md).
setfov(45);
glTranslate(0, 0, -7);
glRotate(45.0, 1, 0, 0);
glRotate(klock() * 100.0, 0, 0, 1);   // ~100 deg/sec about Z

glColor(0, 1, 1);

M = 30;
N = 30;
dx = 2.0 / M;
dy = 2.0 / N;
sx0 = -1.0;
sy0 = -1.0;
for (i = 0; i < M; i = i + 1) {
    sx = sx0 + i * dx;
    for (j = 0; j < N; j = j + 1) {
        sy = sy0 + j * dy;
        x0 = sx;       y0 = sy;       z0 = x0*x0 - y0*y0;
        x1 = sx + dx;  y1 = sy;       z1 = x1*x1 - y1*y1;
        x2 = sx + dx;  y2 = sy + dy;  z2 = x2*x2 - y2*y2;
        x3 = sx;       y3 = sy + dy;  z3 = x3*x3 - y3*y3;
        glBegin(GL_LINE_STRIP);
        glVertex(x0, y0, z0);
        glVertex(x1, y1, z1);
        glVertex(x2, y2, z2);
        glVertex(x3, y3, z3);
        glVertex(x0, y0, z0);
        glEnd();
    }
}
`,

  '26_texture_procedural.pss': `// Procedural checkerboard texture mapped onto a quad (picture/texture.c). pss has
// no file/image loading, so we generate the pattern in a static buffer and
// upload it with glsettex(id, buf, w, h, KGL_BGRA32).
enum { SIZ = 64 }; static buf[SIZ][SIZ];
for (y = 0; y < SIZ; y++) for (x = 0; x < SIZ; x++) {
    cx = int(x / 8) % 2;
    cy = int(y / 8) % 2;
    if (cx == cy) buf[y][x] = 0x40404040 + 0x00e0e000;
    else          buf[y][x] = 0x40000000 + 0x009000d0;
}
glsettex(0, &buf, SIZ, SIZ, KGL_BGRA32);

setfov(45);
glTranslate(0, 0, -3);

glactivetexture(GL_TEXTURE0 + 0);
glbindtexture(0);
glBegin(GL_QUADS);
glTexCoord(0, 0); glVertex(-0.9, -0.9, 0);
glTexCoord(1, 0); glVertex( 0.9, -0.9, 0);
glTexCoord(1, 1); glVertex( 0.9,  0.9, 0);
glTexCoord(0, 1); glVertex(-0.9,  0.9, 0);
glEnd();
`,

  '28_peaks.pss': `// Peaks function surface (MATLAB peaks) as a wireframe grid. fun() is a plain
// user-defined function and can be replaced with any height field.
fun(x, y) {
    z = 3 * (1 - x) * (1 - x) * exp(-x * x - (y + 1) * (y + 1))
        - 10 * (x / 5 - x * x * x - y * y * y * y * y) * exp(-x * x - y * y)
        - 1.0 / 3 * exp(-(x + 1) * (x + 1) - y * y);
    return z / 3.0;
}

setfov(45);
glTranslate(0, 0, -9);
glRotate(45.0, 1, 0, 0);
glRotate(klock() * 40.0, 0, 0, 1);

M = 40;
N = 40;
dx = 6.0 / M;
dy = 6.0 / N;
sx0 = -3.0;
sy0 = -3.0;
for (i = 0; i < M; i = i + 1) {
    sx = sx0 + i * dx;
    for (j = 0; j < N; j = j + 1) {
        sy = sy0 + j * dy;
        x0 = sx;       y0 = sy;       z0 = fun(x0, y0);
        x1 = sx + dx;  y1 = sy;       z1 = fun(x1, y1);
        x2 = sx + dx;  y2 = sy + dy;  z2 = fun(x2, y2);
        x3 = sx;       y3 = sy + dy;  z3 = fun(x3, y3);
        glColor(0.5 + 0.18 * z0, 0.5, 0.6 - 0.18 * z0, 1);
        glBegin(GL_LINE_STRIP);
        glVertex(x0, y0, z0);
        glVertex(x1, y1, z1);
        glVertex(x2, y2, z2);
        glVertex(x3, y3, z3);
        glVertex(x0, y0, z0);
        glEnd();
    }
}
`,
};
