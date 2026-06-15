#!/usr/bin/env python3
"""Probe: can vispy render offscreen and hand back pixels?
Minimal test — draw a red triangle, read it back, check it's not all black.
This validates the headless GL path before building the full renderer."""
import sys
import numpy as np
from vispy import app, gloo

app.use_app('pyglet')  # pyglet is installed; glfw is not

vertex = """
attribute vec2 a_position;
attribute vec3 a_color;
varying vec3 v_color;
void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_color = a_color;
}
"""
fragment = """
varying vec3 v_color;
void main() {
    gl_FragColor = vec4(v_color, 1.0);
}
"""

class Canvas(app.Canvas):
    def __init__(self, size):
        app.Canvas.__init__(self, size=size, show=False, autoswap=False)
        self.program = gloo.Program(vertex, fragment, count=3)
        self.program['a_position'] = np.array([[-0.6,-0.6],[0.6,-0.6],[0.0,0.7]], dtype=np.float32)
        self.program['a_color'] = np.array([[1,0,0],[0,1,0],[0,0,1]], dtype=np.float32)

    def on_draw(self, event):
        gloo.set_clear_color((0,0,0,1))
        gloo.set_viewport(0, 0, *self.physical_size)
        self.program.draw('triangles')

def main():
    c = Canvas((128, 128))
    img = c.render()  # (h,w,4) ubyte
    print(f"render() returned shape={img.shape} dtype={img.dtype}")
    # there should be some non-black pixels (the triangle)
    nonblack = np.count_nonzero(img[..., :3].sum(axis=2))
    total = img.shape[0] * img.shape[1]
    print(f"non-black pixels: {nonblack}/{total}")
    # check center pixel is reddish (triangle covers center)
    h, w = img.shape[:2]
    px = img[h//3, w//2]
    print(f"center-ish pixel (should be red-ish): {px}")
    if nonblack > 0:
        print("PASS: offscreen rendering works")
        sys.exit(0)
    else:
        print("FAIL: image is all black")
        sys.exit(1)

if __name__ == "__main__":
    main()
