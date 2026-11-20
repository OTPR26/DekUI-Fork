"""Windows qualification battery of the line after alpha.2c.

Runs the layers of docs/TRIAEVUM_WINDOWS_RELEASE_QUALIFICATION.md section 3 and
writes one report. Machine-specific paths (build trees, build commands, private
inputs) come from a local JSON configuration; an example lives next to this
file. Nothing here reads or writes ROM-derived data outside the configured
private directories, and every output goes to the report directory.

usage: qualify_windows_battery.py --config LOCAL.json --output DIR [--layers build,unit,scenarios]

Layers implemented here:
- build: the product targets and every test executable CTest knows, so a test
  that does not build fails the battery instead of disappearing;
- unit: CTest of each build tree (JUnit per test), the generator --check
  commands and the Python suites;
- scenarios: deterministic game runs from private checkpoints and input
  timelines at several output sizes, judged by their exit, their framebuffer
  captures (accepted references, full-frame effects) and the renderer's
  per-frame diagnostics.
"""

from __future__ import annotations

import argparse
import contextlib
import functools
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import xml.etree.ElementTree as ElementTree
from dataclasses import asdict, dataclass, field
from pathlib import Path

LAYERS = ("build", "unit", "scenarios")


@dataclass
class Check:
    layer: str
    name: str
    status: str  # pass, fail, notrun, skipped
    detail: str = ""
    seconds: float = 0.0
    log: str = ""


@dataclass
class Report:
    started: str
    configuration: str
    checks: list[Check] = field(default_factory=list)

    def add(self, check: Check) -> Check:
        self.checks.append(check)
        return check


def run_logged(command: list[str], log: Path, *, cwd: Path | None = None, timeout: float | None = None,
               env: dict[str, str] | None = None) -> tuple[int | str, float]:
    """Run a command with its output in a log file; returns (exit code or "timeout", seconds)."""
    log.parent.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    with log.open("wb") as handle:
        handle.write(("$ " + subprocess.list2cmdline(command) + "\n").encode())
        handle.flush()
        try:
            process = subprocess.run(command, cwd=cwd, stdout=handle, stderr=subprocess.STDOUT, timeout=timeout,
                                     env=env)
            code: int | str = process.returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
        except OSError as error:
            handle.write(f"cannot start: {error}\n".encode())
            code = "not started"
    return code, time.monotonic() - start


def wait_idle(names: list[str], poll: float = 20.0) -> None:
    """Wait until none of the named processes runs (builds and game instances share this machine)."""
    if not names:
        return
    lowered = [name.lower() for name in names]
    while True:
        listing = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True,
                                 errors="replace").stdout.lower()
        if not any(f'"{name}"' in listing for name in lowered):
            return
        time.sleep(poll)


def ctest_test_targets(ctest: str, build_dir: Path) -> tuple[list[str], list[Path]]:
    """The CMake targets that build the tests of this tree, and the executables CTest expects.

    A test whose executable exists has a command (its first word is the executable; the target is its stem:
    the Ninja generator defines a phony target per CMake target and target names are global). CTest omits the
    command of a test whose executable was never built; such a test was registered by target name
    (CMake/oot3d/tests.cmake), so its name is the target.
    """
    out = subprocess.run([ctest, "--test-dir", str(build_dir), "--show-only=json-v1"], capture_output=True,
                         text=True, errors="replace")
    if out.returncode != 0:
        raise RuntimeError(f"ctest --show-only failed in {build_dir}: {out.stderr.strip()[:300]}")
    targets: list[str] = []
    executables: list[Path] = []
    root = build_dir.resolve()
    for test in json.loads(out.stdout).get("tests", []):
        command = test.get("command") or []
        if command:
            path = Path(command[0])
            try:
                path.resolve().relative_to(root)
            except ValueError:
                continue
            target = path.stem
            if path not in executables:
                executables.append(path)
        elif re.fullmatch(r"[A-Za-z0-9_\-]+", test.get("name", "")):
            target = test["name"]
        else:
            continue
        if target not in targets:
            targets.append(target)
    return targets, executables


def failed_build_outputs(log: Path) -> list[str]:
    """Ninja's FAILED: lines (with -k 0 every independent failure is listed)."""
    failed = []
    for line in log.read_text(errors="replace").splitlines():
        if line.startswith("FAILED: "):
            failed.append(line[len("FAILED: "):].strip())
    return failed


