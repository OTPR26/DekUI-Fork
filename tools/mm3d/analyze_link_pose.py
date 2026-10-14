"""Correlate read-only human Link skeleton samples with render palettes.

Reports candidates, not proof of gameplay ownership or active shader usage.
Identical bones/instances are ambiguous and must never be picked arbitrarily.
"""
import argparse
from collections import Counter, defaultdict
import json
import math
import struct
from pathlib import Path


def near(a, b):
    return len(a) == len(b) == 12 and all(
        math.isfinite(x) and math.isfinite(y)
        and abs(x-y) <= max(0.0002, max(abs(x), abs(y))*0.00003)
        for x, y in zip(a, b))


def multiply(a, b):
    return [sum(a[r*4+k]*b[k*4+c] for k in range(3)) + (a[r*4+3] if c == 3 else 0)
            for r in range(3) for c in range(4)]


def inverse(m):
    a,b,c,tx,d,e,f,ty,g,h,i,tz = m
    det = a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g)
    if not math.isfinite(det) or abs(det) < 1e-15:
        raise ValueError('Singular bind transform')
    r = [(e*i-f*h)/det,(c*h-b*i)/det,(b*f-c*e)/det,0,
         (f*g-d*i)/det,(a*i-c*g)/det,(c*d-a*f)/det,0,
         (d*h-e*g)/det,(b*g-a*h)/det,(a*e-b*d)/det,0]
    for j in (0,4,8):
        r[j+3] = -sum(r[j+k]*v for k,v in enumerate((tx,ty,tz)))
    return r


def bind_corrections(path):
    # Read the user's local uncompressed GAR/CMB, never distribute model data.
    data = path.read_bytes()
    marker = b'link_child' + bytes(6)
    name = data.find(marker)
    base = name-16
    if name < 16 or data[base:base+4] != b'cmb ':
        raise ValueError('Expected the human Link CMB model')
    at = base + struct.unpack_from('<I', data, base+0x24)[0]
    if data[at:at+4] != b'skl ' or struct.unpack_from('<II', data, at+4) != (1160,26):
        raise ValueError('Unexpected human Link skeleton')
    world = []
    for bone in range(26):
        flags,sx,sy,sz,rx,ry,rz,tx,ty,tz,_ = struct.unpack_from('<I9fI',data,at+16+44*bone)
        if flags & 0xfff != bone:
            raise ValueError('Unexpected bone index')
        cx,cy,cz = map(math.cos,(rx,ry,rz))
        xx,yy,zz = map(math.sin,(rx,ry,rz))
        local = [cy*cz*sx,(xx*yy*cz-cx*zz)*sy,(cx*yy*cz+xx*zz)*sz,tx,
                 cy*zz*sx,(xx*yy*zz+cx*cz)*sy,(cx*yy*zz-xx*cz)*sz,ty,
                 -yy*sx,xx*cy*sy,cx*cy*sz,tz]
        parent = flags >> 16
        if parent != 65535 and parent >= bone:
            raise ValueError('Invalid bone parent')
        world.append(local if parent == 65535 else multiply(world[parent],local))
    return [inverse(m) for m in world]


def analyze(data, pose_frame_offset=0, corrections=None):
    poses = defaultdict(dict)
    for p in data['poses']:
        # Repeated submissions of an instance may use different cameras; retain
        # all distinct matrices rather than letting the last submission win.
        for bone, matrix in enumerate(p['bones']):
            variants = poses[p['frame']+pose_frame_offset].setdefault((p['instance'], bone), [])
            variants.append(matrix)
            if corrections is not None:
                variants.append(multiply(matrix, corrections[bone]))
    instances = Counter(p['instance'] for p in data['poses'])
    matches, ambiguous, matched_draws = [], 0, 0
    for d in data['draws']:
        found = False
        for slot, matrix in enumerate(d['palette']):
            candidates = [key for key, variants in poses[d['frame']].items()
                          if any(near(matrix, variant) for variant in variants)]
            if len(candidates) > 1:
                ambiguous += 1
            elif len(candidates) == 1:
                instance, bone = candidates[0]
                matches.append({'frame': d['frame'], 'instance': instance,
                                'bone': bone, 'uniform': 20+slot*3,
                                'vertex_base': d['vertex_base'],
                                'index_offset': d['index_offset'],
                                'vertices': d['vertices']})
                found = True
        matched_draws += found
    return {'poses': len(data['poses']), 'instances': dict(instances),
            'draws': len(data['draws']), 'matched_draws': matched_draws,
            'ambiguous_slots': ambiguous, 'matches': matches,
            'truncated': data.get('truncated', False),
            'pose_frame_offset': pose_frame_offset, 'bind_corrected': corrections is not None,
            'note': 'Same-frame numerical candidates only; unused palette slots and menu clones require further verification.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--pose-frame-offset', type=int, default=0)
    parser.add_argument('--model', type=Path, help='Local uncompressed human Link GAR/CMB')
    args = parser.parse_args()
    report = analyze(json.loads(args.capture.read_text()), args.pose_frame_offset,
                     bind_corrections(args.model) if args.model else None)
    text = json.dumps(report, indent=2)
    if args.output:
        args.output.write_text(text+'\n')
        print(json.dumps({k: v for k, v in report.items() if k != 'matches'}, indent=2))
    else:
        print(text)
