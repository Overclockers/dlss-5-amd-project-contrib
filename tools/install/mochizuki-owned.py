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
    'runtime_transfer_store', 'runtime_pack_u8', 'runtime_pack_f16', 'runtime_unpack_f16',
    'runtime_pack_r11', 'runtime_pack_u8_bgra', 'runtime_pack_u10', 'runtime_pack_u10_bgra',
    # Motion-vector rescale on compute (NrInlineComputeMotion), one module per motion format.
    'runtime_mv_rescale_rg16f', 'runtime_mv_rescale_rg32f', 'runtime_mv_rescale_rgba16f', 'runtime_mv_rescale_rgba32f',
    # Plan A's colour-format conversion, both directions. The unconvert pass is one module per destination
    # format (PLAN_A_DST_FORMAT is a compile-time storage-image qualifier), and so is the
    # convert pass; ConvertPass::Pipeline::ConvertModule/UnconvertModule name the same set, and a
    # format missing from either side only means that colour format keeps its own path.
    'runtime_planA_convert_r11g11b10', 'runtime_planA_convert_rgba16f',
    'runtime_planA_unconvert_r11g11b10', 'runtime_planA_unconvert_rgba16f',
)
MOTION = ('motion_luma', 'motion_estimate')


def grade_paths(table, prefix):
    grade = table['style_grade']
    return ([f'{prefix}/g_{name}_grade.spv' for name in grade['pipelines']] +
            [f'{prefix}/temporal/{name}_grade.spv' for name in grade['variants']])


def shader_paths(table_path, prefix):
    table = json.loads(table_path.read_text(encoding='utf-8'))
    paths = [f'{prefix}/g_{name}.spv' for name in table['pipelines']]
    paths += [f'{prefix}/{name}' for name in table['markers']]
    paths += [f'{prefix}/temporal/{name}.spv' for name in table['variants']]
    paths += grade_paths(table, prefix)
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
    table = json.loads((vendor / 'linux/shaders/rdna4/pipelines.json').read_text())
    int4 = json.loads((vendor / 'linux/shaders/rdna4/pipelines-int4.json').read_text())
    for prefix in ('aco/int4/shaders', 'int4/shaders'):
        names = set(table['pipelines']) | set(int4['new'])
        paths += [f'{prefix}/g_{name}.spv' for name in names]
        paths += [f'{prefix}/{name}' for name in table['markers']]
        paths += [f'{prefix}/g_{name}.spv.iu4' for name in int4['iu4']]
        paths += [f'{prefix}/temporal/{name}.spv' for name in list(table['variants']) + list(MOTION)]
        paths += grade_paths(table, prefix)
        paths += [f'{prefix}/temporal/shader-constants.txt', f'{prefix}/hs-pipelines.txt']
    paths += [f'aco/int4/records/{p.name}' for p in (vendor / 'windows/data/aco/int4/records').iterdir() if p.is_file()]
    paths.append('aco/int4/shell-aliases.txt')
    data = vendor / 'linux/data/int4/rdna4'
    paths += ['int4/' + p.relative_to(data).as_posix() for directory in ('vit', 'swin')
              for p in (data / directory).rglob('*') if p.is_file()]
    paths.append('int4/settings.txt')
    renodx = vendor / 'windows/shaders/renodx'
    paths += ['shaders/renodx/renodx_' + p.name.removesuffix('.cs_5_1.hlsl') + '.spv'
              for p in renodx.glob('*.cs_5_1.hlsl')]
    paths += ['shaders/renodx/renodx_prep.spv', 'shaders/renodx/renodx_prep_encode.spv', 'shaders/renodx/RenoDX-LICENSE.txt']
    unique = sorted(set(paths))
    if len(unique) != len(paths):
        raise SystemExit('Mochizuki ownership pin has a duplicate path')
    if any('*' in path or '?' in path or path.startswith('/') or '\\' in path for path in unique):
        raise SystemExit('Mochizuki ownership pin is not a list of exact relative paths')
    records_only = [path for path in unique if path.startswith('aco/records/')]
    if len(records_only) != 62:
        raise SystemExit(f'expected 62 pinned ACO records, found {len(records_only)}')
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
