"""Record launcher startup. Targets are p95<=1s warm / <=2s first; not a pass gate if slower."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
EXE = REPO / "exports/launcher/OptScalerNR.Launcher.exe"


@unittest.skipUnless(os.name == "nt" and EXE.is_file(), "requires built launcher")
class LauncherStartupTests(unittest.TestCase):
    def measure_once(self):
        output = REPO / "exports/tests/launcher-startup"
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="startup-", dir=output) as fixture:
            log = Path(fixture) / "t.txt"
            env = os.environ.copy()
            env["OPTSCALER_STARTUP_LOG"] = str(log)
            env["LOCALAPPDATA"] = str(Path(fixture) / "profile")
            p = subprocess.run(
                [str(EXE), "--startup-measure"],
                env=env,
                capture_output=True,
                timeout=30,
            )
            self.assertEqual(p.returncode, 0, p.stderr)
            text = log.read_text(encoding="utf-8")
            self.assertIn("interactiveMs=", text)
            return float(text.split("interactiveMs=")[1].split()[0])

    def test_ten_warm_starts_recorded(self):
        first = self.measure_once()
        warm = [self.measure_once() for _ in range(10)]
        out = REPO / "exports/launcher/startup-times.txt"
        out.parent.mkdir(parents=True, exist_ok=True)
        lines = ["first=%.1f" % first] + ["warm=%.1f" % x for x in warm]
        p95 = sorted(warm)[max(0, int(round(0.95 * (len(warm) - 1))))]
        lines.append("warm_p95=%.1f" % p95)
        out.write_text("\n".join(lines) + "\n", encoding="utf-8")
        # Record only. Slow machines must not hide stalls by skipping the log.
        self.assertGreaterEqual(first, 0)
        self.assertEqual(len(warm), 10)
