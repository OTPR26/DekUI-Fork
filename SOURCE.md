# Source and builds

This repository contains Dekui's application source, tests, setup and packaging
scripts, and platform build workflows. The initial source import is a clean
snapshot; private development history and working notes are not included.
Upstream copyright notices and dependency licenses remain intact.

## Get the source

```sh
git clone --recurse-submodules https://github.com/tgacquin/mm3d-dekui.git
cd mm3d-dekui
```

For an existing checkout, run `git submodule update --init --recursive` after
pulling changes. Dependencies are pinned to the revisions recorded by Git.
GitHub's automatically generated source ZIP and tarball do not include submodule
contents. Use a recursive clone or a complete source archive attached to a release.

## Build reference

The workflows specify the toolchains, dependencies and build commands:

- Windows: `.github/workflows/mm3d-windows-test.yml` and `tools/mm3d/windows/build-ci.sh`.
- Steam Deck / Linux: `.github/workflows/mm3d-deck-test.yml` and `tools/mm3d/deck/build-ci.sh`.
- macOS: `.github/workflows/mm3d-macos-test.yml` and `tools/mm3d/macos/build-ci.sh`.

Windows uses MSYS2 UCRT64, Deck uses the Linux container specified by its workflow,
and macOS targets Apple Silicon. The scripts configure Dekui, build the application,
run tests and prepare portable packages. The embedded crypto-key fallback has been
removed; no built-in key blob is included.

Current implementation references are in `tools/mm3d/INTERPOLATION.md` and
`docs/mm3d/classic-saving.md`. Release checks are in `tools/mm3d/RELEASE-CHECKS.txt`;
opt-in performance capture instructions are in `tools/mm3d/deck/DIAGNOSTICS.md`.

## Release source and notices

Each binary release must include complete corresponding source for the revision
used to build it, SHA-256 checksums, and required dependency notices. From a clean
Git checkout with all pinned submodules initialized, create the source archive with:

```sh
python3 tools/mm3d/package_source.py artifacts/Dekui-Source.tar.gz
```

These archives include pinned submodule sources and can be inspected without Git.
Bundled Project Restoration has its matching source archive, provenance and notices
under `resources/mm3d/project-restoration/`. Source archives preserve licenses and
exclude private working notes. Downloaded build dependencies are identified in the
platform workflows and scripts.
