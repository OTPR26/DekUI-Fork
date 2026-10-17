# 60 FPS interpolation investigation

Branch: `experimental/60fps-interpolation`.

The branch contains capture tools and an opt-in camera midpoint preview. It does
**not** produce 60 FPS presentation, change game speed, or persist a graphics option.

## Capture the next experiment

Use the developer F1 editor's **Camera investigation** section. Enter a descriptive
camera state note, then choose **Capture camera orbit (20 seconds)**.
Close F1 within the three-second countdown. The capture records for twenty seconds after the countdown (with an 8,192-frame
safety cap). It records timestamped CPU eye/target readings and candidate render
matrices continuously, while sampling three consecutive raw-draw frames per
30-frame block. Reaching the 16,384-draw sample cap does not stop the camera timeline. It exports an
`interpolation-<timestamp>.json` beside the existing local presentation settings.

Record separate captures for:

1. Standing still, without moving the camera.
2. Standing still, slowly rotating only the camera in the same area.
3. Walking Link through the same area.
4. The pause menu with its rotating model, then a menu transition.

The pause model may already update at 60 FPS. We must preserve native frames and
measure gameplay and menu cadence separately before adding intermediate frames.
Capture frame IDs alone do not establish native FPS. Timestamps measure host
observation time; CPU snapshots are asynchronous and include sample age/freshness.

Analyze each capture from the repository root:

```sh
python3 tools/mm3d/analyze_interpolation_capture.py /path/to/interpolation-capture.json --output /tmp/interpolation-report.json
```

The audit discards potentially partial first/last frames, never pairs across a
missing frame, and rejects ambiguous signatures rather than pairing by draw order.
It reports changing uniform slots for unique candidates. It compares shader-only
matching against matching with vertex/index source metadata.

## Limits and next gate

Source addresses are not object identities: memory can be reused, actors can share
meshes, and vertex/index contents can change at the same address. Unique candidates
still need geometry-content verification and shader analysis. Uniform slots can
contain lighting, materials, bones, or stale unused data, not just camera matrices.
Software vertex draws are counted but not captured. Menus are not classified by
this capture; a top-screen viewport is only an observation filter.

Existing September camera captures show repeated shader signatures within each
frame. Shader-only matching must not be used to interpolate those draws.

Before a visual prototype, verify the camera's matrix convention and transform
slots with the stationary/camera-only comparison. Then establish a replay path
that retains the geometry, texture versions, attachments, and draw state needed
to render an intermediate scene without executing guest logic again. Reset
history on scene cuts, menu transitions, resizing, and state loads. Keep native
HUD/text and native 60 FPS menu frames outside interpolation. Check frame pacing
and GPU cost on Mac and Steam Deck before expanding to actor interpolation.

Tico 3.0.0 is a separate reference implementation of image-based LSFG. Its build
enables LSFG for Switch/Vulkan and requires the user's Lossless.dll. No Tico or
Lossless Scaling code has been imported into this branch.

## Camera midpoint visual prototype

In the developer F1 editor, open **Camera midpoint prototype** and enable
**Show camera midpoint preview** with Hardware shaders on. A private preview
appears in the upper right when consecutive compatible world frames are seen.
**Use current camera for comparison** renders the same subset using the original
projection, to distinguish replay issues from interpolation issues. Enable both
checkboxes for that comparison; disabling the first removes the preview entirely.
The comparison can render without a previous camera sample. The scene inset is
composited opaquely, independently of guest framebuffer alpha. Its target clears
once per captured guest frame and persists across duplicate host refreshes; it
expires after 200 ms without a supported draw.

The preview immediately replays supported accelerated perspective world draws
with the same geometry and texture resources into its own RGBA/depth target.
The guest framebuffer is not replaced. The host projection becomes
`P * inverse(C_mid) * C_current`, where `C_mid` uses the halfway position and
shortest-path quaternion rotation between consecutive camera poses. Only the
verified shader/swizzle pair and valid world transforms qualify for replay.
Interpolation additionally requires rendering cameras matching fresh CPU eye/target
readings. During camera reacquisition, frame gaps, stalls, projection changes or
camera cuts, the inset uses the native projection rather than disappearing. It
resumes interpolation after two compatible validated samples. Non-rigid poses
are excluded entirely. The developer counter "Using native camera" reports this
fallback (and all comparison-mode draws). This fallback is diagnostic continuity,
not a generated intermediate frame.

The replay includes depth-disabled and always-pass perspective draws using the
verified world shader, retaining their native blending, depth state and draw
order. Sky/effect clipping ranges share camera history without replacing their
individual projection Z rows. Orthographic HUD and unknown shaders remain
excluded; this is not a blanket replay of every top-screen draw.

This is camera-only: actor poses remain current, and unsupported effects, HUD,
menus and other shaders are omitted from the inset. The verified native
transition-overlay signature is replayed separately with an orthographic
projection, retaining native color/alpha and premultiplied coverage. The layer
sits above the inset and persists across duplicate refreshes; a new scene frame
without a fade clears it. This does not classify every possible wipe/transition.
Guest visibility/culling is
also current-frame visibility. Lighting remains current-camera lighting. The
main view remains unchanged in inset-only mode. Native 60 FPS menu models are
not doubled by the experimental presentation mode. Extra GPU work
can still affect performance. No claim of complete scene coverage is made.

The next gate is visual comparison in the captured test scene. Passing the math
and private-target GPU tests does not establish that all game shader conventions,
visibility and effects are handled correctly. The experimental presentation mode below needs in-game validation; complete
60 FPS animation additionally needs wider scene/animation support.

## Experimental 60 Hz camera presentation

Enable VSync and Hardware shaders, then enable **Experimental 60 Hz camera
presentation** in the developer Camera midpoint prototype section. This enables
capture, disables the current-camera comparison, and hides the diagnostic inset.
The presented-pairs counter should increase during supported 30 Hz gameplay.
The status line reports the active fallback gate instead of silently leaving
the toggle enabled with no generated frames.
This setting is not persisted and is off by default.

Eligible frames are composed into two separate present images: midpoint first,
then the matching native frame. Top-screen HUD draws using the verified world
orthographic or layout shaders are replayed into a transparent layer with native
clip scaling. Existing bottom-screen and presentation overlays retain their
normal composition. The native frame is unchanged. Unsupported HUD capture,
unknown top-screen shaders, missing history, other layouts or an unstable/non-30 Hz
cadence fall back to native rendering. Native 60 Hz menus are not doubled.

Duplicate refreshes after a pair do not generate another pair. Only the host
present queue receives additional frames: guest completion, input/game updates,
and the emulator frame limiter still run once per original swap. Pair submissions
are spaced by at least roughly 16.67 ms on the presentation thread, including on
high-refresh displays. This is submission pacing, not a guarantee of physical
scanout timing. Waiting for the current camera entails buffering latency.

Validation: sequencing tests cover duplicate suppression, 30/60 Hz cadence,
stalls, unavailable captures, toggles and session rewinds. The ROM-free native
swapchain fixture exercises paired presentation, synchronous/asynchronous queues,
resize and paused UI while asserting the guest frame count remains zero. Actual
scene completeness, HUD alignment, smoothness and performance need in-game checks.
Characters, animations, lighting and visibility/culling remain current-frame data;
this is camera interpolation, not complete 60 FPS animation.

Captured draws carry the pre-swap guest count; SetBufferSwap increments the
completed-frame count before presentation. Readiness matches capture N to
completion N+1 and stays attached to the scene/HUD capture across duplicate
refreshes. Per-refresh diagnostic counters must not determine readiness.

## Human Link pose correlation (read-only)

The developer panel now has **Capture human Link bones (10 seconds)**. It waits
three seconds, then records ten seconds. Use human Link in gameplay: stand still
briefly, run and turn. Avoid opening menus or transforming during this capture.
Keep Hardware shaders enabled. The overlay automatically exports
`mm3d-local/link-pose-<timestamp>.json`, even when F1 is closed.

The existing verified skeleton model-view builder hook checks all 26 packed bone
IDs/parents and name hashes from the human Link model. It records complete finite
poses with instance/output addresses and the guest draw-frame count. No guest
matrices or game state are written. Render samples collect uniform slots 20–49
for the verified world shader in three consecutive frames out of every ten.
Capture memory is bounded; truncation is explicit. Captures and local Nintendo
model assets are development data, not release resources.

Run `python3 tools/mm3d/analyze_link_pose.py <capture> --output <report>` to correlate
same-frame CPU matrices and shader palettes. Identical transforms across bones or
instances are reported as ambiguous. Numeric matches alone do not establish that
a shader actually uses a slot, or distinguish the gameplay model from menu
instances. This instrumentation supports the experimental interpolation below.

First Link capture audit (2026-10-11): 299 complete poses from one matching
instance; 8,192 sampled draws hit the diagnostic limit. Exact same-frame matching
found zero matches. Explicitly shifting CPU pose tags forward one renderer frame
found 806 matched draws; incorporating inverse bind transforms derived from the
local human Link skeleton raised this to 1,994 candidate draws, with no ambiguous
slots. Only 1,060 candidates match slot 20; later slots can retain unused values.
This establishes a useful pose/palette relationship, not complete live coverage.
The tested analyzer accepts --pose-frame-offset and --model (local uncompressed
GAR/CMB). Neither guest addresses nor captured index offsets are stable actor IDs.
## Experimental human Link interpolation

The developer checkbox **Experimental human Link interpolation** enables a first
human-form prototype alongside the midpoint preview or experimental 60 Hz mode.
It is off by default and is not persisted. The interpolated draw/bone counters
indicate whether the matcher is replacing any intermediate-frame uniforms.

Complete live poses are correlated with renderer frame CPU+1. Slot 20 must identify
a unique human Link pose before other matching slots may be replaced. Direct and
inverse-bind-corrected transforms are supported. Bones are converted to world
space, blended hierarchically using local quaternion rotations, translations and
scales, then converted back through the current view before midpoint projection.
Only the replay uniforms change; guest matrices and native frames are untouched.

Missing history, ambiguous matches, model changes, large root jumps, invalid
transforms and preview changes fall back to native poses. Unmatched slots remain
native. Tests cover hierarchy, camera movement, scale, rotation, ambiguity and
history discontinuities. This does not establish full active-palette coverage or
gameplay ownership of menu clones. Human Link needs visual validation; other
actors, transformations, equipment and effects are not promised smooth animation.

## Shared skeleton prototype

**Experimental NPC/enemy skeleton interpolation** independently enables non-human-Link
models through the same verified skeleton builder hook. Human Link remains on its
own toggle. Neither setting is persisted. Both require midpoint preview or 60 Hz
presentation; the skeleton draw counter now includes both paths.

The reader validates the `skl ` header, exact record size, sequential bone IDs,
parent-before-child topology and finite invertible rest transforms. It accepts
1–128 bones, derives bind corrections at runtime, and fingerprints topology,
names and rest transforms to invalidate histories when model memory is reused.
Complete poses only are published; duplicate bone callbacks cannot complete a
partial pose. The live queue is bounded to 512 poses over four guest frames.

