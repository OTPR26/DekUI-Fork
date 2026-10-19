# Majora ReMasked changes

## Majora ReMasked 0.4.0 — PAK Importer
*A DekUI Fork*

- New Majora ReMasked branding across all platforms.
- Import supported player-model PAK files through Settings → Mods.
- Fix Android game-language settings not persisting after restarting.
- Correct HUD defaults on fresh installations.
- Replace second-screen item labels I/II with L/R.
- Display the app version on the startup screen.

## 0.2.0 — Android and Desktop

- Shared startup and settings experience across Android, Windows, Linux, and macOS.
- Adult Link and Dark Link import from a user-supplied OoT3D ROM, including adult
  voices and matching textures when supplied. Adult Link supports original tunic
  colors and an optional Master Sword with matching HUD icons.
- Shared high-resolution HUD defaults and persistent controller mappings.
- Separate field of view, camera distance, camera recovery, and movement speeds.
- Controller-neutral English item prompts, touch options, and full-screen menus.
- Public Android data folder and macOS Application Support storage, with verified
  migration and preservation of saves and settings.
- Correct Linux launcher icon and readable desktop dropdown menus.
- Upstream DekUI 1.0 interpolation, notebook, mask-selection, widescreen, and
  presentation fixes; optional fog, cutscene FOV, and time settings.
- Separate Fast Time and movement-speed hooks to preserve both features.
- Strip local build paths and reject private paths and macOS metadata in packages.

Desktop and Android release candidates require a fresh validation pass after
upstream integration. The earlier platform playtests do not validate this update.

## Upstream history

> Preserved upstream notes. For this fork’s Android release and current limitations, start with [README.md](README.md).

## 1.0.0-beta.2

### Added
- **60 FPS interpolation**
  - CPU interpolation (default) and GPU interpolation.
  - Original 30 FPS option available under Graphics.
  - **Note:** Interpolation is a work in progress. Expect visual artifacts, some textures/models still running at 30 FPS, and potential slowdowns on lower-end hardware. These issues will continue to be addressed in future updates.
- **French and Spanish language support**
  - Includes translated button prompts and messages.
- **New toggleable restoration features**
  - **Classic Saving**
    - Closely mimics the original N64 saving system.
    - Saving at an owl statue creates a temporary save.
    - Saving returns to the title screen only if a Song of Time save already exists.
    - A glowing icon appears next to your profile name to indicate a temporary save.
    - Temporary saves are consumed when loaded; permanent Song of Time saves remain.
    - Saving at feather statues is disabled while Classic Saving is enabled.
    - Avoid repeatedly toggling this feature to prevent unexpected behavior.
    - **Default:** Project Restoration saving behavior.
  - **Classic Song of Double Time**
    - Mimics N64 behavior by advancing time only to dawn or night.
  - **Bombers Don't Approach Automatically**
    - Prevents Bombers from automatically running up to you.
  - **Activate Owl Statues with a Sword**
    - Owl statues can only be activated by striking them.
    - Skips Tatl's explanation in Clock Town and automatic activation.
- **Draw-distance sliders**
  - Adjustable draw distance for NPCs and other distant objects.
- **New Quality-of-Life (QoL) tab**
  - Disable Tatl interruptions.
  - Disable low-health beeping.
- **New difficulty options**
  - 3× prices and fees.
  - Double defense (reduces damage by 1/3 instead of 1/2).
- **Customizable HUD elements**
  - Minimap opacity.
  - Location banner positioning.
- **Apple Silicon macOS support**
- **Configurable hotkeys**
  - Fullscreen.
  - Custom textures.

### Improved
- Reduced CPU interpolation overhead and frame-presentation stalls.
- **Reworked Majora-themed sidebars**
  - "Select an Item" no longer uses a background.
  - "Songs" now uses a cleaner, semi-transparent background.
- Menus now adapt to screen shrinking at the end of Days 1 and 2.
- Ocarina prompts remain visible while the song list is open.
- Added a fading backdrop behind the Song of Double Time selector.
- Added an F1 hint at startup to help users locate the settings menu.
- Fullscreen/windowed preference is now remembered.
- Pressing Esc exits fullscreen.

### Fixed
- Removed unused editor profiles accidentally included in previous builds.
- Disabled automatic editor snapshots in release builds.
- Reduced cutscene FOV by 2 degrees to prevent missing geometry from being visible.
- Fixed gyro input failing after controller reconnection.
- Fixed Gear-screen issues, including particles and rain continuing to move while paused.
- Fixed controller mapping incorrectly detecting stick movement when assigning buttons.

### Coming Soon
- Further interpolation optimizations and improvements.
- Android support.

## 1.0.0-beta.1 (2026-10-02)

- Initial release.
