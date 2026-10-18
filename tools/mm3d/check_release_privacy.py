"""Reject release files containing local paths or macOS metadata.

Pass private prefixes for the build machine; values are never printed. This is a
release gate, not a substitute for reviewing source, licenses, and screenshots.
"""
import argparse
import io
import glob
from pathlib import Path
import tarfile
import zipfile


CHUNK = 1024 * 1024
PUBLIC_RUNNER_HOME = b'/home/' + b'runner'
PUBLIC_RUNNER_CACHE_FILES = frozenset({
    'externals/catch2/.github/workflows/linux-bazel-builds.yml',
    'externals/dynarmic/externals/catch/.github/workflows/linux-bazel-builds.yml',
})


def patterns(prefixes):
    result = set()
    for prefix in prefixes:
        for value in (prefix.replace('\\', '/'), prefix.replace('/', '\\')):
            for encoding in ('utf-8', 'utf-16-le'):
                result.add(value.encode(encoding).lower())
    return tuple(result)


def inspect_stream(stream, forbidden):
    overlap = max((len(p) for p in forbidden), default=1) - 1
    tail = b''
    while block := stream.read(CHUNK):
        data = tail + block.lower()
        if any(p in data for p in forbidden):
            return True
        tail = data[-overlap:] if overlap else b''
    return False


def metadata(name):
    parts = name.replace('\\', '/').split('/')
    return '__MACOSX' in parts or any(p == '.DS_Store' or p.startswith('._') for p in parts)


def inspect_archive(path, forbidden):
    failures = []
    def entry(name, stream):
        if metadata(name):
            failures.append((name, 'macOS metadata'))
        if inspect_stream(io.BytesIO(name.encode()), forbidden):
            failures.append((name, 'private path in entry name'))
        if stream:
            if name in PUBLIC_RUNNER_CACHE_FILES:
                # These vendored workflows name GitHub's public runner cache.
                # Ignore only that exact convention, retaining all other checks.
                stream = io.BytesIO(stream.read().replace(PUBLIC_RUNNER_HOME + b'/.cache/bazel', b'$HOME/.cache/bazel'))
            if inspect_stream(stream, forbidden):
                failures.append((name, 'private path in file contents'))
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            for member in archive.infolist():
                with archive.open(member) as stream:
                    entry(member.filename, stream)
    elif tarfile.is_tarfile(path):
        with tarfile.open(path, 'r:*') as archive:
            for member in archive:
                stream = archive.extractfile(member) if member.isfile() else None
                try:
                    entry(member.name, stream)
                    if member.issym() or member.islnk():
                        entry(member.linkname, None)
                finally:
                    if stream:
                        stream.close()
    else:
        with Path(path).open('rb') as stream:
            entry(Path(path).name, stream)
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', nargs='+')
    parser.add_argument('--private-prefix', action='append', default=[])
    args = parser.parse_args()
    forbidden = patterns(args.private_prefix or [str(Path.home())])
    failed = False
    files = []
    for value in args.files:
        matches = sorted(glob.glob(value))
        if not matches:
            parser.error('A requested release file was not found')
        files.extend(Path(match) for match in matches)
    for path in dict.fromkeys(files):
        issues = inspect_archive(path, forbidden)
        if issues:
            failed = True
            # File names can themselves contain private paths. Print only issue
            # categories and counts; inspect suspect entries locally.
            counts = {}
            for _, reason in issues:
                counts[reason] = counts.get(reason, 0) + 1
            print(f'{path.name}: FAIL {counts}')
        else:
            print(f'{path.name}: PASS')
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()
