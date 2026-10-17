# Android setup

Android is available as a preview. Windows, Linux, and macOS are coming soon.

## Base game

Select your own MM3D USA Rev 1 (v1.1) ROM. Game data is prepared inside the app’s storage. Choose the folder containing your MM3D custom textures in Settings; Android grants access to that folder through its system picker. You can use an existing Azahar texture directory instead of copying an entire pack.

Use the in-game file-select screen to load saves. A save state restores emulated memory and may restore an old model even after changing generated mod files.

## Adult Link

1. At the startup screen, open Settings → Mods and enable Adult Link.
2. Select your own OoT3D ROM. The importer recognizes the supported USA, Europe, and Japan title IDs; individual ROM modifications can affect compatibility.
3. Optionally select your existing OoT3D texture directory. Only matching textures are imported; the importer does not invent a high-resolution replacement for missing textures.
4. Generate the mod and choose a tunic color and Master Sword preference.
5. Start the game. Relaunch gameplay after changing model, tunic, or sword selection.

Generation builds the converted model and imports Adult Link’s voice samples from your selected ROM. Green, blue, and red use the original OoT3D tunic variants. Selecting Master Sword replaces the held blade, sheath, and regular sword HUD icons; it does not grant new damage or story progression.

The model has adult height while equipment dimensions are retained. Torch contact is adjusted for the taller hand joint. Adult Link is an appearance/audio mod, with normal MM3D human gameplay rules. Disable it and relaunch to restore the original model and voices.

## Controls and settings

Swipe in from the left edge to open the full-screen menu. Set Nintendo/Xbox prompt preference and actual button bindings in Controls. L1/R1 item assignments and L2/R2 targeting/shielding can be customized. The startup screen follows the selected controller prompt layout.

Touch overlay controls remain available for phones. Menu scale can be adjusted for smaller screens or handhelds. Field of View changes the view angle; Camera Distance moves the camera farther from or closer to Link. Movement speed controls are separate for walking, climbing, and swimming.

Disk shader cache is currently disabled in this preview configuration after testing found startup stalls and geometry artifacts with the Adult Link model. Do not treat the experimental Cheats page as a save editor: its compatibility guards may leave it unavailable.
