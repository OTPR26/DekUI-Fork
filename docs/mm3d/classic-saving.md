# Classic saving

Classic saving is an optional **F1 > Restoration** setting, disabled by default.

- A Song of Time cycle reset establishes the permanent checkpoint.
- Owl statues provide a temporary, one-use save while preserving that checkpoint.
- Saving returns to the title screen only when a valid prior Song of Time cycle
  checkpoint exists.
- Saving at feather statues is disabled; owl statues remain usable.
- Ordinary day transitions do not establish a cycle checkpoint.
- Disabling the option restores Restoration's saving behavior.

Existing or replaced saves must pass identity validation before using stored
checkpoints or suspend receipts.

## Implementation map

Paths below are relative to the repository root.

| Area | Files |
| --- | --- |
| Atomic, versioned, checksummed three-slot storage | `src/mm3d/classic_save_store.h` |
| Native validation, coordination and resume | `src/mm3d/classic_saving.cpp` |
| Guarded game hooks | `src/mm3d/classic_save_hooks.h` |
| Shared presentation coordination | `src/mm3d/classic_saving.h` |
| Controls and defaults | `src/mm3d/overlay.cpp`, `src/mm3d/settings.cpp`, `src/mm3d/settings.h` |

## Save integrity constraints

Each suspend has a unique receipt. Reading or previewing a save does not consume
it: consumption requires a successful actual load with the matching receipt.
Resume must atomically restore the permanent native slot before retiring the
receipt. A stale receipt must not consume a newer suspend.

Failed writes must not quit the game or discard the permanent checkpoint. Native
write completion alone is insufficient: validate the operation result, native
header and CRC before accepting the snapshot. Atomic replacement protects against
partial writes; it is not a guarantee against every filesystem or power failure.

Owl sword activation is separate from saving. Preserve the distinction between
owl and feather statues when changing interaction hooks.

## Validation

Run the focused tests from the repository root:

```sh
ctest --test-dir build-macos --output-on-failure -R 'mm3d_(classic_save_storage|classic_save_coordinator|native_inventory)'
```

Use disposable saves for live validation of cycle reset, owl save, resume, return
to title and feather-statue interaction. Preserve users' actual saves. Automated
storage and coordinator tests do not replace live checks of game hooks.
