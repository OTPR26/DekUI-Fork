#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$root"
if ! command -v python3 >/dev/null; then
    echo 'Python 3.9+ is required for this test setup.'
    exit 1
fi
python3 tools/mm3d/setup_game_folder.py --root "$root" --restoration "$root/restoration"
echo 'Setup complete. Run Launch.sh. Your other emulator installations were not changed.'
