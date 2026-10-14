"""Small model-converter fixture; exercises the real installer/hash/commit path.

This does not emulate neural weights. Real graph conversion is covered by the
Mochizuki model/GPU verification against the upstream model hash.
"""
from pathlib import Path
import atexit
import hashlib
import os
import shutil
import subprocess
import tempfile

PAYLOAD = b'Mochizuki installer INT4 fixture'
_temporary = None


def int4_tools(destination):
    global _temporary
    if _temporary is None:
        _temporary = tempfile.TemporaryDirectory(prefix='mochi-converter-fixture-')
        atexit.register(_temporary.cleanup)
        root = Path(_temporary.name)
        source = root / 'converter.cs'
        source.write_text('''using System;
using System.IO;
class Converter {
    static int Main(string[] args) {
        if (args.Length != 3 || !File.Exists(args[0]) ||
            !File.Exists(Path.Combine(args[1], "settings.txt"))) return 2;
        var input = File.ReadAllBytes(args[0]);
        if (input.Length < 12 || System.Text.Encoding.ASCII.GetString(input, 0, 8) != "NRMODEL1") return 3;
        File.WriteAllText(args[2], "Mochizuki installer INT4 fixture", new System.Text.UTF8Encoding(false));
        return 0;
    }
}''', encoding='utf-8')
        compiler = Path(os.environ['SystemRoot']) / 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
        subprocess.run([str(compiler), '/nologo', '/target:exe',
                        '/out:' + str(root / 'converter.exe'), str(source)], check=True, capture_output=True)
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(Path(_temporary.name) / 'converter.exe', destination / 'dlssnr-int4-weights.exe')
    (destination / 'dlssnr-int4.sha256').write_text(hashlib.sha256(PAYLOAD).hexdigest() + '\n', encoding='ascii')
