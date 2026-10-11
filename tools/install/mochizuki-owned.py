"""Exact dlssnr-amd paths this package owns.

The list is the Windows and Linux RDNA4 build outputs plus the pinned ACO
records. Names come from those sources one by one. A filename that merely
looks similar is not owned.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PIN = Path(__file__).with_name('mochizuki-owned.txt')
RUNTIME = (
    'runtime_alpha', 'runtime_encode', 'runtime_transfer', 'runtime_upscale', 'runtime_prep',
    'runtime_depth', 'cascade_lograt', 'cascade_blur', 'cascade_feed', 'runtime_downscale', 'runtime_taps',
    'runtime_transfer_store',
)
MOTION = ('motion_luma', 'motion_estimate')


def shader_paths(table_path, prefix):
    table = json.loads(table_path.read_text(encoding='utf-8'))
    paths = [f'{prefix}/g_{name}.spv' for name in table['pipelines']]
    paths += [f'{prefix}/{name}' for name in table['markers']]
    paths += [f'{prefix}/temporal/{name}.spv' for name in table['variants']]
    paths += [f'{prefix}/temporal/{name}.spv' for name in MOTION]
    paths.append(f'{prefix}/temporal/shader-constants.txt')
    paths += [f'{prefix}/runtime/{name}.spv' for name in RUNTIME]
    paths.append(f'{prefix}/runtime/FidelityFX-FSR1-LICENSE.txt')
    return paths


def owned_paths():
    vendor = ROOT / 'third_party/mochizuki'
    paths = shader_paths(vendor / 'windows/shaders/rdna4/pipelines.json', 'shaders')
    paths += shader_paths(vendor / 'linux/shaders/rdna4/pipelines.json', 'aco/shaders')
    records = vendor / 'windows/data/aco/records'
    paths += [f'aco/records/{path.name}' for path in records.iterdir() if path.is_file()]
    paths.append('aco/shell-aliases.txt')
    unique = sorted(set(paths))
    if len(unique) != len(paths):
        raise SystemExit('Mochizuki ownership pin has a duplicate path')
    if any('*' in path or '?' in path or path.startswith('/') or '\\' in path for path in unique):
        raise SystemExit('Mochizuki ownership pin is not a list of exact relative paths')
    records_only = [path for path in unique if path.startswith('aco/records/')]
    if len(records_only) != 58:
        raise SystemExit(f'expected 58 pinned ACO records, found {len(records_only)}')
    return unique


def main():
    paths = owned_paths()
    text = ''.join(path + '\n' for path in paths)
    if len(sys.argv) > 1 and sys.argv[1] == '--check':
        current = PIN.read_text(encoding='utf-8')
        if current != text:
            raise SystemExit('tools/install/mochizuki-owned.txt does not match the RDNA4 pin')
        print(f'MOCHIZUKI_OWNED_PIN_OK {len(paths)}')
        return
    if len(sys.argv) > 1 and sys.argv[1] == '--write':
        PIN.write_text(text, encoding='utf-8', newline='\n')
        print(f'wrote {PIN} ({len(paths)} paths)')
        return
    sys.stdout.write(text)


if __name__ == '__main__':
    main()
