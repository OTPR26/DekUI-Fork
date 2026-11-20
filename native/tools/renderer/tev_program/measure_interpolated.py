"""ABBA throughput of the product presentation mode (native30 interpolated).

--throughput-benchmark forces one presentation per guest refresh, so it
cannot measure interpolated presentation. This driver launches the host
unpaced with a fixed host delta of one presentation interval (two host frames
per 30 Hz guest refresh at 60 Hz), no-present, and compares milliseconds per
presented frame. Each run also records the interpolation and guest contract;
the arms must agree on it exactly. Developer diagnostic, not a release tool.
"""
import argparse
import json
import os
import shutil
import statistics
import subprocess
from pathlib import Path

CONTRACT_FIELDS = ("frames", "guest_refresh_frames", "memory_content_fingerprint",
                   "memory_state_fingerprint", "process_state_fingerprint",
                   "draws_submitted", "display_transfers_submitted")
DROP_WITH_VALUE = {"--frames", "--max-seconds", "--output", "--renderer-cache-directory",
                   "--benchmark-warmup-frames", "--gameplay-timing", "--presentation-rate",
                   "--fixed-delta-seconds", "--title-plugin", "--config",
                   "--topscreen-config", "--save-data"}
DROP_FLAG = {"--throughput-benchmark", "--profile-a32-runtime"}


