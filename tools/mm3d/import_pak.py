"""Desktop entry point for importing a user-selected PAK player model."""

import argparse
import json
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--mm-rom")
    parser.add_argument("--model")
    parser.add_argument("--probe-runtime", action="store_true")
    args = parser.parse_args()
    sys.path.insert(0, str(args.package / "runtime/tp-modules"))
    from zobj_import.install import generate_and_install

    if args.probe_runtime:
        from zobj_import.convert import texture
        from zobj_import.vendor.gbi import TileState

        assert texture({}, TileState()).shape == (8, 8, 4)
        print("PAK importer runtime loaded successfully.")
        return
    if not args.root or not args.mm_rom or not args.model:
        parser.error("--root, --mm-rom and --model are required")
    report = generate_and_install(
        args.root / "user",
        args.mm_rom,
        args.model,
        lambda message: print(json.dumps({"progress": message}), flush=True),
    )
    print(
        json.dumps(
            {
                "installed": True,
                "name": str(report.get("source_name", "Player model"))[:256],
                "entry": str(report.get("entry", ""))[:256],
            }
        ),
        flush=True,
    )


if __name__ == "__main__":
    main()
