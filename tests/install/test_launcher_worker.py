"""Schema 1 worker inspect + mock worker + launcher compile."""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
PS = Path(os.environ.get("SystemRoot", r"C:\Windows")) / (
    "System32/WindowsPowerShell/v1.0/powershell.exe"
)
WORKER = REPO / "tools/install/launcher-worker.ps1"
MOCK = REPO / "tools/install/launcher/mock-worker.ps1"
BUILD = REPO / "tools/build/build-launcher.ps1"


@unittest.skipUnless(os.name == "nt" and PS.exists(), "requires Windows PowerShell 5.1")
class LauncherWorkerTests(unittest.TestCase):
    def invoke(self, script, action, exe, decisions=None):
        work = Path(tempfile.mkdtemp(prefix="launcher-worker-"))
        req = work / "request.json"
        events = work / "events.jsonl"
        result = work / "result.json"
        req.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "requestId": "test",
                    "action": action,
                    "gameExe": str(exe),
                    "proxyChoice": "auto",
                    "backendChoice": "auto",
                    "explicitDecisions": decisions or {},
                }
            ),
            encoding="utf-8",
        )
        events.write_text("", encoding="utf-8")
        cmd = [
            str(PS),
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(script),
            "-RequestJson",
            str(req),
            "-EventsJsonl",
            str(events),
            "-ResultJson",
            str(result),
        ]
        p = subprocess.run(cmd, capture_output=True, timeout=120)
        out = (p.stdout or b"").decode("utf-8", "replace")
        err = (p.stderr or b"").decode("utf-8", "replace")
        body = result.read_text(encoding="utf-8") if result.exists() else ""
        return p.returncode, json.loads(body) if body else {}, events.read_text(encoding="utf-8")

    def test_inspect_whoami_copy(self):
        d = Path(tempfile.mkdtemp(prefix="launcher-inspect-"))
        exe = d / "whoami.exe"
        shutil.copy2(Path(os.environ["SystemRoot"]) / "System32/whoami.exe", exe)
        code, result, events = self.invoke(WORKER, "inspect", exe)
        self.assertEqual(code, 0, events)
        self.assertEqual(result["status"], "success")
        self.assertEqual(result["schema"], 1)
        self.assertTrue(result["candidates"])
        self.assertEqual(result["effectiveGameDir"], str(d))
        self.assertNotIn("version.dll", [c["name"] for c in result["candidates"]])
        self.assertNotIn("dinput8.dll", [c["name"] for c in result["candidates"]])

    def test_rejects_relative_path(self):
        work = Path(tempfile.mkdtemp(prefix="launcher-rel-"))
        req = work / "request.json"
        events = work / "events.jsonl"
        result = work / "result.json"
        req.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "requestId": "x",
                    "action": "inspect",
                    "gameExe": "whoami.exe",
                    "proxyChoice": "auto",
                    "backendChoice": "auto",
                }
            ),
            encoding="utf-8",
        )
        events.write_text("", encoding="utf-8")
        p = subprocess.run(
            [
                str(PS),
                "-NoProfile",
                "-NonInteractive",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(WORKER),
                "-RequestJson",
                str(req),
                "-EventsJsonl",
                str(events),
                "-ResultJson",
                str(result),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(p.returncode, 0)

    def test_mock_worker(self):
        d = Path(tempfile.mkdtemp(prefix="launcher-mock-"))
        exe = d / "game.exe"
        exe.write_bytes(b"MZ")
        code, result, events = self.invoke(MOCK, "inspect", exe)
        self.assertEqual(code, 0, events)
        self.assertEqual(result["resolvedProxy"], "dxgi.dll")

    def test_windowsapps_name_maps_to_xboxgames(self):
        xg = Path(r"C:\XboxGames")
        hits = list(xg.glob("**/SandFall-WinGDK-Shipping.exe")) if xg.is_dir() else []
        if not hits:
            self.skipTest("XboxGames shipping exe not present")
        fake = Path(r"C:\Program Files\WindowsApps\DummyPkg\SandFall-WinGDK-Shipping.exe")
        code, result, events = self.invoke(WORKER, "inspect", fake)
        self.assertEqual(code, 0, events + str(result))
        folder = Path(result["effectiveGameDir"])
        self.assertTrue("XboxGames" in folder.parts)
        self.assertEqual(folder, hits[0].parent)

    def test_inspect_path_with_spaces(self):
        d = Path(tempfile.mkdtemp(prefix="launcher space-"))
        folder = d / "Clair Obscur- Expedition 33"
        folder.mkdir()
        exe = folder / "game.exe"
        shutil.copy2(Path(os.environ["SystemRoot"]) / "System32/whoami.exe", exe)
        code, result, events = self.invoke(WORKER, "inspect", exe)
        self.assertEqual(code, 0, events)
        self.assertEqual(Path(result["effectiveGameDir"]), folder)

    def test_foreign_proxy_key_value_array_is_accepted(self):
        d = Path(tempfile.mkdtemp(prefix="launcher-kv-"))
        exe = d / "game.exe"
        shutil.copy2(Path(os.environ["SystemRoot"]) / "System32/whoami.exe", exe)
        (d / "dxgi.dll").write_bytes(b"not-optiscaler")
        code, result, events = self.invoke(
            WORKER,
            "install",
            exe,
            {"foreignProxy": [{"Key": "dxgi.dll", "Value": "backup"}]},
        )
        self.assertNotEqual(code, 2, events + str(result))
        ids = [x.get("id") for x in result.get("requiredDecisions") or []]
        self.assertNotIn("foreignProxy", ids)

    def test_foreign_proxy_needs_input(self):
        d = Path(tempfile.mkdtemp(prefix="launcher-foreign-"))
        exe = d / "game.exe"
        shutil.copy2(Path(os.environ["SystemRoot"]) / "System32/whoami.exe", exe)
        (d / "dxgi.dll").write_bytes(b"not-optiscaler")
        code, result, events = self.invoke(WORKER, "install", exe)
        self.assertEqual(code, 2, events)
        self.assertEqual(result["status"], "needsInput")
        ids = [x["id"] for x in result.get("requiredDecisions") or []]
        self.assertIn("foreignProxy", ids)

    def test_install_update_uninstall_isolated(self):
        pkg = Path(tempfile.mkdtemp(prefix="launcher-pkg-"))
        game = Path(tempfile.mkdtemp(prefix="launcher-game-"))
        shutil.copy2(REPO / "tools/install-amd-presr.ps1", pkg / "Setup.ps1")
        shutil.copy2(REPO / "tools/uninstall-amd-presr.ps1", pkg / "Uninstall_OptiScaler_NR.ps1")
        shutil.copy2(REPO / "tools/lmxxf-module-package.ps1", pkg / "lmxxf-module-package.ps1")
        shutil.copy2(REPO / "tools/install/launcher-worker.ps1", pkg / "launcher-worker.ps1")
        shutil.copy2(REPO / "tools/install/proxy-probe.ps1", pkg / "proxy-probe.ps1")
        shutil.copy2(REPO / "VERSION", pkg / "VERSION")
        fake = b"Harmless installer test data; not an executable."
        digest = hashlib.sha256(fake).hexdigest().upper()
        text, n = re.subn(
            r"(\$expectedA030\s*=\s*)'[A-F0-9]{64}'",
            r"\g<1>'" + digest + "'",
            (pkg / "Setup.ps1").read_text(encoding="utf-8-sig"),
        )
        self.assertEqual(n, 1)
        (pkg / "Setup.ps1").write_bytes(
            b"\xef\xbb\xbf" + text.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8")
        )
        (pkg / "OptiScaler.dll").write_bytes(b"fixture proxy")
        (pkg / "version.dll").write_bytes(fake)
        (pkg / "dlssnr_on_amd_weights.bin").write_bytes(bytes(1024 * 1024))
        (pkg / "OptiScaler.ini").write_text("[DlssNr]\nEnabled=true\n", encoding="utf-8")
        exe = game / "game.exe"
        shutil.copy2(Path(os.environ["SystemRoot"]) / "System32/whoami.exe", exe)
        (game / "plugins").mkdir()
        (game / "plugins" / "user.asi").write_bytes(b"plugin")
        (game / "README.txt").write_text("keep me", encoding="utf-8")
        worker = pkg / "launcher-worker.ps1"

        def go(action):
            return self.invoke(
                worker,
                action,
                exe,
                {"existingIni": "keep", "foreignProxy": {"dxgi.dll": "backup"}},
            )

        code, result, events = go("install")
        self.assertEqual(code, 0, events + str(result))
        self.assertTrue((game / "dxgi.dll").exists() or (game / "amd-presr-install.txt").exists(), result)
        code, result, events = go("update")
        self.assertEqual(code, 0, events + str(result))
        code, result, events = go("uninstall")
        self.assertEqual(code, 0, events + str(result))
        self.assertTrue((game / "plugins" / "user.asi").exists())
        self.assertTrue((game / "README.txt").exists())

    def test_build_launcher_exe(self):
        out = REPO / "exports/launcher"
        p = subprocess.run(
            [
                str(PS),
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(BUILD),
                "-OutDir",
                str(out),
            ],
            capture_output=True,
        )
        text = (p.stdout or b"").decode("utf-8", "replace") + (p.stderr or b"").decode(
            "utf-8", "replace"
        )
        self.assertEqual(p.returncode, 0, text)
        exe = out / "OptScalerNR.Launcher.exe"
        self.assertTrue(exe.is_file())
        self.assertGreater(exe.stat().st_size, 4096)
        stamp = (out / "build.json").read_text(encoding="utf-8")
        self.assertIn("exe", stamp)
