"""Extract both supported Mochizuki models locally; never loads the NVIDIA DLL."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

DLL_3108 = 'e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e'
DLL_3109 = '2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f'


def extract(source, output, vendor, temporary_parent=None):
    """The output names the original pack; its sibling holds the new model, when present."""
    with tempfile.TemporaryDirectory(prefix='mochizuki-model-', dir=temporary_parent) as tmp:
        work = Path(tmp)

        def run(script, *params, capture=False):
            return subprocess.run([sys.executable, '-X', 'utf8', str(vendor / script), *map(str, params)],
                                  cwd=vendor, check=True, stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                                  text=True, encoding='utf-8')

        probe = run('inspect_nr.py', source, '--output', work / 'probe', '--extract-weights', capture=True)
        sha = json.loads(probe.stdout)['dll_sha256'].lower()
        if sha not in (DLL_3108, DLL_3109):
            raise ValueError('nvngx_dlssnr.dll 310.9.16 or 310.8.0 is required (SHA256 mismatch)')
        models = [('WEIGHTS_HT', '', 70, output)]
        if sha == DLL_3109:
            models.append(('WEIGHTS_CC_SMALL_FP8', '-small', 30, output.with_name('dlssnr-small.bin')))
        packed_models = []
        # Validate every requested model before replacing any installed pack.
        for resource, suffix, post, destination in models:
            model_work = work / resource
            (model_work / 'graph').mkdir(parents=True)
            shutil.copy2(vendor / ('descriptor' + suffix + '.json'), model_work / 'graph/descriptor.json')
            run('inspect_nr.py', source, '--output', model_work / 'inventory', '--extract-weights', '--resource', resource)
            for script, folder, extra in [('unpack_swin_family.py', 'unpacked', ['--max-c', '256']),
                                          ('unpack_splitswin.py', 'unpacked-splitswin', []),
                                          ('unpack_vit.py', 'unpacked-vit', [])]:
                run(script, '--artifacts', model_work, '--output', model_work / folder, *extra)
            run('unpack_preblock.py', model_work / 'inventory/weights/block0.layer0.layer.bin', model_work / 'unpacked-preblock')
            run('unpack_postblock.py', model_work / f'inventory/weights/block{post}.layer0.layer.bin', model_work / 'unpacked-postblock')
            packed = model_work / destination.name
            run('pack_model.py', '--root', model_work, '--list', vendor / ('model-files' + suffix + '.txt'),
                '--out', packed, '--verify', vendor / ('model-files' + suffix + '.sha256'))
            packed_models.append((packed, destination))
        output.parent.mkdir(parents=True, exist_ok=True)
        staged_models = []
        try:
            for packed, destination in packed_models:
                with tempfile.NamedTemporaryFile(prefix='.mochizuki-', dir=output.parent, delete=False) as f:
                    staged = Path(f.name)
                staged_models.append((staged, destination))
                shutil.copyfile(packed, staged)
            for staged, destination in staged_models:
                staged.replace(destination)
                print('MODEL_OK', destination)
        finally:
            for staged, _ in staged_models:
                staged.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='User-supplied nvngx_dlssnr.dll 310.9.16 / 310.8.0, or its ZIP')
    parser.add_argument('output', type=Path, help='Destination dlssnr-amd/dlssnr.bin; new model is written beside it')
    parser.add_argument('--work', type=Path, help='Parent for temporary extraction files')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    vendor = here / 'model-tools'
    if not vendor.is_dir():
        vendor = here.parents[1] / 'third_party/mochizuki/linux/package/model-tools'
    source, output = args.source.resolve(), args.output.resolve()
    if source in (output, output.with_name('dlssnr-small.bin')) or not source.is_file():
        parser.error('Source must be an existing DLL/ZIP distinct from the output')
    if output.name.lower() == 'dlssnr-small.bin':
        parser.error('Output must name the original dlssnr.bin; the new model is written beside it')
    if args.work:
        args.work.mkdir(parents=True, exist_ok=True)
    try:
        extract(source, output, vendor, args.work)
    except ValueError as error:
        parser.error(str(error))

if __name__ == '__main__':
    main()
