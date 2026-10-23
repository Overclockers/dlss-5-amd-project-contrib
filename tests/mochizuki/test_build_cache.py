"""Validate reuse against actual source/artifact changes, never just file dates."""
import importlib.util
import os
from pathlib import Path
import tempfile
import shutil
import subprocess
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('manifest', ROOT / 'tools/build/mochizuki-manifest.py')
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)


class BuildCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.patch = patch.object(manifest, 'ROOT', self.root); self.patch.start(); self.addCleanup(self.patch.stop)
        self.source = self.root / 'third_party/mochizuki/windows/test.cpp'
        product = 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/'
        paths = [self.source, *(self.root / (product + p) for p in (
            'dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h', 'dlssnr/NrPerformance.h', 'library/vulkan/vulkan-1.lib')),
            *(self.root / 'tools/build' / f for f in ('build-mochizuki-runtime.py',
              'build-mochizuki-runtime.cmd', 'mochizuki-deps.py', 'mochizuki-manifest.py', 'mochizuki-aco.py', 'mochizuki-renodx.py'))]
        paths.append(self.root / 'VERSION')
        for p in paths:
            p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(b'fixture source')
        self.out = self.root / 'exports/mochizuki-runtime'
        self.shader = self.out / 'dlssnr-amd/shaders/test.spv'
        self.shader.parent.mkdir(parents=True); self.shader.write_bytes(b'fixture shader')
        self.dll = self.out / 'MochizukiNrRuntime.dll'; self.dll.write_bytes(b'fixture DLL')
        manifest.write(self.out)

    def test_identical_build_reuses(self):
        self.assertTrue(manifest.reusable(self.out))

    def test_int4_assets_invalidate_but_driver_cache_does_not(self):
        settings = self.out / 'dlssnr-amd/int4/settings.txt'
        settings.parent.mkdir(parents=True)
        settings.write_text('NR_HS_FOLD=1\n')
        self.assertFalse(manifest.reusable(self.out))
        manifest.write(self.out)
        (settings.parent / 'pipeline.cache').write_bytes(b'driver-specific')
        binaries = settings.parent / 'pipeline-binaries'
        binaries.mkdir()
        (binaries / 'driver.nrb').write_bytes(b'local')
        self.assertTrue(manifest.reusable(self.out))
        settings.write_text('NR_HS_FOLD=0\n')
        self.assertFalse(manifest.reusable(self.out))

    def test_source_addition_change_and_toolchain_invalidate(self):
        self.source.write_bytes(b'changed'); self.assertFalse(manifest.reusable(self.out))
        self.source.write_bytes(b'fixture source')
        with patch.dict(os.environ, {'VCToolsVersion': 'different'}):
            self.assertFalse(manifest.reusable(self.out))
        new = self.source.with_name('new.h'); new.write_bytes(b'new include')
        self.assertFalse(manifest.reusable(self.out))

    def test_version_and_commit_invalidate_identity(self):
        (self.root / 'VERSION').write_text('1.13.4\n')
        self.assertFalse(manifest.reusable(self.out))
        manifest.write(self.out)
        self.assertEqual(manifest.build_identity()['version'], '1.13.4')
        with patch.object(manifest.subprocess, 'run') as git:
            git.return_value.stdout = 'a' * 40 + '\n'
            manifest.write(self.out)
            self.assertTrue(manifest.reusable(self.out))
            git.return_value.stdout = 'b' * 40 + '\n'
            self.assertFalse(manifest.reusable(self.out))

    def test_log_identity_has_independent_content_hash_without_git(self):
        with patch.object(manifest.subprocess, 'run', side_effect=FileNotFoundError):
            original = manifest.build_identity()
            self.assertEqual(original['commit'], 'unknown')
            self.source.write_bytes(b'local edit')
            changed = manifest.build_identity()
            self.assertNotEqual(original['source_sha256'], changed['source_sha256'])
            self.assertEqual(original['version'], changed['version'])

    def test_windows005_cache_requires_generator_and_renodx_assets(self):
        for relative in ('model-tools/dlssnr-int4-weights.exe', 'model-tools/dlssnr-int4.sha256',
                         'dlssnr-amd/shaders/renodx/renodx_v6_encode.spv',
                         'dlssnr-amd/aco/int4/records/fixture.nrp'):
            path = self.out / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'original')
            manifest.write(self.out)
            path.unlink()
            self.assertFalse(manifest.reusable(self.out), relative)
            path.write_bytes(b'changed')
            self.assertFalse(manifest.reusable(self.out), relative)

    def test_linux_int4_input_and_renodx_source_invalidate(self):
        for relative in ('third_party/mochizuki/linux/data/int4/rdna4/settings.txt',
                         'third_party/mochizuki/windows/shaders/renodx/fixture.hlsl'):
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'original')
            manifest.write(self.out)
            path.write_bytes(b'changed')
            self.assertFalse(manifest.reusable(self.out), relative)

    def test_missing_or_changed_shader_and_dll_invalidate(self):
        self.shader.unlink(); self.assertFalse(manifest.reusable(self.out))
        self.shader.write_bytes(b'changed shader'); self.assertFalse(manifest.reusable(self.out))
        self.shader.write_bytes(b'fixture shader'); self.dll.write_bytes(b'changed DLL')
        self.assertFalse(manifest.reusable(self.out))

    def test_missing_corrupt_or_legacy_manifest_rebuilds(self):
        path = self.out / 'build-manifest.json'
        for content in ('{broken', '{}', '[]'):
            path.write_text(content); self.assertFalse(manifest.reusable(self.out))
        path.unlink(); self.assertFalse(manifest.reusable(self.out))


