import copy
import unittest
from summarize_aot_abba import summarize, summarize_cpu


def rows():
    out = []
    for duration in (10, 8, 8, 10):
        out.append({'memory_fingerprint': 7, 'benchmark': {
            'measured_frames': 1000, 'frame_times': {'mean_ms': duration},
            'pre_backend_cpu': {'host_envelope_mean_ms': duration, 'mean_ms': duration,
                'guest_seconds': duration, 'guest_clock_included_seconds': 0,
                'host_envelope_p95_upper_ms': duration + 1}}})
    return out


def cpu_report():
    return {'entry': 0x1000, 'eligible': True, 'matched': True,
            'original_memory_untouched': True, 'trials': [
                {'pair': 0, 'trial': index, 'alternative': index % 4 in (1, 2),
                 'engine': 'aot', 'valid': True, 'matched': True,
                 'nanoseconds': 100, 'thread_cycles': 100}
                for index in range(16)]}


class AbbaTests(unittest.TestCase):
    def test_pairs_both_directions(self):
        summary = summarize(rows())
        self.assertEqual(summary['pre_backend_envelope_ms']['paired_ratios'], [0.8, 0.8])

    def test_rejects_mismatched_or_sampled_workloads(self):
        changes = [lambda r: r.pop(), lambda r: r[1].update(memory_fingerprint=8),
            lambda r: r[2].update(intrusive_native_sampling=True),
            lambda r: r[3]['benchmark'].update(measured_frames=999)]
        for change in changes:
            data = copy.deepcopy(rows())
            change(data)
            with self.assertRaises(ValueError):
                summarize(data)

    def test_cpu_noise_limits_include_the_declared_boundary(self):
        for alternative in (95, 105):
            report = cpu_report()
            for row in report['trials']:
                if row['alternative']:
                    row['nanoseconds'] = row['thread_cycles'] = alternative
            self.assertTrue(summarize_cpu([report])['captures'][0]['timing_qualified'])

    def test_cpu_median_cannot_hide_a_noisy_quartet_or_metric(self):
        for metric in ('nanoseconds', 'thread_cycles'):
            report = cpu_report()
            for index in (5, 6):
                report['trials'][index][metric] = 130
            result = summarize_cpu([report])['captures'][0]
            self.assertTrue(result['architecturally_qualified'])
            self.assertFalse(result['timing_qualified'])

    def test_cpu_rejects_partial_sequences_and_architectural_divergence(self):
        report = cpu_report()
        del report['trials'][7]
        with self.assertRaises(ValueError):
            summarize_cpu([report])
        report = cpu_report()
        report['matched'] = False
        self.assertFalse(summarize_cpu([report])['captures'][0]['architecturally_qualified'])


if __name__ == '__main__':
    unittest.main()
