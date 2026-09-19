#!/usr/bin/env python3
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_ppm.py <file.ppm>", file=sys.stderr)
        return 2
    data = Path(sys.argv[1]).read_bytes()
    parts = data.split(b"\n", 3)
    if len(parts) != 4 or parts[0] != b"P6":
        print("not a binary PPM", file=sys.stderr)
        return 1
    width, height = map(int, parts[1].split())
    if parts[2] != b"255":
        print("unexpected max value", file=sys.stderr)
        return 1
    body = parts[3]
    if len(body) != width * height * 3:
        print("unexpected pixel payload size", file=sys.stderr)
        return 1
    if min(body) == max(body):
        print("image is a flat color", file=sys.stderr)
        return 1
    print(f"ok {width}x{height} bytes={len(body)} min={min(body)} max={max(body)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
