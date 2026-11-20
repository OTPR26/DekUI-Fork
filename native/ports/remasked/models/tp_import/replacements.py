"""Convert matching pack images into independent MM3D texture overrides."""

import ctypes as c, io, json, struct
from pathlib import Path
import numpy as np
from PIL import Image
from .binary import require


def image(data, library):
    if (
        data[:4] == b"DDS "
        and len(data) >= 148
        and data[84:88] == b"DX10"
        and struct.unpack_from("<I", data, 128)[0] == 134
    ):
        height, width = struct.unpack_from("<II", data, 12)
        require(
            0 < width <= 4096 and 0 < height <= 4096, "Texture dimensions exceed 4096"
        )
        raw = data[148:]
        out = (c.c_uint32 * (width * height))()
        fn = library.dekui_tp_decode_astc
        fn.argtypes = [
            c.c_char_p,
            c.c_size_t,
            c.c_int,
            c.c_int,
            c.c_int,
            c.c_int,
            c.c_void_p,
        ]
        fn.restype = c.c_int
        require(
            fn(raw, len(raw), width, height, 4, 4, out) != 0,
            "Cannot decode ASTC texture",
        )
        pixels = (
            np.frombuffer(out, dtype=np.uint8)
            .reshape(height, width, 4)[:, :, [2, 1, 0, 3]]
            .copy()
        )
        return Image.fromarray(pixels)
    im = Image.open(io.BytesIO(data))
    require(
        0 < im.width <= 4096 and 0 < im.height <= 4096, "Texture dimensions exceed 4096"
    )
    return im.convert("RGBA")


def prepare(stage, index_json, library, reader):
    stage = Path(stage)
    index = json.loads(index_json)
    lib = c.CDLL(library)
    legacy = lib.dekui_tp_legacy_texture_hash
    legacy.argtypes = [c.c_char_p, c.c_size_t, c.c_int, c.c_int]
    legacy.restype = c.c_uint64
    hashfn = lib.dekui_tp_texture_hash
    hashfn.argtypes = [c.c_char_p, c.c_size_t]
    hashfn.restype = c.c_uint64
    dest = stage / "texture-overrides"
    dest.mkdir(exist_ok=True)
    seen = set()
    matched = 0
    duplicates = 0
    for t in json.loads((stage / "texture-manifest.json").read_text()):
        raw = (stage / t["file"]).read_bytes()
        ha = f"{hashfn(raw,len(raw)):016X}"
        if ha in seen:
            continue
        seen.add(ha)
        candidates = sorted(set(index.get(t["source_hash"], [])))
        if not candidates:
            continue
        chosen = None
        score = 0
        for uri in candidates:
            data = bytes(reader.read(uri))
            require(len(data) <= 64 * 1024 * 1024, "Replacement texture exceeds 64 MiB")
            im = image(data, lib)
            require(
                im.width * t["height"] == im.height * t["width"],
                "Replacement texture aspect ratio differs from its source",
            )
            if im.width * im.height > score:
                chosen = im
                score = im.width * im.height
        if len(candidates) > 1:
            duplicates += 1
        if chosen is None:
            continue
        # Generated model UVs use the opposite origin from the source pack.
        im = chosen.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        for digest in {ha, f"{legacy(raw,len(raw),t['width'],t['height']):016X}"}:
            im.save(dest / f"tex1_{t['width']}x{t['height']}_{digest}_13_mip0.png")
        matched += 1
    (stage / "texture-overrides" / "pack.json").write_text(
        json.dumps(
            {
                "options": {
                    "skip_mipmap": True,
                    "flip_png_files": True,
                    "use_new_hash": True,
                }
            }
        )
    )
    return json.dumps({"matched": matched, "duplicate_hashes": duplicates})
