#!/usr/bin/env python3
from pathlib import Path
import json
root = Path(__file__).resolve().parents[3]
asset = root / "src/android/app/src/main/assets/dekui/presentation.json"
assert json.loads(asset.read_text())["fix_bloom"] is True
setup = (root / "src/android/app/src/main/java/org/citra/citra_emu/utils/DekuiAndroidSetup.kt").read_text()
for token in ("DocumentFile.fromTreeUri", "installDefaults", "looksLikeMm3dContainer", "NCSD"):
    assert token in setup
contract = (root / "src/android/app/src/main/java/org/citra/citra_emu/contracts/OpenFileResultContract.kt").read_text()
assert "CATEGORY_OPENABLE" in contract and "FLAG_GRANT_PERSISTABLE_URI_PERMISSION" in contract
print("Android setup and SAF checks passed")
