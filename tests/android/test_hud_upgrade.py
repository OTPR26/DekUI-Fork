# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the PC HUD texture upgrade using locally owned before/after archives."""
import base64,hashlib,json,struct,sys
from pathlib import Path
spec=json.loads(Path(sys.argv[1]).read_text())
old=Path(sys.argv[2]).read_bytes();expected=Path(sys.argv[3]).read_bytes()
def apply(data):
    assert len(data)==spec['archive_size']
    out=bytearray(data)
    for op in spec['ops']:
        at=op['at'];before=base64.b64decode(op['before']);after=base64.b64decode(op['after'])
        accepted=[before,after]
        if 'alternate' in op:accepted.append(base64.b64decode(op['alternate']))
        assert data[at:at+len(before)] in accepted
        out[at:at+len(after)]=after
    return bytes(out)
assert hashlib.sha256(old).hexdigest()==spec['old_sha256']
assert apply(old)==expected
assert hashlib.sha256(expected).hexdigest()==spec['new_sha256']
assert apply(expected)==expected
if len(sys.argv)>4:assert apply(Path(sys.argv[4]).read_bytes())==expected
# A player-authored position elsewhere in the archive survives the texture-only upgrade.
custom=bytearray(old);struct.pack_into('<f',custom,0x1294+32+8,23.5)
changed=apply(custom)
assert changed[0x1294+32+8:0x1294+32+12]==custom[0x1294+32+8:0x1294+32+12]
assert all(changed[i]==expected[i] for i in range(len(old)) if not 0x1294+32+8<=i<0x1294+32+12)
# Conflicting texture edits are rejected before a caller writes any output.
bad=bytearray(old);at=spec['ops'][0]['at'];bad[at]=(bad[at]+1)%256
try: apply(bad)
except AssertionError: pass
else: raise AssertionError('Conflicting texture data was accepted')
for start in range(len(old)-32):
    if old[start:start+4]!=b'MFL ':continue
    count=struct.unpack_from('<H',old,start+10)[0];end=start+32+count*0x44
    assert old[start:end]==expected[start:end],hex(start)
texture=Path(sys.argv[1]).parent/spec['texture'];png=texture.read_bytes()
assert struct.unpack('>II',png[16:24])==(4096,2048)
assert hashlib.sha256(png).hexdigest()==spec['texture_sha256']
print('HUD upgrade: filter-safe PC atlas, 8x PNG, placement preservation, idempotency and conflict rejection passed')
