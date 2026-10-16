"""Pinned dependency cache reuse without network, across exports directories."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch, MagicMock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('mochizuki_deps', ROOT / 'tools/build/mochizuki-deps.py')
deps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deps)


class DependencyCacheTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / 'work/scratch'
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = patch.object(deps.Path, 'home', return_value=self.root / 'shared')
        self.env.start()
        self.addCleanup(self.env.stop)
        self.data = b'pinned archive fixture'
        self.sha = hashlib.sha256(self.data).hexdigest()
        self.url = 'https://example.invalid/tool/1.0/tool.zip'

    def test_existing_workspace_archive_seeds_shared_cache_then_survives_removal(self):
        archive = self.root / 'checkout/exports/tool.zip'
        archive.parent.mkdir(parents=True)
        archive.write_bytes(self.data)
        with patch.object(deps.urllib.request, 'urlopen') as network:
            deps.fetch(self.url, archive, self.sha)
            archive.unlink()
            other = self.root / 'other-worktree/exports/tool.zip'
            deps.fetch(self.url, other, self.sha)
            network.assert_not_called()
        self.assertEqual(other.read_bytes(), self.data)

    def test_download_once_then_reuse(self):
        response = MagicMock()
        response.__enter__.return_value.read.return_value = self.data
        with patch.object(deps.urllib.request, 'urlopen', return_value=response) as network:
            deps.fetch(self.url, self.root / 'one/tool.zip', self.sha)
            deps.fetch(self.url, self.root / 'two/tool.zip', self.sha)
            self.assertEqual(network.call_count, 1)

    def test_corrupt_shared_archive_is_rejected_without_installing(self):
        shared = deps.shared_archive(self.url, 'tool.zip')
        shared.parent.mkdir(parents=True)
        shared.write_bytes(b'corrupt')
        target = self.root / 'exports/tool.zip'
        with patch.object(deps.urllib.request, 'urlopen') as network:
            with self.assertRaisesRegex(RuntimeError, 'SHA256 mismatch'):
                deps.fetch(self.url, target, self.sha)
            network.assert_not_called()
        self.assertFalse(target.exists())

    def test_failed_download_does_not_publish_partial_archive(self):
        response = MagicMock()
        response.__enter__.return_value.read.side_effect = TimeoutError('fixture timeout')
        target = self.root / 'exports/tool.zip'
        with patch.object(deps.urllib.request, 'urlopen', return_value=response):
            with self.assertRaisesRegex(RuntimeError, 'download failed'):
                deps.fetch(self.url, target, self.sha)
        self.assertFalse(target.exists())
        self.assertFalse(deps.shared_archive(self.url, target.name).exists())

    def test_wrong_download_hash_is_never_published(self):
        response = MagicMock()
        response.__enter__.return_value.read.return_value = b'wrong version'
        target = self.root / 'exports/tool.zip'
        with patch.object(deps.urllib.request, 'urlopen', return_value=response):
            with self.assertRaisesRegex(RuntimeError, 'SHA256 mismatch'):
                deps.fetch(self.url, target, self.sha)
        self.assertFalse(target.exists())
        self.assertFalse(deps.shared_archive(self.url, target.name).exists())

    def test_different_pins_do_not_share_archives(self):
        self.assertNotEqual(deps.shared_archive(self.url, 'tool.zip'),
                            deps.shared_archive(self.url.replace('/1.0/', '/2.0/'), 'tool.zip'))


if __name__ == '__main__':
    unittest.main()
