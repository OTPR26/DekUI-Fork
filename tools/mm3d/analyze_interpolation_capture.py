"""Audit consecutive draw captures before attempting renderer interpolation.

This reports candidates, NOT verified objects or camera matrices. Buffer addresses
can be reused, shared meshes can belong to different actors, and uniform values
can represent material parameters or animation rather than transforms.
"""
import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path


def signature(draw, source):
    key = tuple(draw[k] for k in ('shader', 'swizzle', 'vertices', 'width',
                                  'height', 'depth_state'))
    if source:
        data = draw.get('source', {})
        if not data.get('valid'):
            return None
        key += tuple(data[k] for k in ('vertex_base', 'first_vertex', 'last_vertex',
                                       'index_offset', 'topology', 'indexed', 'index16'))
    return key


def finite_vector(value):
    return isinstance(value, list) and len(value) == 4 and all(
        type(v) in (float, int) and math.isfinite(v) for v in value)


def direction_angle(a, b):
    cross = (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
    return math.degrees(math.atan2(math.sqrt(sum(v*v for v in cross)),
                                   sum(x*y for x, y in zip(a, b))))


def camera_timeline(capture):
    """CPU directions provide an independent check; never infer motion from endpoints alone."""
    frames = capture.get('camera_frames', [])
    previous = None
    angular_path = 0.
    position_path = 0.
    steps = 0
    position_errors = []
    direction_errors = []
    valid = 0
    for frame in frames:
        cpu = frame['cpu']
        eye, target = cpu['eye'], cpu['target']
        usable = cpu['valid'] and cpu['fresh'] and all(
            isinstance(v, list) and len(v) == 3 and all(
                type(x) in (int, float) and math.isfinite(x) for x in v)
            for v in (eye, target))
        direction = [b-a for a, b in zip(eye, target)] if usable else [0, 0, 0]
        if not usable or sum(v*v for v in direction) < 1e-12:
            previous = None
            continue
        valid += 1
        if previous is not None and frame['frame'] == previous[0] + 1:
            angular_path += direction_angle(previous[2], direction)
            position_path += math.sqrt(sum((a-b)**2 for a, b in zip(previous[1], eye)))
            steps += 1
        previous = (frame['frame'], eye, direction)
        candidates = sorted(frame['render_candidates'], key=lambda c: c['draws'], reverse=True)
        # Tied groups provide no dominant view; do not choose arbitrarily.
        if not candidates or (len(candidates) > 1 and candidates[0]['draws'] == candidates[1]['draws']):
            continue
        rows = candidates[0]['rows']
        if len(rows) != 3 or not all(finite_vector(row) for row in rows):
            continue
        position_errors.append(math.sqrt(sum((eye[i]-rows[i][3])**2 for i in range(3))))
        forward = [-row[2] for row in rows]
        if sum(v*v for v in forward) > 1e-12:
            direction_errors.append(direction_angle(direction, forward))
    return {'observed_frames': len(frames), 'fresh_valid_cpu_frames': valid,
            'consecutive_cpu_steps': steps, 'cpu_angular_path_degrees': angular_path,
            'cpu_position_path_game_units': position_path,
            'observed_duration_ms': frames[-1]['elapsed_ms']-frames[0]['elapsed_ms'] if frames else 0,
            'candidate_eye_error_max': max(position_errors, default=None),
            'candidate_negative_z_forward_error_max_degrees': max(direction_errors, default=None),
            'note': 'CPU and renderer are asynchronous. Matrix layout remains a hypothesis; inspect sample ages and per-frame data before accepting it.'}


def analyze(capture, source=False):
    if capture.get('schema') not in (1, 2, 3, 4):
        raise ValueError('Unsupported capture schema')
    by_frame = defaultdict(list)
    last = -1
    for draw in capture['draws']:
        frame = draw['frame']
        if type(frame) is not int or frame < last:
            raise ValueError('Frame IDs must be monotonically increasing integers')
        if len(draw['uniforms']) != 96:
            raise ValueError('Expected 96 vertex uniform slots')
        by_frame[frame].append(draw)
        last = frame
    # Starting/stopping mid-frame produces misleading missing/unique draws.
    frames = sorted(by_frame)[1:-1]
    counts = Counter()
    changed = Counter()
    maxima = defaultdict(float)
    pairs = []
    for first, second in zip(frames, frames[1:]):
        if second != first + 1:
            counts['frame_gaps'] += 1
            continue
        groups = []
        for frame in (first, second):
            group = defaultdict(list)
            for draw in by_frame[frame]:
                key = signature(draw, source)
                if key is None:
                    counts['missing_source_observations'] += 1
                else:
                    group[key].append(draw)
            groups.append(group)
        a, b = groups
        counts['frame_pairs'] += 1
        counts['unmatched_signatures'] += len(a.keys() ^ b.keys())
        for key in a.keys() & b.keys():
            if len(a[key]) != 1 or len(b[key]) != 1:
                counts['ambiguous_signatures'] += 1
                continue
            counts['unique_candidate_pairs'] += 1
            slots = []
            for slot, (previous, current) in enumerate(zip(a[key][0]['uniforms'], b[key][0]['uniforms'])):
                if not finite_vector(previous) or not finite_vector(current):
                    counts['nonfinite_slots'] += 1
                    continue
                delta = max(abs(x - y) for x, y in zip(previous, current))
                if delta > 1e-6:
                    changed[slot] += 1
                    maxima[slot] = max(maxima[slot], delta)
                    slots.append(slot)
            if slots:
                counts['changing_candidate_pairs'] += 1
                # Keep enough examples to locate shaders without duplicating uniforms.
                if len(pairs) < 32:
                    pairs.append({'frames': [first, second], 'signature': key,
                                  'changed_slots': slots})
    return {'matching': 'shader_and_draw_source' if source else 'shader_only',
            'complete_frame_candidates': len(frames), 'counts': dict(counts),
            'changing_slots': [{'slot': s, 'pairs': changed[s], 'max_delta': maxima[s]}
                               for s in sorted(changed)], 'examples': pairs}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    capture = json.loads(args.capture.read_text())
    report = {'label': capture.get('label', ''),
              'warning': 'Candidates only. No object identity, camera matrix, native FPS, or replay safety is established.',
              'stop_reason': capture.get('stop_reason'),
              'software_draws_skipped': capture.get('fallback_draws', 0),
              'camera_timeline': camera_timeline(capture),
              'audits': [analyze(capture), analyze(capture, source=True)]}
    text = json.dumps(report, indent=2, allow_nan=False) + '\n'
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end='')


if __name__ == '__main__':
    main()
