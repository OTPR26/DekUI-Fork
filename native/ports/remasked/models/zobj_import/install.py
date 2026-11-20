"""Convert data-only model packages and atomically replace one appearance slot."""

import json
from pathlib import Path
import shutil
import tempfile
from .reader import require

ACTOR = "romfs/actors/zelda2_link_child_new.gar.lzs"


def install_stage(root, stage):
    parent = Path(root) / "load/mm3d-models"
    stage = Path(stage)
    require(
        stage.parent.resolve() == parent.resolve()
        and stage.name.startswith(".pak-import-")
        and not stage.is_symlink(),
        "Invalid PAK import stage",
    )
    report = json.loads((stage / "import.json").read_text())
    require(
        report.get("schema") == 1
        and report.get("kind") == "pak-import"
        and (stage / ACTOR).is_file()
        and not (stage / ACTOR).is_symlink(),
        "Incomplete PAK import",
    )
    destination = parent / "pak-import"
    backup = parent / ".pak-import-previous"
    require(
        not destination.is_symlink() and not backup.is_symlink(),
        "Invalid model destination",
    )
    if backup.exists():
        shutil.rmtree(backup)
    had_previous = destination.exists()
    if had_previous:
        destination.rename(backup)
    try:
        stage.rename(destination)
    except BaseException:
        if had_previous:
            backup.rename(destination)
        raise
    return report


def generate_and_install(root, mm_rom, package, progress=lambda message: None):
    from .convert import convert

    parent = Path(root) / "load/mm3d-models"
    parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".pak-import-", dir=parent))
    try:
        progress("Reading the PAK model and textures…")
        report = convert(mm_rom, package, stage / "converted")
        progress("Installing the converted player model…")
        actor = stage / ACTOR
        actor.parent.mkdir(parents=True)
        (stage / "converted/zelda2_link_child_new.gar.lzs").rename(actor)
        shutil.rmtree(stage / "converted")
        report = {**report, "kind": "pak-import"}
        (stage / "import.json").write_text(json.dumps(report, indent=2))
        return install_stage(root, stage)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
