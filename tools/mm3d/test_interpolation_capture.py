import copy
import unittest
from analyze_interpolation_capture import analyze, camera_timeline


def draw(frame, source='1000', offset=0):
    uniforms = [[0., 0., 0., 0.] for _ in range(96)]
    uniforms[4][3] = frame + offset
    return dict(frame=frame, shader='abc', swizzle='def', vertices=36, width=480,
                height=400, depth_state=1, uniforms=uniforms,
                source=dict(valid=True, vertex_base=source, first_vertex=0,
                            last_vertex=23, index_offset=100, topology=0,
                            indexed=True, index16=True))


class InterpolationCaptureTests(unittest.TestCase):
    def test_full_orbit_returning_to_start_retains_360_degree_path(self):
        directions = ([0, 0, -1], [1, 0, 0], [0, 0, 1], [-1, 0, 0], [0, 0, -1])
        frames = [dict(frame=i, elapsed_ms=i*100, cpu=dict(valid=True, fresh=True,
                  eye=[0, 0, 0], target=d), render_candidates=[])
                  for i, d in enumerate(directions)]
        result = camera_timeline({'camera_frames': frames})
        self.assertAlmostEqual(result['cpu_angular_path_degrees'], 360)
        self.assertEqual(result['consecutive_cpu_steps'], 4)
        self.assertEqual(result['observed_duration_ms'], 400)
        frames[2]['cpu']['fresh'] = False
        result = camera_timeline({'camera_frames': frames})
        self.assertEqual(result['consecutive_cpu_steps'], 2)
        self.assertAlmostEqual(result['cpu_angular_path_degrees'], 180)

    def test_direct_camera_and_inverse_view_agree(self):
        frame = dict(frame=1, elapsed_ms=0, cpu=dict(valid=True, fresh=True,
                     eye=[10, 20, 30], target=[10, 20, 29]), render_candidates=[
                     dict(draws=100, rows=[[1, 0, 0, 10], [0, 1, 0, 20], [0, 0, 1, 30]])])
        result = camera_timeline({'camera_frames': [frame]})
        self.assertEqual(result['candidate_eye_error_max'], 0)
        self.assertEqual(result['candidate_negative_z_forward_error_max_degrees'], 0)

    def capture(self, draws):
        return {'schema': 3, 'draws': draws}

    def test_reordered_shared_shader_draws_need_source_evidence(self):
        draws = []
        for frame in range(5):
            group = [draw(frame, '1000'), draw(frame, '2000', 100)]
            draws.extend(group if frame % 2 else reversed(group))
        capture = self.capture(draws)
        weak = analyze(capture)['counts']
        self.assertEqual(weak['ambiguous_signatures'], 2)
        self.assertNotIn('unique_candidate_pairs', weak)
        strong = analyze(capture, source=True)
        self.assertEqual(strong['counts']['unique_candidate_pairs'], 4)
        self.assertEqual(strong['changing_slots'], [{'slot': 4, 'pairs': 4, 'max_delta': 1}])

    def test_duplicate_source_is_still_ambiguous(self):
        capture = self.capture([draw(f) for f in range(5) for _ in range(2)])
        counts = analyze(capture, source=True)['counts']
        self.assertEqual(counts['ambiguous_signatures'], 2)
        self.assertNotIn('unique_candidate_pairs', counts)

    def test_never_pairs_across_missing_frames(self):
        capture = self.capture([draw(f) for f in (0, 1, 3, 4)])
        self.assertEqual(analyze(capture)['counts'], {'frame_gaps': 1})

    def test_discards_first_and_last_partial_frames(self):
        capture = self.capture([draw(f) for f in range(4)])
        result = analyze(capture)
        self.assertEqual(result['counts']['frame_pairs'], 1)
        self.assertEqual(result['examples'][0]['frames'], [1, 2])

    def test_buffer_change_is_unmatched(self):
        capture = self.capture([draw(f, str(f)) for f in range(4)])
        self.assertEqual(analyze(capture, True)['counts']['unmatched_signatures'], 2)

    def test_nonfinite_data_is_not_a_transform_candidate(self):
        capture = self.capture([draw(f) for f in range(4)])
        capture['draws'][2]['uniforms'][4][3] = None
        result = analyze(capture)
        self.assertEqual(result['counts']['nonfinite_slots'], 1)
        self.assertEqual(result['changing_slots'], [])

    def test_old_capture_remains_usable_without_source_claims(self):
        capture = self.capture([draw(f) for f in range(4)])
        capture['schema'] = 2
        for sample in capture['draws']:
            del sample['source']
        self.assertEqual(analyze(capture)['counts']['unique_candidate_pairs'], 1)
        self.assertEqual(analyze(capture, True)['counts']['missing_source_observations'], 2)

    def test_reset_and_malformed_uniforms_are_rejected(self):
        with self.assertRaises(ValueError):
            analyze(self.capture([draw(2), draw(1)]))
        sample = copy.deepcopy(draw(1))
        sample['uniforms'].pop()
        with self.assertRaises(ValueError):
            analyze(self.capture([sample]))
