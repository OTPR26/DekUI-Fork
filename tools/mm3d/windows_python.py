"""Pinned official embeddable CPython for Windows first-launch setup."""
import argparse
import hashlib
from pathlib import Path
import urllib.request
import zipfile

VERSION = '3.13.15'
URL = f'https://www.python.org/ftp/python/{VERSION}/python-{VERSION}-embed-amd64.zip'
SHA256 = 'd1f04d990aee1253d8569e8e5104e30fa9f5fa830899f14843448872d936a2cf'
PTH = 'python313._pth'
REQUIRED = ('python.exe', 'python313.dll', 'python313.zip', PTH, 'LICENSE.txt')


def verify(archive):
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise ValueError('Windows Python archive checksum mismatch')


def install(archive, destination):
    verify(archive)
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
        f'CPython {VERSION}, official Windows x64 embeddable distribution.\n'
        f'Source: {URL}\nOriginal archive SHA256: {SHA256}\n'
        'License and dependency notices: LICENSE.txt\n'
        'Dekui changes only python313._pth to add its setup helper directory.\n', encoding='utf-8')


def download(output):
    if output.exists():
        verify(output)
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    data = urllib.request.urlopen(URL, timeout=120).read()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise ValueError('Downloaded Windows Python checksum mismatch')
    output.write_bytes(data)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    download(parser.parse_args().output)
