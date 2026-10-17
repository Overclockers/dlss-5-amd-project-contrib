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


def fixture(name='block0.layer0.layer', broken_weights=False, resource='WEIGHTS_HT'):
    payload = b'opaque weight bytes'
    body_size = 40 + len(payload)
    body = struct.pack('<QQI', body_size, len(payload), 0) + payload
    body += struct.pack('<IIQI', 1, 2, 1, len(payload))
    record = struct.pack('<Q', len(name)) + name.encode() + struct.pack('<Q', body_size) + body
    weights = struct.pack('<Q', 8 + len(record) + int(broken_weights)) + record
    tree = bytearray(160) + weights
    struct.pack_into('<HHII', tree, 12, 1, 0, 0x80000020, 0x60)
    label = resource.encode('utf-16le')
    struct.pack_into('<H', tree, 0x20, len(label) // 2)
    tree[0x22:0x22 + len(label)] = label
    struct.pack_into('<IIII', tree, 0x60, 0x10a0, len(weights), 0, 0)
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

    def test_small_resource_selection_reads_only_its_weights(self):
        self.source.write_bytes(fixture(resource='WEIGHTS_CC_SMALL_FP8')[0])
        output = self.root / 'small'
        summary = self.run_mode(['--extract-weights', '--resource', 'WEIGHTS_CC_SMALL_FP8'], output)
        self.assertIsNone(summary['cuda_modules'])
        self.assertEqual((output / 'weights/block0.layer0.layer.bin').read_bytes(), self.payload)
        with self.assertRaises(inspect.FormatError):
            self.run_mode(['--extract-weights'], self.root / 'wrong-resource')

    def test_converter_selects_both_models_and_validates_before_publish(self):
        spec = importlib.util.spec_from_file_location('mochi_model', ROOT / 'tools/install/mochizuki-model.py')
        model = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(model)
        destination = self.root / 'dlssnr.bin'
        small = self.root / 'dlssnr-small.bin'
        calls = []

        def converter(command, **kwargs):
            calls.append(command)
            if Path(command[3]).name == 'pack_model.py':
                target = Path(command[command.index('--out') + 1])
                if fail_small and target.name == 'dlssnr-small.bin':
                    raise subprocess.CalledProcessError(1, command)
                target.write_bytes(target.name.encode())
            return subprocess.CompletedProcess(command, 0, json.dumps({'dll_sha256': version}))

        version, fail_small = model.DLL_3109, False
        with patch.object(model.subprocess, 'run', side_effect=converter):
            model.extract(self.source, destination, VENDOR, self.root)
        self.assertEqual(destination.read_bytes(), b'dlssnr.bin')
        self.assertEqual(small.read_bytes(), b'dlssnr-small.bin')
        resources = [c[c.index('--resource') + 1] for c in calls if '--resource' in c]
        self.assertEqual(resources, ['WEIGHTS_HT', 'WEIGHTS_CC_SMALL_FP8'])
        packs = [c for c in calls if Path(c[3]).name == 'pack_model.py']
        self.assertEqual([Path(c[c.index('--verify') + 1]).name for c in packs],
                         ['model-files.sha256', 'model-files-small.sha256'])
        destination.write_bytes(b'keep original'); small.write_bytes(b'keep new')
        fail_small = True
        with patch.object(model.subprocess, 'run', side_effect=converter), self.assertRaises(subprocess.CalledProcessError):
            model.extract(self.source, destination, VENDOR, self.root)
        self.assertEqual(destination.read_bytes(), b'keep original')
        self.assertEqual(small.read_bytes(), b'keep new')
        calls.clear(); version, fail_small = model.DLL_3108, False
        with patch.object(model.subprocess, 'run', side_effect=converter):
            model.extract(self.source, destination, VENDOR, models='original', temporary_parent=self.root)
        self.assertEqual(small.read_bytes(), b'keep new')
        self.assertEqual([c[c.index('--resource') + 1] for c in calls if '--resource' in c], ['WEIGHTS_HT'])

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
                                 str(self.source), str(destination), '--models', 'both', '--work', str(self.root)],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('SHA256 mismatch', result.stderr)
        self.assertEqual(destination.read_bytes(), b'existing model')
        self.assertFalse(list(self.root.glob('mochizuki-model-*')))

    def test_modes_are_mutually_exclusive(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            self.run_mode(['--extract', '--extract-weights'], self.root / 'bad')
        self.assertEqual(error.exception.code, 2)

    def test_models_argument_required_for_cli(self):
        destination = self.root / 'dlssnr.bin'
        result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/install/mochizuki-model.py'),
                                 str(self.source), str(destination), '--work', str(self.root)],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('--models', result.stderr)

    def test_single_model_selection_and_legacy_refusal(self):
        spec = importlib.util.spec_from_file_location('mochi_model', ROOT / 'tools/install/mochizuki-model.py')
        model = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(model)
        destination = self.root / 'dlssnr.bin'
        small = self.root / 'dlssnr-small.bin'
        calls = []

        def converter(command, **kwargs):
            calls.append(command)
            if Path(command[3]).name == 'pack_model.py':
                target = Path(command[command.index('--out') + 1])
                target.write_bytes(target.name.encode())
            return subprocess.CompletedProcess(command, 0, json.dumps({'dll_sha256': version}))

        # Small only with 310.9.16
        version = model.DLL_3109
        with patch.object(model.subprocess, 'run', side_effect=converter):
            model.extract(self.source, destination, VENDOR, models='small', temporary_parent=self.root)
        self.assertFalse(destination.exists())
        self.assertEqual(small.read_bytes(), b'dlssnr-small.bin')
        resources = [c[c.index('--resource') + 1] for c in calls if '--resource' in c]
        self.assertEqual(resources, ['WEIGHTS_CC_SMALL_FP8'])
        small.unlink()

        # Original only with 310.9.16
        calls.clear()
        with patch.object(model.subprocess, 'run', side_effect=converter):
            model.extract(self.source, destination, VENDOR, models='original', temporary_parent=self.root)
        self.assertEqual(destination.read_bytes(), b'dlssnr.bin')
        self.assertFalse(small.exists())
        resources = [c[c.index('--resource') + 1] for c in calls if '--resource' in c]
        self.assertEqual(resources, ['WEIGHTS_HT'])

        # 310.8.0 refuses small or both with English explanation
        version = model.DLL_3108
        with patch.object(model.subprocess, 'run', side_effect=converter), self.assertRaises(ValueError) as err:
            model.extract(self.source, destination, VENDOR, models='small', temporary_parent=self.root)
        self.assertIn('cannot provide the new model', str(err.exception))

    def test_zip_candidate_selection_and_safety(self):
        # 1. Multiple duplicate identical candidates in ZIP should be deduplicated
        archive = self.root / 'dup.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr('a/nvngx_dlssnr.dll', self.data)
            z.writestr('b/nvngx_dlssnr.dll', self.data)
        out = self.root / 'dup_out'
        summary = self.run_mode(['--extract-weights'], out, archive)
        self.assertEqual(summary['dll_sha256'], inspect.sha256(self.data))

        # 2. Path traversal in ZIP must be rejected
        bad_archive = self.root / 'bad_traversal.zip'
        with zipfile.ZipFile(bad_archive, 'w') as z:
            z.writestr('../nvngx_dlssnr.dll', self.data)
        with self.assertRaises(inspect.FormatError) as err:
            self.run_mode(['--extract-weights'], self.root / 'bad_out', bad_archive)
        self.assertIn('traversal', str(err.exception).lower())

        # 3. Multiple distinct DLL candidates in ZIP: selects the one providing requested resource
        mixed_archive = self.root / 'mixed.zip'
        small_data = fixture(resource='WEIGHTS_CC_SMALL_FP8')[0]
        ht_data = self.data
        with zipfile.ZipFile(mixed_archive, 'w') as z:
            z.writestr('old/nvngx_dlssnr.dll', ht_data)
            z.writestr('new/nvngx_dlssnr.dll', small_data)
        out_small = self.root / 'mixed_small'
        summary = self.run_mode(['--extract-weights', '--resource', 'WEIGHTS_CC_SMALL_FP8'], out_small, mixed_archive)
        self.assertEqual(summary['dll_sha256'], inspect.sha256(small_data))

    def test_model_pack_verification(self):
        spec = importlib.util.spec_from_file_location('mochi_model', ROOT / 'tools/install/mochizuki-model.py')
        model = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(model)

        # Corrupted / invalid model
        corrupted = self.root / 'corrupted.bin'
        corrupted.write_bytes(b'garbage data')
        self.assertFalse(model.verify_model_pack(corrupted))

        # Truncated header
        truncated = self.root / 'truncated.bin'
        truncated.write_bytes(b'NRMODEL1')
        self.assertFalse(model.verify_model_pack(truncated))

        # Invalid entry count
        invalid_count = self.root / 'invalid_count.bin'
        invalid_count.write_bytes(b'NRMODEL1' + struct.pack('<II', 123, 0))
        self.assertFalse(model.verify_model_pack(invalid_count))


if __name__ == '__main__':
    unittest.main()
