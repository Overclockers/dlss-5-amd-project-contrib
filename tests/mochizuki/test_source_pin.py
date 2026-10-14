"""The pin checker must reject unrecorded vendor edits and broken patch replay."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('mochizuki_source_pin', ROOT / 'tools/mochizuki-sync/check.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class SourcePinTests(unittest.TestCase):
    def setUp(self):
        (ROOT / 'exports').mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='mochizuki-pin-test-', dir=ROOT / 'exports')
        self.root = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)
        self.upstream = self.root / 'upstream'
        self.repo = self.root / 'product'
        self.upstream.mkdir()
        self.hooks = self.root / 'empty-hooks'
        self.hooks.mkdir()
        subprocess.run(['git', 'init', '-q', str(self.upstream)], check=True)
        self.git('config', 'core.autocrlf', 'false')
        (self.upstream / 'core.cpp').write_bytes(b'original\n')
        self.git('add', 'core.cpp')
        self.git('commit', '-qm', 'fixture')
        pin = self.git('rev-parse', 'HEAD').strip()
        (self.repo / 'third_party/mochizuki').mkdir(parents=True)
        (self.repo / 'third_party/mochizuki/core.cpp').write_bytes(b'extended\r\n')
        config = self.repo / 'tools/mochizuki-sync'
        config.mkdir(parents=True)
        self.patch = config / 'feature.patch'
        self.patch.write_text('--- a/core.cpp\n+++ b/core.cpp\n@@ -1 +1 @@\n-original\n+extended\n', encoding='utf-8', newline='\n')
        self.manifest = config / 'manifest.json'
        self.manifest.write_text(json.dumps({'schema': 1, 'commit': pin, 'vendor': 'third_party/mochizuki',
            'files': ['core.cpp'], 'text_extensions': ['.cpp'],
            'patches': [{'path': 'feature.patch'}]}), encoding='utf-8')

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.upstream), '-c', 'user.name=Fixture',
            '-c', 'user.email=fixture@example.invalid', '-c', 'core.hooksPath=' + str(self.hooks),
            *args], text=True)

    def test_fixed_commit_and_patch_reproduce_vendor_despite_dirty_upstream(self):
        (self.upstream / 'core.cpp').write_bytes(b'not the pin\n')
        checker.verify(self.repo, self.upstream, self.manifest)

    def test_unrecorded_vendor_edit_is_rejected(self):
        (self.repo / 'third_party/mochizuki/core.cpp').write_bytes(b'uncaptured edit\n')
        with self.assertRaisesRegex(ValueError, 'Unrecorded vendor'):
            checker.verify(self.repo, self.upstream, self.manifest)

    def test_new_vendor_dependency_is_rejected(self):
        (self.repo / 'third_party/mochizuki/unreviewed.hpp').write_bytes(b'new dependency\n')
        with self.assertRaisesRegex(ValueError, 'Closure inventory'):
            checker.verify(self.repo, self.upstream, self.manifest)

    def test_patch_that_does_not_apply_is_rejected(self):
        self.patch.write_text(self.patch.read_text().replace('-original', '-wrong base'), encoding='utf-8', newline='\n')
        with self.assertRaises(subprocess.CalledProcessError):
            checker.verify(self.repo, self.upstream, self.manifest)

    def test_patch_added_file_is_accepted_only_when_declared(self):
        (self.repo / 'third_party/mochizuki/added.comp').write_bytes(b'new pass\r\n')
        added = self.repo / 'tools/mochizuki-sync/added.patch'
        added.write_text('--- /dev/null\n+++ b/added.comp\n@@ -0,0 +1 @@\n+new pass\n', encoding='utf-8', newline='\n')
        manifest = json.loads(self.manifest.read_text(encoding='utf-8'))
        manifest['text_extensions'].append('.comp')
        manifest['patches'].append({'path': 'added.patch', 'files': ['added.comp']})
        with self.assertRaisesRegex(ValueError, 'Closure inventory'):
            checker.verify(self.repo, self.upstream, self.manifest_with(manifest))
        manifest['added'] = ['added.comp']
        checker.verify(self.repo, self.upstream, self.manifest_with(manifest))
        manifest['patches'][-1]['files'] = []
        with self.assertRaisesRegex(ValueError, 'not declared'):
            checker.verify(self.repo, self.upstream, self.manifest_with(manifest))

    def manifest_with(self, manifest):
        self.manifest.write_text(json.dumps(manifest), encoding='utf-8')
        return self.manifest


if __name__ == '__main__':
    unittest.main()
