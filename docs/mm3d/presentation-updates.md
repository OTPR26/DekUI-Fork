# UI and presentation updates

This branch contains Select Mask placement, prompts and shadows; notebook layout
and scrollbar corrections; fullscreen fade rules with local-pane protection;
the cutscene FOV control; the standalone cheats window; and file-panel layering.

The moon's `MenuMaskPresen` has independent Select Mask controls for its panel,
title, description and Choose/Cancel prompts, with the stamped six-frame slide
in both directions. Its 24
icons use owner+0x50, and its animation player is at owner+0x30 (different from
Give Item). The six-column grid and both selection cursors move together; the
item-only right strip is not applied. Only the mask panel, title and controls
are routed to the top screen; the bottom backdrop is omitted. Legacy settings
seed the mask panel/title/prompts from Give Item and preserve the description's
previous dialogue pose. New `mask_present_*` keys save subsequent edits separately.
An exterior shadow surrounds the union of the three mask-panel texture pieces
without darkening internal seams. Select Mask samples its quadratic opacity
falloff in 24 thin exterior rings instead of three broad bands and coarse corner
cells. The rings contain 96 disjoint rectangles within the existing scratch
allocation; each corner belongs to only one rectangle. Tests check opacity,
edge joins, scaling, routing and restoration of guest memory. Live visual
verification of the smoother falloff is still needed.
Select Mask uses a broader, darker halo weighted toward the bottom for visual
separation from the world at smaller editor scales. The title's delayed visibility and scale/fade tracks
are overridden during calculation/drawing and restored afterward. Guest-memory
tests cover routing, all 24 icons, cursors, prompts, both transitions and malformed
layouts; in-game placement still needs visual verification.

### Notebook portrait flash alignment

Capture `song-effect-1792193513311` contains 18 solid portrait-highlight draws
whose clip scale was 1 while their textured portraits used 15/16. A saved
fullscreen draw-state rule also matched these small layout quads. The renderer
now keeps localized, axis-aligned solid layout panes in the ordinary widescreen
coordinate space, preserving the fullscreen rules for full-screen covers and
full-width banners. Both the Schedule row flash and detail flash are covered;
native timing, opacity, and portrait positions are unchanged.

The shared Events/Schedule scrollbar also now ends its thumb five units inside
the lower track cap, matching its upper inset. Previously it stopped at the
lower cap's origin, leaving an extra three-unit gap even at full coverage.
Tests cover full and partial coverage, scrolling endpoints, native animation
sizes, scale, and restoration of guest memory after drawing.

Follow-up capture `song-effect-1792194355932` includes both pages. Their track
bounds agree (Y=40.2..232.2), but Events stretches its shorter 296-unit track
to Schedule's 320 units: its thumb ends at 228.9567 rather than 229.2, and its
caps are 1.2973 units tall rather than 1.2. Events now uses the same native
356-unit track bottom as Schedule before normalization. A complete Events
Begin/End regression verifies matching final bounds and cap sizes under three
list scales, and exact guest-memory restoration; the previous code fails it.

Capture `song-effect-1792197084902` exposes a separate thumb seam: the layout
shader floors each piece's position. The strip can end at 227.99998 while the
lower cap starts at 228, leaving a whole native pixel uncovered. Of 191 complete
scrollbar samples, 58 have a gap after flooring. Extending the strip half a local
unit into each cap removes those gaps in the captured geometry without changing
the outside bounds. Both notebook pages share the correction. Tests include
fractional positions near the bottom of a 61-entry list and floored join coverage;
final appearance still requires an in-game check.



## Release defaults and settings

The release defaults include all eight saved fullscreen draw rules (three new
rules) and the twenty Select Mask placement/scale/prompt values. Existing
installations receive the new rules only if their list exactly matches the
previously shipped rules; customized lists remain untouched. Missing Select Mask
keys receive the release layout while existing values are preserved. The smaller
magic bar height was already shipped and remains unchanged. Current editor
choices for difficulty, draw distance, gameplay FOV, hidden HUD elements and
indicators are not new release defaults.

The cutscene FOV explanation and original/default values appear in the slider's
hover tooltip, matching the gameplay camera control. The slider remains 45–70
with a default of 49 degrees.

## Branch scope

- `interpolation-fixes`: actor, particle, water, HUD/menu animation, timing,
  file-select entrance/easing and capture/replay support with regression tests.
- `difficulty-options`: Fast time, six-hour death penalty, transition sounds,
  saved settings and native-code checks.
- `ui-presentation-fixes`: Select Mask relocation and controls, prompt appearance,
  shadows, notebook layout, scrollbars/portrait flashes, fullscreen fade defaults,
  file-panel layering, cutscene FOV, cheats-only window, and layout migration.

The source split preserves the accumulated fixes across these branches. Shared
files can require merge resolution when combining them; the branches are each
based on the published main revision, and main is not changed by publishing them.