Renderer histories are separate per skeleton instance, bounded to 128 instances,
and expire after missed frames. A first-palette-slot match must identify a unique
pose across all current instances. Ambiguity falls back to the native pose. Only
matched uniforms in intermediate replay frames change. This supports skeleton
models, not arbitrary particles, cloth or texture animation; shader coverage and
in-game ownership still need visual testing.

Validation: synthetic tests cover two simultaneous actors with different bone
counts/motions, partial and duplicate callbacks, malformed topology, oversized
skeletons, frame offsets, generation changes, overlapping actors and reused model
addresses. Local read-only model inspection confirmed compatible skeleton layouts
for carpenter, Dodongo, Deku Scrub and Skull Kid archives (1–24 bones across their
models). No game assets are added to source or release resources. Mac build and
camera/live interpolation tests passed; NPC/enemy visual testing is pending.

## Presentation pacing correction

A three-second live Mac sample during reported slow/glitchy audio found the
emulation thread waiting in GetRenderFrame in 1,096/1,969 samples, and the primary
presentation thread in the added pacing sleep in 1,030/1,969 samples. Skeleton
matching accounted for a much smaller share. These are sampled stack counts,
not exact timings or proof that all remaining overhead is gone.

The old pacing slept until last submission completion + 16.67 ms, then performed
acquisition/copy/submission work, accumulating that work on every visual frame.
Pacing now uses an absolute 60 Hz frame-start timeline, so presentation work uses
the interval instead of extending it. Long stalls reset the timeline; native and
repeat frames clear it. Deterministic tests cover submission overhead, repeated
intervals, stalls and toggles. Actual speed/audio recovery needs a restart and
live verification; guest timing and animation interpolation are unchanged.

## Area-entry lens changes

The second live transition trace showed 30 lens-change rejections per second for
roughly three seconds, with zero CPU camera mismatches or cut rejections. The
strict equal-lens check was repeatedly discarding history during the entry
projection animation.

Consecutive frames now also accept uniform perspective lens scaling within 5%
per guest frame. The halfway projection averages the two lens scales while
retaining each draw's own Z clipping row; camera and skeleton interpolation remain
unchanged. Aspect/skew changes, orthographic switches, abrupt zooms, discontinuous
frames and camera cuts retain native fallback. Same-frame draw matching still
requires the same current lens. Tests cover sustained zoom, simultaneous camera
motion, separate sky clipping and rejection of abrupt or incompatible changes.
Live area-entry validation remains required.

## Buffered pacing experiment

The 30-second software timing capture contained 1,792 presents, averaging 16.728 ms
between present returns but with 325 intervals below 12 ms and 250 above 22 ms.
Adjacent intervals were strongly negatively correlated (-0.864). This measures
software delivery, not physical scanout. A separate sampler showed presentation
frame waits below 1% of emulation-thread samples, so the earlier throughput
bottleneck was no longer dominant.

Presentation now adds one 8 ms lead-in when entering paired mode or recovering
from a deadline missed by at least one quarter-period. Existing bounded frame
resources supply the queue; no unbounded buffer is introduced. Steady-state
deadlines stay 16.667 ms apart, with no fresh lead-in per frame. Native/repeated
frames clear this timeline. The final 3 ms of waiting yields instead of relying
on an OS sleep for the exact deadline. This trades some presentation-thread CPU
usage and a small latency increase for tighter timing; live validation is needed.
A local timer benchmark improved p95 interval from 20.20 to 17.12 ms, but retained
outliers, so this is not a guarantee of display pacing.

Input timing traces now include `pacing_arrival`, `pacing_deadline` (steady-clock
microseconds) and `paired`, alongside copy/acquire/return timestamps. These
separate late frame arrival from timer overshoot and submission work. Existing
FIFO VSync remains enabled; display-specific present-timing extensions are not
implemented by this change.

Buffered capture result: 1,793 presents averaged 16.708 ms; 90.5% of intervals
were within 2 ms of target (previously 36.7%), and intervals outside 12–22 ms fell
from 32.1% to 3.2%. However, compose-to-present-return time increased from 17.55 ms
to 38.68 ms on average. This is pipeline latency, not measured input latency.
Of 21 intervals over 30 ms, 18 occurred between non-paired frames. Six paired-mode
deadline resets inserted 29–31 ms deadline gaps; three exceeded 30 ms in actual
return spacing. Thus the lead-in/rebuffer policy was counterproductive.

The follow-up removes the 8 ms lead-in entirely, including on recovery. Frames
more than a quarter-period late rebase immediately, with the next deadline one
period later, instead of adding another wait or issuing a short catch-up. The
bounded 3 ms yield wait and detailed timestamp diagnostics remain. Tests cover
immediate late recovery and normal subsequent spacing. Live comparison pending.

## Verify generated image content

The developer button **Capture rendered frame pair (3 second delay)** requests a
one-shot staging readback of the next eligible pair after the delay. It saves
`frame-pair-<timestamp>-midpoint.ppm` and `-native.ppm` in the user's log directory.
Close the developer menu and rotate the camera during the capture so distinct
poses are expected. These are actual composed render-frame images before the
presentation overlay/swapchain, not re-rendered screenshots. Readback synchronizes
the GPU and can hitch; it is for content diagnosis, not pacing measurements.
Comparing them distinguishes duplicate render content from downstream display
problems; it does not measure physical scanout.

## Develop integration

Graphics now exposes **60 FPS interpolation [experimental]**, saved as
`interpolation_60fps` in presentation settings and defaulting to false for new or
older configurations. Enabling it activates camera, human Link and generic
skeleton interpolation together, clears comparison mode and enables VSync/hardware
shaders. Disabling it stops all three paths. Existing VSync/shader preferences
remain enabled afterward. The tooltip recommends a 60 Hz display and 1x draw
distances; display refresh is not changed automatically. Live user testing found
fixed 60 Hz substantially smoother than the reported 120 Hz/ProMotion mode.

Developer camera inspection, prototype controls and capture tools live in the
editor's Camera tab. Individual prototype toggles are diagnostic session overrides;
the saved Graphics setting is reapplied on launch/settings restore. Per-second
interpolation diagnostics are logged only during an input timing trace.

Integration validation: Mac application build, 74 passing CTest tests with five
skipped audio tests, and 16 Python capture-analysis tests. Native swapchain fixture
passed its synchronous/asynchronous configurations, pause/resize checks and zero
guest-frame assertion. No extracted assets or local settings are included.
The broader GPU fixture also passed after replacing stale coordinate-dependent
capture clearing with the texture API and using an actually outside-panel point
for small windows. Midpoint target coverage/depth/resize/reset checks passed.

### End-of-day scene copy

The shrinking sequence uses an orthographic, four-vertex, opaque scene-copy quad.
The compositor already applies the normalized shrink to both native and midpoint
frames. Replaying the native copy as HUD would cover the midpoint with a 30 Hz
image. The Vulkan replay therefore omits that quad only when its texture is the
current captured world framebuffer or a successful display/texture transfer from
it, normalization is active, and its shader, texture count and blend state match.
Unrecognized copies still fall back. Provenance expires each guest frame.

The sequence can also submit a solid, untextured opaque quad before that copy.
Previously this caused replay-state rejection 5 in both CPU and GPU modes,
restoring the native 30 Hz cadence despite recognizing the scene copy itself.
The replay now defers that clear when it targets the captured scene, then requires
the next top-screen draw to be the verified scene copy into the same framebuffer.
Both native draws remain untouched; neither covers the private midpoint. An
unmatched clear, intervening draw, or different destination still forces fallback,
including when the clear is the final draw of the frame. The compositor continues
to supply the surrounding black borders. Camera-midpoint tests cover the matching,
rejection, frame-expiry and reset paths; live day-ending validation is still needed.

Timing traces log rate-limited HUD rejection details (0: failed draw, 1: stencil,
2: unknown shader, 3: layer limit, 4: allocation, 5: unsupported replay state).

### HUD and letterbox ordering

Paired presentation replays native HUD draws and matched screen covers into one
transparent target in submission order. Composing all covers after all HUD can
change occlusion every other frame (visible as flickering HUD on Z-target bars).
The separate fade target remains available only for the diagnostic inset.
The previous projection-based and duplicate-world-draw hypotheses were not
confirmed by visual testing; the speculative world exclusion was removed.

Sun-glare trace: 72-vertex textured orthographic world-shader draw, RGB
SourceAlpha/One/Add. Ordered midpoint HUD replay supports this additive blend
using Zero/One/Add for alpha, so emission does not increase coverage or darken
the world. Other unsupported blend modes still fall back to native frames.

Glare motion capture (local trace, 2026-10-11): 72 indices, position loader 0
contains 49 float3 vertices, UV loader 1 contains float2. First 48 positions form
the sprites; final position is unchanged sentinel data. Projection/model matrices
are fixed. Midpoint replay uses a private position buffer averaging only XY of
vertices 0–47. Native draw, Z, UVs, colors and visibility are unchanged. Missing
frames, texture/generation changes, changed Z/sentinel or XY steps over 40 source
pixels reject history. The capture only verifies the observed glare layout;
other effects remain native. Visual verification of the motion is still required.

Clock Town dog capture: a moving 12-bone instance has 798- and 144-vertex draws
matching distinct bones in slots 1–6 with no slot-zero match (204 sampled draws
also include a subsequent stale-palette draw). Actor selection now tries a
unique three-distinct-bone match in slots 1–9 only when slot zero matches no
instance. Existing slot-zero selection/ambiguity behavior is retained. A later
36-vertex draw with ambiguous slot-zero owners remains native. Visual dog
confirmation is pending; the capture does not carry actor names.

Dog follow-up: slot 0 matches both skin-transformed bones 0 and 1. Unique-actor
selection alone therefore left the main body slot native. After selecting an
actor, palette resolution now evaluates all matching bone interpretations and
accepts only mutually equivalent midpoint results. Divergent histories remain
native. Offline replay of the user's capture produced changed slot-0 overrides
for 90/136 candidate dog draws; the other 46 are initial frames of sampled
segments. Equivalent-transform and divergent-history regressions pass.

The user subsequently confirmed both glare motion and the dog's running motion.

### Fairy glow

Local archive/skeleton correlation identifies `chat` (five bones, full topology,
name and rest-transform signature `09e2df0cf6765f8e`) separately from the frog
(`c0835328e6782411`, twenty bones). The fairy wings already match skeletal
palettes. Two four-vertex glow draws instead use uniformly scaled view-facing
matrices, both centered exactly on the fairy root, with a 2:1 size ratio.

Only four-vertex draws with that fairy signature, a matching root position and a
scaled identity basis enter the glow path. A unique instance is required. The
midpoint reuses the skeletal root history and changes only the glow translation;
native size, orientation, pulse, texture and color are preserved. Native frames
and comparison mode stay unchanged. Existing camera, generation, frame-gap and
teleport history guards apply. Other actor signatures, including the frog, do not
qualify. No extracted model/texture data is included in the implementation.

Offline replay of capture `link-pose-1791730949111.json` identified 146 glow draws:
96 interpolate, with the remaining 50 seeding sampled history segments. Comparing
the new path against the previous path produced zero differences on unrelated
draws in this capture. Regression coverage includes moving-camera world-space
position, native billboard basis, frog rejection, ambiguous instances and history
resets. Visual confirmation of the glow correction is still required.

### Termina map background ordering