def parse_junit(path: Path) -> list[tuple[str, str, float, str]]:
    """(name, status, seconds, message) for every testcase of a CTest JUnit file."""
    results = []
    root = ElementTree.parse(path).getroot()
    for case in root.iter("testcase"):
        name = case.get("name", "?")
        seconds = float(case.get("time") or 0.0)
        status = case.get("status", "")
        failure = case.find("failure")
        skipped = case.find("skipped")
        output = case.findtext("system-out") or ""
        if failure is not None or status == "fail":
            # The test's own failure lines say more than CTest's "Failed".
            lines = [line.strip() for line in output.splitlines()
                     if re.search(r"FAILED|error|Error|exception|assert", line)]
            message = "; ".join(lines[:3]) or (failure.get("message", "") if failure is not None else "")
            status = "fail"
        elif status in ("notrun", "disabled") or skipped is not None:
            message = (skipped.get("message", "") if skipped is not None else "") or status
            status = "skipped" if status == "disabled" or "skip" in message.lower() else "notrun"
        else:
            message = ""
            status = "pass"
        results.append((name, status, seconds, message[:300]))
    return results


def unittest_summary(log: Path) -> tuple[str, str]:
    """Status and a one-line summary of a `python -m unittest` run."""
    lines = log.read_text(errors="replace").splitlines()
    # Tests may print after unittest's summary (subprocess output flushed late): find the last "Ran N tests"
    # and the verdict that follows it.
    for index in range(len(lines) - 1, -1, -1):
        match = re.match(r"^Ran (\d+) tests? in", lines[index])
        if not match:
            continue
        for verdict in lines[index + 1:]:
            verdict = verdict.strip()
            if verdict.startswith("OK"):
                return "pass", f"{match.group(1)} tests, {verdict}"
            if verdict.startswith("FAILED"):
                return "fail", f"{match.group(1)} tests, {verdict}"
        return "fail", f"{match.group(1)} tests, no verdict"
    errors = [line for line in lines if "Error" in line or "error" in line]
    return "fail", (errors[-1] if errors else (lines[-1] if lines else "no output"))[:200]


def pytest_summary(log: Path) -> tuple[str, str]:
    """Status and summary of a `python -m pytest -q` run (its last counts line)."""
    lines = [line.strip("= ").strip() for line in log.read_text(errors="replace").splitlines()]
    for line in reversed(lines):
        if re.search(r"\b\d+ (passed|failed|error|errors|skipped)\b", line):
            bad = re.search(r"\b\d+ (failed|error|errors)\b", line)
            return ("fail" if bad else "pass"), line[:200]
    return "fail", (lines[-1] if lines else "no output")[:200]


def layer_build(config: dict, out: Path, report: Report) -> None:
    ctest = config["ctest"]
    for tree in config["build_trees"]:
        build_dir = Path(tree["dir"])
        wait_idle(config.get("wait_idle_processes", []))
        # Regenerate the build files first (Ninja re-runs CMake when a list file changed), so CTest lists
        # the tests of the current sources.
        log = out / "logs" / f"configure-{tree['name']}.log"
        code, seconds = run_logged(list(tree["build_command"]) + ["build.ninja"], log, timeout=3600)
        if code != 0:
            report.add(Check("build", f"{tree['name']}: configure", "fail", f"exit {code}", seconds, str(log)))
            continue
        try:
            tests, _ = ctest_test_targets(ctest, build_dir)
        except (RuntimeError, OSError, json.JSONDecodeError) as error:
            report.add(Check("build", f"{tree['name']}: test list", "fail", str(error)))
            continue
        targets = list(dict.fromkeys(list(tree.get("targets", [])) + tests))
        log = out / "logs" / f"build-{tree['name']}.log"
        code, seconds = run_logged(list(tree["build_command"]) + ["-k", "0"] + targets, log,
                                   timeout=tree.get("timeout_seconds", 14400))
        failed = failed_build_outputs(log)
        status = "pass" if code == 0 else "fail"
        detail = f"{len(targets)} targets ({len(tests)} test targets), exit {code}"
        if failed:
            detail += f"; {len(failed)} failed outputs: " + ", ".join(failed[:8])
        report.add(Check("build", tree["name"], status, detail, seconds, str(log)))


