# iOS data folder

Data offers **Choose iCloud Drive / Files folder**, **Sync data folder now** and
**Use app data folder**. Files grants access to a folder chosen by the user;
a security-scoped bookmark remembers that choice. The app uses a dedicated
`Majora ReMasked` subfolder and coordinates provider access off the UI thread.

Gameplay always reads the existing local application-support directory. Sync
runs at the startup screen and after returning from gameplay, never while the
guest is running. Unavailable folders leave local working files available.
Users can disconnect the folder without deleting either copy.

Native save slots, portable presentation/language/control settings, imported
models, user textures under `textures/load`, and user game files under
`GameContent` participate. Device graphics settings, touch-layout preferences,
shader caches, logs, launch profiles, credentials and sync metadata remain local.
Bundled private test resources are not automatically copied to the folder.

A local hash baseline detects independent changes. If both copies changed,
users choose the local or Drive version explicitly. A transaction stages and
hash-checks its input before replacing files, retains rollback copies until
completion, rejects symbolic links and invalid save sizes, and never propagates
deletions. Sync is file based; there is no claim of live multiplayer access or
safe simultaneous play on two devices. Close gameplay and complete sync before
switching devices. Existing CloudKit save sync remains a separate service.

The public app must contain none of the user's game data, texture packs, models,
saves, security-scoped bookmarks or personal provisioning. User-imported data in
their chosen folder is not part of the distributed app. CI uses synthetic files.
