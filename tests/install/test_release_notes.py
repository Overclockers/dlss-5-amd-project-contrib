"""Execute the workflow's actual release-notes preparation in isolated checkouts."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap
import unittest
import yaml

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
        doc = yaml.safe_load((REPO / '.github/workflows/release.yml').read_text(encoding='utf-8-sig'))
        build_steps = doc['jobs']['build']['steps']
        step = next(s for s in build_steps if s.get('name') == 'Prepare release notes')
        script = step['run']
        self.script = self.root / 'step.ps1'
        self.template = script
        self.script.write_text(script, encoding='utf-8-sig')
        self.output = self.root / 'exports/release-notes.md'

    def set_lines(self, items):
        items_ps = ', '.join(f"'{x}'" for x in items)
        # Replace $lines = @(...)
        prefix, rest = self.template.split('$lines = @(', 1)
        _, suffix = rest.split(')', 1)
        new_script = prefix + f'$lines = @({items_ps})' + suffix
        self.script.write_text(new_script, encoding='utf-8-sig')

    def run_step(self, version='9.9.9-alpha.2'):
        env = dict(os.environ, RELEASE_VERSION=version, SOURCE_SHA='a' * 40,
                   SOURCE_REPO='owner/repo')
        return subprocess.run([PS, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(self.script)],
                              cwd=self.root, env=env, capture_output=True, timeout=20)

    def test_release_notes_exact_two_lines(self):
        result = self.run_step('1.13.0-alpha')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = self.output.read_text(encoding='utf-8').strip().splitlines()
        self.assertEqual(len(lines), 2)
        self.assertEqual(lines[0], 'Mochizuki后端适配DLSS 10.16最新模型。')
        self.assertEqual(lines[1], 'The Mochizuki backend now supports the latest DLSS 10.16 model.')

    def test_empty_or_wrong_line_count_fails_before_output(self):
        for items in ([], ['Only one line'], ['Line 1', 'Line 2', 'Line 3']):
            with self.subTest(items=items):
                self.set_lines(items)
                self.assertNotEqual(self.run_step().returncode, 0)


if __name__ == '__main__':
    unittest.main()
