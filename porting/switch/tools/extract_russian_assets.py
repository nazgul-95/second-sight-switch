"""Extract and verify the user's Russian Xbox asset tree from the inventoried ISO."""
import csv
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PROJECT = ROOT / "porting/switch"
ISO = ROOT / "external_versions/xbox_eng_rus/Second Sight_ENG-RUStext.iso"
DEST = PROJECT / "switch_sd/switch/secondsight/game/r"
rows = list(csv.DictReader((ROOT / "second_sight_xbox_xdvdfs_inventory.csv").open(encoding="utf-8-sig")))
files = [r for r in rows if r["kind"] == "file" and r["path"].startswith("r/")]
by_path = {r["path"]: r for r in rows}
if by_path["r/ss_ntsc.xbe"]["sha256"] != by_path["e/ss_ntsc.xbe"]["sha256"]:
    raise ValueError("Russian executable differs; asset-only selection is unsupported")
DEST.mkdir(parents=True, exist_ok=True)
marker = DEST / "russian-assets.ready"
marker.unlink(missing_ok=True)
total = 0
with ISO.open("rb") as source:
    for row in files:
        relative = Path(row["path"][2:])
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("Unsafe member path")
        target = DEST / relative
        size = int(row["size"])
        expected = row["sha256"]
        if target.exists() and target.stat().st_size == size and hashlib.sha256(target.read_bytes()).hexdigest() == expected:
            total += size
            continue
        source.seek(int(row["offset"]))
        data = source.read(size)
        if len(data) != size or hashlib.sha256(data).hexdigest() != expected:
            raise ValueError("ISO member hash mismatch: " + row["path"])
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_name(target.name + ".extracting")
        temporary.write_bytes(data)
        temporary.replace(target)
        total += size
marker.write_text(f"Verified {len(files)} files, {total} bytes against XDVDFS SHA256 inventory.\n", encoding="utf-8")
print(f"Russian assets ready: {len(files)} files, {total} bytes: {DEST}")

