"""Continuity of the animated scene light and its shadow keyframes in one game run.

usage: analyze_scene_lighting.py RUN_DIR [--skip FRAMES]

Reads the run's launch log (TRIAEVUM_SCENE_LIGHT_CONTINUITY lines, one per
guest frame, printed with TRIAEVUM_SCENE_LIGHT_TRACE=1) and its per-frame
renderer diagnostics (gpu.json, the virtual shadow map keyframe counters), and
prints the metrics of docs/TRIAEVUM_ANIMATED_LIGHTING_STRATEGY.md section 7 as
JSON. The battery reads them as `lighting:<metric>`.

Light (per guest frame, within a continuity epoch):
- trace_frames, moving_frames, still_frames;
- jumps: turns over 2 degrees between consecutive frames;
- largest_step_change: the largest change of angular step between two
  consecutive moving frames, in degrees;
- below_horizon_casting: frames whose shadow caster is at or below the horizon
  while its shadow strength is above zero.
Shadow keyframes (per presented frame, after --skip):
- keyframed_frames: frames with a displayed layout;
- moving_shadow_frames: frames whose shadow light moved;
- stalled_frames: frames in which the light moved and the displayed shadow
  direction did not; uneven_frames: displayed step outside 0.5x-2x the light's;
- cuts: keyframe cuts; construction_over_budget: frames whose layout under
  construction drew more pages than its budget.
"""

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path

LINE = re.compile(r"TRIAEVUM_SCENE_LIGHT_CONTINUITY frame=(\d+) epoch=(\d+) continuous=(\d) caster=(\w+) "
                  r"strength=([-\d.]+)(.*)")
LIGHT = re.compile(r" L(\d)=(\w+)/([-\d.]+),([-\d.]+),([-\d.]+)/([-\d.]+),([-\d.]+),([-\d.]+)/sky=(\d)/exact=(\d)")


def _unit(vector: tuple[float, ...]) -> tuple[float, ...]:
    length = math.sqrt(sum(c * c for c in vector))
    return tuple(c / length for c in vector) if length > 0 else (0.0, 0.0, 0.0)


def _angle(a: tuple[float, ...], b: tuple[float, ...]) -> float:
    a, b = _unit(a), _unit(b)
    if not any(a) or not any(b):
        return 0.0
    return math.degrees(math.acos(max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b))))))


def light_metrics(log_text: str) -> dict:
    frames = []
    for match in LINE.finditer(log_text):
        lights = {int(light[2], 16): {"direction": _unit(tuple(map(float, light.group(3, 4, 5))))}
                  for light in LIGHT.finditer(match[6])}
        frames.append({"frame": int(match[1]), "epoch": int(match[2]), "caster": int(match[4], 16),
                       "strength": float(match[5]), "lights": lights})
    moving = still = jumps = below = 0
    steps: list[tuple[int, float]] = []
    for previous, current in zip(frames, frames[1:]):
        if current["epoch"] != previous["epoch"]:
            continue
        caster = current["lights"].get(current["caster"])
        if caster and caster["direction"][1] <= 0 and current["strength"] > 0:
            below += 1
        turn = max((_angle(previous["lights"][identity]["direction"], light["direction"])
                    for identity, light in current["lights"].items() if identity in previous["lights"]),
                   default=0.0)
        jumps += turn > 2.0
        if turn > 1e-6:
            moving += 1
        else:
            still += 1
        steps.append((current["frame"], turn))
    change = max((abs(b[1] - a[1]) for a, b in zip(steps, steps[1:])
                  if a[1] > 1e-6 and b[1] > 1e-6 and b[0] == a[0] + 1), default=0.0)
    return {"trace_frames": len(frames), "moving_frames": moving, "still_frames": still, "jumps": jumps,
            "largest_step_change": change, "below_horizon_casting": below}


def keyframe_metrics(frames: list[dict], skip: int = 120) -> dict:
    rows = frames[skip:]
    light = [(f.get("nri_directional_shadow_light_direction_x", 0.0),
              f.get("nri_directional_shadow_light_direction_y", 0.0),
              f.get("nri_directional_shadow_light_direction_z", 0.0)) for f in rows]
    shown = [(f.get("nri_vsm_keyframe_direction_x", 0.0), f.get("nri_vsm_keyframe_direction_y", 0.0),
              f.get("nri_vsm_keyframe_direction_z", 0.0)) for f in rows]
    layouts = [f.get("nri_vsm_keyframe_displayed_layouts", 0) for f in rows]
    moving = stalled = uneven = 0
    for index in range(1, len(rows)):
        if rows[index].get("frame_index") != rows[index - 1].get("frame_index", -2) + 1:
            continue
        if not any(light[index]) or not any(light[index - 1]):
            continue
        step = _angle(light[index - 1], light[index])
        if step <= 1e-4:
            continue
        moving += 1
        if rows[index].get("nri_vsm_keyframe_cuts", 0) > 0 or not layouts[index] or not layouts[index - 1]:
            continue
        displayed = _angle(shown[index - 1], shown[index])
        if displayed < 1e-5:
            stalled += 1
        elif displayed > 2.0 * step or displayed < 0.5 * step:
            uneven += 1
    return {
        "keyframed_frames": sum(1 for count in layouts if count > 0),
        "moving_shadow_frames": moving,
        "stalled_frames": stalled,
        "uneven_frames": uneven,
        "cuts": sum(1 for f in rows if f.get("nri_vsm_keyframe_cuts", 0) > 0),
        "construction_over_budget": sum(
            1 for f in rows
            if f.get("nri_vsm_keyframe_construction_pages", 0) > f.get("nri_vsm_keyframe_construction_budget", 0)),
    }


def scene_lighting_metrics(run_dir: Path, skip: int = 120) -> dict:
    log = run_dir / "launch.log"
    diagnostics = run_dir / "gpu.json"
    metrics = light_metrics(log.read_text(errors="replace")) if log.exists() else {}
    if diagnostics.exists():
        metrics.update(keyframe_metrics(json.loads(diagnostics.read_text(encoding="utf-8")).get("frames", []), skip))
    return metrics


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("run", type=Path)
    parser.add_argument("--skip", type=int, default=120)
    args = parser.parse_args()
    print(json.dumps(scene_lighting_metrics(args.run, args.skip), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
