from __future__ import annotations

import csv
import os
from datetime import datetime, timedelta
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont
import numpy as np


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DAY = os.environ.get(
    "ESP32_VIDEO_DAY", (datetime.now() - timedelta(days=1)).strftime("%Y%m%d")
)
SD_ROOT = Path(
    os.environ.get("ESP32_MEDIA_ROOT", str(PROJECT_ROOT / "media"))
)
OUTPUT = Path(
    os.environ.get("ESP32_VIDEO_OUTPUT", str(PROJECT_ROOT / "output" / DAY))
)


def load_font(size: int) -> ImageFont.FreeTypeFont:
    candidates = [
        Path(r"C:\Windows\Fonts\msyh.ttc"),
        Path(r"C:\Windows\Fonts\simhei.ttf"),
        Path(r"C:\Windows\Fonts\arial.ttf"),
    ]
    for candidate in candidates:
        if candidate.exists():
            return ImageFont.truetype(str(candidate), size)
    return ImageFont.load_default()


def load_events() -> list[datetime]:
    events: list[datetime] = []
    with (SD_ROOT / "data" / "flow.csv").open("r", encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            try:
                stamp = datetime.fromisoformat(row["timestamp"])
            except (KeyError, ValueError):
                continue
            if stamp.strftime("%Y%m%d") == DAY:
                events.append(stamp)
    return events


def group_events(events: list[datetime]) -> list[list[datetime]]:
    groups: list[list[datetime]] = []
    for stamp in events:
        if not groups or stamp - groups[-1][-1] > timedelta(seconds=15):
            groups.append([stamp])
        else:
            groups[-1].append(stamp)
    return groups


def load_photos() -> list[tuple[datetime, Path]]:
    photos: list[tuple[datetime, Path]] = []
    for folder in sorted((SD_ROOT / "timelapse").glob(f"{DAY}??")):
        if not folder.is_dir():
            continue
        for photo in sorted(folder.glob("*.jpg")):
            try:
                stamp = datetime.strptime(folder.name[:8] + photo.stem, "%Y%m%d%H%M%S")
            except ValueError:
                continue
            photos.append((stamp, photo))
    return photos


def nearest_photo(stamp: datetime, photos: list[tuple[datetime, Path]]) -> tuple[datetime, Path]:
    return min(photos, key=lambda item: abs((item[0] - stamp).total_seconds()))


def small_gray(path: Path) -> np.ndarray:
    with Image.open(path) as source:
        return np.asarray(
            source.convert("L").resize((80, 60), Image.Resampling.BILINEAR),
            dtype=np.float32,
        )


def hourly_backgrounds(photos: list[tuple[datetime, Path]]) -> dict[int, np.ndarray]:
    by_hour: dict[int, list[Path]] = {}
    for stamp, path in photos:
        by_hour.setdefault(stamp.hour, []).append(path)
    result: dict[int, np.ndarray] = {}
    for hour, paths in by_hour.items():
        step = max(1, len(paths) // 24)
        samples = [small_gray(path) for path in paths[::step][:24]]
        result[hour] = np.median(np.stack(samples), axis=0)
    return result


def foreground_score(path: Path, background: np.ndarray) -> float:
    image = small_gray(path)
    brightness_delta = np.median(image - background)
    delta = np.abs(image - background - brightness_delta)
    return float(np.mean(delta >= 18.0))


def strongest_photo(
    group: list[datetime],
    photos: list[tuple[datetime, Path]],
    backgrounds: dict[int, np.ndarray],
) -> tuple[datetime, Path]:
    start = group[0] - timedelta(seconds=15)
    end = group[-1] + timedelta(seconds=15)
    candidates = [item for item in photos if start <= item[0] <= end]
    if not candidates:
        return nearest_photo(group[len(group) // 2], photos)
    return max(candidates, key=lambda item: foreground_score(item[1], backgrounds[item[0].hour]))


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    events = load_events()
    groups = group_events(events)
    photos = load_photos()
    backgrounds = hourly_backgrounds(photos)
    selected = [strongest_photo(group, photos, backgrounds) for group in groups]

    cell_w, cell_h = 320, 300
    cols = 4
    rows = (len(selected) + cols - 1) // cols
    sheet = Image.new("RGB", (cell_w * cols, cell_h * rows), "#101412")
    draw = ImageDraw.Draw(sheet)
    font = load_font(20)
    small = load_font(16)
    for index, ((stamp, path), group) in enumerate(zip(selected, groups, strict=True)):
        x = (index % cols) * cell_w
        y = (index // cols) * cell_h
        with Image.open(path) as source:
            image = source.convert("RGB")
            image.thumbnail((cell_w - 16, 230), Image.Resampling.LANCZOS)
            image_x = x + (cell_w - image.width) // 2
            sheet.paste(image, (image_x, y + 8))
        draw.text((x + 10, y + 242), f"事件 {index + 1:02d}  {stamp:%H:%M:%S}", font=font, fill="white")
        draw.text((x + 10, y + 270), f"日志计数 {len(group)}", font=small, fill="#9dd2b2")
    sheet_path = OUTPUT / "event-contact-sheet.jpg"
    sheet.save(sheet_path, quality=90)

    manifest_path = OUTPUT / "event-manifest.csv"
    with manifest_path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["event", "start", "end", "log_count", "nearest_photo"])
        for index, (group, (_, path)) in enumerate(zip(groups, selected, strict=True), 1):
            writer.writerow([index, group[0].isoformat(), group[-1].isoformat(), len(group), str(path)])
    print(sheet_path)
    print(manifest_path)
    print(f"groups={len(groups)} photos={len(photos)} events={len(events)}")


if __name__ == "__main__":
    main()
