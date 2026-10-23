"""Run shared_extended.exe per scenario with and without MZ_TEST_SHARED_EXTENDED=0 and compare.

Pass: both runs succeed, the widened scenarios take the shared colour texture by default and the buffer with
the variable, and the output hashes match (RenoDX: the first frame after a history reset only, see the fixture).
"""
import os
import re
import subprocess
import sys

exe, runtime, assets = sys.argv[1:4]
widened = {'exact-subrect', 'bucket', 'scale', 'passes2', 'passes3', 'reno'}
failed = False
for scenario in ['full', *sorted(widened)]:
    runs = {}
    for name, extra in (('default', {}), ('extended0', {'MZ_TEST_SHARED_EXTENDED': '0'})):
        env = {k: v for k, v in os.environ.items() if not k.startswith('MZ_TEST_SHARED_')}
        env.update(extra)
        out = subprocess.run([exe, runtime, assets, scenario, 'debug'], env=env, capture_output=True, text=True)
        line = next((l for l in out.stdout.splitlines() if l.startswith('SHARED_EXTENDED ')), None)
        if out.returncode or not line:
            print(f'FAIL {scenario}/{name} exit={out.returncode} {out.stderr.strip()}')
            failed = True
            break
        runs[name] = dict(re.findall(r'(\w+)=((?:shared texture)|\S+)', line))
        print(f'{scenario}/{name}: colour={runs[name]["colour"]} motion={runs[name]["motion"]} '
              f'hash0={runs[name]["hash0"]} hash1={runs[name]["hash1"]} wall_median_ms={runs[name]["wall_median_ms"]}')
    if len(runs) != 2:
        continue
    a, b = runs['default'], runs['extended0']
    expect_b = 'buffer' if scenario in widened else 'shared texture'
    checks = [a['colour'] == 'shared texture', b['colour'] == expect_b, a['hash0'] == b['hash0']]
    if scenario != 'reno':
        checks.append(a['hash1'] == b['hash1'])
    if not all(checks):
        print(f'FAIL {scenario}: route or output mismatch')
        failed = True
print('SHARED_EXTENDED_FAIL' if failed else 'SHARED_EXTENDED_OK')
sys.exit(1 if failed else 0)
