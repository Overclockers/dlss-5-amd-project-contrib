"""Compare production FLOAT inline slots with split; no polling-budget override.

Cold ahead-frame hashes precede sustained forward/reverse timing, unpaced and
60 Hz. Successful NR throughput is not game FPS. Failed rows remain in the logs.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('exe', 'runtime', 'assets', 'output'):
        parser.add_argument(name)
    parser.add_argument('--aco', action='store_true')
    parser.add_argument('--height', type=int, default=1440)
    parser.add_argument('--frames', type=int, default=180)
    args = parser.parse_args()
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    # Assert the route under test. MP1/FLOAT at full model scale must use compute.
    env['MZ_TEST_INLINE_CHAIN'] = '1'
    common = [args.exe, args.runtime, args.assets]
    flags = ['--mochizuki', '--history', f'--height={args.height}']
    if args.aco:
        flags.append('--aco')

    def run(name, slots, options):
        with (output / f'{name}.log').open('w', encoding='utf-8') as log:
            result = subprocess.run([*common, str(slots), *flags, *options], env=env,
                                    stdout=log, stderr=subprocess.STDOUT, timeout=300)
        text = (output / f'{name}.log').read_text(encoding='utf-8', errors='replace')
        rows = [dict(token.split('=', 1) for token in row.split())
                for row in re.findall(r'^BENCH (.+)$', text, re.M)]
        (output / f'{name}.rows.json').write_text(json.dumps(rows, indent=2)+'\n', encoding='utf-8')
        if result.returncode or not rows:
            raise RuntimeError(f'{name}: failed ({result.returncode})\n{text[-2000:]}')
        for row in rows:
            if sum(int(row[k]) for k in ('applied', 'timeout', 'skipped')) != int(row['frames']):
                raise RuntimeError(f'{name}: incomplete frame accounting: {row}')
            if int(row['timeout']):
                raise RuntimeError(f'{name}: bounded wait exhausted: {row}')
        return re.findall(r'^HASH .+$', text, re.M), rows

    reference = None
    for slots in (0, 1, 4):
        hashes, _ = run(f'verify-{slots}', slots, ['--verify-ahead', '--frames=9'])
        if len(hashes) != 9 or (reference is not None and hashes != reference):
            raise RuntimeError(f'slots={slots}: different or missing output hashes')
        reference = hashes
        print(f'EXACT_HASH_PASS slots={slots}', flush=True)
    results = []
    for name, extra in [('unpaced', []), ('60hz', ['--interval=16.667'])]:
        _, rows = run(name, 0, ['--sweep', f'--frames={args.frames}', *extra])
        if len(rows) != 10 or [int(row['slots']) for row in rows] != [0,1,2,3,4,4,3,2,1,0]:
            raise RuntimeError(f'{name}: incomplete forward/reverse sweep')
        for row in rows:
            if name == 'unpaced' and int(row['skipped']):
                raise RuntimeError(f'{name}: unexpected bypass: {row}')
            row['profile'] = name
            print('BENCH '+' '.join(f'{k}={v}' for k,v in row.items()), flush=True)
        results.extend(rows)
        (output / 'inline-results.json').write_text(json.dumps(results, indent=2)+'\n', encoding='utf-8')
    print('MOCHIZUKI_INLINE_COMPARISON_PASS', flush=True)


if __name__ == '__main__':
    main()
