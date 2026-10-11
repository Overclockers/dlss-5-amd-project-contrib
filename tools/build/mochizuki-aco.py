"""Verify a packaged ACO bundle: every network SPIR-V has the matching record, and no extra record is shipped."""
import argparse
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def fnv64(data):
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def record_hash(data):
    if len(data) < 96 or data[:8] != b'NRPAL001':
        raise ValueError('Invalid ACO record header')
    spv_hash, code_hash = struct.unpack_from('<QQ', data, 8)
    inline_count, code_size = struct.unpack_from('<II', data, 68)
    if inline_count > 32 or not code_size or code_size % 4 or len(data) != 96 + inline_count * 8 + code_size:
        raise ValueError('Invalid ACO record size')
    if any(data[76:96]) or fnv64(data[96 + inline_count * 8:]) != code_hash:
        raise ValueError('Invalid ACO record checksum/reserved fields')
    return spv_hash


def verify(bundle):
    bundle = Path(bundle)
    shaders = sorted((bundle / 'shaders').glob('g_*.spv'))
    shaders += sorted((bundle / 'shaders/temporal').glob('temporal_*fp32*.spv'))
    if not shaders:
        raise ValueError('ACO network shaders are missing')
    required = set()
    for shader in shaders:
        digest = fnv64(shader.read_bytes())
        name = f'{digest:016x}.nrp'
        if record_hash((bundle / 'records' / name).read_bytes()) != digest:
            raise ValueError('ACO SPIR-V/record mismatch: ' + shader.name)
        required.add(name)
    records = {p.name for p in (bundle / 'records').glob('*.nrp')}
    if required != records:
        raise ValueError('ACO record inventory does not match the shader recipes')
    if not (bundle / 'shell-aliases.txt').is_file():
        raise ValueError('ACO template aliases are missing')
    return len(shaders), len(records)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    if not args.verify:
        parser.error('--verify is required')
    shaders, records = verify(args.out)
    print(f'ACO_BUNDLE_VERIFIED: {shaders} shaders, {records} records')
