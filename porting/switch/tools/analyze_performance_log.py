"""Summarize existing VK-PERF windows; never interpret them as per-frame samples.

This is offline analysis. It does not start the game or change port sources.
Different files/cameras/clocks are not controlled build comparisons.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import statistics
from collections import Counter
from pathlib import Path

NUMBER = r"[-+]?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?"
PAIR = re.compile(r"([A-Za-z_][A-Za-z_0-9]*)=(" + NUMBER + r")")
PHASE = re.compile(r"draw_phase_ms\[([^]]+)\]")
TAG = re.compile(r"^\s*\[([^]]+)\]")
ROOT = Path(__file__).resolve().parents[3]


def label(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return str(path.resolve())


def numeric_pairs(text: str) -> dict:
    return {key: float(value) for key, value in PAIR.findall(text)}


def describe(rows: list[dict]) -> dict:
    if not rows:
        return {"windows": 0}
    result = {"windows": len(rows)}
    for field in ("fps", "avg_ms", "draw_calls_per_frame",
                  "draw_cpu_ms_sampled_frame", "gpu_ms_sampled_frame",
                  "unattributed_draw_elapsed_ms"):
        values = [r[field] for r in rows if field in r]
        if values:
            result[field] = {
                "min": min(values),
                "median_of_window_values": statistics.median(values),
                "max": max(values),
            }
    return result


def parse(path: Path) -> tuple[list[dict], dict]:
    raw = path.read_bytes()
    lines = raw.decode("utf-8-sig", errors="replace").splitlines()
    tags: Counter = Counter()
    rows = []
    mission = None
    max_audio_underruns = None
    identity = []
    warnings = Counter()
    for line_number, line in enumerate(lines, 1):
        match = TAG.match(line)
        if match:
            tags[match[1]] += 1
        if "[STATE]" in line or "[TEMPO]" in line:
            state = numeric_pairs(line)
            mission = int(state["mission"]) if "mission" in state else mission
        if "[boot] Second Sight recomp starting" in line or "[clock]" in line:
            identity.append({"line": line_number, "text": line})
        if "[AUDIO-STATE]" in line:
            value = numeric_pairs(line).get("underruns")
            if value is not None:
                max_audio_underruns = max(max_audio_underruns or 0, value)
        for name, needle in (
            ("ring_full_messages", "frame ring (48 MB) full"),
            ("flip_timeout_messages", "FLIP_STALL: no flip retired"),
            ("synthetic_vblank_messages", "[VBLANK-FALLBACK]"),
            ("unresolved_icall_messages", "[ICALL] unresolved"),
            ("socket_error_messages", "socket error"),
        ):
            if needle in line:
                warnings[name] += 1
        if "[VK-PERF]" not in line:
            continue
        phase_match = PHASE.search(line)
        head = line[:phase_match.start()] if phase_match else line
        row = {"source": label(path), "line": line_number,
               "mission_hint": mission, **numeric_pairs(head)}
        if phase_match:
            phases = numeric_pairs(phase_match[1])
            row.update({"phase_" + key + "_ms": val for key, val in phases.items()})
            row["named_phases_sum_ms"] = sum(phases.values())
            if "draw_cpu_ms_sampled_frame" in row:
                row["unattributed_draw_elapsed_ms"] = (
                    row["draw_cpu_ms_sampled_frame"] - row["named_phases_sum_ms"])
        rows.append(row)
    buckets = {}
    for name, lo, hi in (("draws_0_499", 0, 500), ("draws_500_899", 500, 900),
                         ("draws_900_1199", 900, 1200),
                         ("draws_1200_plus", 1200, float("inf"))):
        selected = [r for r in rows
                    if lo <= r.get("draw_calls_per_frame", -1) < hi]
        buckets[name] = describe(selected)
    summary = {
        "source": label(path), "bytes": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(), "lines": len(lines),
        "identity_and_clock_lines": identity, "tag_counts": dict(tags),
        "warning_message_counts_not_event_totals": dict(warnings),
        "max_reported_audio_underruns": max_audio_underruns,
        "all_windows": describe(rows), "draw_buckets": buckets,
    }
    return rows, summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    rows, files = [], []
    for path in args.logs:
        parsed, summary = parse(path)
        rows.extend(parsed)
        files.append(summary)
    result = {
        "kind": "offline_analysis_of_reported_window_aggregates",
        "limits": [
            "No controlled camera/checkpoint/clock match across logs.",
            "Window median values are not frame-time percentiles or 1% lows.",
            "Draw timers measure elapsed time, which may include descheduling.",
            "CPU/GPU samples may select different frames and have different scope.",
            "Do not subtract draw and GPU from frame interval to derive guest time.",
            "Mission hint is the last preceding STATE/TEMPO line; boundaries may lag.",
            "Throttled warning-line counts are not total event counts.",
        ],
        "files": files,
    }
    (args.output / "summary.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    columns = ["source", "line", "mission_hint"] + sorted(
        {key for row in rows for key in row} - {"source", "line", "mission_hint"})
    with (args.output / "windows.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)
    print(json.dumps({"files": len(files), "windows": len(rows),
                      "output": str(args.output.resolve()),
                      "per_file": [{"source": f["source"],
                                    **f["all_windows"]} for f in files]}, indent=2))


if __name__ == "__main__":
    main()

