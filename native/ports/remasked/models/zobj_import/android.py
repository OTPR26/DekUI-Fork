"""On-demand Android bridge for data-only PAK model imports."""

import json
from .install import generate_and_install
from .errors import message


def import_model(root, mm_rom, package, bridge):
    try:
        report = generate_and_install(
            str(root), str(mm_rom), str(package), lambda text: bridge.progress(text)
        )
    except Exception as error:
        return json.dumps({"error": message(error)})
    return json.dumps(
        {
            "name": str(report.get("source_name", "Player model"))[:256],
            "entry": str(report.get("entry", ""))[:256],
        }
    )
