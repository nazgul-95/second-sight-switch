"""Resolve reported PC hotspots using the exact ELF saved with a profile NRO.

Only completed histogram windows are used. Top-PC counts are a truncated,
busy-thread-biased sample, not percentages of total CPU or whole-frame time.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--windows", type=int, default=3)
    args = parser.parse_args()
    if args.windows < 1:
        parser.error("--windows must be positive")
    windows, rows, slots = [], [], {}
    for line in args.log.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        if line.startswith("[prof] slots:"):
            slots = {int(k): int(v, 16) for k, v in re.findall(r"(\d+)=([A-Fa-f0-9]+)", line)}
            rows = []
        match = re.match(r"\[prof\] hot slot=(\d+) samples=(\d+)", line)
        if match:
            slot, total = map(int, match.groups())
            for pc, count in re.findall(r"([0-9A-Fa-f]{6,8}):(\d+)", line):
                rows.append(dict(slot=slot, entry=slots.get(slot), pc=int(pc, 16),
                                 count=int(count), reported_slot_samples=total))
        if line.startswith("[prof] window samples="):
            windows.append(rows)
    selected = windows[-args.windows:]
    tool_dir = Path("C:/devkitPro/devkitA64/bin")
    # Profile PCs are offsets from _start; add the actual ELF symbol value.
    nm = subprocess.run([str(tool_dir / "aarch64-none-elf-nm.exe"), "--defined-only", str(args.elf)],
                        capture_output=True, text=True, errors="replace", check=True)
    start = re.search(r"^([0-9a-fA-F]+)\s+\w\s+_start$", nm.stdout, re.M)
    if not start:
        raise ValueError("ELF has no unambiguous _start symbol")
    base = int(start.group(1), 16)
    addresses = sorted({r[k] for w in selected for r in w for k in ("pc", "entry")
                        if r[k] is not None and (k != "entry" or r[k] != 0)})
    symbols = {}
    if addresses:
        result = subprocess.run([str(tool_dir / "aarch64-none-elf-addr2line.exe"),
                                 "-f", "-C", "-e", str(args.elf)],
                                input="".join(f"0x{base+a:X}\n" for a in addresses),
                                capture_output=True, text=True, errors="replace", check=True)
        lines = result.stdout.splitlines()
        if len(lines) != 2 * len(addresses):
            raise ValueError("Unexpected addr2line output length")
        symbols = {a: dict(function=lines[i*2], location=lines[i*2+1])
                   for i, a in enumerate(addresses)}
    for window in selected:
        for row in window:
            row["pc_symbol"] = symbols[row["pc"]]
            row["entry_symbol"] = symbols.get(row["entry"],
                                             dict(function="main guest thread (entry label 0)"))
    report = dict(log=str(args.log.resolve()), elf=str(args.elf.resolve()),
                  log_sha256=hashlib.sha256(args.log.read_bytes()).hexdigest(),
                  elf_sha256=hashlib.sha256(args.elf.read_bytes()).hexdigest(),
                  completed_windows=len(windows), selected_windows=selected,
                  limits=__doc__)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(dict(completed_windows=len(windows),
                         selected_hotspots=sum(map(len, selected)), output=str(args.output)), indent=2))


if __name__ == "__main__":
    main()

