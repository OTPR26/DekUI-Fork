Majora ReMasked for Linux

1. Extract the download into a writable location. If downloading from GitHub
   Actions, extract the ZIP first, then the tar.gz inside it. Open the dekui folder.
2. Put exactly one decrypted USA v1.1 game-card dump (.cci or .3ds) beside
   Majora-ReMasked.AppImage. Any filename is accepted. Use a card with v1.1 preinstalled.
3. Open Majora-ReMasked.AppImage. If needed, enable Properties > Permissions > Is executable.
   First launch silently checks the game and prepares the included Restoration
   and HD HUD files before opening the game. Allow extra time on first launch.
   No file-selection prompts or separate mod downloads are needed.

Keep the AppImage, tools and resources together. Keep your game file beside the
AppImage and preserve user/ (including saves) when updating. The game is used in
place, never moved or duplicated. Missing or unsupported games block gameplay.
An incomplete user/
folder is not automatically replaced; preserve it and use a fresh application
folder rather than deleting potential saves.

Python 3.9+ is required for setup, startup resource checks and upgrades. A Vulkan-capable graphics driver is required.

Press F1 for General, Graphics, HUD, Difficulty, Restoration, QOL, Audio,
Controls and About. Fresh setups start at 4x resolution; existing settings are
preserved. Game touch input is disabled; touch still works in settings.
Controller mapping opens a separate dialog. Shader preparation shows a loading
screen inside the game window.

Updating: close Dekui and back up the entire user/ folder. Extract the new
release into a separate writable folder, then copy user/ and your game beside
the new application.
Launch the new version. Saves, settings, textures and unrelated mods are preserved.
If an upgrade is interrupted, relaunch to retry. Keep user/ and upgrade backups.

If FUSE is unavailable, run from a terminal:
APPIMAGE_EXTRACT_AND_RUN=1 ./Majora-ReMasked.AppImage

Optional custom textures: use F1 > Graphics > Open texture folder, install a
compatible pack, enable Use custom textures, then restart Dekui. The folder is
user/load/textures/0004000000125500/. Texture packs are not included.

Project Restoration includes its license and matching source under
resources/mm3d/project-restoration. Game files are not included. Source and
license notices accompany the download.

For troubleshooting, keep the error message and user/log/azahar_log.txt.
Do not share ROMs, keys or saves.
