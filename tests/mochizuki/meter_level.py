"""Checks the levels `run.cmd meter` printed: the meter has to put the frame on the key luminance.

`IMAGE_METER` reports the white point the runtime used, which with the white point argument left
at 1.0 is 1/exposure. For a uniform 0.05 frame the meter's exposure is 0.2073/0.05, so the white
point settles at 0.2412 - the value that makes the network see 0.2073. The first frames still
carry the warm-up blend, so the assertion reads the tail of the run.
"""
import re
import sys

LEVEL = 0.05
KEY_LUMINANCE = 0.2073
EXPECTED = LEVEL / KEY_LUMINANCE
TAIL = 5
TOLERANCE = 0.03


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "exports/mochizuki-tests/meter.log"
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    values = [float(value) for value in re.findall(r"IMAGE_METER frame=\d+ white=([0-9.]+)", text)]
    if not values:
        print("METER_LEVEL_FAIL no IMAGE_METER lines in %s" % path)
        print("METER_LEVEL_LOG_TAIL " + text[-1500:].replace("\n", " | "))
        return 1
    tail = values[-TAIL:]
    worst = max(abs(value - EXPECTED) / EXPECTED for value in tail)
    print("METER_LEVEL frames=%d expected=%.4f tail=%s"
          % (len(values), EXPECTED, " ".join("%.4f" % value for value in tail)))
    if worst > TOLERANCE:
        print("METER_LEVEL_FAIL the metered level is %.1f%% off the key luminance" % (worst * 100.0))
        return 1
    print("METER_LEVEL_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
