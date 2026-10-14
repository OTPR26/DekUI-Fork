import unittest
from analyze_link_pose import analyze, near, inverse, multiply


class LinkPoseTest(unittest.TestCase):
    def data(self):
        return {'poses': [{'frame': 1, 'instance': 'abc', 'bones': [list(range(12))]}],
                'draws': [{'frame': 1, 'vertices': 3, 'vertex_base': '18000000',
                           'index_offset': 0, 'palette': [list(range(12))]}]}

    def test_frame_offset_is_explicit(self):
        data = self.data()
        data['draws'][0]['frame'] = 2
        self.assertEqual(analyze(data)['matched_draws'], 0)
        self.assertEqual(analyze(data, pose_frame_offset=1)['matched_draws'], 1)

    def test_bind_correction(self):
        identity = [1,0,0,0,0,1,0,0,0,0,1,0]
        bind = [0,-1,0,10,1,0,0,20,0,0,1,30]
        for a,b in zip(multiply(bind,inverse(bind)), identity):
            self.assertAlmostEqual(a,b)
        data = self.data()
        data['poses'][0]['bones'] = [identity]
        data['draws'][0]['palette'] = [inverse(bind)]
        self.assertEqual(analyze(data)['matched_draws'], 0)
        self.assertEqual(analyze(data, corrections=[inverse(bind)])['matched_draws'], 1)
        with self.assertRaises(ValueError):
            inverse([0]*12)

    def test_matches_and_quantization(self):
        data = self.data()
        data['draws'][0]['palette'][0][4] += 0.00005
        report = analyze(data)
        self.assertEqual(report['matched_draws'], 1)
        self.assertEqual(report['matches'][0]['uniform'], 20)

    def test_identical_instances_are_ambiguous(self):
        data = self.data()
        data['poses'].append(dict(data['poses'][0], instance='def'))
        report = analyze(data)
        self.assertEqual(report['matched_draws'], 0)
        self.assertEqual(report['ambiguous_slots'], 1)

    def test_repeated_instance_does_not_create_false_ambiguity(self):
        data = self.data()
        data['poses'].append(data['poses'][0])
        self.assertEqual(analyze(data)['matched_draws'], 1)

    def test_stale_frame_and_nonfinite_rejected(self):
        data = self.data()
        data['draws'][0]['frame'] = 2
        self.assertEqual(analyze(data)['matched_draws'], 0)
        self.assertFalse(near([float('nan')]*12, [0]*12))


if __name__ == '__main__':
    unittest.main()
