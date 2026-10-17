Android setup

Startup Settings > Mods > Mod setup accepts the player's own decrypted OoT3D
.3ds/.cci ROM and optional Azahar texture-pack folder. Generate Adult Link mod
creates all six original-color/sword variants on-device, using the MM3D ROM
already selected by the player. No Link model, OoT3D ROM, or texture pack is
bundled. Source files are read-only and full ROMs are not copied.

Blue (Zora) and Red (Goron) body textures are copied verbatim from OoT3D's
link_body.cmab. Matching HD colors are read from the selected texture pack.
Other colors are cloth-only transformations of the green textures.
Unmatched textures use original ROM detail. The reference 4K pack contains
a matching HD Master Sword sheath; the imported blade texture has no exact
replacement in the reference pack and keeps its native texture.

Generation validates all variants before replacing the current model folder,
keeps a backup, and imports only matching HD textures. Imported textures use
both current and legacy hash filenames. PNG/DDS/KTX replacements are supported;
the custom purple/black/white/gold transform currently recolors RGBA8/PNG only.
The original red/blue variants do not rely on that transform.

The Kotlin implementation lives in DekuiModelConverter.kt and
DekuiAdultLinkSetup.kt; it has no Python dependency on Android. The JVM check
in tests/android/adult_link consumes local user-owned assets supplied as args.

Desktop conversion tools

Adult Link appearance mod

Requires Python 3 and NumPy. Contains no Nintendo game models or textures.
Use CMB v6 link_v2.cmb from your own OoT3D Adult Link archive and the
MM3D USA zelda2_link_child_new.gar.lzs archive. Inputs are never modified.

python3 convert.py --adult-model /path/to/link_v2.cmb \
  --human-archive /path/to/zelda2_link_child_new.gar.lzs \
  --kokiri-sword --output-dir /path/to/generated-kokiri

Repeat with --master-sword for the Master Sword variant. Install each generated
zelda2_link_child_new.gar.lzs in the app-private directory:
dekui/load/mm3d-models/adult-link/variants/kokiri/romfs/actors/
dekui/load/mm3d-models/adult-link/variants/master/romfs/actors/
The legacy adult-link/romfs/actors/ model remains a fallback for older installs.

Settings > Mods > Adult Link exposes tunic color and Master Sword options.
All changes apply on the next game launch. Disabling restores normal MM3D Link.
Tunic recoloring handles native and matching RGBA8/PNG HD body/hat textures,
preserving non-green skin, hair, leather, metal, and alpha. Compressed DDS/KTX
replacements are not recolored. The sword toggle changes appearance only,
including sheathed/drawn states across all three native sword upgrade tiers.
Sword damage, collision, saves, movement, and transformation forms remain native.

The MM3D skeleton remains unchanged. Body draw scale matches native Zora height;
existing equipment is counter-scaled to retain its original world size.
Facial expressions and mask fitting remain unfinished. Hand poses use the
closest adult grip. Non-model archive entries are preserved byte for byte.

Format references: xdanieldzd/N3DSCmbViewer and magcius/noclip.website
(src/OcarinaOfTime3D/cmb.ts), both MIT-licensed format references.

Regular sword compatibility: MM3D drawn sword groups are 12 (Kokiri),
14 (Razor), and 16 (Gilded); group 18 is the Great Fairy sword and remains
unchanged. The on-device generator also extracts the Master Sword item
icon from the supplied ROM, uses its matching HD atlas when available,
and swaps only the three regular sword cells while Master Sword is active.
The shared atlas retains all other items and HUD positioning.

The Android generator reads QueenSound.bcsar from the supplied OoT3D ROM
and imports the original adult CWAV clips into a generated MM3D sound
archive. Only human Link voice-key waveform bindings change; original
sound IDs, sequences, music, NPC keys, other voice keys, and all original
wave samples remain intact. Shared embedded group files retain their
original nesting. Adult-only idle/waking/damage aliases use the matching
adult sounds. The source files are read-only; no voice assets are bundled
in the APK or source package. Disable Adult Link and relaunch to restore
the original sound archive. Older generated models need regeneration once.

Torch compatibility
-------------------
Adult Link's taller hand joint can miss MM3D's child-height torch sphere.
The supported USA Rev1 torch contact calculation extends its vertical reach
by the adult height ratio while retaining the native horizontal radius, lower
boundary, ignition requirements and timer behavior. This applies only to the
active adult human model holding a Deku stick. It changes no model/equipment
size, general collision, save data, or other forms. No mod regeneration needed.
