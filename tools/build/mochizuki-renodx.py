"""Build the pinned RenoDX Vulkan shaders with native Windows Slang and glslang."""
from pathlib import Path
import subprocess

KERNELS = ('v6_linearize v6_autoscale v6_exposure_scale v6_feed_transition v6_encode v6_resolve '
           'v6_pedestal_reduce v6_pedestal_reduce_unshaped v6_commit v6_commit_exposure '
           'v6_resolve_commit legacy_encode legacy_decode legacy_reduce legacy_reduce_unshaped '
           'legacy_apply look_compose look_upsample look_band').split()


def build(upstream, out, scratch, slang, glslang):
    source = upstream / 'windows/shaders/renodx'
    prep = upstream / 'windows/shaders/renodx-prep'
    # Keep upstream's preprocessing math drift check on Windows too.
    original = (upstream / 'windows/shaders/passes/runtime_prep.comp').read_text()
    expected = original[original.index('// ---- the curves, scene-linear'):original.index('void main() {')]
    actual = (prep / 'prep_math.glsl').read_text().split('// >>> runtime_prep.comp\n', 1)[1].split('// <<< runtime_prep.comp', 1)[0]
    if expected != actual:
        raise ValueError('RenoDX preprocess math differs from runtime_prep.comp')
    out.mkdir(parents=True, exist_ok=True)
    scratch.mkdir(parents=True, exist_ok=True)
    for kernel in KERNELS:
        wrapper = scratch / ('vk_' + kernel + '.comp.slang')
        wrapper.write_text('#define RENODX_VULKAN 1\n#include "' + kernel + '.cs_5_1.hlsl"\n', encoding='utf8')
        shifts = ['-fvk-t-shift', '0', 'all', '-fvk-u-shift', '4', 'all'] if kernel.startswith('legacy_reduce') or kernel == 'legacy_apply' else []
        subprocess.run([str(slang), str(wrapper), '-I', str(source), '-target', 'spirv', '-profile', 'glsl_460',
                        '-entry', 'main', '-stage', 'compute', '-O2', *shifts, '-o', str(out / ('renodx_' + kernel + '.spv'))], check=True)
    for kernel in ('renodx_prep', 'renodx_prep_encode'):
        subprocess.run([str(glslang), '-V', '--target-env', 'vulkan1.3', '-I' + str(prep),
                        str(prep / (kernel + '.comp')), '-o', str(out / (kernel + '.spv'))], check=True)
    (out / 'RenoDX-LICENSE.txt').write_bytes((source / 'LICENSE').read_bytes())
