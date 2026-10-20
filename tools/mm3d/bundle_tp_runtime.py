"""Copy only staged converter dependencies and source modules into desktop packages."""
from pathlib import Path
import os
import shutil


def copy_tp_runtime(repo, payload):
    staged = repo / 'build-tp-runtime/payload/runtime'
    libraries = [staged/'tp'/name for name in ('nod.dll','libnod.dylib','libnod.so')]
    extensions = list((staged/'tp-modules/numpy').rglob('_multiarray_umath.*'))
    if not any(p.suffix in ('.so', '.pyd') for p in extensions):
        raise ValueError('NumPy native extension is missing')
    if sum(p.is_file() for p in libraries) != 1 or not (staged/'tp-modules/numpy/__init__.py').is_file():
        raise ValueError('Twilight Princess converter runtime is incomplete')
    destination = payload/'runtime'
    destination.mkdir(parents=True,exist_ok=True)
    for name in ('tp','tp-modules','tp-python'):
        source = staged/name
        if source.exists():
            # Keep loader aliases as links instead of duplicating shared libraries.
            # Only links contained within this runtime subtree may be packaged.
            for path in source.rglob('*'):
                if path.is_symlink() and not path.resolve().is_relative_to(source.resolve()):
                    raise ValueError('Converter runtime link escapes its directory')
            shutil.copytree(source,destination/name,symlinks=True,
                            ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
            for path in source.rglob('*'):
                if path.is_symlink():
                    copied = destination/name/path.relative_to(source)
                    if not copied.is_symlink():
                        continue
                    target = destination/name/path.resolve().relative_to(source.resolve())
                    copied.unlink()
                    copied.symlink_to(os.path.relpath(target,copied.parent),
                                      target_is_directory=path.is_dir())
    # These are upstream test suites, not converter dependencies. Keep native
    # extensions, NumPy's runtime helpers and all dependency license files.
    for folder in (destination/'tp-modules/numpy').rglob('tests'):
        if folder.is_dir() and not folder.is_symlink():
            shutil.rmtree(folder)
    tests = destination/'tp-python/python/lib/python3.13/test'
    if tests.is_dir():
        shutil.rmtree(tests)
    scripts = payload/'tools/mm3d'
    shutil.copytree(repo/'tools/mm3d/tp_import',scripts/'tp_import',
                    ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
    shutil.copy2(repo/'tools/mm3d/generate_tp_mod.py',scripts/'generate_tp_mod.py')
    shutil.copytree(repo/'tools/mm3d/zobj_import', scripts/'zobj_import',
                    ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
    shutil.copy2(repo/'tools/mm3d/import_pak.py', scripts/'import_pak.py')
