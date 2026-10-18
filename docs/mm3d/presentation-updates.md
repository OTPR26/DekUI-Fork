# DekUI 1.0 presentation integration

## Menus

The moon's mask-selection screen has independent placement and prompt settings.
Its mask grid, selection cursors, title, and transitions route to the main screen.
Missing mask settings receive defaults; existing custom positions remain intact.

Notebook fixes align portrait flashes with their textured panels and keep the
scrollbar thumb within its track at both endpoints. Menu interpolation uses the
currently submitted layout rather than stale page pointers.

The Gear screen distinguishes the saved pictograph from the menu background.
Fullscreen fade rules exclude localized solid panes so small menu elements do not
cover the screen. Only exact older stock fade lists migrate; custom lists remain.

## Camera and interpolation

Gameplay and cutscene field of view are separate controls. Cutscene FOV ranges
from 45 to 70 degrees, with a default of 49. The fork also retains its independent
camera distance and camera recovery controls.

Actor, water, particle, message, and HUD interpolation use guarded consecutive
samples. The fork retains its first-person aiming handover and ordinary depth
reads. Original 30 FPS remains available for comparison.

## Fork compatibility

The shared HUD preset, input patch, controller mappings, model/voice importer,
and unified Cheats page remain in place. Fast Time has a separate internal hook
range from the fork's movement-speed controls. New optional time and fog settings
are disabled by default.

Run native tests and setup/migration fixtures, followed by gameplay checks of
menus, aiming, custom models, scene changes, and both presentation modes before
publishing an integrated package.
