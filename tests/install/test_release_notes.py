"""Execute the workflow's actual release-notes preparation in isolated checkouts."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap
import unittest

REPO = Path(__file__).resolve().parents[2]
PS = shutil.which('powershell.exe')


@unittest.skipUnless(PS, 'Windows PowerShell required')
class ReleaseNotesTests(unittest.TestCase):
    def setUp(self):
        scratch = REPO / 'work/scratch'
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='release-notes-', dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        workflow = (REPO / '.github/workflows/release.yml').read_text(encoding='utf-8-sig')
        step = workflow.split('      - name: Prepare versioned release notes\n', 1)[1].split('\n      - name:', 1)[0]
        script = textwrap.dedent(step.split('        run: |\n', 1)[1])
        self.script = self.root / 'step.ps1'
        self.script.write_text(script, encoding='utf-8-sig')
        self.notes = self.root / 'docs/release-notes/9.9.9-alpha.2.md'
        self.notes.parent.mkdir(parents=True)
        self.output = self.root / 'exports/release-notes.md'

    def run_step(self):
        env = dict(os.environ, RELEASE_VERSION='9.9.9-alpha.2', SOURCE_SHA='a' * 40,
                   SOURCE_REPO='owner/repo')
        return subprocess.run([PS, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(self.script)],
                              cwd=self.root, env=env, capture_output=True, timeout=20)

    def test_future_version_and_source_link(self):
        self.notes.write_text('### v9.9.9-alpha.2\n\n中文更新\n', encoding='utf-8')
        result = self.run_step()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        body = self.output.read_text(encoding='utf-8')
        self.assertIn('中文更新', body)
        self.assertIn('https://github.com/owner/repo/tree/' + 'a' * 40, body)
        self.assertNotIn('${{', body)

    def test_missing_file_fails_before_output(self):
        self.assertNotEqual(self.run_step().returncode, 0)
        self.assertFalse(self.output.exists())

    def test_empty_or_wrong_version_fails_before_output(self):
        for body in ('', '### v1.12.0-alpha\n', 'No version heading\n',
                     '### v9.9.9-alpha.2\n\n### v1.12.0-alpha\n'):
            with self.subTest(body=body):
                self.notes.write_text(body, encoding='utf-8')
                self.assertNotEqual(self.run_step().returncode, 0)
                self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
