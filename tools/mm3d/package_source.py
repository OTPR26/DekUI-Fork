#!/usr/bin/env python3
"""Stream committed project/submodule source into an archive without a source-stage copy."""
import argparse
from pathlib import Path
import subprocess
import tarfile


# Internal root-level working notes are not part of the release source.
# Keep dependency documentation, licenses, tests and build scripts intact.
INTERNAL_ROOT_FILES = frozenset({'HANDOFF.md', 'DEVELOPMENT.md'})


def package(output):
    if output.exists():
        raise ValueError('Refusing to overwrite source archive')
    print('Checking pinned submodules...', flush=True)
    status = subprocess.check_output(['git', 'submodule', 'status', '--recursive'], text=True)
    if any(line.startswith(('-', '+', 'U')) for line in status.splitlines()):
        raise ValueError('Submodules must be initialized at their pinned revisions')
    repos = [('', Path.cwd())]
    # Git's shell may report MSYS absolute paths that native Windows Python
    # cannot interpret. displaypath is relative to the original calling repo,
    # including for nested submodules, so no absolute-path conversion is needed.
    paths = subprocess.check_output(
        ['git', 'submodule', 'foreach', '--quiet', '--recursive',
         'printf "%s\\n" "$displaypath"'], text=True)
    for line in paths.splitlines():
        relative = Path(line)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError(f'Unexpected submodule path: {line}')
        repos.append((relative.as_posix() + '/', Path.cwd() / relative))
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tarfile.open(str(output), 'w|gz') as dest:
            for index, (prefix, root) in enumerate(repos, 1):
                print(f'Archiving {index}/{len(repos)}: {prefix or "."}', flush=True)
                with subprocess.Popen(['git', 'archive', 'HEAD'], cwd=root,
                                      stdout=subprocess.PIPE) as process:
                    try:
                        with tarfile.open(fileobj=process.stdout, mode='r|') as source:
                            for entry in source:
                                if not prefix and entry.name in INTERNAL_ROOT_FILES:
                                    continue
                                entry.name = prefix + entry.name
                                dest.addfile(entry, source.extractfile(entry) if entry.isfile() else None)
                        # Tar EOF precedes pipe EOF. Drain git's trailing padding
                        # before waiting, or a full pipe can deadlock both sides.
                        while process.stdout.read(64 * 1024):
                            pass
                        if process.wait():
                            raise RuntimeError(f'git archive failed: {prefix or "."}')
                    except BaseException:
                        # Popen.__exit__ waits too; stop a blocked producer if
                        # reading or writing the archive fails partway through.
                        process.kill()
                        process.wait()
                        raise
    except BaseException:
        output.unlink(missing_ok=True)
        raise
    print(f'Created {output} ({output.stat().st_size:,} bytes)', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    package(parser.parse_args().output)
