# Majora ReMasked European Rev 1 preview

In the regular Majora ReMasked app, choose a decrypted USA Rev 1 or European Rev 1 cartridge dump (`.3ds` or `.cci`). To test European support, choose your European Rev 1 dump from the launcher or startup Settings and let setup finish. On desktop, ReMasked reopens in the selected region when you switch cartridges.

European saves are separate from USA saves on every platform. Choose your USA dump again from startup Settings to return to that region. On iPhone and iPad, sign the unsigned IPA for your device. European saves stay local on Apple devices.

## Preparing iPhone and iPad setup

In Files, create `restoration` under **On My iPhone/iPad → Majora ReMasked**. Provide these inputs from the pinned Project Restoration packages:

- `restoration/code.bps` and `restoration/exheader.bin` from the [v110 Citra standard-aiming patch](https://github.com/leoetlino/project-restoration/releases/download/v1.5.9/mm3d_project_restoration_plus_hd_hud_v1.5.9-5-g8bb2a7b.7z).
- `restoration/romfs/layout/Joker.Main.New.gar`, `Joker.Main.Play.gar`, and `Joker.mfpj` from the [pinned HD HUD layout archive](https://zora.re/dl/mm3d_big_screen_layout_779512b.7z).

Setup checks those inputs before reconstructing the ReMasked HUD. The IPA includes the ReMasked HUD artwork and reconstruction recipes.

## What to test

1. Choose the game and start it. Check the title screen and file selection.
2. Confirm the neutral face-button rings, bumper outlines, and D-pad HUD in gameplay.
3. Save, reopen the preview, and load that slot. Switch back to USA Rev 1 and verify that its saves are still present.
4. If reporting an issue, include the platform, selected game language, and the step where it happened.

The iPhone and iPad language menu shows English, French, and Spanish for USA Rev 1. Choosing European Rev 1 adds German and Italian and retains the original European dialogue. Classic Saving and the Classic Song of Double Time are available for USA Rev 1.
