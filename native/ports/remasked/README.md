# ReMasked native mobile prototype

This is a separate development line based on TriAevum
`672d13f15cb336bc7651c861d23cf45924a9f936`. Majora ReMasked 0.4.4 remains
the production app at `42f3010e0f7d4403640a5b4bc160a6baf3d7aa99`.

## Native game device milestone

ReMasked's native game hosts use L/R bumpers for item I/II and ZL/ZR
triggers for targeting/shield. The default donor shoulder map migrates once;
customized shoulder bindings are retained. The same item inputs assign the
selected inventory item, while triggers retain native L/R menu navigation.
The neutral icon-only HUD remains independent of controller button lettering.
The guest input fixtures cover held/pressed/released item input, usability,
inventory assignment, instrument notes and the unmodified donor mode.

The later full-game hosts live separately from the schematic preview below.
`runtime/` is the private tvOS game host; `runtime/android/` and `android-game/`
build the isolated Android game app (`io.github.otpr26.remasked.nativegame`).
The physical Odin has rendered the matching MM3D AOT game and loaded a copied
save, the ReMasked HUD, the Henriko mobile textures and the selected Adult Link
actor at approximately 30 FPS in the tested scene. This is not full gameplay
or feature-parity qualification. The current stock Adreno driver rejects a
pipeline; the app-local driver picker and a validated Turnip package avoid
that failure without changing the system or production app.

The Android game uses ReMasked 0.4.4's touch view and ten original vector
assets, rather than the donor overlay. It retains neutral gameplay artwork,
Start/Select, item bumpers, targeting/shield triggers, floating sticks,
orientation-specific placement/reset, individual control visibility/size,
opacity, and controller auto-hide. Idle controller-axis events cannot hide
active touches. Native HUD item groups and the D-pad follow their touch
anchors; hiding the controls restores the controller HUD. R3 requests the
Pictograph through the existing Restoration input hook, with usability checks
and expiry while paused; L3+R3 still opens native settings. A transparent,
non-focusable application panel keeps the original vector View visible above
SDL Vulkan on the Odin without creating an opaque second SurfaceView. Actual
title/intro rendering, touch Start/settings access and gamepad auto-hide were
checked on-device; full gameplay HUD placement remains unqualified.

The Android launcher retains Majora ReMasked branding and its existing icon.
It includes PAK Model Loader, the existing converter and atomic model install,
original-model restore and native streamed actor replacement. Host package/
transaction tests and the packaged ARM64 NumPy/import-rejection check pass;
a valid PAK's complete import and gameplay still need qualification. Copied
Adult/Dark/Twilight actors can be selected independently of the production app.
Tall built-in human models now use ReMasked's guarded 1.644336x draw-scale
operands and original mounted-player pivot correction. Original Link, nonhuman
forms, imported PAKs, giant-mask scaling and gameplay/collision data are unchanged.
Exact-code/behavior and mounted-transition fixtures pass; physical Android boot
enables 362 patch sites and logs all four scale operands. Full gameplay visual
qualification and the tall-model torch/stick-contact companion remain required.
Saved sword/tunic variants, generated voice banks and model texture overrides
are sampled at launch and streamed from the isolated app's existing files.
The native Models tab edits only its canonical model/appearance fields; changes
apply on the next launch. PAK import leaves the game through a save-and-quit
confirmation. Graphics shows the selected driver with inline system-driver and
driver-ZIP actions; a new selection applies at the next game launch.
PAK selection keeps priority and uses its imported actor without built-in
model overrides. Regular sword HUD icons use the original atlas-cell replacement
with orientation/material-boundary fixtures; other item cells remain intact.

