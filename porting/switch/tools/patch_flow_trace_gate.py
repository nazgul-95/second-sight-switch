"""Gate known generated bring-up traces; preserve all guest instructions.

Idempotent post-generation patch. Default mode checks whether application is
needed. --apply writes only changed files. Does not regenerate game bodies.
"""
import argparse
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
GUARD = "#ifndef SECOND_SIGHT_FLOW_TRACE\n#define SECOND_SIGHT_FLOW_TRACE 0\n#endif\n"


def gated(text):
    replacements = {
        "if ((state_trace_frames % 60u) == 1u)":
            "if (SECOND_SIGHT_FLOW_TRACE && (state_trace_frames % 60u) == 1u)",
        "if ((frame_diag_iterations % 60u) == 1u)":
            "if (SECOND_SIGHT_FLOW_TRACE && (frame_diag_iterations % 60u) == 1u)",
        "int flow_trace = (flow_trace_frame == 1u || (flow_trace_frame % 60u) == 1u);":
            "int flow_trace = SECOND_SIGHT_FLOW_TRACE && (flow_trace_frame == 1u || (flow_trace_frame % 60u) == 1u);",
    }
    result = text
    for before, after in replacements.items():
        result = result.replace(before, after)
    if "SECOND_SIGHT_FLOW_TRACE" in result and "#ifndef SECOND_SIGHT_FLOW_TRACE" not in result:
        result = GUARD + "\n" + result
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated", type=Path, default=PROJECT / "generated_boot")
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    changed = []
    for path in sorted(args.generated.glob("recomp_*.c")):
        text = path.read_text(encoding="utf-8-sig")
        candidate = gated(text)
        if candidate != text:
            changed.append(path.name)
            if args.apply:
                path.write_text(candidate, encoding="utf-8", newline="\n")
    print(("Applied" if args.apply else "Pending") + ": " + ", ".join(changed))
    return int(bool(changed) and not args.apply)


if __name__ == "__main__":
    raise SystemExit(main())

