"""GPU integration: a missing or corrupt ACO bundle still completes one native frame."""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
assets = ROOT / 'exports/mochizuki-runtime'
test = ROOT / 'exports/mochizuki-tests/runtime.exe'
work = ROOT / 'exports/mochizuki-tests'
work.mkdir(parents=True, exist_ok=True)


def run(fixture, label):
    result = subprocess.run([str(test), str(assets / 'MochizukiNrRuntime.dll'), str(fixture), '--aco-fallback'],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding='utf-8', errors='replace', timeout=300)
    if result.returncode:
        raise SystemExit(label + '\n' + result.stdout)
    print(label)
    print(result.stdout, end='' if result.stdout.endswith('\n') else '\n')


def plant(name):
    fixture = work / name
    if fixture.exists():
        shutil.rmtree(fixture)
    fixture.mkdir(parents=True)
    data = fixture / 'dlssnr-amd'
    shutil.copytree(assets / 'dlssnr-amd/shaders', data / 'shaders')
    os.link(assets / 'dlssnr-amd/dlssnr.bin', data / 'dlssnr.bin')
    return fixture


missing = plant('aco-fallback-missing')
run(missing, 'ACO_FALLBACK_CASE missing bundle')

aliases = plant('aco-fallback-aliases')
shutil.copytree(assets / 'dlssnr-amd/aco', aliases / 'dlssnr-amd/aco')
(aliases / 'dlssnr-amd/aco/shell-aliases.txt').unlink()
run(aliases, 'ACO_FALLBACK_CASE missing aliases')

corrupt = plant('aco-fallback-corrupt')
shutil.copytree(assets / 'dlssnr-amd/aco', corrupt / 'dlssnr-amd/aco')
for record in (corrupt / 'dlssnr-amd/aco/records').glob('*.nrp'):
    record.write_bytes(b'corrupt test record')
run(corrupt, 'ACO_FALLBACK_CASE corrupt binary record')
