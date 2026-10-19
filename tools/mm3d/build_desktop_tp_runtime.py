"""Stage native TP disc reading and isolated model-converter dependencies in CI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

NOD = 'ebd80cac99b48a323200d84365e07a58cc27412d'
LINUX_PYTHON = {
    'x86_64': 'ee909a9d0fb16375ebd3b3ca0322fd9cac0904b304e28ad68527036858e301e9',
    'aarch64': '106bf4f243a2d02a1e3995ad0551bf15bdfb33abfc18f85138e6b8dd0d1923fc',
}


def run(*args, **kwargs):
    subprocess.run([str(x) for x in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('build-tp-runtime'))
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(exist_ok=True, parents=True)
    stage = root / 'payload'
    runtime = stage / 'runtime'
    machine = 'aarch64' if platform.machine().lower() in ('arm64', 'aarch64') else 'x86_64'
    target = ('aarch64-pc-windows-msvc' if machine == 'aarch64' else 'x86_64-pc-windows-gnu') if sys.platform == 'win32' else None
    env = dict(os.environ)
    candidates = [Path.home()/'.cargo/bin', Path(os.environ.get('USERPROFILE', str(Path.home())))/'.cargo/bin']
    env['PATH'] = os.pathsep.join(map(str,candidates)) + os.pathsep + env['PATH']
    cargo = shutil.which('cargo', path=env['PATH'])
    if not cargo:
        # Linux toolchain containers do not include Rust. Keep its files in the build folder.
        if not sys.platform.startswith('linux'):
            raise RuntimeError('Install a native Rust toolchain before staging TP dependencies')
        init = root/'rustup-init'
        urllib.request.urlretrieve(f'https://static.rust-lang.org/rustup/dist/{machine}-unknown-linux-gnu/rustup-init', init)
        init.chmod(0o755)
        env.update(CARGO_HOME=str(root/'cargo'), RUSTUP_HOME=str(root/'rustup'))
        run(init,'-y','--no-modify-path','--profile','minimal',env=env)
        cargo = str(root/'cargo/bin/cargo')
        env['PATH'] = str(root/'cargo/bin') + os.pathsep + env['PATH']
    if target and target.endswith('gnu'):
        env['CC_x86_64_pc_windows_gnu'] = 'gcc'
        env['CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER'] = 'gcc'
    if target:
        # Windows CreateProcess does not resolve executables against env['PATH'].
        rustup = shutil.which('rustup', path=env['PATH'])
        if not rustup:
            raise RuntimeError('Rust toolchain manager was not found beside Cargo')
        run(rustup,'target','add',target,env=env)
    source = root/'nod-source'
    if not source.exists():
        archive = root/'nod-source.tar.gz'
        urllib.request.urlretrieve(f'https://codeload.github.com/encounter/nod/tar.gz/{NOD}',archive)
        extracted = root/'source'
        extracted.mkdir(exist_ok=True)
        with tarfile.open(archive) as tar:
            tar.extractall(extracted,filter='data')
        folders = list(extracted.iterdir())
        if len(folders) != 1:
            raise RuntimeError('Unexpected disc-reader source layout')
        folders[0].rename(source)
    flags = [f'--remap-path-prefix={Path.home()}=/build', f'--remap-path-prefix={Path.cwd()}=/build']
    if target and target.endswith('msvc'):
        flags += ['-C','target-feature=+crt-static']
    env['CARGO_ENCODED_RUSTFLAGS'] = '\x1f'.join(flags)
    mappings = [Path.home(), Path.cwd()]
    if target and target.endswith('msvc'):
        cflags = ' '.join('/pathmap:' + str(path) + '=/build' for path in mappings)
    else:
        cflags = ' '.join('-ffile-prefix-map=' + str(path) + '=/build' for path in mappings)
    env['CFLAGS'] = env.get('CFLAGS', '') + ' ' + cflags
    env['CXXFLAGS'] = env.get('CXXFLAGS', '') + ' ' + cflags

    command = [cargo,'build','--manifest-path',str(source/'Cargo.toml'),'--locked','--release',
               '-p','nod-ffi','--no-default-features','--features',
               'compress-bzip2-vendored,compress-lzma-vendored,compress-zlib-vendored,compress-zstd-vendored']
    if target:
        command += ['--target',target]
    run(*command,env=env)
    library = 'nod.dll' if sys.platform == 'win32' else 'libnod.dylib' if sys.platform == 'darwin' else 'libnod.so'
    binary = source/'target'
    if target:
        binary /= target
    binary = binary / 'release' / library
    dest = runtime/'tp'
    dest.mkdir(parents=True,exist_ok=True)
    shutil.copy2(binary,dest/library)
    if sys.platform == 'darwin':
        run('install_name_tool','-id','@rpath/libnod.dylib',dest/library)
    for license in ('LICENSE-MIT','LICENSE-APACHE'):
        shutil.copy2(source/license,dest/license)
    wheel_platform = ('win_arm64' if machine == 'aarch64' else 'win_amd64') if sys.platform == 'win32' else \
                     'macosx_11_0_arm64' if sys.platform == 'darwin' else \
                     f'manylinux2014_{machine}'
    numpy_version = '2.3.5' if wheel_platform == 'win_arm64' else '2.2.6'
    with urllib.request.urlopen(f'https://pypi.org/pypi/numpy/{numpy_version}/json', timeout=60) as response:
        release = json.load(response)
    wheels = [item for item in release['urls']
              if 'cp313-cp313-' in item['filename'] and wheel_platform in item['filename']]
    if len(wheels) != 1:
        raise RuntimeError('No unique NumPy wheel for ' + wheel_platform)
    wheel = root / wheels[0]['filename']
    urllib.request.urlretrieve(wheels[0]['url'], wheel)
    if hashlib.sha256(wheel.read_bytes()).hexdigest() != wheels[0]['digests']['sha256']:
        raise RuntimeError('NumPy wheel checksum mismatch')
    with zipfile.ZipFile(wheel) as archive:
        for entry in archive.namelist():
            if Path(entry).is_absolute() or '..' in Path(entry).parts:
                raise RuntimeError('Unsafe wheel member')
        archive.extractall(runtime / 'tp-modules')
    if sys.platform == 'darwin':
        # Vendor wheels may retain source filenames in Mach-O debug symbols.
        # Keep exported runtime symbols; package_macos signs the resulting files.
        for native in (runtime / 'tp-modules').rglob('*'):
            if native.is_file() and native.suffix in ('.so', '.dylib'):
                run('strip', '-S', native)
    if sys.platform.startswith('linux'):
        archive = root/'python.tar.gz'
        name = f'cpython-3.13.2%2B20250317-{machine}-unknown-linux-gnu-install_only_stripped.tar.gz'
        urllib.request.urlretrieve('https://github.com/astral-sh/python-build-standalone/releases/download/20250317/'+name,archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != LINUX_PYTHON[machine]:
            raise RuntimeError('Linux Python checksum mismatch')
        with tarfile.open(archive) as tar:
            tar.extractall(runtime/'tp-python',filter='data')
    (stage/'runtime/tp/NOTICE.json').write_text(json.dumps({'disc_reader_revision':NOD,'numpy':numpy_version},indent=2))


if __name__ == '__main__':
    main()
