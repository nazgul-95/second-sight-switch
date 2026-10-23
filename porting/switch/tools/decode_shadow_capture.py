"""Render bounded SHADOW-IMAGE records received from the console log."""
import argparse
import json
import re
from pathlib import Path
from PIL import Image, ImageDraw

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("log", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
pattern = re.compile(r"\[SHADOW-IMAGE\] n=(\d+) kind=(atlas|scratch) frame=(\d+) va=([0-9A-F]+) size=64x64 encoding=rgba4444 data=([0-9A-F]{16384})(?:\s|$)")
records = {}
args.output.mkdir(parents=True, exist_ok=True)
for line in args.log.read_text(encoding="utf-8-sig", errors="replace").splitlines():
    match = pattern.search(line)
    if not match:
        continue
    n, kind, frame, va, payload = match.groups()
    image = Image.frombytes("RGBA", (64, 64), bytes(int(c, 16)*17 for c in payload))
    records[(int(n), int(frame), kind)] = (image, va)
rows = sorted({(n, frame) for n, frame, kind in records})
if not rows:
    print("No complete SHADOW-IMAGE records received yet.")
    raise SystemExit(0)
sheet = Image.new("RGB", (4*224, len(rows)*230), "#252525")
draw = ImageDraw.Draw(sheet)
metadata = []
for row, (n, frame) in enumerate(rows):
    for col, (kind, mode) in enumerate((("atlas", "rgb"), ("scratch", "rgb"), ("atlas", "alpha"), ("scratch", "alpha"))):
        item = records.get((n, frame, kind))
        if item is None:
            continue
        rgba, va = item
        rgba.save(args.output/f"{n:02d}-{frame}-{kind}.png")
        image = rgba.convert("RGB") if mode == "rgb" else rgba.getchannel("A").convert("RGB")
        draw.text((col*224+8, row*230+5), f"{n} frame {frame} {kind} {mode}", fill="white")
        sheet.paste(image.resize((192, 192), Image.Resampling.NEAREST), (col*224+8, row*230+24))
        if mode == "rgb":
            channels = rgba.split()
            metadata.append({"capture": n, "frame": frame, "kind": kind, "va": va,
                             "rgb_bbox": image.getbbox(), "alpha_bbox": channels[3].getbbox(),
                             "extrema": rgba.getextrema()})
sheet.save(args.output/"contact-sheet.png")
(args.output/"captures.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
print(f"Decoded {len(records)} images in {len(rows)} frame groups; contact sheet: {args.output/'contact-sheet.png'}")

