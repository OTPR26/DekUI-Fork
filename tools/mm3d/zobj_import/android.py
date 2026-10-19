"""On-demand Android bridge for data-only PAK model imports."""

import json
from .install import generate_and_install


def import_model(root, mm_rom, package, bridge):
    report = generate_and_install(
        str(root), str(mm_rom), str(package), lambda message: bridge.progress(message)
    )
    return json.dumps(
        {
            "name": str(report.get("source_name", "Player model"))[:256],
            "entry": str(report.get("entry", ""))[:256],
        }
    )
