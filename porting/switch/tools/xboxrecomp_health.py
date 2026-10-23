"""Read existing title artifacts/log with upstream doctor and seed admission.

Does not run the game, regenerate production code, edit seeds or execute tests.
Normalizes our console ICALL spelling for upstream seed_from_log's scanner.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT.parents[1] / "external_versions/xboxrecomp"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="New or existing report directory")
    args = parser.parse_args()
    log = args.log.resolve()
    destination = args.output.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    analysis = ROOT / "analysis_boot"
    doctor = [sys.executable, "-m", "tools.doctor",
              "--functions", str(analysis / "functions_repaired.json"),
              "--identified", str(analysis / "func_id/identified_functions.json"),
              "--abi", str(analysis / "abi/abi_functions.json"),
              "--recomp", str(analysis / "recomp/summary.json"),
              "--runtime-log", str(log), "--json", str(destination / "doctor.json")]
    result = subprocess.run(doctor, cwd=UPSTREAM, capture_output=True, text=True)
    (destination / "doctor.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"Upstream doctor failed: {result.stderr}")

    source = log.read_text(encoding="utf-8-sig", errors="replace")
    targets = sorted(set(re.findall(r"\[ICALL\] unresolved target (0x[0-9A-Fa-f]+)", source)))
    normalized = destination / "seed-input.log"
    normalized.write_text(source + "\n" + "\n".join(
        "Failed to resolve VA " + va for va in targets) + "\n", encoding="utf-8")
    seed = [sys.executable, "-m", "tools.seed_from_log", str(normalized),
            str(ROOT / "game_files/game_ntsc.xbe"),
            "--functions", str(analysis / "functions_repaired.json"),
            "--seeds", str(ROOT / "config/seed_functions.json"), "--dry-run"]
    result = subprocess.run(seed, cwd=UPSTREAM, capture_output=True, text=True)
    (destination / "seed-admission.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"Upstream seed admission failed: {result.stderr}")
    admitted = sorted(set(re.findall(r"\+ ([0-9A-Fa-f]{8})", result.stdout)))
    declarations = (ROOT / "generated_boot/recomp_funcs.h").read_text()
    dispatch = (ROOT / "generated_boot/recomp_dispatch.c").read_text()
    manual = (ROOT / "src/recomp_manual.c").read_text()
    existing = {}
    for va in admitted:
        # Upstream seed_from_log says "new" for an exact existing entry too.
        # Reconcile that suggestion with the working dispatch/manual lookup.
        existing[va] = dict(declared=f"void sub_{va.upper()}(void);" in declarations,
                            dispatched=f"0x{va.upper()}u" in dispatch,
                            manual_lookup=f"case 0x{va.upper()}u:" in manual)
    receipt = dict(runtime_log=str(log), toolkit=str(UPSTREAM),
                   normalized_unresolved_targets=targets,
                   admitted_targets_current_coverage=existing,
                   production_mutations=False, seeds_written=False,
                   limits=["Recompiler summary may predate local generated-code repairs.",
                           "No separate icall feedback file supplied.",
                           "Seed admission is a code-body heuristic, not proof of semantic correctness."])
    (destination / "receipt.json").write_text(json.dumps(receipt, indent=2)+"\n", encoding="utf-8")
    print((destination / "doctor.txt").read_text(encoding="utf-8"))
    print((destination / "seed-admission.txt").read_text(encoding="utf-8"))
    print("Current dispatch/manual coverage:", json.dumps(existing))
    print(destination)


if __name__ == "__main__":
    main()

