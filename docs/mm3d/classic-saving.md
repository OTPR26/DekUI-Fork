# Classic saving implementation status

Local work, 2026-10-05. Local prototype connected to guarded native events; off by default. Live gameplay validation remains pending. Not release ready.

Requested behavior:
- Off by default: existing Restoration save behavior.
- On: statue save-and-quit with one-use resume; permanent save automatically at
  Song of Time cycle reset. Ordinary day transitions do not save automatically.
- Never delete the permanent save when consuming a statue resume.

## Implemented

`src/mm3d/classic_save_store.h` stores independent permanent and suspend snapshots
for each of three slots in a checksummed, versioned, atomically replaced container.
A fresh UUID receipt identifies each suspend. Reading never consumes it. Consumption
requires the matching receipt and a successful atomic commit. Failed commits or
stale receipts cannot discard a newer suspend. A permanent checkpoint must exist
before creating a suspend. QLockFile excludes concurrent writers; QSaveFile's
in-place fallback is disabled. This protects against process interruption; no claim
of universal filesystem/power-loss durability is made.

The component accepts opaque 0x13618-byte native snapshots. Its digest detects
container corruption, not invalid native game checksums. Native validation belongs
at the integration boundary. The native coordinator validates the native MJR header and CRC16 (initial 0x1017, reflected polynomial 0xA001, checksum field 0x1a88 zeroed) before accepting a snapshot.

Test target: `mm3d_classic_save_tests`; CTest: `mm3d_classic_save_storage`.
Tests use temporary directories and synthetic snapshots, never personal save data.

## Verified code path evidence

Bundled Restoration source: `source/game/states/state_sot_save.cpp`.
The confirmation is gated by `going_back_in_time`; normal day changes immediately
switch to DayTelop instead. In the installed Restoration binary at
`Documents/mm3d-camera-analysis/restoration-hud/code-patched.bin` (ARM mapping
base 0x100000), disassembly confirms:

- 0x7d1a74: StateSotSave::Calc body.
- 0x7d1b14: reads ending_status at state+0x180; status 4 enters prompt.
- 0x7d1b6c: loads message 0x575e from literal at 0x7d1bc4.
- 0x7d1b20..28: calls native StateEnding::Calc.
- 0x7d1b2c..68: handles saved message (status 10) and DayTelop (status 12).
- Native StateEnding status 7 generates snapshot using 0x188270 and starts
  writing with 0x18da50; status 8 polls 0x18ad44.
- 0x18ad74 loads the asynchronous write result; 0x18ad7c calls an error handler
  for a negative result. The subsequent completion path can still return 1.
  **Do not treat WriteAndFree returning 1 as successful save confirmation.**
- Owl actor 0x4bfab4 creates a snapshot via 0x317000, writes via 0x18da50,
  then polls 0x18ad44. Its normal behavior continues playing after saving.
- Loader 0x346ab0 loads the selected slot, copies into CommonData, and selects
  the entrance using CommonData+0x351e (owl flag) and +0x351c (entrance).
- Native CommonData backup is used by reset logic; not private suspend storage.

These addresses are research evidence, not sufficient instruction signatures for
shipping hooks. Guard full contexts and the supported title before interception.

## Integrated prototype

- `classic_save_hooks.h` verifies six complete instruction/literal ranges before
  installing nine JIT-only SVC substitutions for USA 1.1 Restoration.
- Cycle status 4 enters native status 7 (snapshot/save) after layouts exist;
  successful native write plus checkpoint commit allows status 9 -> 12, skipping
  the save-complete popup and continuing to DayTelop.
- The writer callsite distinguishes cycle saves from both statue paths. Negative
  native write results and failed checkpoint writes never trigger quit.
- Statue completion uses native ChangeState(FileSelect) fields after commit.
- Actual load success is captured before the async job is destroyed. After native
  CommonData copy and job cleanup, a matching owl snapshot is consumed: atomically
  replace the native slot with the permanent checkpoint, then retire its receipt.
- Native slot identity must match the receipt snapshot. Replaced/copied/erased
  files cannot resurrect stale checkpoint data. Changed slots arm on their next
  successful cycle save rather than borrowing another slot's permanent checkpoint.
- The SaveDataArchive registers only exact MM3D save0/1/2 paths; other games and
  archives remain untouched. Disabled hooks replay displaced instructions.
- Player and editor Restoration tabs expose Classic saving, unchecked by default.
  Existing files arm after their next Song of Time cycle save. Disabling retains
  whichever native snapshot is currently present, returning to Restoration behavior.

Tests: storage and native-file coordinator CTests, plus
`tools/mm3d/classic_save_hooks_check.cpp` against the installed binary. Tests cover
synthetic snapshots and temporary storage. CRC implementation additionally checked
read-only against all three existing native save files. These are not gameplay tests.

## Remaining visual/release validation

