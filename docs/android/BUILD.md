# Building the Android preview

The Android performance flavor is the shipping configuration. It uses optimized native code and disables Android debugging and the test-only flag.

## Prerequisites

- Git and the recursive submodules from `.gitmodules`.
- JDK 17 or newer supported by the Gradle wrapper, Android SDK/NDK versions requested by the project, CMake, and Ninja.
- Qt for Android ARM64 and a matching desktop host Qt installation. The tested build used Qt 6.12.0.
- Python 3 for the presentation-data generation tools.

Clone and initialize dependencies:

```sh
git clone --recursive https://github.com/OTPR26/DekUI-Fork.git
cd DekUI-Fork
```

Set your Android SDK and Qt paths, then build:

```sh
export ANDROID_HOME=/path/to/android-sdk
export DEKUI_ANDROID_QT_ROOT=/path/to/Qt/6.12.0/android_arm64_v8a
export DEKUI_HOST_QT_ROOT=/path/to/Qt/6.12.0/your-desktop-kit
cd src/android
./gradlew :app:assembleVanillaPerformance \
  -Pandroid.injected.build.abi=arm64-v8a \
  -Pandroid.injected.testOnly=false \
  -PdekuiPresentation=true \
  -PdekuiQtRoot="$DEKUI_ANDROID_QT_ROOT" \
  -PdekuiQtHost="$DEKUI_HOST_QT_ROOT"
```

The APK is written under `src/android/app/build/outputs` or Gradle’s APK intermediates, depending on the Android Gradle Plugin version. A local build uses your local signing key; release APKs must keep the same signing key to update an existing installation.

Pure Kotlin model and voice converter checks are in `tests/android/adult_link`. They accept paths to local user-owned source assets; those assets are not included. Native feature checks are in `tools/mm3d`.

Windows, Linux, and macOS support is coming soon. Their inherited workflows are manual-only while the Android fork is prepared for wider testing.