@unittest.skipUnless(os.name == 'nt' and shutil.which('powershell') and shutil.which('git'),
                     'Windows PowerShell and Git identity generator')
class HostIdentityTests(unittest.TestCase):
    def test_commit_version_and_source_refresh_without_timestamp_churn(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            product = root / 'host/OptiScaler'
            product.mkdir(parents=True)
            source = product / 'startup.cpp'
            source.write_text('// original\n')
            version = root / 'VERSION'
            version.write_text('1.13.3\n')
            header = root / 'obj/amd_graphics_source_id.h'
            def stamp():
                subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                                str(ROOT / 'tools/build/write-amd-graphics-source-id.ps1'),
                                '-ProjectDirectory', str(product), '-OutputPath', str(header)],
                               check=True, capture_output=True)
                return (product / 'resource_build_commit.h').read_text()
            def git(*args):
                return subprocess.run(['git', '-C', str(root), *args], check=True,
                                      capture_output=True, text=True).stdout.strip()
            git('init', '-q')
            git('add', '.')
            git('-c', 'user.name=Identity Fixture', '-c', 'user.email=identity@example.invalid',
                '-c', 'commit.gpgsign=false', 'commit', '-qm', 'fixture')
            first = stamp()
            self.assertIn(git('rev-parse', 'HEAD'), first)
            files = [header, product / 'resource_build_commit.h', product / 'resource_build_date.h']
            times = [p.stat().st_mtime_ns for p in files]
            self.assertEqual(first, stamp())
            self.assertEqual(times, [p.stat().st_mtime_ns for p in files])
            source.write_text('// local edit\n')
            changed = stamp()
            self.assertNotEqual(first, changed)
            version.write_text('1.13.4\n')
            versioned = stamp()
            self.assertNotEqual(changed, versioned)
            git('add', 'VERSION', 'host/OptiScaler/startup.cpp')
            git('-c', 'user.name=Identity Fixture', '-c', 'user.email=identity@example.invalid',
                '-c', 'commit.gpgsign=false', 'commit', '-qm', 'next identity')
            self.assertNotEqual(versioned, stamp())


if __name__ == '__main__':
    unittest.main()
