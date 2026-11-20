#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Embed the licensed ReTuned interface faces for startup-safe font loading."""
from pathlib import Path
import argparse
parser=argparse.ArgumentParser()
parser.add_argument("--fonts",type=Path,required=True)
parser.add_argument("--output",type=Path,required=True)
args=parser.parse_args()
parts=["// Generated from the included SIL Open Font License faces.\n#pragma once\nnamespace ReMaskedSettings::FontData {\n"]
for symbol,name in [("Body","FiraSans-Regular.ttf"),("Bold","FiraSans-Bold.ttf"),("Heading","FiraSansCondensed-Regular.ttf")]:
    parts.append("inline unsigned char "+symbol+"[]={"+",".join(str(b) for b in (args.fonts/name).read_bytes())+"};\n")
parts.append("}\n")
text="".join(parts)
args.output.parent.mkdir(parents=True,exist_ok=True)
if not args.output.exists() or args.output.read_text()!=text:args.output.write_text(text)
