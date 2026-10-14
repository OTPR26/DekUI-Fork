#!/usr/bin/env python3
from pathlib import Path
import json
root = Path(__file__).resolve().parents[3]
source = root / "resources/mm3d/defaults/presentation.json"
asset = root / "src/android/app/src/main/assets/dekui/presentation.json"
assert asset.is_file(), asset
source_data = json.loads(source.read_text())
asset_data = json.loads(asset.read_text())
assert asset_data == source_data, "Android asset differs from packaged defaults"
required = ("single_screen_text", "fix_bloom", "red_potion_hearts", "classic_saving")
assert all(key in asset_data for key in required)
print(f"Android Dekui asset check passed ({asset.stat().st_size} bytes)")
