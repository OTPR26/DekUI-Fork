"""Pinned relocatable CPython for Apple Silicon first-launch setup."""
import argparse
import hashlib
from pathlib import Path
import tarfile
import urllib.request

URL = ('https://github.com/astral-sh/python-build-standalone/releases/download/20250317/'
       'cpython-3.13.2%2B20250317-aarch64-apple-darwin-install_only_stripped.tar.gz')
SHA256 = '9002e620e4113e7439b2de0db5ff9b2dc914cde4fba2f10134cce3a5cdebac81'


def verify(archive):
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise ValueError('macOS Python archive checksum mismatch')


def install(archive, runtime):
    verify(archive)
    if runtime.exists():
        raise ValueError('Refusing to replace an existing runtime')
    with tarfile.open(archive) as source:
        if any(not m.name.startswith('python/') for m in source.getmembers()):
            raise ValueError('Unexpected macOS Python archive layout')
        source.extractall(runtime, filter='data')
    if not (runtime / 'python/bin/python3').is_file():
        raise ValueError('Missing macOS Python executable')
    (runtime / 'python/DEKUI-NOTICE.txt').write_text(
        f'CPython 3.13.2, python-build-standalone 20250317, Apple Silicon.\n'
        f'Original binary distribution: {URL}\nOriginal SHA256: {SHA256}\n'
        'Build recipes: https://github.com/astral-sh/python-build-standalone/tree/20250317\n'
        'Original licenses are included in this runtime.\n', encoding='utf-8')


def download(output):
    if not output.exists():
        data = urllib.request.urlopen(URL, timeout=120).read()
        if hashlib.sha256(data).hexdigest() != SHA256:
            raise ValueError('Downloaded macOS Python checksum mismatch')
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
    verify(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    download(parser.parse_args().output)