Use the isolated test profile, never the primary saves, for initial gameplay checks.
Verify automatic Song-of-Time transition, first cycle, all statue types, cancel,
quit/resume twice, file-select refresh, copy/delete/new file, credits, death versus
moon crash, World Ends on Death, disabling/re-enabling, and write failure recovery.
Save states can bypass normal save/load semantics and are outside this prototype.
On consumption failure the existing save is retained where possible, and status is
reported in the Restoration tab; the prototype does not halt gameplay automatically.
Cross-file replacement is ordered to preserve permanent progress, but two commits
are not a filesystem transaction. An interrupted receipt retirement leaves an inert
stale receipt if the native slot already contains the permanent checkpoint.

The October 7 message trace identifies the owl prompt as 0x0C01. The isolated
test profile now has expanded storage for that entry, generated by
`tools/mm3d/classic_owl_text.py` from the user's original USA English archive.
All other message payloads and entries are unchanged. The emulator replaces
only the verified expanded entry while Classic saving is enabled and restores
the original wording when disabled. The original 92-byte entry is not modified
by this hook. This is test-profile setup, not yet integrated into release setup.

The three pages contain the requested text, instant first two pages, red phrases,
and native Yes/No controls. The owl-face claim anticipates the pending marker.
Gameplay pagination, wrapping and choice behavior still require verification.
The first page is now four lines. The attempted 0x16 reveal control was wrong:
it inserts remaining hours (observed as "71"); it has been removed. The native
owl entry has flags 0x00ff0001. Bit 0 passes through MessageSet+0x48 to
MessageWindow+0x3ba (reveal state+0x52), forcing full-page reveal in 0x1d85b8.
Classic text now clears only that bit in the guarded expanded entry and restores
it on disable. Explicit 0x27/0x28 commands keep the first two pages instant;
the final page uses normal reveal. This correction is built for isolated testing;
its visible timing still needs gameplay confirmation. Text isolation/toggle tests
pass, including rejection of unexpected message flags.
The user confirmed the reveal correction. Follow-up: final-page reveal runs at
80% of native speed. Choice-command instant flags are deferred until all label
characters have been consumed; the pending-choice flag is restored at the
native end-of-page decision (0x1d8568), within the same update, so the page
cannot auto-close before a later update restores choices. A regression check
covers this same-update handoff; choice input then resumes normally. On an armed
Yes, the copied native label spans retain their size and positions: No becomes
spaces, Yes blinks on a 36-frame cycle, and the arrow is hidden. Saving/success
message calls are suppressed only for this verified confirmation. Errors and
unarmed saves retain native message calls. Successful commit waits 24 actor
updates, then the presentation overlay fades the entire image over 0.8 seconds,
with the native prompt still present. Only after a black frame is presented does
the guest request the actual title state. This recreates the requested visual
transition; it does not invoke the inn's native scene-exit routine.
Updated native hook tests pass against the installed binary for reveal/choice
timing, confirmation, disabled/cancel/failure behavior, byte restoration, and
black-frame gating. macOS test app rebuilt. New confirmation visuals still need
in-game checking, particularly whether native text redraws the edited spans.
Soaring arrival and the file marker remain unfinished.

Raw earlier decompilations reside in /tmp/dekui-classic-save-*.txt and may be transient.

Follow-up: user confirms fade timing works, but confirmation hid all text.
Removed the inactive engine state (0). Confirmation now retains window state 9
and reveal state 5; the guarded update hook returns before advancing the engine,
restoring its already-pushed r4/LR and stack without touching VFP state. Only the
No/Yes copied spans and arrow visibility change. Added tests for drawable states
and balanced early-return replay. Fade timing and 80% reveal are unchanged.
Rebuilt isolated macOS test app; this visual correction awaits in-game validation.

Further follow-up: source-string edits did not affect the visible cached labels.
A scoped native draw hook now matches the last Yes/No glyph records against the
active font and zeroes only their widths (No always, Yes during its blink), then
restores all cached records after draw. Cursor visibility is changed in its
computed draw pose after animation. The existing native root-wrapper technique
adds an untextured black PaneRect before the original textbox root, so the
textbox stays above the world fade. The overlay is retained only for title
handoff. Fade starts 8 actor updates after verified save completion, previously
24. Draw diagnostics log a bitmask (layout=1, labels=2, cursor=4, fade=8).
Synthetic draw checks cover glyph isolation, cursor visibility, child order,
complete restoration and disabled behavior; gameplay validation is still needed.

October 7 correction: live draw diagnostics showed labels were never matched.
The native renderer uses min(glyph count +0x300, visible count +0x308), whereas
+0x304 is capacity. The draw hook now follows those live counts. No keeps its
position but has zero-width glyphs; Yes retains its geometry and uses an unused
native RGBA palette entry for a cosine brightness pulse (40–100%, full alpha).
Every temporary edit is restored after native draw-list generation.

The previous type-2 PaneRect was not drawable in this renderer, which only emits
type-3 text and sprites of types 4–7. The black layer now clones a type-6 sprite,
clears its optional texture, and supplies solid vertex colors. Native children
are visited in reverse order, so the wrapper puts the black sprite last and the
original textbox first. This supersedes the earlier PaneRect/blinking claims.
The 0.8-second world fade and eight-update start delay remain unchanged.
Native save-hook and updated draw-data checks pass; the isolated macOS test app
was rebuilt. In-game verification of these corrected visuals remains pending.

