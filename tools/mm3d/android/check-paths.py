#!/usr/bin/env python3
from pathlib import Path
root = Path(__file__).resolve().parents[3]
source = (root / "src/mm3d/notebook.cpp").read_text()
assert "defined(__ANDROID__)" in source
android = source.split("#elif defined(__ANDROID__)", 1)[1].split("#else", 1)[0]
assert "QDir::homePath" not in android
assert "ConfigDir" in android
print("Android desktop-path guard check passed")
