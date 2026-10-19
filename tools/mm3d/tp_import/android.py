"""Android setup entry points. Imported lazily when the user generates a mod."""

from .generate import generate, install
from .replacements import prepare


def create(root, mm_rom, tp_rom, nod_library, bridge):
    return generate(
        str(root),
        str(mm_rom),
        str(tp_rom),
        str(nod_library),
        lambda message: bridge.progress(message),
    )


def textures(stage, index, library, bridge):
    return prepare(str(stage), str(index), str(library), bridge)


def finish(root, stage):
    return install(str(root), str(stage))
