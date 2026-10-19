"""Copy only staged converter dependencies and source modules into desktop packages."""
from pathlib import Path
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
            shutil.copytree(source,destination/name,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
    scripts = payload/'tools/mm3d'
    shutil.copytree(repo/'tools/mm3d/tp_import',scripts/'tp_import',
                    ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
    shutil.copy2(repo/'tools/mm3d/generate_tp_mod.py',scripts/'generate_tp_mod.py')
