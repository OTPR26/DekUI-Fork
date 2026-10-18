"""Pinned official embeddable CPython for Windows first-launch setup."""
import argparse
import hashlib
from pathlib import Path
import urllib.request
import zipfile

VERSION = '3.13.15'
URL = f'https://www.python.org/ftp/python/{VERSION}/python-{VERSION}-embed-amd64.zip'
SHA256 = 'd1f04d990aee1253d8569e8e5104e30fa9f5fa830899f14843448872d936a2cf'
ARM64_SHA256 = 'cd992cbfb33be433ff20f150691595efb2862e56f4f1bec684c6077d4775af8e'
PTH = 'python313._pth'
REQUIRED = ('python.exe', 'python313.dll', 'python313.zip', PTH, 'LICENSE.txt')


def distribution(architecture):
    if architecture == 'x64':
        return URL, SHA256
    if architecture == 'arm64':
        return URL.replace('amd64.zip', 'arm64.zip'), ARM64_SHA256
    raise ValueError(f'Unsupported Windows architecture: {architecture}')


def verify(archive, architecture='x64'):
    if hashlib.sha256(archive.read_bytes()).hexdigest() != distribution(architecture)[1]:
        raise ValueError('Windows Python archive checksum mismatch')


def install(archive, destination, architecture='x64'):
    verify(archive, architecture)
    source_url, digest = distribution(architecture)
    if destination.exists():
        raise ValueError('Refusing to replace an existing Python runtime')
    with zipfile.ZipFile(archive) as source:
        names = source.namelist()
        if any(n not in names for n in REQUIRED):
            raise ValueError('Windows Python archive is incomplete')
        if any('/' in n or '\\' in n or ':' in n or n in ('.', '..') for n in names):
            raise ValueError('Unexpected Windows Python archive path')
        source.extractall(destination)
    # _pth enables isolated mode: no registry, PYTHONPATH or user site packages.
    # Explicitly add our helper directory; do not enable import site or pip.
    with (destination / PTH).open('w', encoding='utf-8', newline='\n') as path_file:
        path_file.write('python313.zip\n.\n../../tools/mm3d\n')
    (destination / 'DEKUI-NOTICE.txt').write_text(
        f'CPython {VERSION}, official Windows {architecture} embeddable distribution.\n'
        f'Source: {source_url}\nOriginal archive SHA256: {digest}\n'
        'License and dependency notices: LICENSE.txt\n'
        'Dekui changes only python313._pth to add its setup helper directory.\n', encoding='utf-8')


def download(output, architecture='x64'):
    source_url, digest = distribution(architecture)
    if output.exists():
        verify(output, architecture)
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    data = urllib.request.urlopen(source_url, timeout=120).read()
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError('Downloaded Windows Python checksum mismatch')
    output.write_bytes(data)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--architecture', choices=('x64', 'arm64'), default='x64')
    args = parser.parse_args()
    download(args.output, args.architecture)
