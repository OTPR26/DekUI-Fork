# Majora ReMasked European Rev 1 preview

Use a decrypted European Rev 1 cartridge dump (`.3ds` or `.cci`) of Majora’s Mask 3D. This preview identifies the cartridge by its main partition checksum. Choose the file from the ReMasked launcher and let setup finish before starting the game.

The preview uses its own saves and settings. Android installs alongside the standard app and uses `mm3dport-eur-preview`. On Windows and Linux, extract the entire preview archive into a new folder and keep the `eur-preview` marker beside the app. On iPhone and iPad, sign the unsigned IPA for your device. Apple devices keep preview saves local.

## Preparing iPhone and iPad setup

In Files, create `restoration` under **On My iPhone/iPad → ReMasked EUR Preview**. Provide these inputs from the pinned Project Restoration packages:

- `restoration/code.bps` and `restoration/exheader.bin` from the [v110 Citra standard-aiming patch](https://github.com/leoetlino/project-restoration/releases/download/v1.5.9/mm3d_project_restoration_plus_hd_hud_v1.5.9-5-g8bb2a7b.7z).
- `restoration/romfs/layout/Joker.Main.New.gar`, `Joker.Main.Play.gar`, and `Joker.mfpj` from the [pinned HD HUD layout archive](https://zora.re/dl/mm3d_big_screen_layout_779512b.7z).

Setup checks those inputs before reconstructing the ReMasked HUD. The IPA includes the ReMasked HUD artwork and reconstruction recipes.

## What to test

1. Choose the game and start it. Check the title screen and file selection.
2. Confirm the neutral face-button rings, bumper outlines, and D-pad HUD in gameplay.
3. Save, reopen the preview, and load that slot. Verify the regular app still shows its own saves.
4. If reporting an issue, include the platform, selected game language, and the step where it happened.

The preview uses the cartridge’s original English, French, Spanish, German, and Italian dialogue. Classic Saving and the Classic Song of Double Time are omitted from this preview.
