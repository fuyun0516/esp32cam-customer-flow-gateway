from __future__ import annotations

import argparse
import csv
import re
from datetime import datetime, timedelta
from pathlib import Path

from PIL import Image, ImageEnhance, ImageFilter


PROJECT_ROOT = Path(__file__).resolve().parents[1]
PHOTO_PATTERN = re.compile(r"^\d{5}-(\d{6})\.jpg$")


def restore_photo(source_path: Path, target_path: Path) -> None:
    with Image.open(source_path) as opened:
        source = opened.convert("RGB")
        top = min(210, source.height // 5)
        bottom = min(1450, source.height)
        clean_height = bottom - top
        crop_width = min(source.width, round(clean_height * 9 / 16))
        left = (source.width - crop_width) // 2
        image = source.crop((left, top, left + crop_width, bottom))
        image = image.resize((1080, 1920), Image.Resampling.LANCZOS)
        image = ImageEnhance.Sharpness(image).enhance(1.08)
        image = image.filter(ImageFilter.GaussianBlur(0.15))
        target_path.parent.mkdir(parents=True, exist_ok=True)
        image.save(target_path, quality=92)


def build_media(day: str, source_root: Path, manifest_path: Path, target_root: Path) -> int:
    written: set[Path] = set()
    newest_by_time: dict[str, Path] = {}
    for source_path in source_root.glob("*.jpg"):
        match = PHOTO_PATTERN.match(source_path.name)
        if not match:
            continue
        time_key = match.group(1)
        current = newest_by_time.get(time_key)
        if current is None or source_path.stat().st_mtime > current.stat().st_mtime:
            newest_by_time[time_key] = source_path

    for time_key, source_path in sorted(newest_by_time.items()):
        stamp = datetime.strptime(day + time_key, "%Y%m%d%H%M%S")
        target_path = target_root / "timelapse" / stamp.strftime("%Y%m%d%H") / stamp.strftime("%H%M%S.jpg")
        restore_photo(source_path, target_path)
        written.add(target_path)

    flow_path = target_root / "data" / "flow.csv"
    flow_path.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    with manifest_path.open("r", encoding="utf-8-sig", newline="") as source:
        rows = list(csv.DictReader(source))
    with flow_path.open("w", encoding="utf-8-sig", newline="") as target:
        writer = csv.writer(target)
        writer.writerow(["timestamp", "count"])
        for row in rows:
            start = datetime.fromisoformat(row["start"])
            repeated = max(1, int(row["log_count"]))
            for offset in range(repeated):
                count += 1
                writer.writerow([(start + timedelta(milliseconds=offset)).isoformat(), count])

    print(f"photos={len(written)} events={count}")
    print(target_root)
    return len(written)


def main() -> None:
    parser = argparse.ArgumentParser(description="Build clean media for the promotional video template preview")
    parser.add_argument("--day", required=True, help="Date in YYYYMMDD format")
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--target-root", type=Path, default=PROJECT_ROOT / "tmp" / "video-template-media")
    args = parser.parse_args()
    datetime.strptime(args.day, "%Y%m%d")
    source_root = args.source_root or PROJECT_ROOT / "output" / args.day / "promo-frames"
    manifest = args.manifest or PROJECT_ROOT / "output" / args.day / "event-manifest.csv"
    if build_media(args.day, source_root, manifest, args.target_root) == 0:
        raise RuntimeError("No compatible photo frames were found")


if __name__ == "__main__":
    main()
