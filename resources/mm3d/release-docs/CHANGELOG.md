# Changelog

## 1.0.0-beta.2

### Added

- 60fps CPU interpolation (default) and GPU interpolation, with an Original 30fps option under Graphics.
  Interpolation is still a work in progress; expect artifacts and some textures/models running at 30fps.
- New toggleable restoration features:
  - **Classic saving:** Saving that closely mimics the N64 original. Saving at an owl statue creates a temporary
    save. Saving returns to the title screen only if a save from a Song of Time reset already exists.
    A glowing icon will be visible next to your profile name indicating
    the temporary save. The temporary save is consumed when loaded; the permanent Song of Time save remains.
    You cannot save at feather statues while Classic Saving is enabled.
    Avoid switching this on and off to avoid unexpected behavior. The default is Project Restoration behavior.
  - **Classic Song of Double Time:** Mimics N64 behavior of advancing time to only dawn or night.
  - **Bombers don't approach automatically:** Stops Bombers from running up to you.
  - **Activate owl statues with a sword:** Owl statues can only be activated by hitting
    them. Skips Tatl telling you about the statue in Clock Town and automatic activation.
- Draw-distance sliders for NPCs and other distant objects.
- New QoL tab to disable Tatl interruptions and low health beeping.
- New 3x prices and fees difficulty option.
- New double defense difficulty option (reduces damage by 1/3 instead of 1/2).
- New modifiable HUD elements (minimap opacity and location banner positioning).
- Apple Silicon macOS support.
- French and Spanish language support that includes the new button prompts and messages.
- Configurable hotkeys for fullscreen and custom textures.

### Improved

- Reduced CPU interpolation overhead and frame-presentation stalls.
- Reworked the brown Majora-themed sidebar on "Select an item" and "Songs" screens. Select an item
  now doesn't use a background and the song list uses a cleaner semi-transparent background.
- Menus now adapt to screen shrinking at the end of Days 1 and 2.
- Ocarina prompts now remain visible while the song list is open.
- Added a fading backdrop behind the Song of Double Time selector.
- Added F1 hint at startup so users know how to access the settings menu.
- Fullscreen/windowed preference is now remembered; Esc exits fullscreen.

### Fixed

- Removed accidentally left in unused editor profiles and stopped automatic editor snapshots in release builds.
- Reduced cutscene FOV by 2 degrees to prevent missing geometry being visible.
- Fixed gyro input failing after controller reconnection.
- Fixed Gear-screen issues, including particles and rain still moving while paused.
- Fixed controller mapping incorrectly detecting stick movement when assigning buttons.

Android support is planned next...

## 1.0.0-beta.1 (2026-10-02)

- Initial release.
