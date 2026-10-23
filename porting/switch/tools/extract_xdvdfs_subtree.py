"""Extract one directory tree from a validated Xbox XDVDFS image manifest."""

import argparse
import csv
import hashlib
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[2]
EXPECTED_IMAGE_SHA256 = "20df35050f62009c9058168d4784f7e71ef1cbcc1ab810ea305a2f8dae07cadb"
CHUNK_SIZE = 8 * 1024 * 1024


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(CHUNK_SIZE):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--image",
        type=Path,
        default=ROOT / "external_versions/xbox_eng_rus/Second Sight_ENG-RUStext.iso",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=ROOT / "second_sight_xbox_xdvdfs_inventory.csv",
    )
    parser.add_argument(
        "--destination",
        type=Path,
        default=ROOT / "porting/switch/game_files",
    )
    parser.add_argument("--prefix", default="e/")
    args = parser.parse_args()

    image_sha256 = sha256_file(args.image)
    if image_sha256.lower() != EXPECTED_IMAGE_SHA256:
        raise SystemExit(f"Unexpected image SHA-256: {image_sha256}")

    args.destination.mkdir(parents=True, exist_ok=True)
    selected = 0
    total_bytes = 0
    with args.image.open("rb") as image, args.manifest.open(
        newline="", encoding="utf-8"
    ) as manifest:
        for row in csv.DictReader(manifest):
            source_path = row["path"]
            if source_path == "e":
                args.destination.joinpath("e").mkdir(exist_ok=True)
                continue
            if not source_path.startswith(args.prefix):
                continue

            relative = PurePosixPath(source_path)
            if relative.is_absolute() or ".." in relative.parts:
                raise SystemExit(f"Unsafe path in manifest: {source_path!r}")
            output_path = args.destination.joinpath(*relative.parts)
            if row["kind"] == "directory":
                output_path.mkdir(parents=True, exist_ok=True)
                continue

            expected_size = int(row["size"])
            expected_hash = row["sha256"].lower()
            if output_path.exists():
                if output_path.stat().st_size != expected_size or sha256_file(output_path) != expected_hash:
                    raise SystemExit(f"Existing file differs from source image: {output_path}")
                selected += 1
                total_bytes += expected_size
                continue

            output_path.parent.mkdir(parents=True, exist_ok=True)
            digest = hashlib.sha256()
            remaining = expected_size
            with output_path.open("xb") as output:
                image.seek(int(row["offset"]))
                while remaining:
                    chunk = image.read(min(CHUNK_SIZE, remaining))
                    if not chunk:
                        raise SystemExit(f"Short read for {source_path}")
                    output.write(chunk)
                    digest.update(chunk)
                    remaining -= len(chunk)
            if digest.hexdigest() != expected_hash:
                output_path.unlink(missing_ok=True)
                raise SystemExit(f"Payload hash mismatch for {source_path}")
            selected += 1
            total_bytes += expected_size
            if selected % 50 == 0:
                print(f"Extracted {selected} files ({total_bytes:,} bytes)")

    print(f"Extracted/verified {selected} files ({total_bytes:,} bytes) under {args.destination}")


if __name__ == "__main__":
    main()