def layer_unit(config: dict, out: Path, report: Report) -> None:
    ctest = config["ctest"]
    for tree in config["build_trees"]:
        build_dir = Path(tree["dir"])
        wait_idle(config.get("wait_idle_processes", []))
        junit = out / f"ctest-{tree['name']}.xml"
        log = out / "logs" / f"ctest-{tree['name']}.log"
        command = [ctest, "--test-dir", str(build_dir), "--output-junit", str(junit.resolve()),
                   "--timeout", str(tree.get("test_timeout_seconds", 900)), "-j", str(tree.get("test_jobs", 1))]
        # Private inputs reach the tests through the environment (for example OOT3D_TEST_EXTRACT_ROOT);
        # a test that lacks them reports itself skipped.
        environment = os.environ.copy()
        environment.update(tree.get("test_env", {}))
        code, seconds = run_logged(command, log, timeout=tree.get("ctest_timeout_seconds", 21600), env=environment)
        if not junit.exists():
            report.add(Check("unit", f"{tree['name']}: ctest", "fail", f"no JUnit output, exit {code}", seconds,
                             str(log)))
            continue
        cases = parse_junit(junit)
        counts: dict[str, int] = {}
        for name, status, case_seconds, message in cases:
            counts[status] = counts.get(status, 0) + 1
            if status != "pass":
                report.add(Check("unit", f"{tree['name']}: {name}", status, message, case_seconds, str(log)))
        summary = ", ".join(f"{key} {value}" for key, value in sorted(counts.items()))
        clean = not counts.get("fail") and not counts.get("notrun")
        report.add(Check("unit", f"{tree['name']}: ctest", "pass" if clean else "fail",
                         f"{len(cases)} tests: {summary}", seconds, str(log)))
    for check in config.get("generator_checks", []):
        log = out / "logs" / f"generator-{check['name']}.log"
        code, seconds = run_logged(list(check["command"]), log, cwd=Path(check.get("cwd", ".")),
                                   timeout=check.get("timeout_seconds", 1800))
        last = log.read_text(errors="replace").strip().splitlines()[-1:] or [""]
        report.add(Check("unit", f"generator: {check['name']}", "pass" if code == 0 else "fail",
                         f"exit {code}: {last[0][:200]}", seconds, str(log)))
    repository = Path(config["repository"])
    for suite in config.get("python_suites", []):
        name = suite["name"]
        log = out / "logs" / f"python-{name.replace('/', '_')}.log"
        python = config.get("python", sys.executable)
        pytest = suite.get("runner") == "pytest"
        if pytest:
            command = [python, "-m", "pytest", "-q", "-p", "no:cacheprovider", suite["start"]]
        else:
            command = [python, "-m", "unittest", "discover", "-s", suite["start"], "-p",
                       suite.get("pattern", "test_*.py")]
            if suite.get("top"):
                command += ["-t", suite["top"]]
        environment = os.environ.copy()
        for key, value in suite.get("env", {}).items():
            # Relative paths in a suite's environment are relative to the repository.
            environment[key] = os.pathsep.join(str((repository / part).resolve()) if part and not Path(part).is_absolute()
                                               else part for part in value.split(os.pathsep))
        code, seconds = run_logged(command, log, cwd=repository, timeout=suite.get("timeout_seconds", 3600),
                                   env=environment)
        status, detail = pytest_summary(log) if pytest else unittest_summary(log)
        if code not in (0, 1):
            status, detail = "fail", f"exit {code}; {detail}"
        report.add(Check("unit", f"python: {name}", status, detail, seconds, str(log)))


