#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Audit the user's English GMSG without changing or packaging game assets."""
import argparse
from collections import Counter
import json
import re
from pathlib import Path
from single_screen_text import SPEC, compile_messages, decode, messages

CONTROLLER_ICONS = {1,2,3,4,5,6,8,10,12,17,18,19,20,21,23,24,27,28,30,31,32,45,46,53}
EXACT_BUTTONS = re.compile(r'\b(?:(?i:buttons?)|START|SELECT|ZR|ZL|L-Targeting)\b')

def check(data, spec):
    rows = compile_messages(data, spec)
    patched = {row[0]: row[4] for row in rows}
    original = messages(data)
    reviewed = {int(edit['id'],16) for edit in spec['neutral_runs']}
    assert reviewed <= patched.keys()
    for mid, size, _, new, tokens, raw in rows:
        assert len(new) <= size
        assert decode(new) == tokens
        # Unknown metadata, page transitions, sound, timing, choices and
        # dynamic numeric/plural operands remain byte-for-byte equivalent.
        flow = lambda ts: [t for t in ts if isinstance(t,tuple) and t[0] not in (1,0x25,0x3a)]
        assert flow(tokens) == flow(decode(raw)), hex(mid)
        lines = 1
        for t in tokens:
            if t == (1,): lines += 1
            if isinstance(t,tuple) and t[0] in (0,2):
                assert lines <= 4, (hex(mid), 'page overflow')
                lines = 1
    # Check the whole archive, including messages not in the replacement list.
    # Unsupported unrelated controls remain untouched and are skipped only
    # after checking their raw text and icon words for instruction candidates.
    for mid, raw in original.items():
        try:
            tokens = patched.get(mid) or decode(raw)
        except (ValueError,KeyError,UnicodeError):
            has_icon = any(b'\x25\x00'+i.to_bytes(2,'little') in raw for i in CONTROLLER_ICONS)
            assert not has_icon and not re.search(rb'\b(?:button|START|SELECT|L-Targeting)\b',raw), hex(mid)
            continue
        text = ''.join(t for t in tokens if isinstance(t,str))
        assert not EXACT_BUTTONS.search(text), (hex(mid), text)
        assert not any(isinstance(t,tuple) and t[0]==0x25 and t[1] in CONTROLLER_ICONS for t in tokens), hex(mid)
    # Unrelated map-location icons and the credits logo must not be removed.
    for mid, _, _, _, tokens, raw in rows:
        retained = lambda ts: Counter(t[1] for t in ts if isinstance(t,tuple) and t[0]==0x25 and t[1] not in CONTROLLER_ICONS)
        assert retained(tokens) == retained(decode(raw)), (hex(mid), 'non-controller icon changed')
    # Pitches replace button glyphs; the two musical puzzle clues retain all notes.
    assert 're fa la re' in ''.join(t for t in patched[0x51a2] if isinstance(t,str))
    assert 'ti ti la fa fa la ti fa' in ''.join(t for t in patched[0x51a3] if isinstance(t,str))
    # A changed review anchor must fail closed instead of editing the wrong run.
    broken = dict(spec, neutral_runs=[dict(spec['neutral_runs'][0], original='changed source')])
    try:
        compile_messages(data, broken)
    except ValueError as error:
        assert 'reviewed source changed' in str(error)
    else:
        raise AssertionError('changed source was accepted')
    print(f'{len(reviewed)} controller-neutral messages; {len(rows)} total replacements; flow, capacity, page layout, full-archive coverage and review guards passed')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('gmsg', type=Path)
    args = parser.parse_args()
    check(args.gmsg.read_bytes(), json.loads(SPEC.read_text()))
