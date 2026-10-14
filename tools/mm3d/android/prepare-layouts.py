#!/usr/bin/env python3
"""Prepare local Android test assets from the verified ROM. Never copies the ROM."""
import argparse
from pathlib import Path
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from setup_game_folder import validate, extract
from build_release_assets import build, RESOURCES

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('rom', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
validate(args.rom)
args.output.mkdir(parents=True, exist_ok=True)
stage = Path(tempfile.mkdtemp(prefix='android-layouts-', dir=args.output))
original = stage / 'original'
extract(args.rom, original)
count = build(original, RESOURCES / 'project-restoration', stage / 'mods/0004000000125500')
print(f'Verified {count} mod files in {stage}', file=sys.stderr)
print(stage)
