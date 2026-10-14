#!/usr/bin/env python3
"""Summarize opt-in software timing; does not measure game response or scanout."""
import argparse
import json
import math
import re
from collections import defaultdict


def summarize(text):
    sessions = defaultdict(lambda: defaultdict(list))
    for line in text.splitlines():
        match = re.search(r"MM3D input timing (key|hid|frame) (.*)", line)
        if not match:
            continue
        values = dict(re.findall(r"(\w+)=([^ ]+)", match[2]))
        required = {
            "key": {"session", "key_id", "t", "code", "pressed"},
            "hid": {"session", "hid_id", "t", "latest_key", "buttons", "changed"},
            "frame": {"session", "frame", "begin", "queued", "copy", "acquired", "returned", "key_at_begin", "hid_at_begin"},
        }
        if not required[match[1]].issubset(values):
            continue  # A live or size-limited log can end in a partial record.
        for name in list(values):
            if name not in ("buttons", "changed", "pressed"):
                values[name] = int(values[name])
        sessions[values["session"]][match[1]].append(values)
    result = {}
    for session, records in sessions.items():
        measurements = defaultdict(list)
        keys = {k["key_id"]: k for k in records["key"]}
        for event in records["hid"]:
            key = keys.get(event["latest_key"])
            if key and 0 <= event["t"] - key["t"] <= 250000:
                # Candidate pairing only; a controller can change HID while a
                # keyboard event is recent. Inspect button masks/test procedure.
                measurements["candidate_key_to_hid_ms"].append((event["t"] - key["t"]) / 1000)
        for frame in records["frame"]:
            for label, start, end in (
                ("frame_prepare_ms", "begin", "queued"),
                ("worker_and_present_queue_ms", "queued", "copy"),
                ("swapchain_acquire_ms", "copy", "acquired"),
                ("record_submit_present_call_ms", "acquired", "returned"),
                ("frame_begin_to_present_return_ms", "begin", "returned"),
            ):
                if frame[end] >= frame[start]:
                    measurements[label].append((frame[end] - frame[start]) / 1000)
        summary = {"keys": len(records["key"]), "hid_edges": len(records["hid"]),
                   "frames": len(records["frame"])}
        for label, values in measurements.items():
            values.sort()
            summary[label] = {"n": len(values), "p50": values[(len(values) - 1) // 2],
                              "p95": values[math.ceil(len(values) * .95) - 1], "max": values[-1]}
        result[session] = summary
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log")
    args = parser.parse_args()
    with open(args.log) as source:
        print(json.dumps(summarize(source.read()), indent=2))
    print("Key/HID pairs are candidates. Frame timing is not input-to-photon latency;")
    print("the first rendered frame tagged with input need not contain the game's response.")
