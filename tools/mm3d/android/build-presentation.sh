#!/usr/bin/env bash
# Build the experimental Android presentation frontend; local game assets deploy separately.
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
: "${DEKUI_ANDROID_QT_ROOT:?Set DEKUI_ANDROID_QT_ROOT to the Android ARM64 Qt SDK}"
: "${DEKUI_HOST_QT_ROOT:?Set DEKUI_HOST_QT_ROOT to the matching host Qt SDK}"
bash "$repo_root/tools/mm3d/android/build-baseline.sh" \
  -PdekuiPresentation=true \
  "-PdekuiQtRoot=$DEKUI_ANDROID_QT_ROOT" \
  "-PdekuiQtHost=$DEKUI_HOST_QT_ROOT" "$@"