`dekui/movement_speed.h` and `dekui/camera_distance.h` now have guarded native
relocations and pass the original behavior fixtures against the exact owned
code image. Matching private AOT entry/continuation boundaries and live native
settings are now connected; actual Android boot enables all 358 guarded patch
sites. The regular ReMasked launcher (original mask artwork/fonts and four menu
entries) is restored. Startup Settings and the in-game Settings button use the
same native menu, with ReMasked camera/movement, HUD, Player and advanced touch
settings alongside its existing tabs; separate Android settings dialogs are no
longer used for these options. The native menu uses ReMasked purple (#B97DF2),
scoped to this app. Responsive geometry checks pass at 16:9, 19.5:9 and 4:3
across three display scales; Android stays in landscape. Actual Android
multi-window resizing still needs device qualification. Atomic patches preserve unrelated settings;
physical-device JNI, Unicode, preset and validation tests pass. Canonical FOV
parity and full gameplay comparisons remain unqualified.

Classic saving, classic Song of Double Time, and sword-only owl activation
remain required native ports: they are enabled in the previous profile but
are not yet complete here. No full ReMasked feature-parity claim is made.

`tools/build_android_game.py --help` documents the explicit SDK, private static
AOT title, A32 registry and cached-source inputs. It builds a private APK and
never installs or publishes it. `tools/stage_android_owner.py` transfers only
explicitly supplied owner inputs to the isolated app through its own UID and
refuses to overwrite existing native saves. Owner ROMs, converted models,
textures and saves stay outside this checkout and outside the APK. No native
build replaces 0.4.4 until the required parity gates are qualified.

## Original settings/input preview milestone

Android arm64, iOS and tvOS hosts compile the actual native MM3D settings
parser and TriAevum input validation through a shared C++ core. The hosts show
a **schematic HUD layout preview**, preserve all 586 shipped presentation
settings and unknown model/voice fields, allow bounded HUD edits, and route
touch/controller buttons through the same input ABI. The layout surface is
not a claim that game HUD composition is integrated.

In this original preview-only target, game rendering, AOT execution, audio,
ROM preparation and model import are not connected. The later full-game hosts
above use separate build targets; complete feature parity remains unqualified.
The required features and promotion gates are recorded in `feature-parity.json`.
The default host build packages no game, title module, generated model, ROM or personal texture. Private backend builds may link the locally generated title; they are not release assets.

The renderer integration adds drawable-backed Metal surfaces below the retained
HUD preview and an optional statically linked NRI/MoltenVK device diagnostic for
iOS/tvOS. A clear frame is real GPU work, but is not the game's render pipeline.
The Android NRI device probe also runs on the physical Odin. Private MM3D AOT
generation passes the whole-program and generated-file audits; the static
loader passes ABI checks and bounded execution of the real entry block on the
host. These preview checks are distinct from the later full-game hosts above.

The original JSON document owns persistence. TriAevum's serializer currently
migrates `accurate_hle_audio` and can discard unknown fields; the prototype
therefore parses through its typed settings but edits only explicitly allowed
HUD keys in the original document. The audio migration remains a gameplay
parity gate, not an advertised fix. Lifecycle interruption clears held input;
touch and controller holds have separate ownership.
The Android instrumentation runner exercises the actual JNI library, including
Unicode/unknown-field preservation, Select/Start separation and source release.
Physical controllers and touch gameplay still require device qualification.

## Build

Requirements: CMake/Ninja, Python 3, and a C++20 compiler. The default build
fetches nlohmann/json 3.12.0 with its pinned SHA-256. An existing header tree
can be supplied with `--json-include`.

```sh
python3 ports/remasked/tools/build_mobile.py --platform host
python3 ports/remasked/tools/build_mobile.py --platform android --gradle /path/to/gradle
python3 ports/remasked/tools/build_mobile.py --platform ios
python3 ports/remasked/tools/build_mobile.py --platform tvos
```

Android requires SDK 35, NDK 29.0.14206865, CMake 3.31.6, Gradle 8.14.5 and
JDK 17 or a compatible newer JDK; set `ANDROID_HOME` and `JAVA_HOME`.
Apple builds require Xcode and XcodeGen. They default to unsigned simulator
apps; `--device` produces unsigned arm64 device builds, requiring the owner's
signing setup before installation. No signing accounts or billing are changed.

Build logs and products default to `~/Library/Caches/ReMaskedNativePrototype`.
The tests use bundled defaults and temporary in-memory fixtures, never the
shipping app's configuration or saves.

## Isolation and next integration

The Android application ID is `io.github.otpr26.remasked.nativeprototype`;
Apple hosts use `.ios` and `.tvos` suffixes. Each uses its own application
sandbox. No existing app is updated or installed by the build script.

Next: connect game submissions to the native surface and MM3D title session,
then port ReMasked-specific HUD composition and model pipelines. The static AOT
path avoids external title loading; iOS/tvOS app linkage is being qualified.
tvOS also needs an explicit owned-content transfer
and durable-save strategy; the host preview is not that implementation.

The existing ROM/game-data distribution boundary remains in force. Upstream's
precompiled title/source packaging needs a separately reviewed approach for
this project. iMMERSE/MXAO stays excluded without permission from its author.
No native prototype is promoted over 0.4.4 until all required features pass.

Android Gradle accepts `-PremaskedTitleBuild=/private/android-title-build` and
`-PremaskedNriBuild=/private/android-nri-build` for optional real backend linkage.
These must be Android arm64 archives built with the matching NDK. Instrumentation
arguments `requireNativeTitle=true` and `requireNativeRenderer=true` turn backend
readiness into required checks; absent backends never count as passing them.

## Private AOT and Apple renderer builds

Use `tools/prepare_local_title.py --inputs /private/extracted --output /private/aot`
with an owned, decrypted title extracted by the upstream `ctr_rom` tool. Both
paths must be outside this checkout. The generator verifies the exact pinned
MM3D code and executable layout, preserves a separate local metadata contract,
audits all 18,751 selected functions and 259 generated C++/header files, and
keeps outputs private. USA code equivalence does not establish regional content
compatibility. Python requires Capstone 5.0.6. Never commit generated title code.

`title/CMakeLists.txt` builds the private static title and production-loader
probe. Configure with `REMASKED_TITLE_DIR`; optionally set `REMASKED_CODE_FILE`
to enable the bounded entry-block CPU smoke. Cross builds use the platform's
toolchain and SDK. Host and generated dispatch functions have distinct symbols
so they can coexist in one statically linked executable.

`tools/build_apple_renderer.py` builds unsigned arm64 iOS/tvOS simulator hosts
with the real NRI backend. Supply `--platform`, `--build-root`, `--json-include`
and `--molten-root`. `--device` builds an unsigned device app, while optional
`--title-build` links the private static title libraries for that exact SDK.
The script does not install apps or copy game inputs. The normal host build
remains independent of these private inputs and graphics dependencies.

Renderer dependencies: NRI commit `4b485316463969f182db15e67aad2aec2f40a3d7`
and official MoltenVK 1.4.2 `MoltenVK-all.tar`, SHA-256
`562a15a29bc358446a56a4091c5f7e08f604184187c1d34f712148b61ef17276`.
Retain their license notices. The static loader patch resolves the linked
Vulkan entry point and requests portability enumeration only when advertised.

## Attribution

MM3D presentation settings and HUD structures derive from tgacquin/mm3d-dekui
and contributors (GPL-2.0-or-later), ported by TriAevum. Input service code is
from TriAevum; its component notices remain in their source locations. New
prototype glue is GPL-3.0-or-later. Preserve the repository's license scope and
third-party notices. The bundled presentation profile is copied unchanged
from Majora ReMasked 0.4.4; its hash is recorded in `feature-parity.json`.

The app configures a writable Mesa shader-cache directory with a 256 MiB limit
before loading its selected driver. The previous `/data/.cache` permission
error is absent on the Odin. This does not establish that first-encounter shader
compilation or the observed long scene-load/gameplay stalls are solved; same-save
performance and actual driver-cache population still require qualification.

### Android game and app menus

Start opens the game Items menu. Native trigger page navigation runs through
Gear, Masks, Items, Map, and the Bombers’ Notebook once obtained. From the
Notebook’s main Event Notes page, the previous-page trigger returns to Map;
Notebook detail and schedule controls keep their native behavior. Notebook
transitions retain the game’s normal resource readiness and animations.

Select opens the app settings menu and is consumed before guest input, for
physical, remapped, and original ReMasked touch controls. Android Back and
touch Settings remain available. R3 retains Pictograph; L3+R3 opens advanced
settings. This layout is Android-only; the installed Apple TV app is unchanged.

Android uses the same main category order as ReTuned: General, Controls,
Touch, Graphics, Audio, HUD, Quality of life, Models, Data and About. The
menu uses ReTuned’s two-region settings layout: left categories and a single
scrolling page with inline switches, sliders, dropdowns and collapsible groups.
The original ReTuned spacing, corner radii, typography and controller navigation
are retained with the ReMasked purple palette. Back returns to categories before
closing. The default and Reset menu size are exactly 1.00x, with Android density
and overlay-to-renderer scaling so reduced game resolutions do not enlarge the
menu. The slider supports 0.50x–1.25x. Nearby baked Fira glyph sizes avoid severe
minification and lost strokes at compact menu scales. ReMasked defaults to native
Android output resolution, keeping menus independent of the game render scale.
Graphics offers Native display/720p/1080p output (next launch) and a separate
Render scale in one-percent steps. On a 1080p display, 67% renders the game near
720p while keeping the interface at 1080p; 100% renders gameplay at 1080p. Fira’s font atlas includes typographic apostrophes, quotes, dashes and
ellipses; old canvas-only controller font markers are decoded to plain text. Controls contains pad
bindings, gyro, dead zones and camera aiming. Touch is a separate page with
fixed/floating movement, hidden-at-rest camera stick or drag-to-look, opacity,
individual sizes and placement. Camera distance lives in Graphics; movement
speed lives in Quality of life. Menu size is in General.

Startup Settings uses a lightweight GLES interface in a separate settings process,
with the same ImGui view, file-backed graphics/audio/control owners and ReMasked
settings callbacks. It creates no title session or Vulkan game renderer and runs
no game shader preparation. Start game retains the normal preparation path.
Settings during a running game use its existing in-game menu, avoiding competing
settings writers. Startup graphics edits apply at the next launch; live advanced
graphics, touch layout preview and controller gyro calibration remain in-game.

General provides the five original app icons, with clickable previews in the
right-hand choice pane. Selection updates the recent-app icon and enables the new launcher
alias immediately; old aliases retire after the app is backgrounded, across
the launcher/game processes, preserving active gameplay. Android has no Photo mode panel, menu or free-camera
shortcut. The game’s Pictograph item and normal exploration camera remain.

Quality of life exposes the native MM implementations for Tatl and Bomber
interruptions, low-health warnings, damage/defense, drops, death penalties,
bottle healing and next-launch double shop prices, alongside movement, quick
put-away and roll dash. Fields retained only for compatibility (classic saving,
classic Double Time, sword-activated owls, fast time and six-hour death penalties)
remain unexposed until their native implementations are available.

Data provides direct import/export actions for the native GameContent/saves folder. Transfer uses the
system document picker, verifies the manifest and MM3D slot header/CRC, stages
before confirmation and preserves replaced files. It does not use the old
installer’s full-backup service.

Touch HUD placement identifies equipped-item groups even when their atlas
glyph was recognized first. Visible touch anchors own placement regardless
of a fixed menu prompt family; the full native D-pad group supplies its own
bounds so masks/ocarina/Tatl do not depend on a particular cross texture.
Touch artwork, menu callbacks and responsive category navigation have isolated
host/Android instrumentation coverage. Actual gameplay qualification is recorded
in the private device checkpoints.

The Android Custom textures switch controls the bundled Henriko world pack as
well as optional user textures. The required HUD and selected-model texture
layers remain available independently. An empty user folder does not select a
conflicting hash mode or exclude the bundled pack. Regression coverage checks
On/Off/On, required HUD preservation, modern/legacy metadata and user priority.

Android Data also shows the current data directory and offers a custom local
folder or the standard app folder. Android file access is requested only when
selecting a custom location. The app creates a dedicated Majora ReMasked child
folder, stages and hashes every file, rewrites only actual absolute-root JSON
references, atomically selects the verified root, then removes verified old
copies. Missing permission/storage is reported rather than silently choosing an
empty data tree. Existing destination files are not overwritten. Game, settings,
models, logs and save transfer share the same selected root; executable driver
libraries and disposable shader caches remain in private app storage. Moves need
sufficient free space for verified staging. They run only after the game and the
settings host have closed.


### Android dual-screen output

General → Screen mode selects Single Screen (the default) or Dual Screen. On
AYN Thor, Dual Screen presents the native lower LCD on an Android Presentation
using a second Vulkan swapchain on the existing GPU device. Touch is mapped to
the native 320×240 picture, with letterbox input rejected. A missing display
uses the single-screen layout without discarding the saved selection.

MM3D reuses a 512×400 framebuffer: the lower display copy starts at a tiled
subregion rather than the attachment base. The submission queue recognizes
that bounded alias, and the renderer handles the native viewport and copy
origin independently from the top-screen UI fit. Lower scanout uses its own
surface dimensions and the guest LCD aspect, even when the shared attachment
has different horizontal and vertical sampling densities. No CPU framebuffer
readback or second game instance is involved.

Verified on Thor: file selection and save loading using lower-screen touch,
Items opened on the lower display, Settings pause/resume, and live mode changes
while Items is open. Nine display-transfer tests, settings-view tests and the
Android touch/controller/menu/PAK/data regression suite pass. Extended play and
physical display hotplug remain broader qualification work. Android graphics
filtering does not remove the desktop features tracked in feature-parity.json.

Dual Screen uses native centered Items/Masks model and description placement,
while retaining the existing backing panels. Single-screen model/description
offsets remain saved and resume when Single Screen is selected. The paired
Begin/End hooks stay balanced when those presentation transforms are bypassed.

Android Start opens Items during gameplay; after release, a fresh Start press
uses native Back on ready Items/Masks/Gear/Map/Notebook pages. The opening edge
cannot close the menu again. Native B, page cleanup and close animations remain
in control; transitions, title/file selection and modal detail states are excluded.
Thor verification covers Start opening/closing Items, closing Masks, lower-screen
Items/Masks/Gear touch navigation and B returning from Gear. Touch controls stay off.

Android Dual Screen omits the separate X/Y letter panes in native lower-screen
ItemIconSet/ItemIconSet2 widgets. Item pictures, counters, circular backing and
touch hit targets remain native. Top-screen and Single Screen emissions are
unchanged. Thor gameplay and Items-page captures verify the neutral buttons.

Android custom-texture loading retries identified replacements after decoded
cache eviction, without blocking the renderer or changing cache budgets. The
loader services interface jobs ahead of scenery jobs. Oversized manifest-listed
interface atlases use at most 4096×4096 working images on Android, with alpha-safe
reduction before mip generation; source files, native texture identities and
non-interface texture dimensions are preserved. tvOS and desktop callers keep
the default uncapped dimensions. Thor cold-launch testing confirmed the sword,
mirror shield, quiver and wallet appear with the rest of the file screen.

iOS uses the same 4096-pixel ceiling for manifest-classified UI atlases; the
bundled pack and model files are unchanged. Its GPU texture cache is 256 MiB,
with a 1024 MiB GPU budget cap independent of Metal's device-wide heap report.
Decoded images use a 96 MiB cache, reduced to 32/16 MiB under pressure; critical
warnings hold that lower limit for 30 seconds. Upload scratch keeps one 8 MiB
buffer per frame slot and releases burst allocations only after that slot's
fence completes, including frames without uploads. Decode reduction avoids a
second full-size atlas copy, and mip generation borrows the immutable base
level instead of duplicating it. These limits preserve streamed HD scenery,
Adult Link and the existing HUD; on-device sustained gameplay still requires
qualification with both enabled.

The iOS native input path now samples the handheld's SDL/CoreMotion gyro and
accelerometer when no controller gyro is active. Device vectors follow portrait,
landscape and flipped display orientation before entering the shared aiming
mapper. Sensors close while disabled/backgrounded and reopen on resume; menu
calibration can still collect motion samples. This is adapted from ReTuned's
device-motion path. Orientation, controller priority, lifecycle and invalid-data
regressions pass. Physical gyro aiming is confirmed on the iPhone.

The scene owning the gameplay window publishes motion orientation directly;
the device-motion path uses it instead of SDL2's application-wide status-bar
orientation. The iOS Aim source menu exposes Disabled, Right stick, Motion
(phone/controller), and Automatic (stick + motion). Legacy gyro/accelerometer
selections migrate to Motion without changing axis inversion or sensitivity.
The shared composer also carries ReTuned's fix that keeps accumulated stick yaw
from rotating physical pitch into roll. Portrait aiming was user-confirmed with
both invert switches enabled in the earlier build. The scene-driven orientation
update is installed, and the user confirmed correct aiming in both portrait and
landscape. Device logs also show live scene rotation while SDL's older lookup
remains unknown.


### iOS and iPhone Duo bring-up

`tools/build_ios_game.py` builds a separate **Majora ReMasked** iOS app from a
matching private static title library, existing verified dependency cache and
owner-provided content. The approved combined ReMasked icon is retained. Set
`DEVELOPER_DIR` explicitly; Duo observation requires an SDK with UIHingeInteraction
and is runtime gated to supported OS versions. Older devices retain single-screen
rendering. No physical Apple TV or Android installation is changed by this build.

The Duo observer follows ReTuned's UIKit hinge, reserved-region and safe-area
approach. General > Screen mode preserves the user's requested mode across
closure and rotation. Both LCDs are composed directly by the GPU into independent
viewports; drawing the lower LCD loads the upper LCD target without alpha blending.
Lower touch maps into native 320x240 coordinates and excludes gutters/occlusions.
Native Items/Map presentation retains backing panels and neutral item glyphs.

`tests/duo_display.cpp`, `tests/pica_rebase_indices.cpp` and the mobile variants of
`tests/pause_navigation.cpp` cover layout, input and simulator geometry policy.
Simulator Metal's unsupported base-vertex operation is replaced at the owned draw
plan boundary, covering scene/shadow/replay consumers. Physical devices retain
their native operation. Full iOS feature and physical-device qualification are
tracked separately in feature-parity.json; simulator boot is not a release gate.


Duo simulator interaction checks cover portrait/landscape gameplay, Items/Masks
switching in both directions, item selection and Return. Native lower-tab callbacks
synthesize shoulder edges after release; their inventory-pair routing is separated
from the host's L/R and ZL/ZR controller page ordering. Pure regression fixtures
cover both touch directions and all four host shoulders. Closed-screen Single
fallback and latest-build reopen-to-Dual are verified while Items remains open,
without an unwanted OS keyboard. Physical device signing and qualification remain
separate gates.

For private simulator qualification only, `REMASKED_SIMULATOR_INPUT_HANDOFF`
accepts a run-frame index at which a local input timeline releases control to real
UIKit input. `REMASKED_DUO_TRACE_TOUCH` enables coordinate/HID diagnostics. Device
builds exclude both test paths; normal bundled profiles prohibit input timelines.
Temporary timelines belong only in the isolated simulator data directory and are
removed after the check.

The iOS host now opens the purple ReMasked launcher before starting SDL or the
game renderer. Settings uses the shared Android settings document through native
UIKit controls, both before and during gameplay. Unsupported Android driver/data
folder controls are omitted. Save import/export and model replacement are startup
operations, so active game files are not replaced during play.

For iOS PAK Model Loader, first prepare the model on Android or desktop. Copy the
prepared folder into Majora ReMasked's Documents through Files/Finder, then choose
Models > PAK Model Loader and the preloaded folder, or choose a prepared folder
through Files. It must contain `romfs/actors/zelda2_link_child_new.gar.lzs`; optional
texture/voice overrides are retained. Raw PAK conversion does not run on iOS.
Imports stage before replacement and reject symbolic links. Builtin Adult/Dark/TP
models are separately hash-verified private assets.

The native touch overlay provides neutral item buttons, item I/II bumpers,
Start/Select/Settings, adjustable placement/opacity/size, fixed or floating movement
and an invisible-at-rest camera stick. Simulator import/rejection and Single
landscape rendering checks passed. Physical iPhone build 3 is installed and its
launcher is verified; final hardware touch/gameplay and lifecycle checks remain
tracked in feature-parity.json.

Ordinary iPhones in portrait use a safe-area game picture above a separate neutral
touch-control area. The normal ReMasked HUD stays inside the game picture.
Landscape keeps equipped artwork on the touch controls. Portrait control-layout
customizations are stored separately; hinge-capable phones retain the Duo policy.
The shared presentation-picture owner applies the portrait region to scene, HUD
and pointer mapping, using the same geometry at Retina density.

The ordinary-iPhone portrait picture uses the full safe width; neutral controls
use the space below it with aligned D-pad/face rows and clearance above Settings
and Start. The duplicate touch Select control is omitted on iOS; controller
Select retains its app-settings binding.

For comparison on ordinary iPhones, Touch > Portrait item artwork on controls
(default off) draws live item icons, counts and action text in the lower touch
area. Off retains the normal in-picture HUD. Hidden controls or unavailable
artwork restore the normal HUD. The experimental pass captures native emission
geometry, glyphs and pane UV transforms and uses the existing HD texture source;
it does not read back the rendered framebuffer. User comparison confirmed correct
icons and Off restoration; broader scene/action/performance qualification remains.
