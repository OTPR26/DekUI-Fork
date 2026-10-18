# Shared application experience

Android and desktop share the single-screen presentation, HUD defaults, model
converter, gameplay patches, and saved presentation settings. Desktop uses a Qt
startup/settings frontend and the shared full-screen in-game menu. Android uses
a native startup screen and the same in-game menu, with touch-specific options.

## Setup and mods

Select the supported MM3D ROM from startup. Custom texture directories are
selected in Graphics and may remain outside the application data directory.
Mods accepts an OoT3D ROM and optional texture directory. The bundled converter
generates Adult Link tunics, the optional Master Sword, Dark Link, and adult
voices before gameplay. Imported game assets are never shipped in packages.
Generated texture overrides are separate from the original texture pack.

## Settings and controls

Startup and gameplay settings share saved values. Explicit controller mappings
update both the saved and loaded profile. Default bumpers use item slots I/II;
triggers target and shield. The HUD uses the shared high-resolution atlas and
hash-guarded archive recipe. Recognized older defaults migrate with backups;
custom HUD archives and positions remain intact.

Desktop settings open through the visible Settings action, Command-comma on
macOS, Ctrl-comma elsewhere, F1, or Start+Select. Reset warns about unsaved progress
and returns to startup. Quit on startup exits the application.

## Storage and packaging

- Android: Internal storage/mm3dport. Existing data migrates through a staged,
  hash-verified copy; original private data remains available for recovery.
- macOS: ~/Library/Application Support/DekUI Fork. Existing portable data is
  copied and verified before switching roots.
- Windows and Linux: the writable user directory beside the application.

Packages include setup tools and their runtime dependencies. Release checks must
cover fresh setup, upgrades, ROM/texture import, model generation, controller
mapping, menu navigation, save loading, camera recovery, aiming, and mod changes.
Automated fixtures supplement gameplay checks; platform validation is recorded
for each release candidate.
