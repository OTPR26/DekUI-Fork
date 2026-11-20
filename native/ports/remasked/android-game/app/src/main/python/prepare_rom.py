# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare local game inputs for the native Android launcher."""

import json
from pathlib import Path
from tempfile import TemporaryDirectory

from build_process_manifest import build_manifest
from ctr_rom import extract_decrypted_rom


def prepare(rom_path, staging_path, expected_code_hash):
    staging = Path(staging_path)
    # Keep unvalidated output separate from the files the launcher will publish.
    with TemporaryDirectory(dir=staging) as scratch:
        inputs = extract_decrypted_rom(
            Path(rom_path),
            Path(scratch) / "inputs",
            max_code_bytes=32 * 1024**2,
            expected_program_id=0x0004000000125500,
            expected_code_sha256=expected_code_hash,
        )

        manifest = build_manifest(inputs.exheader.path, inputs.code.path, inputs.romfs.path)
        for key, source in (
            ("exheader_path", inputs.exheader),
            ("code_bin_path", inputs.code),
            ("romfs_image_path", inputs.romfs),
        ):
            manifest["source"][key] = source.path.name
            source.path.rename(staging / source.path.name)
        (staging / "process-manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
