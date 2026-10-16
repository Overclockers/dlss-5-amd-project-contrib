"""Pinned build-only dependencies; no Vulkan SDK installation or machine changes."""
import hashlib
import os
from pathlib import Path
import tempfile
import urllib.request
import zipfile

HEADER_PIN = 'e3b1eec08173d6b825cd3ac88c885a63b621504a'
GLSLANG_SHA = '06b71298b750268c127f2ee7ae0ef7525e2068120c6c8a3a08b2f58ca6f325ce'

def shared_archive(url, name):
    # Build output may be purged or live in another worktree. Keep downloads
    # independently of exports, keyed by the complete pinned upstream URL.
    # Use the user profile rather than LocalAppData: Store Python virtualizes
    # LocalAppData file IO, which can break atomic archive replacement.
    return Path.home() / '.cache/OptScalerNR/build-dependencies' / hashlib.sha256(url.encode()).hexdigest() / name


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
            temporary = Path(output.name)
            output.write(data)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def fetch(url, path, expected=None):
    shared = shared_archive(url, path.name)
    for candidate in (path, shared):
        if not candidate.is_file():
            continue
        data = candidate.read_bytes()
        if expected and hashlib.sha256(data).hexdigest() != expected:
            raise RuntimeError('Cached build dependency SHA256 mismatch: ' + str(candidate))
        if candidate == shared:
            atomic_write(path, data)
        elif not shared.is_file():
            atomic_write(shared, data)
        print('BUILD_DEPENDENCY_REUSED ' + str(candidate), flush=True)
        return
    try:
        with urllib.request.urlopen(url, timeout=120) as response:
            data = response.read()
    except OSError as error:
        raise RuntimeError(f'Build dependency download failed: {url}. '
                           f'Place the pinned archive at {path} or {shared}, then retry. {error}') from error
    if expected and hashlib.sha256(data).hexdigest() != expected:
        raise RuntimeError('Build dependency SHA256 mismatch: ' + url)
    atomic_write(shared, data)
    atomic_write(path, data)

def unpack(path, target):
    target = target.resolve()
    with zipfile.ZipFile(path) as archive:
        for item in archive.infolist():
            resolved = (target / item.filename).resolve()
            if not resolved.is_relative_to(target):
                raise RuntimeError('Archive member escapes dependency directory')
        archive.extractall(target)

def dependencies(repo):
    cache = repo / 'exports/mochizuki-toolchain'
    cache.mkdir(parents=True, exist_ok=True)
    headers = cache / ('Vulkan-Headers-' + HEADER_PIN)
    archive = cache / ('Vulkan-Headers-' + HEADER_PIN + '.zip')
    fetch('https://codeload.github.com/KhronosGroup/Vulkan-Headers/zip/' + HEADER_PIN, archive)
    if not (headers / 'include/vulkan/vulkan.h').is_file():
        unpack(archive, cache)
    glslang = cache / 'glslang/bin/glslang.exe'
    archive = cache / 'glslang-16.5.0.zip'
    fetch('https://github.com/KhronosGroup/glslang/releases/download/16.5.0/glslang-16.5.0-windows-x86_64-release.zip',
          archive, GLSLANG_SHA)
    if not glslang.is_file():
        unpack(archive, cache / 'glslang')
    return headers / 'include', glslang


def slang_dependency(repo):
    cache = repo / 'exports/mochizuki-toolchain'
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / 'slang-2026.19-windows-x86_64.zip'
    fetch('https://github.com/shader-slang/slang/releases/download/v2026.19/' + archive.name,
          archive, 'fc922f214b5d115f663932e59e80e0f6b6cc6fae140979cf307da6459b6f8f33')
    target = cache / 'slang-2026.19'
    compiler = target / 'bin/slangc.exe'
    if not compiler.is_file():
        unpack(archive, target)
    return compiler

if __name__ == '__main__':
    print(*dependencies(Path(__file__).resolve().parents[2]), sep='\n')
