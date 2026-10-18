# Installation

## What you need

A supported decrypted USA v1.1 game-card dump in `.cci` or `.3ds` format, from a card with v1.1 preinstalled. The filename can be anything. Select it from the startup screen; it can remain in its existing folder.

Project Restoration and the required HUD layouts are included. Optional custom texture packs are separate.

## Windows

1. Extract `Dekui-1.0.0-beta.2-Windows-x64.zip` into a writable location and open the `dekui` folder.
2. Keep `Dekui.exe`, its DLLs, and the `plugins`, `tools`, `resources`, and `runtime` folders together.
3. Open `Dekui.exe` and select your game from the startup screen.

Windows 10/11 x64 and a Vulkan-capable graphics driver are required. Python is bundled privately for setup; you do not need to install it or modify PATH. The application is unsigned.

## Linux

1. Extract `Dekui-1.0.0-beta.2-Linux-x86_64.tar.gz` and open the `dekui` folder. If downloading a build artifact from Actions, extract its outer ZIP first.
2. Put the extracted folder somewhere writable under your home directory. Keep `Dekui.AppImage`, `tools`, and `resources` together.
3. Make the AppImage executable if needed; select your game from its startup screen.
4. Launch `Dekui.AppImage`.

Python 3.9 or newer and a Vulkan-capable graphics driver are required. Keep Python installed for startup resource checks and upgrades.

If FUSE is unavailable, launch from a terminal with:

```sh
APPIMAGE_EXTRACT_AND_RUN=1 ./Dekui.AppImage
```

## macOS (Apple Silicon)

1. Extract `Dekui-1.0.0-beta.2-macOS-arm64.zip`. If downloading a build artifact from Actions, extract its outer ZIP first.
2. Move the entire `dekui` folder with Finder to a writable location, such as Documents. Keep `Dekui.app`, `tools`, and `resources` together; do not move only the app.
3. Open the app and select your game from the startup screen.
4. If macOS blocks the app because the developer cannot be verified, open **System Settings → Privacy & Security → Open Anyway**, then confirm **Open**.

An Apple Silicon Mac (M1 or newer) running macOS 13.4 or newer is required. This package does not support Intel Macs. Python and app dependencies are bundled; Homebrew and developer tools are not required. The app is ad-hoc signed and is not Apple-notarized.

If the app cannot find its adjacent files after downloading, quit it, move the entire extracted folder with Finder, and reopen it.

## First launch and later launches

The startup screen offers Start game / Select ROM, Settings, About and Quit. First launch validates the selected game and prepares required files in `user/`, with a progress window. Unsupported files are rejected before changes are made.

Setup references your selected game in place. Later launches check for resource upgrades while preserving saves and custom settings. Keep the complete application package together, including its tools and resources.

## Settings

Use Settings on the startup screen. In game, choose DekUI Fork → Settings, press Command-comma on macOS or Ctrl-comma elsewhere, or press Start + Select together on a controller. F1 remains an alternative. Confirm/back follow the saved controller bindings.

The same sidebar organizes General, Controls, Touch, Graphics, Audio, HUD, Difficulty, Restoration, Quality of life, Mods, Cheats and About. Graphics includes an existing custom texture-folder picker. Touch → Swap screens exposes the game's touchscreen for mouse input.

Before gameplay, Mods accepts your own decrypted OoT3D ROM and optional existing texture pack, then generates Adult Link or Dark Link locally. Adult Link supports tunic colors and an optional Master Sword. Imported mods include adult voices. Restart gameplay after changing models.

Hold ZR and press SELECT during gameplay to toggle the minimap. In-game cheats require a recognized loaded save.

## Updates and saves

1. Close Dekui and back up your entire `user` folder.
2. Extract the complete new release into a separate writable folder.
3. Copy your existing `user` folder and game file beside the new application.
4. Launch the new version.

Saves, HUD settings, graphics preferences, custom textures, and unrelated mods are preserved. On first launch, recognized older menu layouts update automatically with backups; customized layouts are kept. To restore the current Songs, file-select, and item-selection layouts manually, use **General → Reset menu layouts**. Keep the app with its bundled `tools`, `resources`, and any runtime folders.

If the upgrade fails or is interrupted, the game will not open; relaunch to retry. Resource backups are retained under `user/setup/resource-upgrade-backup-*` after success, or `user/setup/resource-upgrade-pending/backup` while an upgrade is pending. Do not delete your `user` folder or these backups to troubleshoot.
