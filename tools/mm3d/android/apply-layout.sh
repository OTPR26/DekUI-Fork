#!/usr/bin/env bash
# Rebuild and deploy the authored presentation to the local Android test install.
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
export DEKUI_ANDROID_QT_ROOT=${DEKUI_ANDROID_QT_ROOT:-"$HOME/Qt/6.12.0/android_arm64_v8a"}
export DEKUI_HOST_QT_ROOT=${DEKUI_HOST_QT_ROOT:-"$HOME/Qt/6.12.0/macos"}
adb_bin=${ADB:-"$HOME/Library/Android/sdk/platform-tools/adb"}
rom=${1:-"$HOME/Documents/Legend of Zelda, The - Majora's Mask 3D (USA) (En,Fr,Es) (Rev 1).cci"}
# This helper targets the user folder already selected for this experiment.
remote=/sdcard/Azahar
package=io.github.tgacquin.dekui.experiment.debug
"$adb_bin" get-state
"$adb_bin" shell test -d "$remote/config"
stage=$(python3 "$repo_root/tools/mm3d/android/prepare-layouts.py" "$rom" --output "$repo_root/artifacts/android")
bash "$repo_root/tools/mm3d/android/build-presentation.sh"
apk="$repo_root/src/android/app/build/intermediates/apk/vanilla/debug/app-vanilla-debug.apk"
python3 - "$apk" "$repo_root/resources/mm3d/defaults" <<'PY'
from pathlib import Path
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as apk:
    for source in Path(sys.argv[2]).rglob('*.json'):
        member = 'assets/dekui-defaults/' + source.relative_to(sys.argv[2]).as_posix()
        if apk.read(member) != source.read_bytes():
            raise SystemExit('APK defaults mismatch: ' + member)
    for lib in ('libcitra-android.so', 'libQt6Core_arm64-v8a.so', 'libc++_shared.so'):
        if 'lib/arm64-v8a/' + lib not in apk.namelist():
            raise SystemExit('APK missing ' + lib)
print('APK presentation defaults and native libraries verified.')
PY
"$adb_bin" install -r -t "$apk"
"$adb_bin" shell am force-stop "$package"
backup="$remote/dekui-layout-backup-$(date +%Y%m%d-%H%M%S)"
"$adb_bin" shell "mkdir -p '$backup' '$remote/load/mods' '$remote/config/mm3d-original/romfs' && if [ -d '$remote/load/mods/0004000000125500' ]; then cp -R '$remote/load/mods/0004000000125500' '$backup/'; fi"
"$adb_bin" push "$stage/mods/0004000000125500" "$remote/load/mods/"
"$adb_bin" push "$stage/original/romfs/layout" "$remote/config/mm3d-original/romfs/"
"$adb_bin" shell am start -n "$package/org.citra.citra_emu.ui.main.MainActivity"
echo "Layout installed. Open the game. Previous mods (if present): $backup"
