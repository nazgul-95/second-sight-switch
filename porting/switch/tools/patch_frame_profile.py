"""Idempotently insert host observations at pinned NTSC loop boundaries."""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = '#include "../src/frame_profile.h"\n'
MARKERS = {
    "loc_00014690: ;": "second_sight_frame_begin(MEM32(0x62DFBC), MEM32(0x3856F4) + 1u);",
    "loc_000146CC: ;": "second_sight_frame_phase(1);",
    "loc_000146D1: ;": "second_sight_frame_phase(2);",
    "loc_000146ED: ;": "second_sight_frame_phase(3);",
    "loc_000146F2: ;": "second_sight_frame_phase(4);",
    "loc_00014728: ;": "second_sight_frame_phase(5);",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    path = ROOT / "generated_boot/recomp_0000.c"
    text = path.read_text(encoding="utf-8-sig")
    result = text
    for label, call in MARKERS.items():
        if result.count(label) != 1:
            raise ValueError("Ambiguous or missing pinned boundary: " + label)
        old = label + "\n"
        new = old + "    " + call + "\n"
        if new not in result:
            result = result.replace(old, new)
    if INCLUDE not in result:
        result = INCLUDE + result
    changed = result != text
    if changed and args.apply:
        path.write_text(result, encoding="utf-8", newline="\n")
    print(("Applied" if args.apply else "Pending") + ": " + (path.name if changed else ""))
    return int(changed and not args.apply)


if __name__ == "__main__":
    raise SystemExit(main())