Frame pair `855491745147` confirms the native frame has the purple background
while the midpoint frame has black in its place; the map and later UI are present.
The HUD capture previously began only after the first perspective scene draw,
omitting layouts submitted earlier. Recognized screen-space draws before the
first perspective draw now seed the depth-backed scene target, so the 3D map
is drawn over them. Later HUD continues into its ordered overlay. The first
perspective draw clears depth for its depth convention without clearing the
already drawn background color. Background-only frames cannot qualify as
interpolated scenes, and unsupported replay states still force native fallback.
Visual verification of this change is pending.

### Zora transformation investigation

Capture `link-pose-1791732394605.json` includes the mask, the `msa_efx_model`
transformation effect and the 28-bone Zora body. Draw capture reached its 8192
limit (frames 6340–6610); actor poses extend to 6631. This does not fully verify
mask removal or other transformations. The corresponding timing trace mostly
maintains paired presentation, with brief lens/camera resets; those camera
fallbacks require a separate camera-matrix capture.

The effect's four equivalent bones prevented unique-bone actor selection. Its
verified signature now permits unique-instance selection, with the existing
palette resolver requiring agreement between every matching midpoint transform.
The two verified Zora mask models can share a current transform with human Link's
head. For that specific two-owner case, independently interpolated histories are
compared per slot; only mutually agreeing results are used. Other ambiguous actor
owners remain rejected, and no interpolation crosses a model identity change.

