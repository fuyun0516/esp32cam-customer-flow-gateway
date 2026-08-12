from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]


def ffmpeg_executable() -> str:
    configured = os.environ.get("FFMPEG_EXE", "").strip()
    if configured:
        path = Path(configured).expanduser()
        if path.is_file():
            return str(path.resolve())
        raise RuntimeError(f"FFMPEG_EXE does not point to a file: {path}")

    system_ffmpeg = shutil.which("ffmpeg")
    if system_ffmpeg:
        return system_ffmpeg

    bundled_dependencies = PROJECT_ROOT / ".tools" / "video-deps"
    if bundled_dependencies.is_dir():
        sys.path.insert(0, str(bundled_dependencies))

    try:
        import imageio_ffmpeg
    except (ImportError, OSError) as error:
        raise RuntimeError(
            "FFmpeg was not found. Install imageio-ffmpeg, put ffmpeg on PATH, "
            "or set FFMPEG_EXE to the executable path."
        ) from error

    getter = getattr(imageio_ffmpeg, "get_ffmpeg_exe", None)
    if getter is None:
        raise RuntimeError(
            "The imported imageio_ffmpeg module is incomplete. Reinstall "
            "imageio-ffmpeg or set FFMPEG_EXE."
        )
    return str(getter())

