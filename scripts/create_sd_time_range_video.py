from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

from ffmpeg_utils import ffmpeg_executable



def collect_photos(
    timelapse_root: Path, source_day: str, start_hour: int, end_hour: int
) -> list[Path]:
    photos: list[Path] = []
    for hour in range(start_hour, end_hour):
        folder = timelapse_root / f"{source_day}{hour:02d}"
        if not folder.is_dir():
            raise RuntimeError(f"Missing hourly photo directory: {folder}")
        photos.extend(sorted(folder.glob("*.jpg"), key=lambda path: path.name))
    if not photos:
        raise RuntimeError("No JPEG photos found in the requested time range")
    return photos


def write_concat(path: Path, photos: list[Path], fps: float) -> None:
    duration = 1.0 / fps
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        for photo in photos:
            escaped = photo.resolve().as_posix().replace("'", "'\\''")
            stream.write(f"file '{escaped}'\n")
            stream.write(f"duration {duration:.6f}\n")
        escaped = photos[-1].resolve().as_posix().replace("'", "'\\''")
        stream.write(f"file '{escaped}'\n")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Create a sequential MP4 from SD-card hourly photo folders"
    )
    parser.add_argument("--timelapse-root", type=Path, required=True)
    parser.add_argument("--source-day", required=True, help="Folder date in YYYYMMDD")
    parser.add_argument("--start-hour", type=int, required=True)
    parser.add_argument("--end-hour", type=int, required=True)
    parser.add_argument("--fps", type=float, default=5.0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if not (0 <= args.start_hour < args.end_hour <= 24):
        raise SystemExit("The hour range must satisfy 0 <= start < end <= 24")
    if args.fps <= 0:
        raise SystemExit("FPS must be positive")

    photos = collect_photos(
        args.timelapse_root.resolve(),
        args.source_day,
        args.start_hour,
        args.end_hour,
    )
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    concat = output.with_suffix(".concat.txt")
    write_concat(concat, photos, args.fps)

    command = [
        ffmpeg_executable(),
        "-y",
        "-f",
        "concat",
        "-safe",
        "0",
        "-i",
        str(concat),
        "-vf",
        "scale=640:480:flags=lanczos,format=yuv420p",
        "-r",
        f"{args.fps:g}",
        "-c:v",
        "libx264",
        "-preset",
        "medium",
        "-crf",
        "20",
        "-movflags",
        "+faststart",
        "-an",
        str(output),
    ]
    subprocess.run(command, check=True, cwd=output.parent)
    if not output.exists() or output.stat().st_size < 100_000:
        raise RuntimeError("FFmpeg did not create a valid MP4")
    print(f"photos={len(photos)} fps={args.fps:g} duration={len(photos) / args.fps:.2f}")
    print(output)


if __name__ == "__main__":
    main()