Local replay (human inverse binds reconstructed from the user's local archive)
produced 30 interpolated effect draws out of 45, the other 15 seeding sampled
history. The first mask gained six interpolated draws and human Link gained 15.
Tests cover equivalent/divergent effect histories and agreeing/disagreeing
mask/head attachments. No model data is distributed. Visual testing and camera
investigation remain outstanding.

Camera capture `interpolation-1791732818725.json` contains 599 camera frames
and 6763 sampled draws, neither truncated. Dominant render cameras agree with
the CPU eye/direction throughout. The transformation's linear FOV ramp crosses
the 5% lens-scale threshold on frames 2276–2278 (ratios 1.0506–1.0558).
Using the captured CPU FOV and zoom factor verified in sampled projections,
these steps change the displayed FOV by only 1.858–1.885 degrees. Lens continuity
now also accepts angular steps up to 2.5 degrees within a 0.9–1.1 relative cap,
still requiring uniform projection scaling and unchanged aspect/skew/type.
The abrupt entry/exit lens changes (ratios 0.6502 and 1.2976) remain cuts. The
existing spatial camera-cut guards are unchanged. Regression tests cover the
narrow zoom in both directions, abrupt lens jumps and aspect changes. This
addresses the measured zoom fallback; visual verification remains necessary.

Follow-up off/on timing trace has no unsupported-HUD rejections and mostly 30
pairs/second, despite perceived 30 FPS movement. One window drops to 14 pairs
with cadence rejection, but there is no sustained effect-rejection fallback.
Earlier camera draw samples show an extra four-vertex orthographic draw during
transformation: projection Z row `(0,0,.5,-.5)`, model scale `1.0008`, translation
`(-.1600037,-.0960007,0)`, before the two letterbox bars. It may be a scene-copy
effect; texture ownership and blend behavior were not captured, so that remains
a hypothesis. Explicit input-timing traces now sample accepted orthographic
four-vertex draws every ten frames, logging texture/framebuffer addresses,
same-frame scene provenance, blend factors, model and uniform color. This is
diagnostic only; the suspected effect is not removed or otherwise changed.

The follow-up on/off trace identifies that quad's texture as `18300000`,
SourceAlpha/OneMinusSourceAlpha/Add, full color mask, with one texture. It is
not in current-frame scene provenance when sampled, but becomes a known scene
copy by the later bar draws. A default-off, unsaved editor diagnostic now
suppresses the exact captured projection/model/blend/geometry combination only
when the texture has previous-frame scene-transfer provenance and 60 FPS is on.
It skips both the native draw and private replay, avoiding deliberate alternating
blur. This changes the feedback effect's appearance and is only a test of the
suspected cause, not a final effect-preserving solution. Tests reject unrelated
textures, geometry, untextured bars/fades and stale provenance. Visual test pending.

### Great Fairy fountain bloom

The fountain timing trace reports zero pairs and unsupported-HUD fallback for
every frame. The rejected draw is the verified world shader/swizzle, four
orthographic vertices, two enabled texture units (mask 3), no stencil, full
color mask and One/One/Add blending. This differs from the supported
SourceAlpha/One/Add glare blend. Ordered replay now also accepts this captured
bloom composite, preserving native RGB factors while using Zero/One/Add for
coverage alpha. Bloom emission must add light without obscuring the underlying
interpolated scene. Existing bloom sampling correction is unchanged. Other
geometry, texture masks and shaders are excluded from the new One/One path.
The trace contains no separate Lens of Truth rejection signature; applicability
to that effect is not established. Build/tests and visual verification are
required; bloom texture motion itself still uses the native sampled effect.

User visually confirmed fountain bloom no longer forces 30 FPS.

### Lens of Truth stencil replay

The subsequent lens on/off trace starts at 30 pairs/second, drops to zero for the
entire enabled interval, and recovers when disabled. Rejection reason 1 is an
18-vertex orthographic world-shader draw before the first world pass, stencil
enabled, One/One/Add, full color mask and texture unit 0 only. This is a distinct
failure from bloom.

That narrowly identified leading mask now starts an ordered stencil scene for
the current frame. Screen-space mask/UI draws and stencil-tested perspective
geometry share the private D24S8 target, retaining native depth/stencil state,
dynamic reference/masks and blending. Later UI goes into the same scene in draw
order rather than into an overlay without stencil. Initialization of the first
world pass resets depth only, preserving a previously drawn stencil mask; fresh
frames still clear both. Unrecognized stencil scenes retain the fallback.

Visual verification must cover smooth camera motion, lens activation/deactivation
and correct reveal/occlusion of actual hidden objects—not just the frame counter.

User subsequently confirmed Lens of Truth looked correct; hidden-object coverage
was not separately reported.

### Spinning lock-on marker

Existing actor capture `link-pose-1791730949111.json` includes two one-bone
instances identified from the local archive as `target_model` and
`target_depth_model`, signature `578e07cd3309c81a`. Both match the same palette,
so conservative actor ownership rejected every marker draw. These two distinct
model/instance owners may now resolve a slot only when their independent
midpoint histories agree. Other ambiguous pairs remain rejected. This changes
only interpolated transforms, not native geometry, depth or draw ordering.

Offline replay changes 160 of 248 matching draws per owner; 88 retain native
transforms where sampled history is not ready. Regression tests cover rotation
and position, disappearance/frame gaps, extra owners and divergent histories.
Visual testing should cover continuous targeting, release/reacquisition and
switching targets. Target actor identity is not captured by this path; existing
frame-gap/model-change/teleport guards apply rather than an explicit target-switch
signal. No extracted model data is included.

### Tatl attention arrow

The same capture identifies `mark_model` and `mark_depth_model`, one-bone
signature `018a783e5ef2d2bd`, as another paired marker. The agreed-history path
now accepts this pair too, requiring both owners to share the same verified
signature. Mixed arrow/lock-on pairs remain rejected. Offline replay now
interpolates 14 of 22 matching draws for each arrow layer; the remaining eight
lack ready history. Existing actor replay counts are unchanged. Both marker
signatures run the rotation/translation, disappearance and ambiguity regressions,
including an explicit mixed-signature rejection. In-game verification of the
attention arrow is pending; native color, geometry and depth ordering are unchanged.

### Pickup rupee

Capture `link-pose-1791735108660.json` contains 31 samples across six instances
of the locally identified `drop_gi_rupy` model, signature `df35e5b3de06d85e`.
Its seven bones share transforms and identity binds, so unique-bone ownership
rejected it. The verified equivalent-bone path now also accepts this signature
and seven-bone shape, requiring a unique instance and agreement between all
matching bones' midpoint results. Replay interpolates 19 of 31 matching draws;
12 retain native transforms without ready history. The capture is truncated,
so this only establishes behavior for its retained samples. Regression tests
cover the captured .015 scale, rotation/translation, disabled interpolation,
overlapping instances, gaps, divergent bone histories and unrelated signatures.
Visual confirmation of the overhead pickup animation remains pending.

### Deku bubble charging and flight

Capture `link-pose-1791735478169.json` correlates with `pn_tama_model`
(signature `ce5abb274d304de4`, three equivalent bones) and `nuts_soap`
(`69e723ba0b961e6b`, four bones, including a shared pair). Both verified shapes
now use the equivalent-bone owner path. Unique instance ownership and agreement
between matching midpoint bones are still required. Existing scale interpolation
handles the captured nonuniform bubble growth without relaxing scale-jump guards.
Replay changes pn_tama from 0/56 to 36/56 interpolated matching draws and
nuts_soap from 6/18 to 9/18; other model counts are unchanged. This capture is
truncated and sampled histories contain gaps, so the remaining native draws
are not a measurement of live smoothness. Regressions cover growth plus motion,
disabled interpolation, ambiguous instances, disappearance and disagreeing bones.
In-game charging and flight verification is pending.

### Bomber mixed animation cadence

Capture `link-pose-1791735725445.json` contains the verified 17-bone `bombers`
model from `zelda2_cs`, signature `c299a4754ffae267`. Some materials uniquely
identify a bone and already interpolate; others match equivalent transforms
and fail ownership resolution. All 918 checked consecutive bone pairs support
midpoint math. The verified equivalent-bone owner path now includes this model,
retaining unique-instance and matching-midpoint requirements. Offline replay
increases interpolated slot-zero draws from 204/572 to 374/572; other model
counts are unchanged. Missing sampled histories still retain native transforms.
Regression coverage includes equivalent skin transforms with distinct binds,
multiple instances, gaps, divergent histories and unverified models. In-game
verification of the previously uneven movements remains pending.

### Postman cap and mailbag

Capture `link-pose-1791769334003.json` contains 86 samples of the 15-bone
`postman` model from local `zelda2_mm`, signature `bf2f38bb6502de36`, but no
render draws. The capture gate incorrectly required accelerated vertex shading;
it now records the original PICA palette for CPU draws as well. Offline checks of
the captured poses find unique direct/skin bone matches and valid midpoint math
for all 855 consecutive local-bone pairs.

Follow-up `link-pose-1791769767602.json` includes render palettes. The body already
interpolates, but cap draws (204/96 vertices, `10aa2e239eaea424`) share bone 14's
direct transform and the mailbag (414 vertices, `61a2e0e2eed80dcc`) shares bone 7.
The body/accessory ownership ambiguity previously rejected all three draws. The
verified attachment path now follows the body's interpolated parent hierarchy,
requiring an identity accessory bind, matching current transforms, exactly two
owners, and the same body/accessory instances, models and generations on successive
frames. Cap and bag histories are independent and bounded. Unverified draws and
additional matching owners remain rejected.

Offline replay adds 43 successful midpoint draws for each affected material;
the body's existing results are unchanged. The capture reaches its draw limit
and samples three frames per ten, so these are diagnostic counts, not gameplay
FPS measurements. Regression tests cover both attachments together, hierarchy
rotation, disabling interpolation, ambiguous owners, unverified models/draws,
model replacement, gaps, generation changes and reset. Live visual validation
is pending. Both CPU and GPU interpolation use this shared matching path.

### Carpenter's carried wood

Capture `link-pose-1791770243166.json` identifies the 20-bone `disciple` carpenter
(`228b499bc4c2bef7`) and one-bone `dk2_hashira_mod` wood (`a81ec0d047c2eaad`)
in local `zelda_daiku`. The 36-vertex wood draw matches both its identity-bind
root and carpenter bone 8, so generic ownership resolution rejected it. The
shared verified attachment table now includes that exact model/draw/bone pairing,
using the parent's hierarchy midpoint and the same continuity/ambiguity guards
as the Postman attachments. It applies to CPU and GPU modes.

Offline capture replay changes the wood from 0/73 to 48/73 midpoint draws; the
remaining samples have no consecutive captured history. The capture is truncated
and sampled, so those counts are not a live frame-rate measurement. Tests cover
parent rotation, disabled interpolation, overlapping carpenters, unverified
tools/draws, replacement, disappearance and reset, alongside the Postman checks.
Live visual verification is pending.

### ChuChu death deformation

Capture `link-pose-1791770630428.json` shows the one-bone ChuChu model from local
`zelda2_slime` (`f0c54784cce31d97`) retaining normal ownership matches during death.
Its death wobble introduces substantial shear, so the usual rotation/scale
decomposition fails and returns the current transform unchanged. A fallback now
averages small affine changes for its captured 18/444/612-vertex materials only;
the skeleton signature alone also occurs in unrelated props. Both relative basis
changes must have Frobenius norm at most 0.5; nonfinite/singular transforms,
non-affine matrices and movement of 100 world units or more remain rejected.
Normal rotation/scale interpolation remains the first choice. Both CPU and GPU
modes share this actor path.

Offline replay increases changed midpoint draws from 15 to 22 for each single
material and 30 to 44 for the two 612-vertex passes. These sampled/truncated
capture counts are not live frame rates. Tests cover the death shear through
actor replay, symmetry, material/signature guards, disabled interpolation, frame
gaps, singular/flipped/large deformations, teleporting and nonfinite inputs.
Live visual validation is pending.

### Zora underwater electric barrier

Capture `link-pose-1791771372254.json` identifies the three-bone `zora_thunder_mo`
model from local `zelda2_link_zora_new`, signature `6735aa3cacc26ca6`. Its 216- and
108-vertex draws use the two identical sibling bone transforms, preventing unique
bone identification. The verified equivalent-bone owner path now includes this
model, retaining unique actor ownership and agreement between the candidate
midpoint transforms. Offline replay changes each material from 0/44 to 29/44
interpolated draws; remaining samples lack consecutive captured history. These
sampled/truncated capture counts are not live frame-rate measurements.

Regression tests exercise both materials with identical siblings and rotated
binds, disabled interpolation, duplicate actors, disappearance, divergent sibling
histories and unverified models. This interpolates the barrier model's transforms
in both CPU and GPU modes; the skeleton capture does not establish the cadence
of any separate texture/shader animation. Live visual validation is pending.

### Zora Hall waterfall texture scrolling

The CPU-mode effect capture `song-effect-1791771935664.json` contains steadily
scrolling room materials with 276/237, 282/270, 423 and 549 vertices. These now
use the existing song UV-uniform history: recognition requires the complete
fixed UV basis and draw size, plus the renderer's verified shader and blend/depth
guards. Rows 10/11 and 14/15 get midpoint offsets in both CPU vertex replay and
GPU uniforms; texture addresses identify histories but are not recognition constants.
Gaps, offset resets and basis changes retain native values. Unrelated draws with
inherited uniforms are excluded; the third UV channel is unchanged.

Offline replay recognizes six draw sizes, with 81 interpolated samples per 122
sampled frames (162 for each twice-drawn alpha sheet). The remaining samples
start a capture sampling interval and correctly have no preceding frame.
Unit checks cover recognition, both scrolling layers, preserved rotation and
gap fallback. Actual waterfall appearance still requires an in-game check.

### Deku pipes and colored song effect

Capture `link-pose-1791736167235.json` identifies seven-bone `link_deknuts_ga`
(signature `79937db4f6cb5c95`) and four-bone `ocarina_time_model`
(`04e3fb346228fd20`). The pipe draw shares its base with Deku Link: selecting
Link alone overrides slot zero but leaves the five moving pipe branches native.
A verified pipe attachment with at least three distinct matched later bones now
uses its own history. Shared slots must agree with the independently interpolated
24-bone Deku Link owner; unrelated or multiple owners remain rejected. Replay
of the 1284-vertex pipe draw now overrides slots 0 through 5 rather than only 0.
Tests cover independent branch motion, shared base, ambiguous instances,
unrelated owners and disabling interpolation.

The song-effect model is admitted through the verified equivalent-bone path.
Replay overrides 63/96 matching draws, 12 with changed transforms; many effect
transforms are static. This handles model motion only, not texture scrolling,
color animation or particle simulation. Visual testing must establish whether
this improves the colored effect sufficiently; additional material animation
capture may be required. The capture is truncated and has sampled-history gaps.

User reports pipe-mouth expansion still looks 30 FPS. Timing trace keeps scene
interpolation active (no unsupported HUD), with occasional cadence dips. Frame
pair `860305975489` shows pipe geometry differences, but a single pair does not
verify note-triggered expansion. Existing pipe skeleton samples contain scale
ranges approximately .009–.012 on bones 2–6. An input-timing-only diagnostic now
logs native and overridden column scales for slots 0–5 of the captured
1284-vertex pipe draw, every tenth guest frame. It requires the verified pipe
signature in current poses. Next capture should repeatedly tap individual notes
to test expansion directly, separately from the colored song effect.

Live note-tapping trace confirms intermittent missing pipe branch overrides:
slot zero overrides in 81/82 sampled draws, while slot one overrides in only
26/82. Some expansion scales (.0112–.016) are rejected; the later song interval
has all five branches at approximately .0105 with no overrides. Scene pairs
remain active. This disproves assuming the earlier replay covered live expansion.
The timing diagnostic now also logs each current pipe skeleton bone scale and
instance to distinguish draw-time scaling from equivalent-bone ownership before
relaxing matching or implementing draw-palette interpolation.

The subsequent scale trace and an exact residual replay establish a different
cause from loose scale matching: failed slots match current skin bones precisely,
but equivalent openings have different past poses. The verified 1284-vertex pipe
draw now validates the complete skin palette order [2,3,5,6,4] before selecting
the attachment and resolves these slots by explicit bone identity. Only slot zero
shares history agreement with Link; resting openings coinciding with Link's
transform must retain their own identities. Every mapped slot must still match
its current bone/bind transform under the original tolerances. Replay no longer
reports missing opening overrides on ready-history pipe draws. A regression
covers one expanded opening returning to the same current transform as all the
others while preserving its distinct midpoint scale. Visual confirmation pending.

### Song effect material investigation

A targeted input-timing diagnostic now samples three consecutive frames per ten
for draws whose slot-zero palette resolves to the verified four-bone song effect.
It records the native vertex shader uniforms, shader/swizzle hashes, texture
addresses and blending before midpoint overrides. This is read-only and active
only during the requested timing trace. It should reveal animated texture
transforms or other shader values independently of skeleton motion. It does not
yet capture texture contents or all fragment-stage material state; absence of
changing vertex uniforms would require examining those paths next. No effect
animation interpolation is added by this diagnostic.

The material trace identifies three 90-vertex song-effect layers, using the
verified world shader/swizzle, source-alpha blending (destination one or
one-minus-source-alpha), and scrolling offsets in vertex uniform rows
10/11/14/15 component W. A bounded per-instance/texture/blend history now
interpolates only those offsets in the private midpoint draw. Native uniforms
and other components remain unchanged. Recognition requires the verified song
skeleton palette. Frame gaps, generation resets, basis changes, nonfinite
values, or offset jumps of .25 or more retain native values. Layers maintain
independent histories. Tests cover midpoint values, separate layers, disabled
comparison, gaps, jumps, basis changes and reset. Visual verification pending;
this does not interpolate texture contents or fragment color animation.

### Dedicated multi-song recorder

F1 → Camera → Camera midpoint prototype → Capture song effect (20 seconds)
arms a three-second delay and exports `song-effect-<timestamp>.json` under
mm3d-local automatically. Capture one song per file, with F1 closed. Three
consecutive guest frames per ten record all native top-screen color draws,
independent of the known song model/shader. Each sample contains frame/time,
shader/swizzle hashes, 96 vertex-uniform vectors, texture addresses, blend/depth/
stencil state, geometry base/index offset/count and acceleration status. Data is
bounded to 16,000 draws; the exported truncated flag reports reaching the cap.
The recorder does not modify rendering or enable interpolation. It stores local
diagnostic data only; do not package captures. Geometry is represented by
references, not vertex payloads; texture pixels and fragment uniforms are not
captured. Those require follow-up for effects driven outside vertex uniforms.
Lifecycle checks cover arming delay, sample cadence, cap, restart and timeout.

Song inventory: Healing capture `song-effect-1791739439827.json` reaches the
16,000-draw cap at 12.75 seconds, but includes the effect's full observed interval
(3.73–8.74 seconds) and subsequent gameplay/UI. Two world-shader layers appear
only during the song: 90 vertices with SourceAlpha/One and 288 vertices with
SourceAlpha/OneMinusSourceAlpha. Both use swizzle `6f0b398b1fc186a9`. Across
46 samples each, vertex uniforms 0–19 change only in texture translation W:
90-vertex layer rows 10,11,14,15; 288-vertex layer rows 11,15. Texture matrix
bases remain fixed. Both fit the existing offset interpolation math. The existing
90-vertex/skeleton recognition does not cover the 288-vertex layer; recognition
must be extended using verified evidence, not raw session texture addresses.
This is classification evidence, not a claim that Healing is fully interpolated.

Song inventory: Soaring capture `song-effect-1791739532576.json` contains
15,538 draws, not capped, spanning 0.01–19.75 seconds. The song interval includes
90- and 288-vertex additive world-shader layers at 2.62–7.37 seconds. Both vary
texture translation W in rows 10/11/14/15 with stable bases, supporting reuse
of offset interpolation. Two four-vertex alpha-blended draws per sampled frame
share the 288-vertex texture and its uniforms; these must be checked for actual
texture usage and draw ordering before assuming those inherited values drive
visible motion. Later capture data includes a scene change; do not classify its
new scenery/materials as song effects. No Soaring recognition changes made yet.

Song inventory: Storms capture `song-effect-1791739637170.json` reaches the
16,000-draw cap at 10.61 seconds; the two observed 360-vertex additive effect
layers occur from 4.13–7.21 seconds, before truncation. Both use the verified
world shader/swizzle and fixed texture bases with animated W offsets in rows
10/11/14/15. They fit the texture-scroll math, but need separate recognition
from the 90/288-vertex layers. Associated four-vertex alpha-blended draws also
change row-8 color/alpha; inherited unused uniforms must not be treated as proof
of visible animation. Later variable-count draws may be weather/particles and
are not covered by the texture-scroll classification. No Storms fix added yet.

Song inventory: Sonata capture `song-effect-1791739707497.json` reaches the
16,000-draw limit at 10.66 seconds but includes the observed effect interval
3.64–8.39 seconds. The 90-vertex additive layer animates translation W in rows
10/11/15; its row 14 is static. The 480-vertex additive layer animates W in
10/11/14/15. Both have fixed texture bases and verified world shader/swizzle,
so their material motion fits the shared scrolling approach. A separate
90-vertex alpha layer changes row 15, and two four-vertex alpha draws per frame
share the 480-vertex texture/uniforms; actual active texture usage needs checking
before interpolating inherited state. No additional material animation type is
established by this capture. Recognition and visual validation remain pending.

Song inventory: user-labeled Goron Lullaby capture
`song-effect-1791739782917.json` reaches the 16,000-draw cap at 9.88 seconds;
its observed effect interval is 4.13–8.88 seconds. The 90/480-vertex additive
layers have the same texture references, fixed bases and scrolling components
as the preceding Sonata capture. This supports reuse of the same material path;
it does not independently establish song identity or identical texture contents
(the recorder stores addresses, not pixels). No new interpolation method is
identified. Recognition/visual verification remain pending.

Song inventory: user-labeled New Wave Bossa Nova capture
`song-effect-1791739931276.json` contains 16,000 draws, capped at 12.04 seconds.
The observed effect is present 2.98–7.73 seconds. It again uses the 90/480-vertex
additive layers seen in the Sonata/Lullaby captures, with W-only translation
changes in rows 10/11/15 and 10/11/14/15 respectively. No new animation method
is established. Song labels come from the user, not a captured song ID, and
texture addresses alone cannot prove identical appearance. Recognition and
visual tests remain outstanding.

Song inventory: user-labeled Elegy capture `song-effect-1791740013241.json`
contains 16,000 draws, capped at 15.10 seconds. The observed song effect interval
is 2.40–7.14 seconds. Its 90/480-vertex additive layers again animate texture
translation W (rows 10/11/15 and 10/11/14/15), with fixed bases. Texture addresses
differ from the preceding captures, reinforcing that session addresses cannot
be the recognition key. Later geometry after the effect is outside this material
classification. No additional animation method established; fixes remain pending.

Inverted Time (`song-effect-1791740094178.json`) and Oath
(`song-effect-1791740160511.json`) include their full observed material intervals
before their caps. Inverted Time matches the two original Time scrolling bases;
Oath matches the 90/480 temple-song pattern. Recognition now uses an explicit
allowlist of observed vertex counts and full fixed UV bases for Time, Healing,
Soaring, Storms and the temple-song family, plus the verified world shader,
source-alpha blend, depth enabled, stencil disabled and full color mask. This
avoids dependence on one song skeleton or session texture addresses. Histories
are separated by geometry base/index offset/count, texture addresses and blend.
Only perspective world replay receives these overrides; four-vertex overlays,
color/opacity animation, particles and unverified materials remain native.
Tests reject inherited song uniforms on screen quads and changed UV bases.
The material-scroll batch is implemented; visual checks across songs are pending.

Epona capture `song-effect-1791740432286.json` includes effect interval
2.56–7.31 seconds before capping at 13.29 seconds. Its 90-vertex alpha background,
90-vertex additive and 144-vertex additive layers use a fixed rotated second
texture basis (approximately [4.6982,1.7100;-1.7100,4.6982]) and animated W offsets.
Recognition now includes these exact observed bases/counts, including the shared
background also seen in the original Time trace. The existing interpolation math
already supports static rotated bases. Tests exclude altered rotation and
four-vertex overlays with inherited values. Visual verification pending.

### Goron drum idle movement

Capture `link-pose-1791740727715.json` identifies seven-bone `link_goron_gakk`
from the local Goron archive, signature `d56b42acae52759f`. Several drum bones
have equal current transforms; the 264/252/258-vertex draws were often missing
a slot-zero override while unrelated stale slots were matched instead. Admitting
this verified skeleton to the existing equivalent-bone path increases matched
slot-zero overrides from 103/384 to 252/384, with the remaining samples lacking
ready history. Agreement of candidate midpoint histories and unique-instance
requirements remain intact. The shared regression suite now exercises the
seven-bone drum signature, including gaps and divergence. User request focuses
on idle movement; independent drum-hit animation has not been established by
this fix. Visual verification pending.

### Water ripple investigation

Water capture `song-effect-1791741159210.json` reaches the 16,000-draw cap
at 16.63 seconds. Small dynamic batches with changing vertex counts appear during
water movement; uniforms alone cannot establish which geometry is the ripple
versus splashes. Recorder schema 2 adds attribute layouts and bounded vertex
buffer bytes for transparent depth-tested world-shader draws up to 144 vertices.
Limits: 4 KiB per buffer, 8 KiB per draw, 16 MiB total geometry, plus the existing
draw limit. Export reports geometry truncation separately. This is local,
read-only diagnostic data; no water interpolation has been implemented yet.

Water schema-2 capture `song-effect-1791741376271.json` identifies expanding
horizontal ring meshes: eight vertices per ring plus a trailing sentinel,
12 indices per ring, float XYZ/black RGBA/UV in separate 12/16/8-byte streams.
The two-half UV pattern and flat square geometry distinguish these from the
captured animated splash billboards. Ripple interpolation now validates this
layout/shader/blend/UV/shape, matches uniquely by stationary center and monotonic
radius/fade, and blends positions plus alpha only in the midpoint world replay.
Native buffers, UVs and the sentinel remain unchanged. New rings, duplicate
centers, frame gaps, texture/generation changes and large growth stay native.
Replay processes 155 sampled batches, producing 85 intermediate batches; sparse
capture gaps account for some remaining native samples. Tests cover expansion,
fade, sentinel preservation, new centers, gaps, texture changes and nonplanar
rejection. Visual verification pending; splash sprite animation is not changed.

First visual test unchanged: the renderer gate erroneously required
SourceAlpha/OneMinusSourceAlpha while all 155 captured ripple batches actually
use SourceAlpha/One/Add. Corrected to the observed blend. Offline verification
of the draw-count/layout/UV/shader/blend conditions now accepts all 155 captured
ripple batches; interpolation history still independently determines readiness.
This fixes the integration gate, not the mesh math. New visual test pending.

### Open-water hopping splash

Capture `song-effect-1791741919424.json` contains a distinct six-index splash
billboard: four active vertices plus a trailing entry, float XYZ/black RGBA/UV,
8x2 atlas advancing one cell per native frame. This differs from the already
supported expanding rings and the colored particle batch. A narrowly validated
single-sprite path now interpolates geometry and alpha for consecutive frames
at a stationary landing point; when the atlas advances, the midpoint draws the
previous and current cells at half additive weight each. Native frames retain
the original sprite. This is a crossfade, not reconstructed in-between artwork,
and may look softer. UV layout, shader, attribute layout and additive blend are
checked. Gaps, resets, texture/generation changes, jumps and new landing centers
stay native. Replay accepts 10 of 16 sampled sprite frames (sparse gaps remain).
Regression checks cover half weights, old UVs, gaps, moved landings and restarts.
Other multi-sprite particles remain native. Visual test pending.

### Chopped sign fragments

Capture `link-pose-1791742401976.json` identifies ten one-bone board/post
fragments from local `zelda_kanban.gar.lzs`, sharing a palette and rejected as
multiple owners. A bounded group of these exact fragment signatures now resolves
only if every independent history agrees per slot; duplicate signatures, model
or instance identities and unrelated owners remain rejected. This does not
select an arbitrary fragment or extend to the separate sign effect model.
Replay changes each fragment from 0/490 overrides to 320/490 (140 changed poses),
with sampled-history gaps retaining native transforms. Tests cover agreement,
rotation/translation, duplicate fragment identities, unrelated owners and
divergent histories. Visual fall verification pending. Hopping-splash work is
paused unresolved at the user's request; do not treat its last test as fixed.

### Zora underwater camera

Effect capture `song-effect-1791742864333.json` shows the sky and main world
sharing a rigid camera but using distinct clip ranges and different X/Y lens
scales underwater. At frame 9290 the sky lens is 1.395306/-0.837184 versus
1.416380/-0.872287 for the world. Same-frame SameLens rejected the latter even
while the sky kept presentation pairs active. The captured same-frame lens gate
accepts 7,065/12,735 draws before the change and 12,735/12,735 afterward.
A bounded alternate-lens path now shares only the camera delta within the same
frame, for an identical camera, distinct clip ranges, rotated perspective and
axis ratios within 8%. It retains the draw's native underwater lens, including
its native-rate wobble; it does not interpolate that wobble or broaden cross-frame
aspect-change acceptance. Large changes, skew, orthographic UI and camera
mismatches remain rejected. Visual verification pending.

Follow-up CPU capture `song-effect-1791773087522.json` exposes the complementary
case: the first world draw itself has an anisotropic lens that changes between
frames. The previous uniform-scale guard rejects 32 of 77 consecutive sampled
frame pairs. `GradualWorldWobble` accepts only rotated perspective with unchanged
clipping, both axes changing, and at most 0.5% change per axis per frame (capture
maximum 0.186%). It averages each lens axis independently alongside the camera
pose. Same-frame alternate clipping and the existing alternate-lens path retain
their separate behavior. Capture gaps, camera cuts, skew, nonfinite values,
orthographic UI, single-axis resizing and larger aspect changes remain rejected.
Offline replay improves from 4,797 to 8,248 accepted world draws out of 12,499;
all 8,248 draws with consecutive captured history are now accepted. Remaining
draws seed history after sampling gaps. Tests exercise the recorded projection
change, midpoint lens/camera composition and the rejection guards. In-game
verification in Zora's Cape and the water temple remains pending.

The follow-up swimming capture `song-effect-1791773575166.json` shows why the
first fix was intermittent: consecutive underwater lens axes change by up to
4.821%, rather than the first capture's 0.186%. For the verified depth row
`{0,0,1.0004167556762695,5.002084255218506}`, the per-axis envelope is now 5%;
other projections retain 0.5%. Both axes must change, and all other continuity,
camera, clipping and projection-shape guards remain active. Offline replay of
the new capture improves from 5,240/8,203 eligible draws to 8,203/8,203. The
earlier capture retains 8,248/8,248. Regression tests cover the stronger captured
change, exclusion of the normal scene depth range and rejection above the bound.
This corrects the observed guard failures; visual confirmation is still needed.

### Like Like squash/stretch

Capture `link-pose-1791743124754.json`, locally correlated to `zelda_rr.gar.lzs`
model `likelike`, identifies five-bone signature `e5524c5eb3c8c915`. Palette
matching already succeeds, but 526/900 consecutive local bone pairs reject the
rigid/scale decomposition because scale-compensated joints contain shear.
A signature/count-gated fallback averages local affine transforms only when
both are invertible and relative basis changes stay within .2 per component,
with relative translation at most 200 model units (capture maxima .191826 and
187.603). Native poses, actor identity, gap, camera and palette guards remain.
Offline draw replay changes from 0 to 144 changed midpoint transforms among
219 matching Like Like draws; 75 samples lack continuous history. Other actors'
replay counts are unchanged. Tests cover shear, translation, scale/position
jumps, singular matrices and exclusion of unrelated actors. Visual test pending.

### Great Bay Leevers

Capture `link-pose-1791743406717.json` correlates locally with
`zelda2_rb.gar.lzs`, model `reeba`, eight-bone signature `7526de611fb49ce3`.
All 320 sampled consecutive bone pairs pass the existing animation math.
The 162-vertex main section instead has equivalent skin bones in slots 0/1,
so generic owner matching misses it (and can match stale later palette slots).
Added this exact signature/count to the existing equivalent-bone resolver.
It still requires a unique actor and agreement among all matching midpoint
transforms. Replay increases changed slot-zero results from 72 to 111 of 168
Leever draws; gaps in sampled history retain native poses. Existing equivalent
bone tests now also cover Leevers, including ambiguity, divergent history,
disabling interpolation and gaps. Visual verification pending.

### Hookshot aiming dot

Effect capture `song-effect-1791743687771.json` identifies a paired 24/6-index
marker. Captured geometry matches local `zelda2_link_child_new.gar.lzs`, model
`pointer_modelT`. Actor capture `link-pose-1791743808032.json` confirms its
three-bone signature `3ed3e32e1ee38993`, with equivalent bone transforms and
identity binds. Generic Identify rejects the ambiguity, leaving the dot native
while the camera moves at the midpoint. Added this exact signature/count to
the equivalent-bone resolver; unique actor and midpoint agreement remain
required. Replay changes from 0/180 to 120/180 interpolated draws, covering
both marker sections; the remaining 60 are sampled-history gaps. Other replay
results, including the separate one-bone pointer/beam model, are unchanged.
Equivalent-bone regression cases include this signature. Visual test pending.

### Zora combat fin deployment

Capture `link-pose-1791744036367.json` confirms the fins are bones 24/25 of
28-bone Zora Link (`42c4c515d83599db`), drawn as two 120-index parts. Matching
already works. Four captured local bone pairs fail the default 2:1 scale guard
when fin scale switches between .4 and 1. Permit up to 2.51:1 only for those
two bones of that exact skeleton; other bones/models retain 2:1 and all rigid,
finite, nonzero, history and palette checks. Replay changes the native-size
results at frame 2512 into intermediate fin sizes (two additional changed draws,
597 to 599); other actor results are unchanged. Tests cover extension/retraction,
excessive/zero scales and the actor-signature restriction. Visual test pending.

### Extended Zora blades following running arms

User clarified deployment was not the reported problem. Running capture
`link-pose-1791744384883.json` shows the 120-index blades matching both direct
forearm bones 16/20 and fin bones 24/25 in the current pose. The fins retain
parent 23, unlike the articulated arm chain; their independently computed
midpoints disagree, so ResolvePalette rejects slot zero. For the exact Zora
signature/count and 120-index draw, when the current palette matches BOTH a
forearm and its corresponding fin, select that forearm's midpoint directly.
Retracted fins that no longer match, other geometry and other actors remain
excluded; ambiguous arms with disagreeing midpoint results remain rejected.
Replay changes moving fin slot-zero results from 19 to 72 of 108 matched fin
draws, with the remaining 36 at sampled-history boundaries. Regression checks
cover both arms, divergent fin hierarchy, retracted fins, wrong geometry and
wrong actor. Visual running verification pending.

### Shared worn-mask head attachment

Blast Mask capture `link-pose-1791744635042.json` (single-bone `mask_bomb`,
`9c5589d73f0deea4`) and Stone Mask `link-pose-1791744831269.json` (`mask_stone`,
`533d208b50f73607`) match human Link's head bone 11 in all 90/89 available
paired samples. Both appear as ambiguous owners alongside Link. A shared rule
now resolves a single-bone attachment through the verified human head hierarchy,
requiring current root/head/palette agreement, distinct model/instance and the
same attachment identity/generation on consecutive frames. No per-mask signature
list is needed. Gaps and newly equipped instances seed history. Multiple owners
beyond the human/attachment pair and multi-bone animated masks remain outside
this rule. Existing transformation-mask handling remains as a fallback.

Offline replay MUST include the separate `poses` human samples and locally
derived human binds: `actors` alone omits human Link and hides this ambiguity.
Complete replay changes Blast Mask from 0 to 235/355 changed draws and Stone
Mask from 0 to 210/320, with capture gaps remaining native. Tests cover both
signatures plus an arbitrary single-bone attachment, gaps, instance changes and
disagreeing attachment history. Visual tests of both masks pending.

### Great Fairy's Mask animated attachment

Capture `link-pose-1791745198774.json` correlates to seven-bone `mask_fairy`
(`2090ecc88f377de1`). Root times inverse bind matches human head 11 in all
90 paired samples, although the raw root has a fixed rotation offset. Extend
the head-attachment path to this verified animated mask. Reconstruct its root
from the head midpoint and fixed bind offset, then interpolate the mask's own
local child transforms under that root. Other multi-bone models remain excluded.
Replay including human poses changes from 0 to 252/382 mask draws interpolated;
sampled-history gaps remain native. Regression checks verify head-root authority
and independent child animation. Visual verification pending; Garo not captured.

### Garo's Mask

Capture `link-pose-1791745689989.json` identifies three-bone `mask_garo`
(`15ad1b7410d5bd8d`). Root times bind matches human head 11 in all 90 paired
samples. Added this exact signature/count to the animated head-attachment path
used by Great Fairy's Mask. Complete replay goes from 0 to 250/375 interpolated
mask draws; remaining samples are history boundaries. Child-animation regression
checks also exercise the three-bone case. Visual Garo verification pending.

### Death darkening order

Effect capture `song-effect-1791745928949.json` contains 72 early full-screen
black orthographic quads after four translucent/additive sky draws but BEFORE
opaque world geometry. Example frame 17390: sky draws 0–3, black quad 4, opaque
geometry from 5 onward. The previous split considered sky to be the start of the
world and moved that black quad to the final HUD layer, covering geometry that
the native frame draws afterward. Track the first opaque depth-tested world
draw separately; preceding screen-space draws remain in the private scene in
native order. Later HUD stays separate. Capture replay identifies only those
72 black quads as changing destination; other ortho draws keep their placement.
Camera-midpoint regression suite passes. Death visual verification pending.
Moon-crash effect diagnosis is deferred per user request for this capture.

### Moon-crash background flash

Capture `song-effect-1791746232099.json` shows the same early background overlay
as death, but preceded by an opaque 288-index night sky dome. The initial death
fix therefore still classifies it as late HUD. Recognize that captured dome by
geometry count, sky clipping projection, camera-centered translation and rigid
320-scale basis; exclude it from the first opaque foreground marker. Ordinary
288-index foreground models remain excluded by position/scale/projection guards.
Offline ordering replay changes only the 167 early background quads; late bloom,
fades and feedback remain at their previous positions. Regression tests cover
sky recognition and rejection of foreground, wrong geometry and orthographic
matrices. Moon-crash visual confirmation pending.

### Title-screen camera diagnosis (pending)

Title timing trace at log seconds 121–143 reports no presented midpoint pairs,
about 360 CPU-camera rejections and 30 first-frame/history gaps per second, with
no sustained unsupported-HUD, lens-change or camera-cut rejection. This suggests
mixed accepted/rejected camera draws resetting history, but does not establish
why those cameras differ. Added timing-trace-only camera diagnostics: two frames
per 30 guest frames, at most 24 draws each, including CPU snapshot age/position,
render camera and projection. No camera acceptance rules changed. Next step is
a fresh title-screen timing trace using this build.

Follow-up title trace (log seconds 35–64) includes accepted world draws but no
presented pairs. During the faster orbit, the CPU camera can already be ahead of
the submitted render camera (e.g. frame 541 render eye matches frame 540 CPU
eye); this continues to be rejected. The first-24-draw diagnostic missed later
rejections in otherwise matching frames, so accepted/rejected draws now have
separate bounded trace quotas.

A rejected draw now returns native projection without erasing the last validated
world-camera history. It cannot advance or borrow interpolation history. This
allows consecutive validated world frames to survive intervening auxiliary draws;
a whole missing/rejected frame still triggers the existing frame-gap guard, and
camera cuts/lens changes still fall back. Regression cases cover mixed cameras,
same-frame return to the world camera, fully rejected frames and cuts. Title
visual verification and any remaining fast-orbit rejection are pending.

### Title logo ordering

Capture `song-effect-1791747472142.json` (13,881 draws, not truncated) shows
12 perspective logo draws following a screen-space overlay. They use a fixed
identity camera translated to Z=30 and a fixed projection, unlike the moving
world camera. Previously they replayed into the world target, underneath the
separately composited screen overlay. Route this narrowly matched camera/lens
through the ordered HUD replay instead, retaining native logo transforms and
its depth test. The HUD target now has its own depth attachment, reset at the
first logo draw each frame; its clear alpha stays transparent. Alpha coverage
and additive highlights use the existing HUD blend handling. Recognition tests
exclude orthographic and displaced world views. Visual verification pending.

### Opening-cutscene Tatl wings and Skull Kid mask

Skeleton capture `link-pose-1791748112354.json` contains two identical five-bone
Tatl instances (model 85d72a4, instances 149c6a48/149ca0bc) and a separate fairy
(model 8fecae4) already interpolating. Permit duplicate verified fairy owners
only when complete current poses/binds/topology agree AND each independently
tracked midpoint palette agrees. Divergent histories and partial-pose matches
remain rejected. Applies to wings and the existing glow path.

Skull Kid `stalkid` (24 bones, signature 2507dd32b95b7c14, model 8e09748)
has equivalent skin transforms on some materials. Added its exact signature/count
to the equivalent-bone resolution list, retaining midpoint-agreement checks.
Complete capture replay: Tatl changes from 0 to 896/1360 matched draws (duplicate
owner accounting); Skull Kid from 684 to 798/1204. Other model counts are unchanged.
The three-of-ten capture sampling creates deliberate history gaps. Regression
checks cover fairy duplicate agreement, divergent current poses, crossing prior
trajectories, disabled mode and Skull Kid equivalent-bone ambiguity. Visual check
pending.

### Interpolation performance comparison

Advanced now offers `Capture interpolation performance (30 seconds)`, with a
three-second delay to close settings. Run once with the user-facing interpolation
toggle off and again on in the same scene, resolution and draw-distance settings.
Keep both runs in the same session/log and avoid input-timing/effect captures at
the same time. `Interpolation performance` log lines identify each session and
both replay/presentation flags. A mode change cancels the capture.

CPU wall-time scopes report draw handling (inclusive), extra-frame scene/overlay
replay (inclusive of skeleton work), and skeleton matching/interpolation. Scopes
overlap and must not be added together. Window-callback intervals are reported
separately; they are neither display scanout nor GPU-completion measurements.
Inactive scopes only check an atomic flag. This diagnostic establishes CPU costs;
GPU bottlenecks still require GPU timestamps or an external GPU profiler.

### First interpolation CPU optimization

User's 30-second off/on captures: draw CPU time 1818.48 ms off versus 4115.00 ms
on; replay scopes 2968.41 ms, of which skeleton scopes 2797.44 ms. Window-callback
mean interval 16.716 ms off versus 18.249 ms on. These are CPU wall-time samples,
not GPU timings, and the number of draws differs between the runs.

Cache expanded direct/skin matrices per skeleton instance (128-entry bound),
refreshing on any bone/bind byte change, including same-frame updates. Candidate
matching retains singular/nonfinite/ambiguity/tolerance guards and checks position
before basis. Prune old actor/candidate entries once per frame instead of once
per material. Frame reset clears both caches. No midpoint math is changed.

Local parsed-capture benchmark, 40 replays of `link-pose-1791748595682.json`:
397.373 ms before, 297.621 ms after (~25% reduction), identical 293120 accepted
palette-slot count. Aggregate per-model changed/accepted counts also match.
This is a CPU microbenchmark, not an overall FPS gain; live profiling remains
necessary. Added reference-matcher equivalence tests for pose/bind changes,
ambiguous bones, tolerance boundaries, nonfinite values, singular and empty poses.
Second capture (`1791748112354`, opening cutscene): 1268.14 ms before versus
932.175 ms after (~26% reduction), identical 809000 accepted palette slots.

Live follow-up after the first cache: skeleton mean 14.248 us/call (previous
19.196), inclusive replay 13.587 us/call (previous 17.943); mean window interval
17.745 ms (previous 18.249), still over 16.667 ms. User reports unsatisfactory
smoothness. Counts differ, so per-call CPU changes are more useful than totals.

Second pass adds conservative translation bounds over all direct/skin candidates,
using the original .03 subtraction tolerance and full matching on survivors.
Within each immutable Apply call, each pose snapshot is validated only once,
including when later palette slots query the same actor. Cross-call/same-frame
pose changes are still checked. Saved-capture benchmarks: 251.405 ms and 784.111 ms,
versus first-cache 297.621/932.175 ms (~16% additional CPU reduction). Accepted
palette counts remain 293120/809000. Added positional tolerance boundary checks.
GPU cost is not measured; testing lower internal resolution remains necessary to
separate CPU matching overhead from extra-frame GPU rendering cost.

### Replay attachment barriers

PromptLayer previously ended every private replay draw with an attachment-write
→ fragment-read barrier, then began the next write with broad all-command/
transfer dependencies. Private scene/HUD targets are not sampled until composition.
Their End now defers the sampling barrier; DrawScreens prepares visible targets
before starting the presentation render pass. Repeated presentation without writes
adds no new barrier. Consecutive unsampled, uncleared writes use color-output and
early/late-depth dependencies; first use, clears, depth resets and writes after
sampling retain the broader path. Other prompt layers keep immediate preparation.

Extended ROM-free Vulkan fixture with consecutive deferred passes followed by
sampling preparation. Both --midpoint-target and --prompt-only pass on Metal
outside the sandbox: color/depth occlusion, coverage, accumulation, reset, resize,
and toggling. Mac app build succeeded. This reduces synchronization commands but
is not yet a measured live GPU/FPS gain. User's audio slowdown and resolution
sensitivity still need live verification at unchanged resolution.

### CPU wait and GPU submission profiling

The existing 30-second performance capture now includes scheduler worker waits,
GPU completion waits, driver submission, presentation-frame acquisition, screen
composition and presentation queueing. CPU scopes overlap; their totals must not
be added together as a frame budget.

Vulkan records top/bottom timestamps around command-buffer submissions while the
capture is active. A bounded 64-slot query ring is read asynchronously after the
submission completion tick; busy slots are skipped rather than forcing GPU waits.
Query resets occur outside render passes. Timestamp valid bits and device period
are respected. Results are completed submission spans (including GPU waits), not
GPU utilization, display latency, or separate native/interpolated draw timings.
Spans can overlap; the unfinished capture tail is excluded. Unsupported devices
still report CPU timings. Query allocation failure disables GPU measurements.

Validation: `mm3d_vulkan_fixture --midpoint-target --performance` exercises real
GPU timestamp availability, readback and recycling beyond the ring capacity while
checking replay color/depth behavior. Its blocking submissions are test-only.
Use the same slow scene, resolution and draw distances for off/on comparisons;
avoid other diagnostic captures. No runtime speedup is claimed from this profiling.

### Upload-buffer wait investigation

`StreamBuffer::WaitPendingOperations` now waits once for the latest submission
covering the requested reuse range. Completion of that tick on the same ordered
graphics queue protects all earlier overlapping allocations. Already-completed
ticks return without entering the wait/profiling path. No buffer is overwritten
before its required completion tick, and buffer capacities remain unchanged.
Capture CPU counters distinguish uniform, texel, vertex, upload and download
buffer reuse waits. These overlap the scheduler wait counter, not additive time.
This reduces redundant checks but does not eliminate necessary GPU waits; a
runtime capture is needed before claiming a performance benefit.

The presentation-phase GPU labels are submission labels only. Native and replay
draws are interleaved before presentation; these labels do NOT isolate replay GPU
cost. The CPU native/midpoint pass scopes cover presentation preparation, not
scene generation or interpolation calculations. Do not infer isolated rendering
costs or display frame misses from these scopes alone.

### Forced-wait source attribution

`Scheduler::Wait` captures its C++20 source location; `Finish` forwards its caller
so profiling names the actual rendering/readback site instead of the scheduler.
During the existing capture, a bounded in-memory table aggregates call count,
total/mean/maximum CPU wait duration by file and line. `Interpolation GPU wait
source` records are emitted only in the capture summary. These overlap the
scheduler GPU-wait totals; they are CPU blocking durations, not GPU draw costs.
Already-completed waits remain excluded. No synchronization is removed by this
instrumentation, and no automatic capture is started.

Readback attribution additionally records the initiating purpose and requested
memory range: presentation texture identity, diagnostic texture recording,
vertex data, external memory reads/writes, invalidation, or flush-all. Thread-local
scoped context survives synchronous nested cache downloads and restores the
previous context on exit. Unclassified cache operations remain explicitly marked.
The bounded source table now groups by source, purpose, address and size; overflow
is reported. This instrumentation does not relax readback synchronization.

The recorded physical addresses 0x1853xxxx–0x1854xxxx are in VRAM
(physical base 0x18000000), mapping to guest virtual 0x1f53xxxx–0x1f54xxxx.
They are not FCRAM. Four-byte reads take the cache's small-read path, which
flushes the overlapping dirty region, not just four bytes. Current attribution
cannot alone distinguish a game read from a host hook using MemorySystem.
A targeted trace now records the first eight downloads per performance capture:
surface address/dimensions/format and whether a direct Dynarmic 32-bit VRAM
read is active. It records guest PC/LR registers as leads for disassembly; the
PC can be a JIT block location rather than the exact load instruction. No
readback semantics are changed, and a missing JIT label does not itself prove
that a presentation hook caused the read.

### Depth readback consumer (local disassembly investigation)

Capture: direct Dynarmic 32-bit reads at virtual 0x1f5c2434 and nearby
addresses, register PC/LR 0x183e20, surface physical 0x18537fc0,
512x400 D24S8. All eight sampled origins are direct guest CPU reads.
The restored-game local binary (classic-actual-code.bin) has a matching
routine: 0x183e1c calls the accessor at 0x1d71ac; its return is 0x183e20.
The actual depth load is 0x183e98. The code computes a tiled pixel offset,
loads a word, masks the upper byte at 0x183e9c, and compares depth at
0x183ea8. Nested loops cover offsets -1..1 (3x3 samples), then the outer
loop processes eight entries. A majority threshold writes a visibility result.
This explains why the JIT-reported PC was a return location, not the load.
This is local-binary evidence, not an instruction-signature check against the
currently running ROM; do not introduce guest patches from this alone.

The unrelated cci-test code_decompressed.bin has different instructions at
these addresses and must not be used for this investigation. Temporary ELF
wrappers/disassemblies stay in /tmp; no game binary is added to the repository.

The cache deliberately broadens <=8-byte reads to the whole overlapping dirty
region to avoid repeated downloads for nearby pixel reads. Blindly narrowing
to a single pixel risks multiple forced GPU waits for the 3x3/eight-entry loop.
D24S8 uses DepthToBuffer compute conversion followed by Scheduler::Finish.
Removing that wait or supplying older depth would change visibility semantics.
A next optimization must preserve current native depth while reducing readback
work or separating unrelated replay work from its completion dependency.

### Separate presentation queue experiment

Opt in per process with `DEKUI_SEPARATE_PRESENT_QUEUE=1`. When the graphics
family exposes at least two queues, reserve queue 0 for emulation/rendering and
queue 1 for final presentation copies and WSI presents. Both use the same family,
so image ownership does not change. The render-ready semaphore orders composition
before the copy; the present-done fence protects frame reuse. All windows share a
separate presentation-queue mutex; the default single-queue path retains the
scheduler submission lock. Device capability and activation are logged at startup.
Without the environment variable, or with fewer than two queues, behavior remains
on the original queue. Nothing is saved into user settings.

This isolates presentation dependencies only. It does not move world replay draws
out of the native render command stream, skip visibility reads, or provide older
depth values. A full replay reorder would require preserving texture/buffer state
across intervening native draws; simply postponing existing commands is unsafe.
The experiment tests whether shared presentation ordering contributes to the
observed depth-readback wait. It is not a claimed GPU throughput improvement.

The captured M1 Pro run reported `requested=true active=false family_queue_count=1`.
That run used the fallback and cannot establish a separate-queue performance result.

### Isolated depth readback baseline

Build `mm3d_vulkan_fixture` and run it with `--depth-readback` on a Vulkan-capable
host. No ROM or gameplay capture is required. It exercises production
`Surface::Download` / `DepthToBuffer` at 1x, 2x and 4x, using a 512x400 D24S8
surface like the observed game surface. It compares a full rectangle with an
offset 8x8 rectangle, verifies every returned depth/stencil word against changing
clear values, and reports 32 warm samples after four warmup samples per case.
Clears finish before timing; the measured wall time includes compute conversion,
submission, synchronization and staging commit, without queued gameplay or WSI.
Uniform clears verify current contents and packing, not spatial sampling accuracy.

These numbers are a baseline, not a gameplay speedup prediction. A small read
may lead to multiple stalls for the game's subsequent reads. This fixture does
not change the game's flush-region policy or return stale depth. If isolated
readbacks are much cheaper than the captured waits, investigate preceding work
and submission dependencies before optimizing transfer size. East Clock Town
near the jugglers is the user's reported problem scene.

### Correlated synchronous submission timeline

The existing **Capture interpolation performance (30 seconds)** button now
records the CPU timeline of each `Scheduler::Finish` during the capture:

- Request to worker reaching the submission command (includes caller setup,
  dispatch, queued commands, worker scheduling and command recording).
- Worker waiting for the graphics queue mutex.
- Acquiring that mutex to the return from queue submission (includes final
  timestamp recording and command-buffer finalization).
- Submission return to the caller observing completion (includes GPU queue
  dependencies, execution and host wakeup; **not pure GPU execution**).

Each record carries the scheduler tick, caller and memory-read context. GPU
submission timestamp spans are attached to the same record when available;
their clock domain is separate and their spans can include earlier rendering
and dependencies. CPU completion can be observed before `vkQueueSubmit` returns
on the worker; that overlap is reported separately, not counted twice or wrapped.

The presentation thread separately records pacing, image acquisition/recreation,
command recording, the combined queue/surface lock acquisition, queue submission,
the actual present call and the full lock hold. For shared queues, completed
presentation lock holds are intersected with each Finish's worker lock wait.
This establishes temporal overlap, not GPU utilization or physical scanout.
Recreation lock holds are not separately correlated (avoid resizing while capturing).

Logs contain aggregate phase totals, the first four and twelve slowest completed
synchronous submissions, and the twelve longest presentation lock holds, using
capture-relative CPU timestamps. Records are bounded to 2048 Finish calls and
4096 presentations; overflow and unfinished tails are reported. The existing
per-address wait-site overflow does not truncate this separate timeline.
There are no new GPU waits or changes to submission/presentation ordering.
Tracing is inactive outside the existing capture window.

`mm3d_interpolation_timeline` tests phase accounting, overlapping host completion,
incomplete records and presentation correlation across queues, schedulers and
capture sessions. The Mac isolated readback baseline passed at all three scales:
full 512x400 downloads averaged 0.27–0.30 ms versus 0.23–0.25 ms for 8x8.
That result excludes preceding gameplay work and does not prove an equivalent
reduction is achievable in the captured ~28 ms Finish waits.

First correlated M1 Pro capture (East Clock Town, interpolation enabled): 666
completed synchronous readbacks, no incomplete/dropped submission records.
Mean Finish was 26.995 ms: 0.022 ms before the worker, 0.093 ms graphics mutex,
3.627 ms submission/finalization, and 23.254 ms after submission returned.
The actual present call averaged 0.008 ms. Shared presentation lock holds
overlapped 61.743 ms of Finish waits across the full capture; a few individual
lock waits reached 11–13 ms, so contention explains some spikes, not the steady
~27 ms wait. Typical logged readback submission GPU spans were about 22–29 ms.
The first logged submission was an exception (0.154 ms GPU span after a ~31 ms
host wait), demonstrating why a single sample cannot stand in for the workload.

This rules out late worker dispatch / CPU queue-mutex contention as the dominant
cost in this capture. It does not distinguish shader work from GPU barriers,
render-target load/store traffic or queue dependencies. Native and midpoint
draws still alternate render targets per replayed draw (PromptLayer::Begin/End).
That pass switching is a concrete next rendering-cost candidate, especially on
tile-based Apple GPUs; no speedup from batching has been measured. A change must
preserve live texture, geometry and uniform lifetimes rather than postponing
draw commands blindly. These Mac findings are not Steam Deck measurements.

### Render-target switching A/B fixture

`mm3d_vulkan_fixture --pass-switching` measures the proposed batching benefit
before changing live game resource lifetimes. It uses the production PromptLayer
colour/depth targets with 192 overlapping synthetic triangle draws per target,
at 512x400 times 1x, 2x and 4x. Modes are native only (one pass), alternating
native/midpoint per draw (384 passes), and grouped native then midpoint (two
passes). Each uses three warmup frames and twelve timed frames. Allocation is
outside the timed frames; clear, command recording, submission and completion
are included. It reports wall time and GPU timestamp spans if supported.
Timestamp readout polls availability for at most 50 ms outside wall timing.
If results remain unavailable, GPU timing is disabled for the rest of the run;
wall timing and pixel comparisons continue. GPU means/ratios are explicitly
`unavailable` unless all twelve measured samples have timestamps. Missing
timestamps never count as zero-duration work. The initial M1 Pro run stopped
on a not-ready timestamp before any mode completed; it provided no A/B result.

Readback is outside timing. Full colour images must match byte-for-byte between
alternating and grouped modes for both targets, while native and midpoint must
be visibly distinct. Varying depth and overlapping geometry exercise preserved
occlusion across render passes. These simple shaders isolate switching overhead;
they do not simulate MM3D shader cost, native framebuffer format, presentation,
or changing textures/vertex buffers. Neither timing ratios nor matching synthetic
pixels establish a safe or equivalent gameplay speedup. This executable changes
no user settings and requires no ROM. The initial fixture did not change game
draw ordering; the subsequent implementation is described below.

M1 Pro results after the bounded timestamp-readout fix (all image comparisons
passed; all twelve measured GPU timestamp samples available per mode):

| Scale | Native wall ms | Alternating wall ms | Grouped wall ms | Alternating GPU ms | Grouped GPU ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1x | 0.574 | 21.865 | 0.979 | 17.716 | 0.307 |
| 2x | 0.813 | 24.689 | 1.166 | 19.324 | 0.417 |
| 4x | 1.096 | 25.361 | 1.386 | 20.039 | 0.637 |

Grouping identical synthetic draws reduced wall time by about 18–22x, showing
substantial pass-switching overhead on this device. It is strong justification
for a guarded batching implementation, not a predicted gameplay speedup or a
Steam Deck result. Real batching must retain per-draw bindings/uniforms and
drain deferred work before resource mutation/reuse, feedback copies, readbacks,
submission or presentation. The synthetic fixture has no such dependencies.

### Guarded replay batching

Guarded Vulkan replay batching is enabled by default. No special launch command
or saved setting is required. `DEKUI_BATCH_INTERPOLATION=0` disables it for
diagnostic comparisons; `=1` explicitly enables it. This does not enable
interpolation itself: the existing interpolation toggle still controls that.
Compatible accelerated
world replays are recorded separately, retaining their vertex/index bindings,
uniform offsets and descriptor sets. Native and replay passes have separate
recording state, allowing several native draws followed by their replays.
Pipeline state is rebound when switching recording lanes or draining replay.

Ordinary commands, framebuffer changes, render-pass endings, worker waits and
submission drain pending replay first. Texture uploads, feedback copies and
readbacks therefore bound batches. Shadow and CPU geometry draws do not bypass
the drain. Replay commands remain in their original scheduler resource tick;
stream-buffer waits and submission drain before that tick advances. Captured
scopes contain no stream-buffer mapping or submission. A chunk limit bounds
queued replay memory, with a drain between scopes when the limit is reached.
The performance capture reports batch count, mean draws per batch and maximum
draws per batch so frequent boundaries can be distinguished from effective
grouping in gameplay.

The pass-switch fixture now also runs `guarded_batching` and
`guarded_boundaries` through the production scheduler and render manager. These
compare full target images with the immediate path, verify actual command
execution order, and report measured render-pass counts. Boundary mode drains
every 24 draws. A separate command test exercises copied payloads, chunk
rollover, the memory limit and worker-wait draining. These checks do not model
every game-resource dependency or establish gameplay correctness.

Validation order: build and CPU tests, then run
`./build-macos/bin/Release/mm3d_vulkan_fixture --pass-switching` on an accessible
GPU before testing the path in-game. The user ran the updated fixture on
the M1 Pro successfully: command snapshots, chunk rollover, bounded batches,
resource boundaries, worker draining and all target image comparisons passed.
All twelve GPU timestamp samples were available per mode. The latest full run:

| Scale | Alternating wall ms | Guarded wall ms | Guarded passes | Boundary wall ms | Boundary passes |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1x | 21.440 | 0.926 | 2 | 1.675 | 16 |
| 2x | 25.120 | 1.272 | 2 | 1.945 | 16 |
| 4x | 25.739 | 1.882 | 2 | 2.582 | 16 |

At 4x, guarded GPU time was 1.034 ms versus 20.610 ms alternating;
boundary mode was 1.622 ms. The guarded implementation closely matched ideal
grouping in this synthetic workload. The user then tested batching in-game and
reported that it fixed the slowdown. At the user's request, batching is now the
default. A quantified gameplay comparison and Steam Deck validation remain
pending; the synthetic timing ratios are not gameplay speedup measurements.

### CPU vertex interpolation option

Graphics now offers a **60 FPS method** selector alongside the existing enable
checkbox, in both the player menu and editor: **60 FPS - GPU interpolation** or
**60 FPS - CPU interpolation**. The selection is saved as `interpolation_cpu_vertices`
in presentation settings and retained when 60 FPS is turned off. Missing or
invalid values default to CPU; explicitly saved GPU selections are respected. Changing
the method resets interpolation history and requests the matching hardware
shader setting. While 60 FPS is enabled, that setting is controlled by the
method selector. No restart or launch flag is required; the initial
`DEKUI_CPU_INTERPOLATION` prototype flag is no longer used.

CPU interpolation requests hardware shaders off and also bypasses hardware
vertex acceleration while interpolation is active. Shader JIT remains available: it
executes the game's vertex programs on the CPU; Vulkan still rasterizes and
runs fragment shaders. GPU pipeline compilation is not eliminated.

The native CPU draw captures resolved vertex inputs for the verified MM3D world
shader. It retains native cache-hit ordering and copies default attributes,
instead of rereading guest memory for the extra frame. Replay uses an isolated
shader setup with the existing interpolated projection, bone palettes and song
uniform overrides. It runs the existing CPU shader engine and a private
primitive assembler. Native shader uniforms, shader cache pointer, guest
primitive state and original triangles are preserved. Index reuse and triangle
lists, strips and fans are supported; guest geometry shaders, cross-draw
primitive carry and immediate-mode world geometry are not replayed by this
prototype. Captures are bounded and cleared at draw boundaries.

Native and midpoint CPU output buffers are uploaded before entering recording
scopes. Their immutable stream-buffer ranges can therefore use guarded replay
batching with the same resource-tick lifetime rules as hardware draws. HUD,
bars, fades and supported screen-space effects reuse CPU-transformed native
geometry in the existing presentation layers. The raw-vertex history paths for
ripples, splashes and glare now also transform captured CPU inputs before
replay. Song effects continue to use their interpolated uniform overrides. The
splash path's separate blend-only extra draw and other unsupported CPU world
geometry still need dedicated handling; they fall back safely rather than
silently disappearing.

`mm3d_cpu_vertex_replay_tests` exercises both CPU engines with a small vertex
program: snapshot lifetime, input register routing, indexed reuse, primitive
ordering, changed transform uniforms, unchanged native shader state, reset,
incomplete captures and invalid indices. These tests do not establish in-game
visual equivalence. The existing performance capture now includes the nested
`cpu_vertex_replay_and_upload` scope for measuring the added CPU cost.

The user tested the initial CPU prototype on Mac and reported that it looked
fine and possibly felt better. Quantified performance and Steam Deck validation
remain pending. Compare methods at the same resolution and draw distances in
East Clock Town, checking camera/Link motion, NPCs and HUD stability, then use
the 30-second interpolation performance capture to measure costs.

Local validation: the macOS app builds, and the CPU replay (interpreter and JIT),
camera midpoint, player settings and interpolation timeline tests all pass.

### Lens of Truth after sky draws

CPU effect capture `song-effect-1791775216747.json` has two 18-vertex
additive stencil masks after four translucent sky draws but before opaque
scenery. Camera history already accepts every eligible world draw (8,348).
The mask guard previously rejected the frame after any world draw; it now
allows the verified mask until opaque foreground geometry begins. Shader,
projection, blend, texture and stencil guards are unchanged. Visual validation
is pending.

Pre-push review also removed the provisional CPU raw ripple/splash/glare input
mutator. Cache-miss order is not guest mesh index order, and that path lacked
verified attribute mapping and the splash's second blend pass. Those CPU effects
retain native input animation. Camera, skeleton and UV-uniform interpolation
remain enabled. The earlier CPU raw-effect port notes describe an incomplete
attempt, not supported release behavior.
