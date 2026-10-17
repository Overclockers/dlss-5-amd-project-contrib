"""Package entry: Setup.bat no-args launches the GUI EXE; missing EXE is a full-package error."""
from pathlib import Path
import re
import unittest

REPO = Path(__file__).resolve().parents[2]
PKG = REPO / "tools/release/PACKAGE_RELEASE.ps1"


class LauncherPackageTests(unittest.TestCase):
    def test_setup_bat_template_opens_gui_without_args(self):
        text = PKG.read_text(encoding="utf-8-sig")
        self.assertIn("OptScalerNR.Launcher.exe", text)
        self.assertIn("if \"%~1\"==\"\"", text)
        self.assertIn("Complete package required: OptScalerNR.Launcher.exe is missing", text)
        self.assertIn("launcher-worker.ps1", text)
        self.assertIn("proxy-probe.ps1", text)
        self.assertIn("build-launcher.ps1", text)
        body = re.search(
            r"title OptScaler\(NR\) @VERSION@ - Setup\n(.*)\n'@", text, re.S
        )
        self.assertIsNotNone(body)
        bat = body.group(1)
        self.assertIn("start \"\" \"%~dp0OptScalerNR.Launcher.exe\"", bat)
        self.assertIn("Setup.ps1", bat)

    def test_required_zip_names_include_launcher(self):
        text = PKG.read_text(encoding="utf-8-sig")
        self.assertIn("'OptScalerNR.Launcher.exe'", text)
        self.assertIn("'launcher-worker.ps1'", text)
        self.assertIn("'proxy-probe.ps1'", text)
