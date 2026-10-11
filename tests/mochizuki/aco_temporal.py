"""Byte comparisons against a separate all-barrier test DLL; no product scheduling switch.

Matrix and moving/replay cases adapted from Overclockers PR #5, commit 60f9202f.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def validate_reference():
    reference = ROOT / 'exports/mochizuki-test-reference'
    proof = json.loads((reference / 'test-reference.json').read_text(encoding='utf-8'))
    production = json.loads((ROOT / 'exports/mochizuki-runtime/build-manifest.json').read_text(encoding='utf-8'))
    if not proof.get('test_only') or proof.get('policy') != 'all-inter-dispatch-barriers':
        raise ValueError('Not the full-barrier test reference')
    if proof['sources'] != production['sources']:
        raise ValueError('Reference and production must be built from identical sources')
    if hashlib.sha256((reference / 'MochizukiNrRuntime.dll').read_bytes()).hexdigest() != proof['runtime_sha256']:
        raise ValueError('Reference DLL identity mismatch')
    original = (ROOT / 'third_party/mochizuki/windows/src/core/nr_runtime.cpp').read_text(encoding='utf-8')
    skip = 'if (!last && i < s.runner.no_barrier_after.size() && s.runner.no_barrier_after[i]) continue;'
    derived = original.replace(skip, '// Test reference: retain every inter-dispatch barrier.')
    if original.count(skip) != 1 or hashlib.sha256(derived.encode('utf-8')).hexdigest() != proof['translation_unit_sha256']:
        raise ValueError('Reference must differ only by retaining the known barrier skip')


def cases():
    # width, height, frames, moving, scale; author cases plus reduced model scale.
    for width, height, frames, moving, scale in (
        (256, 256, 128, 0, 100), (512, 512, 128, 1, 100),
        (2258, 1270, 16, 1, 100), (1129, 635, 8, 1, 100), (512, 512, 16, 1, 50),
    ):
        for fmt in ('fp16', 'srgb'):
            for passes in (1, 3):
                for enlargement in (0, 1):
                    yield (width, height, fmt, passes, frames, moving, enlargement, int(passes == 3), scale, 0, 0)
    # Our 30-layer capacity and selection routes are absent from the contribution branch.
    for passes in (1, 15, 30):
        yield (256, 256, 'fp16', passes, 8, 1, 0, 0, 100, 30, 0)
    # Classic enlargement, including the first-pass-adjacent composition path.
    yield (512, 512, 'fp16', 3, 16, 1, 2, 0, 50, 0, 0)
    yield (512, 512, 'fp16', 3, 16, 1, 2, 0, 50, 0, 1)


def compare(left, right):
    if left.stat().st_size != right.stat().st_size:
        raise ValueError('Output sizes differ')
    digest = hashlib.sha256()
    offset = 0
    with left.open('rb') as a, right.open('rb') as b:
        while True:
            x, y = a.read(8 * 1024 * 1024), b.read(8 * 1024 * 1024)
            if x != y:
                at = next(i for i, (p, q) in enumerate(zip(x, y)) if p != q)
                raise ValueError(f'Temporal output differs at byte {offset + at}')
            if not x:
                break
            digest.update(x)
            offset += len(x)
    return offset, digest.hexdigest()


def select_cases(matrix, start, single=None):
    if not 1 <= start <= len(matrix):
        raise ValueError('start is outside the matrix')
    if single is not None and (not 1 <= single <= len(matrix) or single < start):
        raise ValueError('case is outside the selected matrix')
    selected = [(n, case) for n, case in enumerate(matrix, 1)
                if n >= start and (single is None or single == n)]
    if not selected:
        raise ValueError('no test cases selected')
    return selected


def controlled_environment():
    # This is a fixture, not a game launch: inherited diagnostics must not disable
    # overlap, change its groups, or inject unrelated OOM/device/fallback failures.
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith(('NR_', 'MZ_TEST_'))}
    env.update(NR_TCHAIN='1', NR_TCHAIN_KINDS='0x1ff', NR_TCHAIN_C512='0xf')
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', type=int, help='run one numbered case')
    parser.add_argument('--start', type=int, default=1, help='resume at a numbered case')
    args = parser.parse_args()
    matrix = list(cases())
    try:
        selected = select_cases(matrix, args.start, args.case)
    except ValueError as error:
        parser.error(str(error))
    validate_reference()
    subprocess.run([sys.executable, '-X', 'utf8', str(ROOT / 'tools/build/mochizuki-manifest.py'),
                    str(ROOT / 'exports/mochizuki-runtime')], check=True)
    work = ROOT / 'exports/mochizuki-temporal'
    work.mkdir(parents=True, exist_ok=True)
    test = ROOT / 'exports/mochizuki-tests/aco_temporal.exe'
    assets = ROOT / 'exports/mochizuki-runtime'
    environment = controlled_environment()
    for number, case in selected:
        outputs = []
        for mode, runtime_dir in (('explicit', 'mochizuki-test-reference'), ('optimized', 'mochizuki-runtime')):
            output = work / f'case-{number:02d}-{mode}.raw'
            log = work / f'case-{number:02d}-{mode}.log'
            dll = ROOT / 'exports' / runtime_dir / 'MochizukiNrRuntime.dll'
            with log.open('w', encoding='utf-8') as stream:
                result = subprocess.run([str(test), str(dll), str(assets), str(output), *map(str, case)],
                                        stdout=stream, stderr=subprocess.STDOUT, timeout=900, env=environment)
            if result.returncode:
                raise RuntimeError(f'Case {number} {mode} failed: {log}\n' + log.read_text(encoding='utf-8', errors='replace')[-4000:])
            outputs.append(output)
        size, digest = compare(*outputs)
        width, height, fmt, passes, frames, moving, enlargement, preprocess, scale, capacity, first = case
        expected = width * height * (8 if fmt == 'fp16' else 4) * frames
        if size != expected:
            raise ValueError(f'Case {number} did not compare every requested frame')
        line = f'ACO_TEMPORAL_BYTES_OK case={number}/{len(matrix)} config={case} bytes={size} sha256={digest}'
        print(line, flush=True)
        (work / f'case-{number:02d}-passed.json').write_text(json.dumps({'case': case, 'bytes': size,
            'sha256': digest, 'production_dll': hashlib.sha256((assets / 'MochizukiNrRuntime.dll').read_bytes()).hexdigest(),
            'graph_environment': {key: environment[key] for key in ('NR_TCHAIN','NR_TCHAIN_KINDS','NR_TCHAIN_C512')}}, indent=2)+'\n', encoding='utf-8')
        # Remove only these two generated synthetic frame streams; retain failure artifacts and logs.
        for output in outputs:
            if output.resolve().parent != work.resolve():
                raise ValueError('Synthetic output escaped test directory')
            output.unlink()
    print(f'ACO_TEMPORAL_SUITE_OK completed={len(selected)}/{len(matrix)}', flush=True)


if __name__ == '__main__':
    main()
