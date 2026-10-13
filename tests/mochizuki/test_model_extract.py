"""Installation and analysis share the PE/weight parser, with distinct outputs."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
VENDOR = ROOT / 'third_party/mochizuki/linux/package/model-tools'
spec = importlib.util.spec_from_file_location('inspect_nr', VENDOR / 'inspect_nr.py')
inspect = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inspect)


def fixture(name='block0.layer0.layer', broken_weights=False):
    payload = b'opaque weight bytes'
    body_size = 40 + len(payload)
    body = struct.pack('<QQI', body_size, len(payload), 0) + payload
    body += struct.pack('<IIQI', 1, 2, 1, len(payload))
    record = struct.pack('<Q', len(name)) + name.encode() + struct.pack('<Q', body_size) + body
    weights = struct.pack('<Q', 8 + len(record) + int(broken_weights)) + record
    tree = bytearray(128) + weights
    struct.pack_into('<HHII', tree, 12, 1, 0, 0x80000020, 0x40)
    label = 'WEIGHTS_HT'.encode('utf-16le')
    struct.pack_into('<H', tree, 0x20, len(label) // 2)
    tree[0x22:0x22 + len(label)] = label
    struct.pack_into('<IIII', tree, 0x40, 0x1080, len(weights), 0, 0)
    data = bytearray(512) + tree
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HH', data, 0x84, 0x8664, 1)
    struct.pack_into('<H', data, 0x94, 240)
    struct.pack_into('<H', data, 0x98, 0x20b)
    struct.pack_into('<II', data, 0x98 + 128, 0x1000, len(tree))
    data[0x188:0x190] = b'.rsrc\0\0\0'
    struct.pack_into('<IIII', data, 0x190, len(tree), 0x1000, len(tree), 512)
    # A minimal CUDA ELF with a string table, so analysis exports a real cubin.
    elf = bytearray(144)
    elf[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<H', elf, 18, 190)
    struct.pack_into('<Q', elf, 40, 80)
    struct.pack_into('<HHH', elf, 58, 64, 1, 0)
    struct.pack_into('<IIQQQQIIQQ', elf, 80, 0, 3, 0, 0, 64, 1, 0, 0, 1, 0)
    return bytes(data + elf), weights, payload


class ModelExtractionTests(unittest.TestCase):
    def setUp(self):
        (ROOT / 'exports').mkdir(exist_ok=True)
        temp = tempfile.TemporaryDirectory(prefix='model-test-', dir=ROOT / 'exports')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.source = self.root / 'nvngx_dlssnr.dll'
        self.data, self.weights, self.payload = fixture()
        self.source.write_bytes(self.data)

    def run_mode(self, mode, output, source=None):
        argv = ['inspect_nr.py', str(source or self.source), '--output', str(output), *mode]
        with patch.object(sys, 'argv', argv), contextlib.redirect_stdout(io.StringIO()) as stdout:
            inspect.main()
        return json.loads(stdout.getvalue())

    def test_install_skips_scanner_and_copies_for_dll_and_zip(self):
        archive = self.root / 'source.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr('nested/nvngx_dlssnr.dll', self.data)
        for source in (self.source, archive):
            output = self.root / source.suffix[1:]
            with patch.object(inspect, 'cuda_elfs', side_effect=AssertionError('CUDA scan forbidden')):
                summary = self.run_mode(['--extract-weights'], output, source)
            self.assertEqual(summary['dll_sha256'], inspect.sha256(self.data))
            self.assertIsNone(summary['cuda_modules'])
            self.assertEqual({p.relative_to(output).as_posix() for p in output.rglob('*') if p.is_file()},
                             {'manifest.json', 'weights_ht.bin', 'weights/block0.layer0.layer.bin'})
            self.assertEqual((output / 'weights_ht.bin').read_bytes(), self.weights)
            self.assertEqual((output / 'weights/block0.layer0.layer.bin').read_bytes(), self.payload)
            report = json.loads((output / 'manifest.json').read_text())
            self.assertTrue(report['cuda_inventory_skipped'])

    def test_analysis_default_and_full_extraction_unchanged(self):
        for mode, folder in (([], 'inventory'), (['--extract'], 'full')):
            output = self.root / folder
            summary = self.run_mode(mode, output)
            self.assertEqual(summary['cuda_modules'], 1)
            report = json.loads((output / 'manifest.json').read_text())
            self.assertNotIn('cuda_inventory_skipped', report)
        self.assertEqual((self.root / 'full/nvngx_dlssnr.dll').read_bytes(), self.data)
        self.assertEqual(len(list((self.root / 'full/cubins').glob('*.cubin'))), 1)
        self.assertFalse((self.root / 'inventory/weights').exists())

    def test_irrelevant_broken_cuda_is_not_parsed_at_install(self):
        self.source.write_bytes(self.data + b'\x7fELF\x02\x01')
        self.run_mode(['--extract-weights'], self.root / 'install')
        with self.assertRaises(inspect.FormatError):
            self.run_mode(['--extract'], self.root / 'analysis')

    def test_weight_validation_and_path_safety_preserved(self):
        for args in ({'broken_weights': True}, {'name': '../outside'}):
            self.source.write_bytes(fixture(**args)[0])
            for mode in ('--extract', '--extract-weights'):
                with self.assertRaises(inspect.FormatError):
                    self.run_mode([mode], self.root / 'bad')
        self.assertFalse((self.root / 'outside.bin').exists())

    def test_installer_rejects_source_hash_without_replacing_destination(self):
        # This must reach the source hash check, not fail in the unused CUDA
        # scanner: exercise the installer's actual subprocess mode selection.
        self.source.write_bytes(self.data + b'\x7fELF\x02\x01')
        destination = self.root / 'dlssnr.bin'
        destination.write_bytes(b'existing model')
        result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/install/mochizuki-model.py'),
                                 str(self.source), str(destination), '--work', str(self.root)],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('SHA256 mismatch', result.stderr)
        self.assertEqual(destination.read_bytes(), b'existing model')
        self.assertFalse(list(self.root.glob('mochizuki-model-*')))

    def test_modes_are_mutually_exclusive(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            self.run_mode(['--extract', '--extract-weights'], self.root / 'bad')
        self.assertEqual(error.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
