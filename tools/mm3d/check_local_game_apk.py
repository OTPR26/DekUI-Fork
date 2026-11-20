"""Check the isolated emulator boot artifact before upload."""
import os
from pathlib import Path
import shutil
import subprocess
import zipfile

apks = list(Path("src/android/app/build").glob("intermediates/apk/vanilla/performance/*.apk"))
if len(apks) != 1:
    raise SystemExit("Expected one prototype APK")
sdk = Path(os.environ["ANDROID_HOME"]) / "build-tools/36.1.0"
badging = subprocess.check_output([sdk / "aapt", "dump", "badging", apks[0]], text=True)
if "name='io.github.otpr26.dekuifork.localtest'" not in badging.splitlines()[0]:
    raise SystemExit("Wrong test app identity")
with zipfile.ZipFile(apks[0]) as apk:
    setup = {name for name in apk.namelist() if name.startswith("assets/dekui-setup/") and not name.endswith("/")}
    if setup != {"assets/dekui-setup/supported-rom.json"}:
        raise SystemExit("Unexpected game setup input in APK")
    for name in apk.namelist():
        lower = name.lower()
        if (lower.endswith((".3ds", ".cci", ".cia", ".bps", ".gar", ".mfpj")) or
            Path(lower).name in {"code.bin", "romfs.bin", "exheader.bin", "libtriaevum.so"} or
            "/project-restoration/" in lower):
            raise SystemExit(f"Unexpected game payload: {name}")
out = Path("artifacts/Local-Game-Android-arm64.apk")
out.parent.mkdir(exist_ok=True)
shutil.copy2(apks[0], out)
