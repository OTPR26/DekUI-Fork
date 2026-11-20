"""Build both TP appearance variants from the user's selected discs."""

import json, os, shutil, tempfile, hashlib
from pathlib import Path
from .binary import Reader, require
from .disc import Disc, archive
from .model import build
from .voices import convert


def generate(root, mm_rom, tp_rom, nod_library, progress=lambda message: None):
    root = Path(root)
    parent = root / "load/mm3d-models"
    parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".twilight-link-", dir=parent))
    disc = mm = None
    try:
        progress("Reading your Twilight Princess disc…")
        disc = Disc(tp_rom, nod_library)
        mm = Reader(mm_rom)
        native = mm.actor("actors/zelda2_link_child_new.gar.lzs")
        texture_meta = []
        with tempfile.TemporaryDirectory(prefix="tp-source-", dir=stage) as folder:
            folder = Path(folder)
            base = folder / "Kmdl/bmwr"
            base.mkdir(parents=True)
            for path, data in archive(disc.file("res/Object/Kmdl.arc")).items():
                if path in (
                    "bmwr/al.bmd",
                    "bmwr/al_head.bmd",
                    "bmwr/al_face.bmd",
                    "bmwr/al_hands.bmd",
                ):
                    (base / Path(path).name).write_bytes(data)
            for path, data in archive(disc.file("res/Object/Alink.arc")).items():
                if path in ("bmwe/al_swm.bmd", "bmwe/al_podm.bmd"):
                    dest = folder / path
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    dest.write_bytes(data)
            for master, name in [(False, "kokiri"), (True, "master")]:
                progress(
                    "Creating TP Link / "
                    + ("Master Sword…" if master else "native sword…")
                )
                dest = stage / "build" / name
                data, meta = build(native, base, dest, master)
                target = (
                    stage
                    / "variants"
                    / name
                    / "romfs/actors/zelda2_link_child_new.gar.lzs"
                )
                target.parent.mkdir(parents=True)
                target.write_bytes(data)
                for t in meta:
                    texture_meta.append(
                        {**t, "file": "build/" + name + "/" + t["file"]}
                    )
        progress("Importing TP Link’s action voices…")
        sound, clips, bindings = convert(
            mm.actor("sound/sequence/JokerSound.bcsar"),
            disc.file("Audiores/Z2Sound.baa"),
            disc.file,
        )
        target = stage / "voices/romfs/sound/sequence/JokerSound.bcsar"
        target.parent.mkdir(parents=True)
        target.write_bytes(sound)
        (stage / "texture-manifest.json").write_text(json.dumps(texture_meta))
        (stage / "generated.json").write_text(
            json.dumps(
                {
                    "schema": 1,
                    "model": "twilight-link",
                    "variants": 2,
                    "voices": {"clips": clips, "human_bindings": bindings},
                },
                indent=2,
            )
        )
        return str(stage)
    except BaseException:
        shutil.rmtree(stage)
        raise
    finally:
        if disc:
            disc.close()
        if mm:
            mm.close()


def install(root, stage):
    root = Path(root)
    stage = Path(stage)
    parent = root / "load/mm3d-models"
    require(
        stage.parent.resolve() == parent.resolve()
        and stage.name.startswith(".twilight-link-"),
        "Invalid generated stage",
    )
    for name in ("kokiri", "master"):
        require(
            (
                stage / "variants" / name / "romfs/actors/zelda2_link_child_new.gar.lzs"
            ).is_file(),
            "Generated model is incomplete",
        )
    require(
        (stage / "voices/romfs/sound/sequence/JokerSound.bcsar").is_file(),
        "Generated voices are missing",
    )
    shutil.rmtree(stage / "build")
    (stage / "texture-manifest.json").unlink()
    destination = parent / "twilight-link"
    backup = parent / ".twilight-link-previous"
    # Keep one recoverable previous generation; never replace another model's files.
    if backup.exists():
        shutil.rmtree(backup)
    if destination.exists():
        destination.rename(backup)
    try:
        stage.rename(destination)
    except BaseException:
        if backup.exists():
            backup.rename(destination)
        raise
    return "Ready: Twilight Princess Link, two sword variants and TP action voices."
