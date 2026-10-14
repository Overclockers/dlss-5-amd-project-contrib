"""Verify the fixed upstream closure plus reviewed patches, without modifying vendor."""
import argparse
import io
import json
from pathlib import Path, PurePosixPath
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = Path(__file__).with_name('manifest.json')


def relative(value):
    path = PurePosixPath(value)
    if not value or path.is_absolute() or '..' in path.parts or '\\' in value or ':' in value:
        raise ValueError('Not a relative source path: ' + value)
    return value


def verify(repo, upstream, manifest_path=MANIFEST):
    repo, upstream, manifest_path = Path(repo).resolve(), Path(upstream).resolve(), Path(manifest_path).resolve()
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    if manifest['schema'] != 1:
        raise ValueError('Unsupported source-pin schema')
    pin = manifest['commit']
    if len(pin) != 40 or any(c not in '0123456789abcdef' for c in pin):
        raise ValueError('The pin must be a full commit SHA')
    files = [relative(p) for p in manifest['files']]
    added = [relative(p) for p in manifest.get('added_files', [])]
    if len(set(added)) != len(added) or set(added) & set(files):
        raise ValueError('Added source inventory duplicates upstream or local paths')
    closure = set(files) | set(added)
    if not files or len(set(files)) != len(files):
        raise ValueError('Source closure is empty or has duplicate paths')
    vendor = (repo / relative(manifest['vendor'])).resolve()
    if not vendor.is_relative_to(repo):
        raise ValueError('Vendor is outside this repository')
    extensions = set(manifest['text_extensions'])

    def normalize(path, data):
        return data.replace(b'\r\n', b'\n') if Path(path).suffix in extensions or Path(path).name == 'LICENSE' else data

    # Read Git objects at the pin, never the upstream checkout's working files.
    resolved = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', pin + '^{commit}'], text=True).strip()
    if resolved != pin:
        raise ValueError('Upstream did not resolve to the exact pin')
    archive = subprocess.check_output(['git', '-C', str(upstream), 'archive', '--format=tar', pin, '--', *files])
    expected = {}
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:') as tar:
        for path in files:
            entry = tar.getmember(path)
            if not entry.isfile():
                raise ValueError('Upstream source is not a regular file: ' + path)
            expected[path] = normalize(path, tar.extractfile(entry).read())

    scratch = (repo / 'exports/mochizuki-pin').resolve()
    if not scratch.is_relative_to(repo) or scratch == repo:
        raise ValueError('Replay scratch directory escaped the repository')
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='replay-', dir=scratch) as temporary:
        stage = Path(temporary).resolve()
        if not stage.is_relative_to(scratch):
            raise ValueError('Replay directory escaped exports')
        subprocess.run(['git', 'init', '-q', str(stage)], check=True)
        subprocess.run(['git', '-C', str(stage), 'config', 'core.autocrlf', 'false'], check=True)
        for path, data in expected.items():
            output = stage / path
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(data)
        for patch in manifest['patches']:
            patch_path = (manifest_path.parent / relative(patch['path'])).resolve()
            if not patch_path.is_relative_to(manifest_path.parent):
                raise ValueError('Patch escaped its directory')
            command = ['git', '-C', str(stage), 'apply', '--whitespace=error-all']
            subprocess.run([*command, '--check', str(patch_path)], check=True)
            subprocess.run([*command, str(patch_path)], check=True)
        staged = {p.relative_to(stage).as_posix() for p in stage.rglob('*')
                  if p.is_file() and '.git' not in p.relative_to(stage).parts}
        actual = {p.relative_to(vendor).as_posix() for p in vendor.rglob('*')
                  if p.is_file() and '__pycache__' not in p.parts and p.relative_to(vendor).as_posix() != 'UPSTREAM.md'}
        if staged != closure or actual != closure:
            raise ValueError('Closure inventory differs: extra=' + repr(sorted(actual - closure)) +
                             ' missing=' + repr(sorted(closure - actual)) +
                             ' replay_delta=' + repr(sorted(staged ^ closure)))
        different = [path for path in closure
                     if normalize(path, (stage / path).read_bytes()) != normalize(path, (vendor / path).read_bytes())]
        if different:
            raise ValueError('Unrecorded vendor changes or stale patches: ' + ', '.join(different))
    print(f'MOCHIZUKI_SOURCE_PIN_OK {pin} files={len(files)} patches={len(manifest["patches"])}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--upstream', type=Path, required=True, help='local upstream clone containing the pinned commit')
    args = parser.parse_args()
    try:
        verify(ROOT, args.upstream)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError, tarfile.TarError) as error:
        parser.exit(1, 'Mochizuki source-pin verification failed: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
