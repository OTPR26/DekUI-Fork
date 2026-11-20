# Native mobile release preparation

Production 0.4.4 is unchanged. The preparation branch is not a qualified release.
The native source is based on TriAevum
`672d13f15cb336bc7651c861d23cf45924a9f936`; upstream main was identical when
reviewed before this fork. Unmerged upstream FPS/macOS/threading proposals are
not implicitly part of this release.

## Public package requirements

All release builds and compilation checks run through GitHub Actions. Current
checks cover the actual iOS launcher/Data-menu translation unit, native shared
settings/input core, folder-sync transactions, compiler path mapping and public
package privacy-policy tests. These checks do not produce a release APK or IPA.

Private device-test packaging helpers are not public release builders. In
particular, the following must never enter a public package or CI upload:

- Original ROM, code image, ExHeader, RomFS, extracted game assets or game saves.
- Henriko textures, prepared owner models, initial save slots or owner content.
- Personal provisioning profiles, device UDIDs, signing keys or certificates.
- Private title-input bundles, local-only generated title archives, logs or
  user folder bookmarks and preferences.

A source-only runtime tree is distinct from an approved translated-title source
package. Existing local-only generated title outputs remain private. Before a
public full-game build, a reviewed, hash-bound translated-logic source package
must exist with matching runtime ABI, hook boundaries and reproducible source.
It must include no original data image or assets and cannot be substituted by a
private device library. Release tooling must reject `local_only` title inputs.

## Remaining release gates

1. Complete clean first-run game preparation/import, including iOS startup
   settings without any bundled private game. Users supply their own supported
   game input. Generate the Restoration data locally rather than bundling a
   user's extracted game or mod payloads.
2. Add clean full-game APK/IPA workflows using reviewed, pinned build inputs.
   Compile on Actions, not on a developer machine outside Actions. Run the
   package audit on the exact final bytes; keep private fixtures out of CI.
3. Android retains `io.github.otpr26.dekuifork` and the original release signer,
   required ARM64/16 KiB alignment and packaged PAK converter dependencies.
   Validate both a clean install and an in-place update on Odin and Thor.
4. The public iOS package cannot contain the private test profile or signing
   identity. Distribution/sideload signing must be explicit; owner CloudKit
   entitlement availability must not be promised to unrelated signers. The
   user-selected Files/iCloud Drive folder is independent of that container.
5. Verify the real iCloud Drive picker, persistent bookmark, offline relaunch,
   conflicting saves and post-game sync on a signed iPhone build. Retain local
   gameplay/caches and existing portrait/Duo, motion, HUD and model behavior.
6. Complete final device regression and audit exact release assets and source.
   Record asset digests and source provenance before creating a release draft.

## Desktop attachments

Retain the already-published 0.4.4 desktop ZIP/tar filenames and bytes. Label
these as legacy desktop builds, with new native Mac, Windows and Linux ports
coming soon. Do not imply that they share the native mobile engine.

## Release notice draft

“Native Android and iOS are arriving in Majora ReMasked 0.5.0. Native Mac,
Windows and Linux versions are coming soon. The attached desktop packages are
previous 0.4.4 builds. Supply your own game files; no ROM, game assets, texture
packs, models, saves or personal data are included in the mobile downloads.”

This notice is a preparation draft, not a publication receipt or a claim that
all release gates have passed.
