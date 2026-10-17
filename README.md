# DekUI Fork

**An Android emulator fork tailored for a single-screen Majora’s Mask 3D experience.**

Android is available now. **Windows, Linux, and macOS are coming soon.**

DekUI Fork builds on [tgacquin’s DekUI](https://github.com/tgacquin/mm3d-dekui), the [Azahar emulator](https://github.com/azahar-emu/azahar), and [Project Restoration](https://restoration.zora.re/). It runs MM3D through an adapted 3DS emulator; it is not a game recompilation or a native rewrite of the game.

[Download the Android preview](https://github.com/OTPR26/DekUI-Fork/releases) · [Android setup](docs/android/SETUP.md) · [Build instructions](docs/android/BUILD.md)

![DekUI Fork startup screen on the Odin 2 Portal](docs/artwork/android-startup.jpg)

## Android features

- Single-screen gear, masks, items, maps, file select, and Bomber’s Notebook inherited from DekUI.
- A full-screen menu designed for touch and controllers; swipe in from the left edge during gameplay.
- Controller bindings and button-layout preferences, touch overlay controls, and screen swapping.
- Adjustable HUD positions, scale, spacing, and visibility; custom texture-folder selection through Android’s file picker.
- Separate Field of View and Camera Distance controls; adjustable walk, climb, and swim speeds.
- Optional controller-neutral English item prompts.
- Adult Link appearance mod generated on the device from your own OoT3D ROM, with adult voice clips imported from that ROM.
- Original green, blue, and red tunic variants, an optional Master Sword model and matching HUD icon, and matching textures from your own OoT3D texture pack when available.
- Adult Link torch compatibility: Deku sticks can light torches and burn cobwebs with the taller model.
- Optimized ARM64 Android build. Development and gameplay testing have focused on the Odin 2 Portal.

## Get started

1. Install the APK from [Releases](https://github.com/OTPR26/DekUI-Fork/releases).
2. Select your own **Majora’s Mask 3D USA Rev 1 / v1.1** ROM during setup.
3. Open Settings to choose a custom-texture folder, controller bindings, and HUD options.
4. For Adult Link, open **Settings → Mods**, select your own supported OoT3D ROM and optional texture folder, then generate the mod before starting gameplay.

No game ROM, imported Adult Link model, imported voices, save file, or personal texture pack is supplied with the release. Generated assets stay on your device. Changing Adult Link, tunic, sword, or voice selection requires restarting gameplay. See [setup](docs/android/SETUP.md) for details.

## Preview limitations

This is an early Android preview. Some masks, facial expressions, and attachments still need broader testing with Adult Link. Texture replacement depends on a matching texture being present in the selected pack. The experimental Cheats page can remain disabled when its live save-layout check does not match; it is not advertised as a reliable save editor.

Desktop sources and inherited build workflows are present, but this fork’s Windows, Linux, and macOS releases are not ready yet. The Android APK uses the existing installation ID to preserve saves and settings for testers; its historical `.debug` suffix does not mean that release is a debug build.

## Credits and license

- **tgacquin / DekUI:** single-screen MM3D design, game presentation, and the original fork.
- **Azahar and Citra contributors:** emulator foundations.
- **leoetlino / Project Restoration contributors:** Restoration patches and features.
- Additional libraries and contributors retain their original notices throughout the source tree.

This project retains its upstream GPL licensing; see [license.txt](license.txt) and the notices in individual files and dependencies. DekUI Fork is an unofficial fan project and is not affiliated with Nintendo. The new Deku flower branding and Fierce Deity startup artwork are included with this fork; inherited emulator credits remain in place.

For the upstream DekUI feature and development notes, see [the preserved upstream README](docs/UPSTREAM-DEKUI.md).
