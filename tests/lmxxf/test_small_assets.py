"""Offline Small publication safety; optional real supported pack golden check."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('lmxxf_model', ROOT/'tools/install/lmxxf-model.py')
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)


class SmallAssets(unittest.TestCase):
    def test_rejects_untrusted_pack_before_mutation(self):
        with tempfile.TemporaryDirectory() as tmp:
            base=Path(tmp); pack=base/'bad.bin'; pack.write_bytes(b'NRMODEL1')
            dest=base/'small'; dest.mkdir(); (dest/'keep').write_bytes(b'old')
            with self.assertRaises(ValueError): model.convert(pack,dest,True)
            self.assertEqual((dest/'keep').read_bytes(),b'old')
            self.assertEqual(sorted(p.name for p in base.iterdir()),['bad.bin','small'])

    def test_conversion_and_publication_failures_preserve_old_assets(self):
        with tempfile.TemporaryDirectory() as tmp:
            dest=Path(tmp)/'small'; dest.mkdir(); (dest/'keep').write_bytes(b'old')
            def good(converter): (converter.output/'new').write_bytes(b'new')
            rename=Path.rename
            def fail_publish(path,target):
                if path.parent.name.startswith('.lmxxf-small-'): raise OSError('simulated publication failure')
                return rename(path,target)
            with patch.object(model,'read_pack',return_value={}):
                with patch.object(model.Converter,'run',side_effect=ValueError('hash mismatch')):
                    with self.assertRaises(ValueError):model.convert('unused',dest,True)
                self.assertEqual((dest/'keep').read_bytes(),b'old')
                with patch.object(model.Converter,'run',good),patch.object(Path,'rename',fail_publish):
                    with self.assertRaises(OSError):model.convert('unused',dest,True)
                self.assertEqual((dest/'keep').read_bytes(),b'old')
                with patch.object(model.Converter,'run',good),contextlib.redirect_stdout(io.StringIO()):
                    model.convert('unused',dest,True)
                self.assertEqual((dest/'new').read_bytes(),b'new')
                backups=list(dest.parent.glob('small.previous-*'))
                self.assertEqual(len(backups),1)
                self.assertEqual((backups[0]/'keep').read_bytes(),b'old')

    def test_installed_verification_rejects_corruption(self):
        with tempfile.TemporaryDirectory() as tmp:
            base=Path(tmp); dest=base/'small'; dest.mkdir()
            manifest={'model':'small','source_dll_sha256':'test','files':{'weight.f16':{
                'bytes':4,'sha256':hashlib.sha256(b'good').hexdigest()}}}
            pin=base/'pin.json'; pin.write_text(json.dumps(manifest),encoding='utf-8')
            (dest/'model-manifest.json').write_text(json.dumps(manifest),encoding='utf-8')
            with patch.object(model,'MANIFEST',pin):
                (dest/'weight.f16').write_bytes(b'good'); self.assertTrue(model.verify_assets(dest))
                (dest/'weight.f32').write_bytes(b'shadow'); self.assertFalse(model.verify_assets(dest))
                (dest/'weight.f32').unlink()
                (dest/'weight.f16').write_bytes(b'evil'); self.assertFalse(model.verify_assets(dest))

    @unittest.skipUnless(os.getenv('LMXXF_SMALL_PACK'),'set LMXXF_SMALL_PACK for real 93-file golden conversion')
    def test_supported_pack_matches_all_pinned_weights(self):
        with tempfile.TemporaryDirectory() as tmp,contextlib.redirect_stdout(io.StringIO()):
            dest=model.convert(os.environ['LMXXF_SMALL_PACK'],Path(tmp)/'small')
            self.assertTrue(model.verify_assets(dest))
            self.assertEqual(len(list(dest.iterdir())),94)


if __name__=='__main__':unittest.main(verbosity=2)
