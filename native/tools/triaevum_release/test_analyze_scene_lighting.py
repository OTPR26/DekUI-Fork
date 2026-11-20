import json
import math
import tempfile
import unittest
from pathlib import Path

from analyze_scene_lighting import keyframe_metrics, light_metrics, scene_lighting_metrics


def trace(frame: int, epoch: int, caster: int, strength: float, lights: dict[int, tuple[float, float, float]]) -> str:
    text = f"TRIAEVUM_SCENE_LIGHT_CONTINUITY frame={frame} epoch={epoch} continuous=1 caster={caster:x} strength={strength:.4f}"
    for index, (identity, (x, y, z)) in enumerate(lights.items()):
        text += f" L{index}={identity:x}/{x:.6f},{y:.6f},{z:.6f}/1.0000,1.0000,1.0000/sky=1/exact=1"
    return text + "\n"


def sun(degrees: float) -> tuple[float, float, float]:
    radians = math.radians(degrees)
    return (math.cos(radians), math.sin(radians), 0.0)


class LightMetricsTests(unittest.TestCase):
    def test_smooth_motion_jumps_and_light_below_the_horizon(self):
        log = "".join(trace(frame, 1, 0xA, 1.0, {0xA: sun(30 + 0.1 * frame)}) for frame in range(1, 5))
        metrics = light_metrics(log)
        self.assertEqual((metrics["trace_frames"], metrics["moving_frames"], metrics["jumps"]), (4, 3, 0))
        self.assertEqual(metrics["below_horizon_casting"], 0)
        self.assertLess(metrics["largest_step_change"], 1e-3)
        # A 5 degree turn is a jump; a caster at -1 degree that still casts is below the horizon.
        log += trace(5, 1, 0xA, 1.0, {0xA: sun(35.4)}) + trace(6, 1, 0xA, 0.5, {0xA: sun(-1.0)})
        metrics = light_metrics(log)
        self.assertEqual((metrics["jumps"], metrics["below_horizon_casting"]), (2, 1))
        # A new epoch (a cut) is not compared with the frame before it; a faded caster does not cast.
        log += trace(7, 2, 0xA, 0.0, {0xA: sun(-40.0)})
        self.assertEqual(light_metrics(log)["jumps"], 2)
        self.assertEqual(light_metrics(log)["below_horizon_casting"], 1)


class KeyframeMetricsTests(unittest.TestCase):
    @staticmethod
    def frame(index: int, light: float, shown: float, **extra) -> dict:
        lx, ly, lz = sun(light)
        dx, dy, dz = sun(shown)
        return {"frame_index": index, "nri_directional_shadow_light_direction_x": lx,
                "nri_directional_shadow_light_direction_y": ly, "nri_directional_shadow_light_direction_z": lz,
                "nri_vsm_keyframe_direction_x": dx, "nri_vsm_keyframe_direction_y": dy,
                "nri_vsm_keyframe_direction_z": dz, "nri_vsm_keyframe_displayed_layouts": 2,
                "nri_vsm_keyframe_construction_pages": 16, "nri_vsm_keyframe_construction_budget": 16, **extra}

    def test_shadows_follow_the_light_every_frame(self):
        frames = [self.frame(i, 30 + 0.02 * i, 29.4 + 0.02 * i) for i in range(10)]
        metrics = keyframe_metrics(frames, skip=0)
        self.assertEqual(metrics, {"keyframed_frames": 10, "moving_shadow_frames": 9, "stalled_frames": 0,
                                   "uneven_frames": 0, "cuts": 0, "construction_over_budget": 0})
        # Frame 5: the light moves, the displayed shadow stays where frame 4 showed it.
        frames[5] = self.frame(5, 30.1, 29.4 + 0.08, nri_vsm_keyframe_construction_pages=20)
        frames[8] = self.frame(8, 30.16, 29.4 + 0.16 + 0.2, nri_vsm_keyframe_cuts=1)
        metrics = keyframe_metrics(frames, skip=0)
        self.assertEqual((metrics["stalled_frames"], metrics["construction_over_budget"], metrics["cuts"]),
                         (1, 1, 1))
        # Frames before --skip are not counted.
        self.assertEqual(keyframe_metrics(frames, skip=6)["construction_over_budget"], 0)

    def test_run_directory_combines_both_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            (run / "launch.log").write_text(trace(1, 1, 0xA, 1.0, {0xA: sun(30)}) +
                                            trace(2, 1, 0xA, 1.0, {0xA: sun(30.1)}))
            (run / "gpu.json").write_text(json.dumps({"frames": [self.frame(i, 30 + 0.02 * i, 30 + 0.02 * i)
                                                                 for i in range(4)]}))
            metrics = scene_lighting_metrics(run, skip=0)
            self.assertEqual((metrics["moving_frames"], metrics["moving_shadow_frames"]), (1, 3))


if __name__ == "__main__":
    unittest.main()