def read_bmp(path: Path) -> tuple[int, int, list[bytes]]:
    """Width, height and top-down rows of BGR triples of an uncompressed 24- or 32-bit framebuffer capture."""
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError(f"not a BMP: {path}")
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height, planes, depth, compression = struct.unpack_from("<iiHHI", data, 18)
    if width <= 0 or height == 0 or planes != 1 or depth not in (24, 32) or compression != 0:
        raise ValueError(f"unsupported BMP layout: {path}")
    stride = ((width * depth + 31) // 32) * 4
    count = abs(height)
    if offset < 54 or offset + stride * count > len(data):
        raise ValueError(f"truncated BMP: {path}")
    step = depth // 8
    rows = []
    for row in (range(count) if height < 0 else range(count - 1, -1, -1)):
        pixels = data[offset + stride * row:offset + stride * row + width * step]
        if step == 4:
            pixels = b"".join(pixels[index:index + 3] for index in range(0, len(pixels), 4))
        rows.append(pixels)
    return width, count, rows


def desaturated_columns(path: Path, tolerance: int = 8) -> tuple[int, list[int]]:
    """Image width and the columns whose every pixel is grey (channel spread within the tolerance).

    The title's full-frame effects (the revelation's black-and-white flashback, fades to black) must reach
    every output column; a placed rectangle leaves coloured margins.
    """
    width, _, rows = read_bmp(path)
    grey = [True] * width
    for row in rows:
        for column, pixel in enumerate(zip(row[0::3], row[1::3], row[2::3])):
            if grey[column] and max(pixel) - min(pixel) > tolerance:
                grey[column] = False
    return width, [column for column, value in enumerate(grey) if value]


def fitted_picture(width: int, height: int, native: tuple[int, int]) -> tuple[float, float, float, float]:
    """The rectangle (x0, y0, x1, y1) a native-aspect picture fills when fitted, centred, into the output."""
    scale = min(width / native[0], height / native[1])
    picture_width, picture_height = native[0] * scale, native[1] * scale
    x0, y0 = (width - picture_width) / 2, (height - picture_height) / 2
    return x0, y0, x0 + picture_width, y0 + picture_height


def black_outside_picture(path: Path, native: tuple[int, int], margin: int = 2,
                          tolerance: int = 0) -> tuple[int, int]:
    """Pixels outside the fitted native-aspect picture, and how many of them are not black.

    A movie frame is shown whole at its own proportions with black bars around it. Pixels within `margin` of
    the picture's edge are not judged: presentation filtering may blend the edge.
    """
    width, height, rows = read_bmp(path)
    x0, y0, x1, y1 = fitted_picture(width, height, native)
    outside = 0
    lit = 0
    for y, row in enumerate(rows):
        for x in range(width):
            near = x0 - margin <= x < x1 + margin and y0 - margin <= y < y1 + margin
            if near:
                continue
            outside += 1
            if max(row[3 * x:3 * x + 3]) > tolerance:
                lit += 1
    return outside, lit


def compare_captures(a: Path, b: Path) -> tuple[int, tuple[int, int, int, int] | None]:
    """Number of differing pixels between two captures and their bounding box (x0, y0, x1, y1)."""
    width_a, height_a, rows_a = read_bmp(a)
    width_b, height_b, rows_b = read_bmp(b)
    if (width_a, height_a) != (width_b, height_b):
        raise ValueError(f"capture sizes differ: {width_a}x{height_a} and {width_b}x{height_b}")
    count = 0
    box: tuple[int, int, int, int] | None = None
    for y, (row_a, row_b) in enumerate(zip(rows_a, rows_b)):
        if row_a == row_b:
            continue
        for x in range(width_a):
            if row_a[3 * x:3 * x + 3] != row_b[3 * x:3 * x + 3]:
                count += 1
                box = (x, y, x, y) if box is None else (min(box[0], x), min(box[1], y), max(box[2], x),
                                                        max(box[3], y))
    return count, box


def merge_settings(target: dict, overrides: dict) -> dict:
    """Deep-merge a scenario's settings overrides into a configuration."""
    for key, value in overrides.items():
        if isinstance(value, dict) and isinstance(target.get(key), dict):
            merge_settings(target[key], value)
        else:
            target[key] = value
    return target


# Gameplay timing of a scenario: interpolated is the default product mode, original30 the Original30 frame rate
# (native 30, no interpolation). Fixed steps make captures deterministic; realtime is the interpolated mode on the
# wall clock, as a player runs it (pacing, never reference captures).
SCENARIO_TIMING = {
    "interpolated": ["--gameplay-timing", "native30_interpolated", "--presentation-rate", "60",
                     "--fixed-delta-seconds", "0.016666666666666666"],
    "original30": ["--gameplay-timing", "native30_no_interpolation", "--presentation-rate", "30",
                   "--fixed-delta-seconds", "0.033333333333333333"],
    "realtime": ["--gameplay-timing", "native30_interpolated", "--presentation-rate", "60"],
}

# Item inputs given relative to the repository (an input timeline of the checkout, for example).
SCENARIO_PATH_KEYS = ("checkpoint", "timeline", "config", "topscreen_config", "save_data_seed")


@contextlib.contextmanager
def gpu_lock(path: str | None, owner: str):
    """Serialise GPU runs with other agents through an atomic lock directory (mkdir), if one is configured."""
    if not path:
        yield
        return
    lock = Path(path)
    while True:
        try:
            lock.mkdir()
            break
        except FileExistsError:
            time.sleep(20)
    try:
        (lock / "owner").write_text(owner, encoding="utf-8")
        yield
    finally:
        shutil.rmtree(lock, ignore_errors=True)


def stage_executable(executable: Path, directory: Path) -> Path:
    """Hard-link (or copy) the executable as TriAevum.exe beside its DLLs: captures reproduce only under that name."""
    directory.mkdir(parents=True)
    for source, name in [(executable, "TriAevum.exe")] + [(dll, dll.name) for dll in executable.parent.glob("*.dll")]:
        try:
            os.link(source, directory / name)
        except OSError:
            shutil.copy2(source, directory / name)
    return directory / "TriAevum.exe"


def scenario_command(scenarios: dict, item: dict, run_dir: Path, save_dir: Path | None = None) -> list[str]:
    timing = item.get("timing", "interpolated")
    command = list(scenarios.get("arguments", [])) + SCENARIO_TIMING[timing] + [
        "--config", (run_dir / "config.json").as_posix(),
        "--topscreen-config", (run_dir / "topscreen-config").as_posix(),
        "--save-data", (save_dir or run_dir / "savedata").as_posix(),
        "--output", (run_dir / "runtime.json").as_posix(),
        "--frames", str(item["frames"]), "--max-seconds", str(item.get("max_seconds", 600)),
        "--screenshot", (run_dir / "framebuffer.bmp").as_posix(),
        "--screenshot-start-frame", str(item["shot_start"]),
        "--screenshot-interval", str(item.get("shot_interval", 1000)), "--screenshot-sequence",
    ]
    if item.get("checkpoint"):
        command += ["--load-state", Path(item["checkpoint"]).as_posix()]
    if item.get("timeline"):
        command += ["--input-timeline", Path(item["timeline"]).as_posix()]
    # Further runtime arguments of the item ({run} is its run directory), for example a savestate to write.
    command += [str(argument).replace("{run}", run_dir.as_posix()) for argument in item.get("arguments", [])]
    return command


def resolve_scenario_paths(item: dict, repository: Path) -> dict:
    """The item with its relative inputs (SCENARIO_PATH_KEYS) made relative to the repository."""
    return dict(item, **{key: str(repository / item[key]) for key in SCENARIO_PATH_KEYS
                         if item.get(key) and not Path(item[key]).is_absolute()})


def scenario_passes(item: dict) -> list[tuple[str, dict]]:
    """The runs of one item and size: `cache_passes` names ("cold", then "warm" from the cache the cold run left)
    or objects {"name": ..., <item keys>} whose keys replace the item's for that run."""
    passes = []
    for entry in item.get("cache_passes", ["cold"]):
        if isinstance(entry, str):
            passes.append((entry, item))
        else:
            passes.append((entry["name"], dict(item, **{key: value for key, value in entry.items() if key != "name"})))
    return passes


# Per-frame renderer counters a scenario can bound: `<stem>_max` bounds every frame, `<stem>_min_total` the sum
# over the run's diagnostic frames.
FRAME_COUNTERS = {
    "screen_content": "screen_content_draw_count",
    "whole_target_ui": "whole_target_ui_draw_count",
    "own_picture": "own_picture_draw_count",
    "picture_bars": "picture_bar_rect_count",
    "picture_overlay": "picture_overlay_draw_count",
    "dlss_upscale": "dlss_upscale_pass_count",
    "fsr_upscale": "fsr_upscale_pass_count",
    "native_ui_overlay": "native_ui_overlay_count",
    "taa": "temporal_aa_pass_count",
    "smaa": "smaa_1x_pass_count",
    "before_ui_aa": "before_ui_anti_aliasing_count",
}


def select_scenarios(items: list[dict], names: str) -> list[dict]:
    """The scenario items to run: all but the optional ones (real-time measurements that need a quiet host), or
    those named (comma-separated), in configuration order."""
    wanted = [name.strip() for name in names.split(",") if name.strip()]
    if not wanted:
        return [item for item in items if not item.get("optional")]
    unknown = [name for name in wanted if name not in {item["name"] for item in items}]
    if unknown:
        raise ValueError(f"unknown scenarios: {', '.join(unknown)}")
    return [item for item in items if item["name"] in wanted]


def counter_bound(key: str) -> tuple[str, bool] | None:
    """The diagnostic counter an expectation key bounds, and whether it is a per-frame maximum."""
    for suffix, maximum in (("_max", True), ("_min_total", False)):
        if key.endswith(suffix) and key[:-len(suffix)] in FRAME_COUNTERS:
            return FRAME_COUNTERS[key[:-len(suffix)]], maximum
    return None


def log_metric(text: str, tag: str, name: str):
    """A field of the last `TAG key=value ...` (or `TAG {json}`) line a run printed, or None."""
    lines = [line[len(tag):].strip() for line in text.splitlines() if line.startswith(tag + " ")]
    if not lines:
        return None
    if lines[-1].startswith("{"):
        value = json.loads(lines[-1])
        for part in name.split("."):
            value = value.get(part) if isinstance(value, dict) else None
        return value
    for token in lines[-1].split():
        key, _, value = token.partition("=")
        if key == name:
            try:
                return float(value)
            except ValueError:
                return value
    return None


@functools.lru_cache(maxsize=8)
def _lighting_metrics(run_dir: str, stamps: tuple[int, int]) -> dict:
    """The scene lighting metrics of a run, analysed once per version of its outputs."""
    try:
        from .analyze_scene_lighting import scene_lighting_metrics
    except ImportError:
        from analyze_scene_lighting import scene_lighting_metrics
    return scene_lighting_metrics(Path(run_dir))


def run_metric(run_dir: Path, source: str):
    """A metric of one run: `log:TAG.field` from its launch log, `runtime:a.b` from its runtime report,
    `lighting:metric` from analyze_scene_lighting (light continuity and shadow keyframes)."""
    kind, _, path = source.partition(":")
    if kind == "lighting":
        stamps = tuple((run_dir / name).stat().st_mtime_ns if (run_dir / name).exists() else 0
                       for name in ("launch.log", "gpu.json"))
        return _lighting_metrics(str(run_dir), stamps).get(path)
    if kind == "log":
        tag, _, name = path.partition(".")
        log = run_dir / "launch.log"
        return log_metric(log.read_text(errors="replace"), tag, name) if log.exists() else None
    if kind == "runtime":
        report = run_dir / "runtime.json"
        value = json.loads(report.read_text(encoding="utf-8")) if report.exists() else None
        for part in path.split("."):
            value = value.get(part) if isinstance(value, dict) else None
        return value
    raise ValueError(f"unknown metric source: {source}")


def metric_checks(name: str, run_dir: Path, metrics: dict) -> list[Check]:
    """One check per metric bound: {"log:TAG.field" or "runtime:path": {"min": a, "max": b, "equals": c}}."""
    checks = []
    for source, bound in metrics.items():
        value = run_metric(run_dir, source)
        numeric = isinstance(value, (int, float)) and not isinstance(value, bool)
        ok = value is not None
        if "equals" in bound:
            ok = ok and value == bound["equals"]
        if "min" in bound:
            ok = ok and numeric and value >= bound["min"]
        if "max" in bound:
            ok = ok and numeric and value <= bound["max"]
        limits = ", ".join(f"{key} {bound[key]}" for key in ("min", "max", "equals") if key in bound)
        checks.append(Check("scenarios", f"{name}: {source} ({limits})", "pass" if ok else "fail",
                            "missing" if value is None else f"observed {value}"))
    return checks


def scenario_expectation(item: dict, cache_pass: str) -> dict:
    """The expectations of one cache pass: the item's, then those of `expect_<pass>`, metrics merged."""
    expect = dict(item.get("expect", {}))
    extra = item.get(f"expect_{cache_pass}", {})
    metrics = {**expect.get("metrics", {}), **extra.get("metrics", {})}
    expect.update(extra)
    if metrics:
        expect["metrics"] = metrics
    return expect


def evaluate_scenario(name: str, run_dir: Path, code: int | str, expect: dict, references: Path | None,
                      seconds: float, log: str) -> list[Check]:
    """Judge one scenario run; every expectation becomes one check."""
    checks = [Check("scenarios", f"{name}: exit", "pass" if code == expect.get("exit", 0) else "fail",
                    f"exit {code}", seconds, log)]
    captures = sorted(run_dir.glob("framebuffer_*.bmp"))
    if "captures" in expect:
        checks.append(Check("scenarios", f"{name}: captures",
                            "pass" if len(captures) == expect["captures"] else "fail",
                            f"{len(captures)} of {expect['captures']}: " + ", ".join(c.name for c in captures)))
    for capture in expect.get("full_width_desaturation", []):
        path = run_dir / capture
        if not path.exists():
            checks.append(Check("scenarios", f"{name}: {capture} full-width effect", "fail", "capture missing"))
            continue
        width, grey = desaturated_columns(path)
        span = f"{grey[0]}..{grey[-1]} ({len(grey)} columns)" if grey else "none"
        checks.append(Check("scenarios", f"{name}: {capture} full-width effect",
                            "pass" if len(grey) == width else "fail", f"grey columns {span} of {width}"))
    native = tuple(expect.get("picture_native", (400, 240)))
    for capture in expect.get("black_bars", []):
        path = run_dir / capture
        if not path.exists():
            checks.append(Check("scenarios", f"{name}: {capture} black bars", "fail", "capture missing"))
            continue
        outside, lit = black_outside_picture(path, native)
        checks.append(Check("scenarios", f"{name}: {capture} black bars", "pass" if lit == 0 else "fail",
                            f"{lit} of {outside} pixels outside the fitted picture are not black"))
    diagnostics = run_dir / "gpu.json"
    bounds = [(key, expect[key], counter_bound(key)) for key in expect if counter_bound(key)]
    if bounds:
        frames = json.loads(diagnostics.read_text(encoding="utf-8")).get("frames", []) if diagnostics.exists() else []
        for key, bound, (counter, maximum) in bounds:
            values = [frame.get(counter, 0) for frame in frames]
            observed = max(values, default=0) if maximum else sum(values)
            ok = bool(frames) and (observed <= bound if maximum else observed >= bound)
            checks.append(Check("scenarios", f"{name}: {key.replace('_', ' ')} {bound}", "pass" if ok else "fail",
                                f"{observed} over {len(frames)} diagnostic frames"))
    checks += metric_checks(name, run_dir, expect.get("metrics", {}))
    if expect.get("reference"):
        for capture in captures:
            reference = references / capture.name if references else None
            if reference is None or not reference.exists():
                checks.append(Check("scenarios", f"{name}: {capture.name} reference", "notrun",
                                    "no accepted reference"))
                continue
            differing, box = compare_captures(reference, capture)
            checks.append(Check("scenarios", f"{name}: {capture.name} reference",
                                "pass" if differing == 0 else "fail",
                                "identical" if differing == 0 else f"{differing} pixels differ in {box}"))
    return checks


def layer_scenarios(config: dict, out: Path, report: Report, only: str = "") -> None:
    scenarios = config.get("scenarios")
    if not scenarios:
        report.add(Check("scenarios", "configuration", "notrun", "no scenarios configured"))
        return
    executable = Path(scenarios["executable"]).resolve()
    references_root = Path(scenarios["references"]) if scenarios.get("references") else None
    repository = Path(config["repository"])
    for item in select_scenarios(scenarios.get("items", []), only):
        # Passes: each run starts from an empty renderer cache unless the item asks for passes ("cold", then
        # "warm" from the cache the cold run left); a pass object replaces item keys for its run. With
        # share_save_data the passes also continue on one save directory (save, then restart and load).
        passes = [(pass_name, resolve_scenario_paths(pass_item, repository))
                  for pass_name, pass_item in scenario_passes(item)]
        distinct_passes = any(not isinstance(entry, str) for entry in item.get("cache_passes", []))
        for size in item.get("sizes", ["1280x720"]):
            width, height = (int(value) for value in size.split("x"))
            size_dir = out / "scenarios" / item["name"] / size
            if size_dir.exists():
                shutil.rmtree(size_dir)
            cache = size_dir / "cache"
            shared_save = size_dir / "savedata" if item.get("share_save_data") else None
            first_captures: list[Path] = []
            for cache_pass, run_item in passes:
                name = f"{item['name']} {size}" + (f" {cache_pass}" if len(passes) > 1 else "")
                run_dir = size_dir / cache_pass if len(passes) > 1 else size_dir
                run_dir.mkdir(parents=True)
                settings = json.loads(Path(run_item.get("config", scenarios["config"])).read_text(
                    encoding="utf-8-sig"))
                graphics = settings.setdefault("Graphics", {})
                graphics.setdefault("Output", {}).update(Width=width, Height=height)
                settings.setdefault("Window", {}).update(Width=width, Height=height)
                if run_item.get("timing") == "original30":
                    graphics.setdefault("FrameRate", {})["Mode"] = "Original30"
                merge_settings(settings, run_item.get("settings", {}))
                (run_dir / "config.json").write_text(json.dumps(settings, indent=2), encoding="utf-8")
                shutil.copyfile(run_item.get("topscreen_config", scenarios["topscreen_config"]),
                                run_dir / "topscreen-config")
                save_dir = shared_save or run_dir / "savedata"
                if not save_dir.exists():
                    shutil.copytree(run_item.get("save_data_seed", scenarios["save_data_seed"]), save_dir)
                staged = stage_executable(executable, run_dir / "bin")
                environment = os.environ.copy()
                # Diagnostic switches a scenario reads (for example TRIAEVUM_SCENE_LIGHT_TRACE).
                environment.update({key: str(value) for key, value in run_item.get("env", {}).items()})
                environment.update({
                    "TRIAEVUM_RENDERER_CACHE_DIR": str(cache),
                    "OOT3D_VULKAN_DIAGNOSTICS_PATH": str(run_dir / "gpu.json"),
                    "OOT3D_VULKAN_DIAGNOSTICS_MAX_FRAMES": str(run_item.get("diagnostic_frames", run_item["frames"])),
                })
                log = run_dir / "launch.log"
                with gpu_lock(scenarios.get("gpu_lock"), f"qualify_windows_battery {name}"):
                    wait_idle(config.get("wait_idle_processes", []))
                    code, seconds = run_logged(
                        [str(staged)] + scenario_command(scenarios, run_item, run_dir, save_dir), log,
                        cwd=Path(scenarios["working_directory"]),
                        timeout=run_item.get("max_seconds", 600) + 120, env=environment)
                shutil.rmtree(run_dir / "bin", ignore_errors=True)
                if shared_save is None:
                    shutil.rmtree(save_dir, ignore_errors=True)
                references = references_root / item["name"] / size if references_root else None
                if references and distinct_passes:
                    references = references / cache_pass
                for check in evaluate_scenario(name, run_dir, code, scenario_expectation(run_item, cache_pass),
                                               references, seconds, str(log)):
                    report.add(check)
                captures = sorted(run_dir.glob("framebuffer_*.bmp"))
                if not first_captures:
                    first_captures = captures
                elif item.get("identical_passes"):
                    # A prepared cache changes when programs are ready, never the images.
                    for capture in first_captures:
                        other = run_dir / capture.name
                        if not other.exists():
                            report.add(Check("scenarios", f"{name}: {capture.name} as {passes[0][0]}", "fail",
                                             "capture missing"))
                            continue
                        differing, box = compare_captures(capture, other)
                        report.add(Check("scenarios", f"{name}: {capture.name} as {passes[0][0]}",
                                         "pass" if differing == 0 else "fail",
                                         "identical" if differing == 0 else f"{differing} pixels differ in {box}"))
            shutil.rmtree(cache, ignore_errors=True)
            if shared_save is not None:
                shutil.rmtree(shared_save, ignore_errors=True)


def write_report(report: Report, out: Path) -> tuple[Path, Path]:
    data = asdict(report)
    totals: dict[str, dict[str, int]] = {}
    for check in report.checks:
        totals.setdefault(check.layer, {}).setdefault(check.status, 0)
        totals[check.layer][check.status] += 1
    data["totals"] = totals
    json_path = out / "battery-report.json"
    json_path.write_text(json.dumps(data, indent=2), encoding="utf-8")
    lines = [f"# Windows battery report", "", f"Started {report.started}; configuration `{report.configuration}`.", ""]
    for layer, counts in totals.items():
        lines.append(f"- {layer}: " + ", ".join(f"{key} {value}" for key, value in sorted(counts.items())))
    lines += ["", "| Layer | Check | Status | Detail |", "| --- | --- | --- | --- |"]
    for check in report.checks:
        if check.status != "pass" or check.name.endswith(": ctest") or check.layer in ("build", "scenarios"):
            detail = check.detail.replace("|", "/").replace("\n", " ")[:240]
            lines.append(f"| {check.layer} | {check.name} | {check.status} | {detail} |")
    md_path = out / "battery-report.md"
    md_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return json_path, md_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--layers", default=",".join(LAYERS))
    parser.add_argument("--scenarios", default="",
                        help="comma-separated scenario names to run in the scenarios layer (default: all)")
    args = parser.parse_args()
    config = json.loads(args.config.read_text(encoding="utf-8"))
    layers = [layer.strip() for layer in args.layers.split(",") if layer.strip()]
    unknown = [layer for layer in layers if layer not in LAYERS]
    if unknown:
        parser.error(f"unknown layers: {', '.join(unknown)} (known: {', '.join(LAYERS)})")
    try:
        select_scenarios(config.get("scenarios", {}).get("items", []), args.scenarios)
    except ValueError as error:
        parser.error(str(error))
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = Report(started=time.strftime("%Y-%m-%d %H:%M:%S"), configuration=str(args.config))
    if "build" in layers:
        layer_build(config, out, report)
    if "unit" in layers:
        layer_unit(config, out, report)
    if "scenarios" in layers:
        layer_scenarios(config, out, report, args.scenarios)
    json_path, md_path = write_report(report, out)
    failing = [check for check in report.checks if check.status in ("fail", "notrun")]
    print(f"battery: {len(report.checks)} checks, {len(failing)} failing; report {md_path}")
    return 1 if failing else 0


if __name__ == "__main__":
    sys.exit(main())
