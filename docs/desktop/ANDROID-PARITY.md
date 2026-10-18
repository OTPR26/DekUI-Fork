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
non-HUD preferences. Python helper tests: 89 passed. Native tests: 78 passed, five
existing audio tests skipped. Desktop HUD files and 168 HUD values now match
the active Odin settings; face bindings remain user-configurable.

Follow-up parity work: the desktop gameplay input patch was still the original
Restoration patch. Startup now installs the same hash-verified PC input patch
used by Android, retaining a backup and rejecting custom conflicting patches.
New desktop controller mappings also retain the PC Start/Select swap. The shared HUD preset and both layout archives were reconstructed and checked
against the active Odin files, including the custom bumper and button glyphs.

Android shared storage: the default user-visible data location is Internal
storage/mm3dport. A staged, SHA-256-verified migration copies existing user data
and the cached game, retains originals, and publishes only the verified folder.
The app requests Android file access before proceeding. HUD/settings/models/saves
use the shared root; controller bindings are mirrored in config/android-controls.json
and loaded from that file on startup. Runtime executable libraries remain private.
Local migration tests cover preservation, idempotency, conflicting destinations
and symlink rejection. The optimized Android build was installed and the user confirmed the shared
storage migration completed on the Odin. Public HUD/settings files were read
back and verified.

macOS storage: generated user data and desktop preferences live under
`~/Library/Application Support/DekUI Fork/`. The first upgrade copies and hashes
the previous portable user folder and preferences, publishes atomically, and
retains the source as a backup. Existing destinations are never merged or
overwritten. External texture links remain references, while app-owned paths
in active settings are rebased. Setup tools, resources, Python and licenses are
embedded in the signed app. Tests cover save preservation, external links,
conflicts, failed publication and repeat launches after newer save progress.

Recent desktop fixes: settings reloads replace rather than append collections;
the single-game build skips the hidden emulator library scan. Manual bindings
update both loaded and stored profiles and remain custom even if their values
match a generated layout. Rebuilding a settings page defers deletion of the
previous page until the dropdown event completes. Graphics includes Reset
current camera, which rebuilds the orbit from guarded native form parameters
without changing the distance multiplier/FOV or bypassing wall collision.

Live Mac migration verification: all 125 original files were retained. All 52
save and generated-mod files matched SHA-256 after migration; controller
settings matched exactly. The game launched using the Application Support log
folder and the existing external texture link. Python helpers: 89 passed;
native suite: 78 passed, five existing audio cases skipped.
