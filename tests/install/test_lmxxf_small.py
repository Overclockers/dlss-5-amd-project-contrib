"""Exercise source and extracted-package helper paths without a proprietary model."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
PS=shutil.which('powershell.exe')


@unittest.skipUnless(PS,'Windows PowerShell required')
class SmallInstaller(unittest.TestCase):
    def test_bad_pack_preserves_existing_assets_in_both_layouts(self):
        for packaged in (False,True):
            with self.subTest(packaged=packaged),tempfile.TemporaryDirectory(prefix='lmxxf small ') as tmp:
                base=Path(tmp); dest=base/'weights'/'small'; dest.mkdir(parents=True)
                (dest/'keep').write_bytes(b'old'); pack=base/'bad.bin';pack.write_bytes(b'bad')
                helper=ROOT/'tools/install/lmxxf-small.ps1'
                if packaged:
                    for name in ('lmxxf-small.ps1','lmxxf-model.py','lmxxf-small-manifest.json'):
                        shutil.copy2(ROOT/'tools/install'/name,base/name)
                    helper=base/'lmxxf-small.ps1'
                script=base/'invoke.ps1'
                script.write_text("param($Helper,$Pack,$Output,$Python)\n$ErrorActionPreference='Stop'\n. $Helper\nInitialize-LmxxfSmall -Pack $Pack -Output $Output -Python $Python\n",encoding='utf-8-sig')
                r=subprocess.run([PS,'-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',str(script),str(helper),str(pack),str(dest),sys.executable],capture_output=True)
                self.assertNotEqual(r.returncode,0)
                self.assertIn(b'previous model assets were preserved',r.stderr)
                self.assertEqual((dest/'keep').read_bytes(),b'old')
                self.assertEqual(list(dest.parent.iterdir()),[dest])


if __name__=='__main__':unittest.main(verbosity=2)
