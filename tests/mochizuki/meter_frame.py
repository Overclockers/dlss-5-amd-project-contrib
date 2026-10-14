"""Writes the frame the meter test measures: a uniform linear image of a known level.

A uniform frame is the one case where the level the meter should derive is arithmetic rather
than a judgement: it averages a constant, so the exposure it reports must be 0.2073/level for
any level inside its range, whatever the model makes of the picture afterwards.
"""
import struct
import sys

LEVEL = 0.05
WIDTH = 256
HEIGHT = 256


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "exports/mochizuki-tests/meter-in.f32"
    count = WIDTH * HEIGHT * 3
    with open(path, "wb") as handle:
        handle.write(struct.pack("<%df" % count, *([LEVEL] * count)))
    print("METER_FRAME_OK %s %dx%d level=%g" % (path, WIDTH, HEIGHT, LEVEL))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