The user confirmed the fade/choice visuals are nearly correct, but Yes lost its
green. The pulse now multiplies the original Yes palette RGB instead of replacing
it with gray, preserving original alpha. The draw check verifies hue and alpha
preservation and complete restoration; rebuilt the isolated test executable.

Timing/audio polish: final question and choices now reveal at 90% native speed.
The world fade starts after fifteen guest actor updates (about half a second at
30 Hz) from confirmation, including time spent writing, rather than waiting an
additional eight updates after commit. Only a verified save can exit to title;
a failed commit cancels the presentation/fade. Music gain follows 1 minus the
visual fade alpha through fade-out, black, and title fade-in using the existing
guarded native music-volume hook. User volume/mute settings are unchanged and
sound effects keep their native gain. Checks cover timing, slow writes, failed
commit recovery, music attenuation/restoration and excluded effects.

Music handoff follow-up: the outgoing gameplay sound instances are remembered
through fade-out/black and held at zero during title fade-in, even if StreamMgr
has detached their handles but native tail updates continue. New track IDs or
new sound instances retain the normal title fade-in envelope. Idle/cancellation
clears the temporary hold. Successful title handoff no longer briefly sets the
shared fade state to Idle while clearing the retained prompt, avoiding a render
thread race that could restore full music volume. Music checks cover outgoing
and detached tails, new title instances, native object reuse, and cancellation;
native save-hook checks pass. In-game listening still needs verification.

Classic file marker: a read-only, throttled native/sidecar identity check exposes
pending owl saves per slot. Consumption and successful writes invalidate the
cache; ordinary, replaced, corrupt, or disabled-classic saves get no marker.
The file list appends a native flag after each marked name's actual glyph bounds,
inheriting its computed transform and opacity. The selected-file summary uses
the selected slot observed in native file-entry animations. The sprite is the
Hud icon_sakasa_uta_pane atlas cell: main resource15, 368,240,16,16 on a 512x256
atlas. This first version pulses the cyan flag's brightness; it does not layer
the separate glow cell. Guest-memory draw checks and native-file coordinator
checks pass. Built/copied the isolated macOS app; placement and appearance still
need in-game verification. No save file contents are changed by marker queries.

Marker appearance follow-up: removed the custom cyan tint. All RGB channels now
use the same brightness multiplier, preserving the atlas's original white flag
and outline. Raised the sprite three native layout units to compensate for its
transparent top padding and align the visible flag with the name. The existing
pulse and save-state gating remain unchanged. Updated draw checks cover neutral
color and vertical offset; in-game alignment still needs checking.

Marker glow: now uses both original atlas cells, with a steady untinted normal
flag and a smoothly pulsing glow overlay instead of dimming the normal flag.
The native glow cell is (344,240,18,16), offset one unit left to align with the
16-wide normal cell. Both inherit name opacity and transforms; only glow alpha
pulses. Tests verify both UVs, alignment, steady base visibility, glow extrema,
and reversible guest-memory writes. Isolated macOS test build updated; visual
confirmation of the layered effect remains pending.

Separate Restoration option: "Activate owl statues with a sword" (default off).
For an inactive soaring owl, the normal A offer/acceptance is suppressed and an
actual native AC collision from Sword/GreatFairySword routes its idle action to
the existing activation sequence. Native animation/sound/cutscene and soaring
save-bit code run unchanged. Activated owls retain A interaction. Feather save
statues retain it only when Classic Saving is off; intro action states are
preserved. Sword activation is independent of Classic Saving. Guards match the installed binary; guest-memory checks cover weapon
classification, collision requirement, invalid actors, and exemptions. Settings
default/persistence checks pass. The isolated macOS app was rebuilt; actual
sword activation still needs in-game verification with an inactive statue.

Separate Restoration option: "Bombers don't approach automatically" (default
off). Only actor0x20f's ordinary wandering/rumor routine at0x56dd04 is affected.
The transition into approach state2 is redirected to native stop/idle cleanup;
active approach/wait states2/3 likewise select cleanup state4 on the next update.
Manual talk acceptance, rumor selection, and other action functions are retained.
The approach and state-load instruction signatures match the installed binary;
guest-memory checks cover action/actor isolation and disabled behavior. Settings
persistence checks pass. Rebuilt isolated macOS app; gameplay validation of
manual conversations, rumors, and hide-and-seek is still pending.

Classic Saving now suppresses feather checkpoint interaction (actor 0x1b2 with
parameter bit 0x200), using the existing guarded statue update and talk-offer
hooks. It clears pending talk acceptance and rejects new interaction offers.
Owl statues, other actors, save flags and action states are unchanged. Turning
Classic Saving off restores native feather interactions. Guest-memory checks
cover both classic/sword settings, both statue types and malformed actors.
Visual verification at a feather and an owl remains pending.
