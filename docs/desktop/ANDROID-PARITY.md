# Desktop experience parity

Android is the product reference for macOS, Windows (x64/ARM64), and Linux
(x86_64/ARM64). The initial desktop port established runtime feasibility;
it is not the finished desktop app experience.

Required before calling desktop feature-complete:

- Same Fierce Deity startup presentation, Start game / Select ROM, Settings,
  About, Quit, and controller layout-aware menu input.
- Full settings available before gameplay and during gameplay, with the same
  sidebar organization and shared saved values. No exposed Azahar drawer or
  emulator configuration detour.
- MM3D ROM selection/setup with clear validation and progress.
- Existing custom texture folder selection under Graphics, without copying
  the entire pack or requiring it beside the executable.
- Mods: OoT3D ROM and optional texture-pack pickers, generation/progress,
  Adult Link variants/tunics/Master Sword/voices, Dark Link/sword/red eyes/voices.
- Controls and binding capture, HUD customization, graphics/interpolation,
  FOV/distance, audio, difficulty, Restoration, movement/QOL, runtime cheats.
- Desktop-appropriate settings access: visible entry, Command-comma on macOS,
  ordinary keyboard shortcut elsewhere, controller access; F1 optional.
- Platform-specific Touch behavior only where supported; do not expose dead
  Android touch controls on desktop or remove actual game touchscreen access.
- Preserve user saves, settings, custom HUD, and mod inputs during upgrades.
- Package all generator dependencies; no system developer tools required.

Reuse Android's behavior and labels; share desktop frontend code across all
three desktop systems. Validate generated assets against the working Android
converter and verify cold start, imports, settings, Home/focus transitions,
controller navigation, save load, arrows/Deku bubbles, and mod switching.

Current runtime: Dark Link + voices and aiming fix tested on Android. macOS
loads the generated mod and renders the game. Desktop UI/import parity remains
under implementation. Do not advertise the runtime milestone as UI parity.

Implementation checkpoint (2026-10-18):

- Shared Qt startup/settings frontend and shared full-screen in-game sidebar
  added for desktop packages, with the Android Fierce Deity artwork and font.
- External MM3D/OoT3D ROM selection, texture folders, prelaunch generation,
  transactional model/texture publication, controller binding capture and
  binding-aware navigation implemented. Command-comma/Ctrl-comma and a visible
  Settings action replace F1 as the primary desktop route.
- Pure Python converter checked against all six working Android Adult Link
  variants and Dark Link: all model chunks identical except floating-point
  differences below 9e-18 in vertex/normal data. Adult voice output byte-identical
  (38 clips, 42 bindings). Full native Dark Link icon/texture/publication check
  passed using private local inputs; none are committed or packaged.
- macOS compilation and automated checks pass. Windows/Linux share this source
  but require their own builds and device playtests. New UI and swap-screen
  gameplay testing remain pending; the user's active preview is left alone.
- The first raw UI preview lacked adjacent setup resources. Only a complete
  deployed package is suitable for gameplay testing.

Desktop Reset returns to startup after an explicit unsaved-progress warning.
The Mods import route uses the same confirmation and opens the Mods setup page.
Startup Quit remains the app exit action.

Desktop texture installation correction (macOS playtest): generated textures now
live under user/load/mm3d-model-textures rather than inside the ordinary texture
pack directory, which can point to another volume. The renderer merges these
app-owned overrides after the existing pack. Texture backups remain outside the
scanned directory. The regression fixture covers a symlinked ordinary pack and
checks it remains untouched. A real user-selected OoT3D ROM/texture import on Mac
completed with six Adult Link variants, 38 adult voice clips (42 keys), and 11
matching texture replacements. Gameplay appearance remains a user playtest.

Controller/HUD parity correction: configuration reload now replaces input-profile
and touch-map collections rather than appending stale copies. A native regression
changes the confirm binding repeatedly and checks both loaded bindings and stable
collection sizes. SDL button names remain unchanged; saved logical bindings
determine gameplay actions. Startup/resource checks reconstruct the Android fork's
authored HUD using a hash-guarded copy recipe, backed-up archive and presentation
settings, and the same filter-safe 8x atlas as Android. Only recognized desktop
HUD defaults migrate; custom archives and custom HUD positions are retained.
The current Mac fixture reconstructs byte-identically to Android and preserves
non-HUD preferences. Python tests: 86 passed. Native tests: 78 passed, five
existing audio tests skipped. Live controller/HUD playtesting is pending.
