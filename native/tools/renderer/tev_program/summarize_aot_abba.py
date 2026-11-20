"""Summarize steady-state ABBA AOT runs; reject incomplete/mismatched workloads."""
import argparse
import json
from pathlib import Path
import statistics


def summarize(rows):
    if not rows or len(rows) % 4:
        raise ValueError('Expected complete ABBA groups')
    if len({r['memory_fingerprint'] for r in rows}) != 1:
        raise ValueError('Final game memory differs')
    if any(r.get('intrusive_native_sampling') for r in rows):
        raise ValueError('Sampled runs are not performance evidence')
    pairs = [(a, b) for i in range(0, len(rows), 4) for a, b in ((i, i + 1), (i + 3, i + 2))]
    frames = {r['benchmark']['measured_frames'] for r in rows}
    if len(frames) != 1:
        raise ValueError('Different steady-state window lengths')
    result = {'pairs': pairs, 'fingerprint': rows[0]['memory_fingerprint'], 'measured_updates_per_run': frames.pop()}
    getters = {
        'pre_backend_envelope_ms': lambda b: b['pre_backend_cpu']['host_envelope_mean_ms'],
        'accounted_pre_backend_ms': lambda b: b['pre_backend_cpu']['mean_ms'],
        'guest_ms': lambda b: b['pre_backend_cpu']['guest_seconds'] * 1000 / b['measured_frames'],
        'guest_excluding_clock_ms': lambda b: (b['pre_backend_cpu']['guest_seconds'] -
            b['pre_backend_cpu']['guest_clock_included_seconds']) * 1000 / b['measured_frames'],
        'whole_frame_ms': lambda b: b['frame_times']['mean_ms'],
        'pre_backend_p95_ms': lambda b: b['pre_backend_cpu']['host_envelope_p95_upper_ms'],
    }
    for name, getter in getters.items():
        values = [getter(r['benchmark']) for r in rows]
        ratios = [values[b] / values[a] for a, b in pairs]
        result[name] = {
            'control_median': statistics.median(values[a] for a, _ in pairs),
            'candidate_median': statistics.median(values[b] for _, b in pairs),
            'paired_ratios': ratios,
            'paired_median_ratio': statistics.median(ratios),
        }
    return result


def summarize_cpu(reports):
    """Keep architectural exclusions and A/A noise distinct from CPU speed."""
    captures = []
    for report in reports:
        capture = {'entry': f"{report['entry']:08X}", 'architecturally_qualified':
                   bool(report.get('matched') and report.get('original_memory_untouched'))}
        captures.append(capture)
        if not capture['architecturally_qualified']:
            capture['reason'] = ('uncaptured' if report.get('uncaptured') else
                                 'excluded' if not report.get('eligible') else 'divergence')
            continue
        pairs = {}
        rows = report['trials']
        for pair in sorted({r['pair'] for r in rows}):
            measured = sorted((r for r in rows if r['pair'] == pair and r['trial'] >= 0),
                              key=lambda r: r['trial'])
            if ([r['trial'] for r in measured] != list(range(16)) or
                    any(not r['valid'] or not r['matched'] or
                        r['alternative'] != (r['trial'] % 4 in (1, 2)) for r in measured)):
                raise ValueError('Incomplete or invalid CPU ABBA sequence')
            result = {'engine': measured[1]['engine']}
            # Always discard the first complete quartet, never a slow sample
            # chosen after observing the result.
            for metric in ('nanoseconds', 'thread_cycles'):
                values = [r[metric] for r in measured[4:]]
                if any(not 0 < v < float('inf') for v in values):
                    raise ValueError('Invalid CPU clock measurement')
                ratios = [(values[i + 1] + values[i + 2]) /
                          (values[i] + values[i + 3]) for i in range(0, 12, 4)]
                result[metric] = {
                    'baseline_median': statistics.median(values[i] for i in (0, 3, 4, 7, 8, 11)),
                    'alternative_median': statistics.median(values[i] for i in (1, 2, 5, 6, 9, 10)),
                    'quartet_ratios': ratios, 'median_ratio': statistics.median(ratios)}
            pairs[str(pair)] = result
        if '0' not in pairs:
            raise ValueError('Missing CPU A/A control')
        capture['pairs'] = pairs
        capture['timing_qualified'] = all(
            0.95 <= pairs['0'][metric]['median_ratio'] <= 1.05 and
            all(0.80 <= ratio <= 1.20 for ratio in pairs['0'][metric]['quartet_ratios'])
            for metric in ('nanoseconds', 'thread_cycles'))
        capture['dirty_reset_bytes'] = report.get('dirty_reset_bytes')
    return {'diagnostic_only': True, 'not_gameplay_timing': True,
            'discarded_initial_quartets': 1, 'captures': captures}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('measurements', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--cpu-replay', action='store_true')
    args = parser.parse_args()
    summarize_input = summarize_cpu if args.cpu_replay else summarize
    result = json.dumps(summarize_input(json.loads(args.measurements.read_text())), indent=2)
    if args.output:
        args.output.write_text(result)
    print(result)
