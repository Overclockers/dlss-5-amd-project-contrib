"""Build the Mochizuki adapter and upstream shaders with the repository toolchain."""
import argparse
import hashlib
import json
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import sys
import runpy

repo = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('mochizuki_deps', Path(__file__).with_name('mochizuki-deps.py'))
deps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deps)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('out', nargs='?', default=None)
    parser.add_argument('--skip-shaders', action='store_true', help='reuse already built shaders for local adapter iteration')
    parser.add_argument('--reuse', action='store_true', help='reuse only matching source, toolchain and artifact hashes')
    parser.add_argument('--test-full-barriers', action='store_true',
                        help='build a separate test-only DLL with all inter-dispatch barriers retained')
    args = parser.parse_args()
    manifest = runpy.run_path(str(Path(__file__).with_name('mochizuki-manifest.py')))
    source_hashes = manifest['sources']()
    test_out = repo / 'exports/mochizuki-test-reference'
    out = (repo / args.out).resolve() if args.out else (test_out if args.test_full_barriers else repo / 'exports/mochizuki-runtime')
    if not out.is_relative_to((repo / 'exports').resolve()):
        parser.error('Build output must be under this workspace exports directory')
    if args.test_full_barriers and (out != test_out.resolve() or not args.skip_shaders or args.reuse):
        parser.error('The test reference requires --skip-shaders, its dedicated exports/mochizuki-test-reference directory, and no --reuse')
    if args.reuse and not args.skip_shaders and manifest['reusable'](out):
        print('BUILD_REUSED', out / 'MochizukiNrRuntime.dll')
        return
    obj = out / 'obj'
    obj.mkdir(parents=True, exist_ok=True)
    headers, glslang = deps.dependencies(repo)
    up = repo / 'third_party/mochizuki'
    product = repo / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler'
    adapter = product / 'dlssnr/backend/mochizuki_runtime'
    includes = [headers, up / 'windows/src/core', adapter / 'compat', product / 'dlssnr/backend/lmxxf_runtime']
    defines = re.findall(r'-D([A-Z0-9_]+=[A-Za-z0-9_]+)', (up / 'windows/build/arch/rdna4.sh').read_text())
    cflags = ['/nologo', '/std:c++20', '/O2', '/EHsc', '/MT', '/utf-8', '/bigobj', '/W3', '/DNOMINMAX',
              '/D_CRT_SECURE_NO_WARNINGS', '/DNDEBUG', '/wd4244', '/wd4267', '/wd4305', '/wd4018']
    rename = ['vkCreateComputePipelines', 'vkCreateShaderModule', 'vkCreateDescriptorSetLayout',
              'vkCreatePipelineLayout', 'vkCreatePipelineCache', 'vkDestroyPipelineCache']
    sources = [(up / 'windows/src/core/nr_runtime.cpp', ['/D' + d for d in defines] + ['/D' + n + '=mzi_' + n for n in rename]),
               (up / 'windows/src/core/nr_native_plan.cpp', []), (adapter / 'mz_interpose.cpp', []),
               (adapter / 'MochizukiNrRuntime.cpp', ['/DLMXXF_NR_RUNTIME_EXPORTS'])]
    if args.test_full_barriers:
        # Derive only this test translation unit. The production/vendor file and
        # graph/arena layout stay identical; no menu, INI, environment or ABI switch.
        original = sources[0][0].read_text(encoding='utf-8')
        skip = 'if (!last && i < s.runner.no_barrier_after.size() && s.runner.no_barrier_after[i]) continue;'
        if original.count(skip) != 1:
            raise SystemExit('The full-barrier reference requires exactly one known dispatch skip')
        reference = out / 'reference-src/nr_runtime.cpp'
        reference.parent.mkdir(parents=True, exist_ok=True)
        reference.write_text(original.replace(skip, '// Test reference: retain every inter-dispatch barrier.'), encoding='utf-8', newline='\n')
        sources[0] = (reference, sources[0][1])
    objects = []
    for src, extra in sources:
        target = obj / (src.stem + '.obj')
        subprocess.run(['cl', *cflags, *extra, *['/I' + str(p) for p in includes], '/c', str(src), '/Fo' + str(target)], check=True)
        objects.append(str(target))
    subprocess.run(['link', '/nologo', '/DLL', '/OUT:' + str(out / 'MochizukiNrRuntime.dll'), *objects,
                    str(product / 'library/vulkan/vulkan-1.lib'), 'd3d12.lib', 'delayimp.lib',
                    '/DELAYLOAD:vulkan-1.dll'], check=True)
    if args.test_full_barriers:
        if source_hashes != manifest['sources']():
            raise SystemExit('Sources changed during the reference build')
        proof = {'test_only': True, 'policy': 'all-inter-dispatch-barriers', 'sources': source_hashes,
                 'runtime_sha256': hashlib.sha256((out / 'MochizukiNrRuntime.dll').read_bytes()).hexdigest(),
                 'translation_unit_sha256': hashlib.sha256(reference.read_bytes()).hexdigest()}
        (out / 'test-reference.json').write_text(json.dumps(proof, indent=2)+'\n', encoding='utf-8')
        print('TEST_REFERENCE_BUILD_OK', out / 'MochizukiNrRuntime.dll')
        return  # Never issue a production build manifest or release/ABI proof here.
    if not args.skip_shaders:
        subprocess.run([sys.executable, str(up / 'windows/build/build_network.py'), 'rdna4',
                        '--glslang', str(glslang), '--out', str(out / 'dlssnr-amd/shaders')], check=True)
        aco = out / 'dlssnr-amd/aco'
        if aco.exists():
            shutil.rmtree(aco)
        subprocess.run([sys.executable, str(up / 'linux/build/build_network.py'), 'rdna4',
                        '--glslang', str(glslang), '--out', str(aco / 'shaders')], check=True)
        shutil.copytree(up / 'windows/data/aco/records', aco / 'records')
        shutil.copy2(up / 'windows/data/aco/shell-aliases.txt', aco / 'shell-aliases.txt')
    if not args.skip_shaders:
        if source_hashes != manifest['sources']():
            raise SystemExit('Sources changed during build; rebuild before using these artifacts')
        manifest['write'](out)
    print('BUILD_OK', out / 'MochizukiNrRuntime.dll')

if __name__ == '__main__':
    main()
