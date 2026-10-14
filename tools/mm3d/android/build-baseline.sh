#!/usr/bin/env bash
# Android frontend baseline only: Dekui presentation/setup are not ported yet.
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
android_sdk=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-"$HOME/Library/Android/sdk"}}
if [[ ! -d "$android_sdk/platform-tools" ]]; then
  echo 'Set ANDROID_HOME to an installed Android SDK.' >&2
  exit 1
fi
export ANDROID_HOME="$android_sdk"
export ANDROID_SDK_ROOT="$android_sdk"
cd "$repo_root/src/android"
./gradlew :app:assembleVanillaDebug -Pandroid.injected.build.abi=arm64-v8a \
  --no-daemon --console=plain --max-workers=4 "$@"