def foreground_pid() -> int | None:
    """Process id owning the current foreground window (Windows only)."""
    if os.name != "nt":
        return None
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    hwnd = user32.GetForegroundWindow()
    if not hwnd:
        return None
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def contract(data: dict) -> dict:
    result = {key: data.get(key) for key in CONTRACT_FIELDS}
    result["visual_interpolation"] = {
        k: v for k, v in data.get("visual_interpolation", {}).items()
        if not isinstance(v, (dict, list)) and "seconds" not in k}
    frame_rate = data.get("frame_rate", {})
    result["frame_rate"] = {k: v for k, v in frame_rate.items()
                            if not isinstance(v, (dict, list))}
    dsp = data.get("dsp_audio", {})
    result["pcm"] = {k: v for k, v in dsp.items() if "fnv" in k or "fingerprint" in k}
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--invocation", type=Path, required=True,
                   help="template command.json (argument list) of a throughput run")
    p.add_argument("--executable", type=Path, required=True)
    p.add_argument("--comparison-executable", type=Path)
    p.add_argument("--baseline-plugin", type=Path, required=True)
    p.add_argument("--comparison-plugin", type=Path)
    p.add_argument("--comparison-env", action="append", default=[],
                   help="NAME=VALUE set only in comparison runs")
    p.add_argument("--comparison-window", choices=("normal", "background", "minimized"), default="normal",
                   help="start comparison runs without activation: background (visible, same size) or minimized")
    p.add_argument("--frame-rate-mode", default="Interpolated2x")
    p.add_argument("--presentation-rate", type=int, default=60)
    p.add_argument("--frames", type=int, default=1800)
    p.add_argument("--warmup-frames", type=int, default=360)
    p.add_argument("--runs", type=int, default=8)
    p.add_argument("--timeout-seconds", type=int, default=300)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()

    template = json.loads(args.invocation.read_text())
    config_source = Path(template[template.index("--config") + 1])
    topscreen_source = Path(template[template.index("--topscreen-config") + 1])
    save_source = Path(template[template.index("--save-data") + 1])
    args.output.mkdir(parents=True, exist_ok=False)
    cache = args.output / "cache"
    runs = []
    for run in range(args.runs):
        comparison = run % 4 in (1, 2)
        root = args.output / str(run)
        root.mkdir()
        config = json.loads(config_source.read_text())
        config["Graphics"]["FrameRate"]["Mode"] = args.frame_rate_mode
        (root / "config").write_text(json.dumps(config, indent=2))
        shutil.copy2(topscreen_source, root / "topscreen-config")
        shutil.copytree(save_source, root / "save-data")
        exe = args.comparison_executable if comparison and args.comparison_executable else args.executable
        plugin = args.comparison_plugin if comparison and args.comparison_plugin else args.baseline_plugin
        command = [str(exe.resolve())]
        rest = template[1:]
        i = 0
        while i < len(rest):
            if rest[i] in DROP_WITH_VALUE:
                i += 2
            elif rest[i] in DROP_FLAG:
                i += 1
            else:
                command.append(rest[i])
                i += 1
        command += ["--title-plugin", str(plugin.resolve()), "--config", str(root / "config"),
                    "--topscreen-config", str(root / "topscreen-config"),
                    "--save-data", str(root / "save-data"),
                    "--gameplay-timing", "native30_interpolated",
                    "--presentation-rate", str(args.presentation_rate),
                    "--fixed-delta-seconds", repr(1.0 / args.presentation_rate),
                    "--benchmark-warmup-frames", str(args.warmup_frames),
                    "--frames", str(args.frames + args.warmup_frames),
                    "--max-seconds", str(args.timeout_seconds - 30),
                    "--output", str(root / "runtime.json"),
                    "--renderer-cache-directory", str(cache.resolve())]
        (root / "command.json").write_text(json.dumps(command, indent=2))
        env = dict(os.environ, DISABLE_VULKAN_OBS_CAPTURE="1", TRIAEVUM_BENCHMARK_NO_PRESENT="1")
        if comparison:
            env.update(item.split("=", 1) for item in args.comparison_env)
        startup = None
        show = {"minimized": 7, "background": 4}.get(args.comparison_window) if comparison else None
        if show is not None:
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = show  # SW_SHOWMINNOACTIVE / SW_SHOWNOACTIVATE
        with (root / "stdout.log").open("w") as out, (root / "stderr.log").open("w") as err:
            process = subprocess.Popen(command, cwd=root, env=env, stdout=out, stderr=err,
                                       startupinfo=startup)
            foreground_samples = []
            try:
                while process.poll() is None:
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        foreground_samples.append(foreground_pid() == process.pid)
                code = process.returncode
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
        if code:
            raise RuntimeError(f"run {run} failed with {code}: {root}")
        data = json.loads((root / "runtime.json").read_text())
        window = data["benchmark_window"]
        record = {"run": run, "comparison": comparison,
                  "ms_per_presentation": 1000.0 / window["frames_per_second"],
                  "measured_frames": window["measured_frames"],
                  # Share of 2 s samples in which the game window was the
                  # foreground window: focus changes the measured speed.
                  "foreground_share": (sum(foreground_samples) / len(foreground_samples)
                                       if foreground_samples else None),
                  "contract": contract(data)}
        runs.append(record)
        print(json.dumps({k: record[k] for k in ("run", "comparison", "ms_per_presentation",
                                                  "foreground_share")}), flush=True)

    ratios = []
    for base in range(0, len(runs) - 3, 4):
        a = [runs[base]["ms_per_presentation"], runs[base + 3]["ms_per_presentation"]]
        b = [runs[base + 1]["ms_per_presentation"], runs[base + 2]["ms_per_presentation"]]
        ratios += [b[0] / a[0], b[1] / a[1]]
    baseline_contracts = [r["contract"] for r in runs if not r["comparison"]]
    comparison_contracts = [r["contract"] for r in runs if r["comparison"]]
    summary = {
        "frame_rate_mode": args.frame_rate_mode,
        "baseline_ms_median": statistics.median(r["ms_per_presentation"] for r in runs if not r["comparison"]),
        "comparison_ms_median": statistics.median(r["ms_per_presentation"] for r in runs if r["comparison"]),
        "paired_ratios": ratios,
        "paired_median_ratio": statistics.median(ratios) if ratios else None,
        "baseline_runs_agree": all(c == baseline_contracts[0] for c in baseline_contracts),
        "arms_agree": all(c == baseline_contracts[0] for c in comparison_contracts),
        "contract": baseline_contracts[0],
    }
    (args.output / "measurements.json").write_text(json.dumps(runs, indent=1))
    (args.output / "summary.json").write_text(json.dumps(summary, indent=1))
    print(json.dumps({k: summary[k] for k in summary if k != "contract"}, indent=1))


if __name__ == "__main__":
    main()
