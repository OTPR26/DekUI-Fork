# Android setup

Use the Android ARM64 package from Releases. Desktop packages have separate installation instructions.

## Base game

Select your own MM3D USA Rev 1 (v1.1) ROM. Generated game data, settings, saves, and mods are stored in Internal storage/mm3dport. Grant file access when prompted. Choose the folder containing your MM3D custom textures in Settings; Android grants access to that folder through its system picker. You can use an existing Azahar texture directory instead of copying an entire pack.

Use the in-game file-select screen to load saves. A save state restores emulated memory and may restore an old model even after changing generated mod files.

## Adult Link and Dark Link

1. At the startup screen, open Settings → Mods and choose Adult Link or Dark Link.
2. Select your own OoT3D ROM. The importer recognizes the supported USA, Europe, and Japan title IDs; individual ROM modifications can affect compatibility.
3. Optionally select your existing OoT3D texture directory. Only matching textures are imported; the importer does not invent a high-resolution replacement for missing textures.
4. Generate the mod and choose a tunic color and Master Sword preference.
5. Start the game. Relaunch gameplay after changing model, tunic, or sword selection.

Generation builds the converted model and imports Adult Link’s voice samples from your selected ROM. Dark Link uses its own dark model, red eyes, sword appearance, and adult voices. Green, blue, and red use the original OoT3D tunic variants. Selecting Master Sword replaces the held blade, sheath, and regular sword HUD icons; it does not grant new damage or story progression.

The model has adult height while equipment dimensions are retained. Torch contact is adjusted for the taller hand joint. Adult Link is an appearance/audio mod, with normal MM3D human gameplay rules. Disable it and relaunch to restore the original model and voices.

## Controls and settings

Swipe in from the left edge to open the full-screen menu. Set Nintendo/Xbox prompt preference and actual button bindings in Controls. L1/R1 item assignments and L2/R2 targeting/shielding can be customized. The startup screen follows the selected controller prompt layout.

Touch overlay controls remain available for phones. Menu scale can be adjusted for smaller screens or handhelds. Field of View changes the view angle; Camera Distance moves the camera farther from or closer to Link. Movement speed controls are separate for walking, climbing, and swimming.

Disk shader cache is currently disabled in this preview configuration after testing found startup stalls and geometry artifacts with the Adult Link model. Do not treat the experimental Cheats page as a save editor: its compatibility guards may leave it unavailable.

## Upgrading from the prototype

The release uses `io.github.otpr26.dekuifork`. Android treats it as a new app,
so it installs beside the early prototype. Close the prototype first, grant
file access in the new app, and keep Internal storage/mm3dport in place. The
new app reuses its prepared game, saves, generated mods, HUD, and controller
bindings. Reselect external texture folders; Android folder grants belong to
the old app ID. Select the OoT3D source again only when regenerating a mod.
After checking the new app, the prototype may be uninstalled.

The Pictograph Box defaults to R3 (right-stick click), independent of equipped items. Change or clear its binding in Controls. Android Back opens settings; the L3+R3 settings shortcut also remains available.

Before launching, choose English, French or Spanish under Settings → General → Game language. Startup and in-game settings share the saved language; changes during gameplay apply after restarting the game. Settings menus remain in English.
