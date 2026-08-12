from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import wave
from datetime import datetime, timedelta
from pathlib import Path

import hot_trends
import sync_esp32_media
from ffmpeg_utils import ffmpeg_executable


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MEDIA_ROOT = PROJECT_ROOT / "media"


def count_inputs(media_root: Path, day: str) -> tuple[int, int]:
    flow_path = media_root / "data" / "flow.csv"
    events = sync_esp32_media.load_flow_events(flow_path, day) if flow_path.exists() else []
    photos = sum(
        1
        for folder in (media_root / "timelapse").glob(f"{day}??")
        if folder.is_dir()
        for _ in folder.glob("*.jpg")
    )
    return len(events), photos


def split_sentences(text: str) -> list[str]:
    return [
        part.strip()
        for part in re.findall(r"[^。！？!?]+[。！？!?]?", text)
        if part.strip()
    ]


def subtitle_text(sentence: str) -> str:
    if len(sentence) <= 22:
        return sentence
    middle = len(sentence) // 2
    separators = [index for index, char in enumerate(sentence) if char in "，；、"]
    split_at = (
        min(separators, key=lambda index: abs(index - middle)) + 1
        if separators
        else middle
    )
    return sentence[:split_at] + "\n" + sentence[split_at:]


def srt_time(seconds: float) -> str:
    milliseconds = max(0, round(seconds * 1000))
    hours, remainder = divmod(milliseconds, 3_600_000)
    minutes, remainder = divmod(remainder, 60_000)
    whole_seconds, milliseconds = divmod(remainder, 1000)
    return f"{hours:02d}:{minutes:02d}:{whole_seconds:02d},{milliseconds:03d}"


def synthesize_voice(text_path: Path, voice_path: Path, subtitles_path: Path) -> None:
    script = PROJECT_ROOT / "scripts" / "synthesize_voice.ps1"
    sentences = split_sentences(text_path.read_text(encoding="utf-8"))
    if not sentences:
        raise RuntimeError("narration contains no subtitle sentences")
    parts_directory = voice_path.parent / "voice-parts"
    parts_directory.mkdir(parents=True, exist_ok=True)
    wav_parts: list[Path] = []
    durations: list[float] = []
    for index, sentence in enumerate(sentences, 1):
        part_text = parts_directory / f"{index:02d}.txt"
        part_wav = parts_directory / f"{index:02d}.wav"
        part_text.write_text(sentence, encoding="utf-8")
        command = [
            "powershell.exe",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(script),
            "-TextPath",
            str(part_text),
            "-OutputPath",
            str(part_wav),
        ]
        subprocess.run(
            command, check=True, cwd=PROJECT_ROOT, stdout=subprocess.DEVNULL
        )
        wav_parts.append(part_wav)
        durations.append(wav_duration(part_wav))

    parameters = None
    with wave.open(str(voice_path), "wb") as merged:
        for part in wav_parts:
            with wave.open(str(part), "rb") as source:
                if parameters is None:
                    parameters = source.getparams()
                    merged.setparams(parameters)
                elif (
                    source.getparams()[:3] != parameters[:3]
                    or source.getparams()[4:] != parameters[4:]
                ):
                    raise RuntimeError("voice part audio formats do not match")
                merged.writeframes(source.readframes(source.getnframes()))

    elapsed = 0.0
    blocks: list[str] = []
    for index, (sentence, duration) in enumerate(
        zip(sentences, durations, strict=True), 1
    ):
        end = elapsed + duration
        blocks.append(
            f"{index}\n{srt_time(elapsed)} --> {srt_time(end)}\n"
            f"{subtitle_text(sentence)}\n"
        )
        elapsed = end
    subtitles_path.write_text("\n".join(blocks), encoding="utf-8-sig")


def wav_duration(path: Path) -> float:
    with wave.open(str(path), "rb") as stream:
        return stream.getnframes() / stream.getframerate()


def extend_body_frames(concat: Path, target: float) -> None:
    lines = concat.read_text(encoding="utf-8").splitlines()
    current = sum(
        float(line.split(maxsplit=1)[1])
        for line in lines
        if line.startswith("duration ")
    )
    if target <= current:
        return
    candidates: list[int] = []
    excluded = ("opening-", "-title", "-chapter", "-peak", "-summary")
    for index, line in enumerate(lines):
        if not line.startswith("duration ") or index == 0:
            continue
        file_line = lines[index - 1]
        filename = file_line.rsplit("/", 1)[-1]
        if not any(token in filename for token in excluded):
            candidates.append(index)
    if not candidates:
        candidates = [
            index for index, line in enumerate(lines) if line.startswith("duration ")
        ]
    addition = (target - current) / len(candidates)
    for index in candidates:
        original = float(lines[index].split(maxsplit=1)[1])
        lines[index] = f"duration {original + addition:.3f}"
    concat.write_text("\n".join(lines) + "\n", encoding="utf-8")


def encode_video(output: Path, day: str) -> Path:
    output = output.resolve()
    concat = output / "promo-video-concat.txt"
    music = output / "promo-background-music.wav"
    narration = output / "promo-narration.txt"
    voice = output / "voice.wav"
    subtitles = output / "promo-subtitles.srt"
    duration = float((output / "promo-duration.txt").read_text(encoding="ascii").strip())
    final_video = output / f"{day}_店铺宣传_每日高光版.mp4"
    synthesize_voice(narration, voice, subtitles)
    target_duration = max(duration, wav_duration(voice) + 0.5)
    extend_body_frames(concat, target_duration)
    command = [
        ffmpeg_executable(),
        "-y",
        "-f",
        "concat",
        "-safe",
        "0",
        "-i",
        str(concat),
        "-stream_loop",
        "-1",
        "-i",
        str(music),
        "-i",
        str(voice),
        "-filter_complex",
        "[0:v]subtitles=filename='promo-subtitles.srt':charenc=UTF-8:"
        "force_style='FontName=Microsoft YaHei,FontSize=10,Bold=-1,PrimaryColour=&H00FFFFFF,"
        "OutlineColour=&H00101814,BorderStyle=1,Outline=1,Shadow=0,"
        "MarginL=36,MarginR=36,MarginV=54,Alignment=2'[video];"
        "[1:a]volume=0.24[music];[2:a]volume=1.0[voice];"
        "[music][voice]amix=inputs=2:duration=longest:dropout_transition=2[a]",
        "-map",
        "[video]",
        "-map",
        "[a]",
        "-c:v",
        "libx264",
        "-preset",
        "medium",
        "-crf",
        "20",
        "-pix_fmt",
        "yuv420p",
        "-fps_mode",
        "vfr",
        "-c:a",
        "aac",
        "-b:a",
        "160k",
        "-t",
        f"{target_duration:.3f}",
        str(final_video),
    ]
    subprocess.run(command, check=True, cwd=output)
    if not final_video.exists() or final_video.stat().st_size < 100_000:
        raise RuntimeError("FFmpeg did not create a valid final video")
    return final_video


def main() -> None:
    default_day = (datetime.now() - timedelta(days=1)).strftime("%Y%m%d")
    parser = argparse.ArgumentParser(description="Download and create the daily ESP32-CAM video")
    parser.add_argument("--device", default="http://esp32cam.local")
    parser.add_argument("--media-root", type=Path, default=DEFAULT_MEDIA_ROOT)
    parser.add_argument("--output-root", type=Path, default=PROJECT_ROOT / "output")
    parser.add_argument("--day", default=default_day, help="Date in YYYYMMDD format")
    parser.add_argument("--skip-sync", action="store_true")
    parser.add_argument("--all-photos", action="store_true")
    parser.add_argument(
        "--skip-hot-trends",
        action="store_true",
        help="Create the video with generic shop copy without reading Douyin trends",
    )
    parser.add_argument(
        "--hot-topic",
        help="Use a reviewed topic instead of reading the current Douyin hot list",
    )
    parser.add_argument(
        "--store-name",
        default=os.environ.get("ESP32_STORE_NAME", "").strip(),
        help="Store name used only in an optional safety notice",
    )
    parser.add_argument(
        "--store-region",
        default=os.environ.get("ESP32_STORE_REGION", "").strip(),
        help=(
            "Shop province/city used to verify local weather alerts, for example "
            "福建省福州市; no automatic safety notice is added when omitted"
        ),
    )
    parser.add_argument(
        "--safety-notice",
        help="Use a manually reviewed closing safety notice for this video",
    )
    args = parser.parse_args()

    day_date = datetime.strptime(args.day, "%Y%m%d")
    if day_date.date() >= datetime.now().date():
        raise SystemExit("The daily task only processes a completed day")

    media_root = args.media_root.resolve()
    output = args.output_root.resolve() / args.day
    output.mkdir(parents=True, exist_ok=True)
    status: dict[str, object] = {
        "day": args.day,
        "device": args.device,
        "started_at": datetime.now().isoformat(timespec="seconds"),
    }

    if not args.skip_sync:
        status["sync"] = sync_esp32_media.sync(
            args.device, media_root, args.day, 20.0, args.all_photos
        )

    event_count, photo_count = count_inputs(media_root, args.day)
    status.update({"events": event_count, "photos": photo_count})
    if event_count == 0 or photo_count == 0:
        status["result"] = "skipped: no customer events or photos"
        (output / "daily-task-status.json").write_text(
            json.dumps(status, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        print(status["result"])
        return

    hot_topic = hot_trends.prepare_hot_topic(
        output,
        args.day,
        manual_topic=args.hot_topic,
        store_name=args.store_name,
        store_region=args.store_region,
        manual_safety_notice=args.safety_notice,
        enabled=not args.skip_hot_trends,
    )
    status["hot_topic"] = {
        "status": hot_topic["status"],
        "topic": hot_topic.get("topic"),
        "source": hot_topic["source_name"],
    }
    safety_alert = hot_topic["safety_alert"]
    status["safety_alert"] = {
        "status": safety_alert["status"],
        "store_region": safety_alert.get("store_region"),
        "source_title": safety_alert.get("source_title"),
        "notice": safety_alert.get("notice"),
    }

    for directory in (output / "promo-frames", output / "video-frames"):
        if directory.exists():
            shutil.rmtree(directory)

    environment = os.environ.copy()
    environment.update(
        {
            "ESP32_VIDEO_DAY": args.day,
            "ESP32_MEDIA_ROOT": str(media_root),
            "ESP32_VIDEO_OUTPUT": str(output),
        }
    )
    subprocess.run(
        [sys.executable, str(PROJECT_ROOT / "scripts" / "create_promotional_highlight_video.py")],
        check=True,
        cwd=PROJECT_ROOT,
        env=environment,
    )
    final_video = encode_video(output, args.day)
    status.update(
        {
            "result": "complete",
            "video": str(final_video),
            "bytes": final_video.stat().st_size,
            "finished_at": datetime.now().isoformat(timespec="seconds"),
        }
    )
    (output / "daily-task-status.json").write_text(
        json.dumps(status, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(final_video)


if __name__ == "__main__":
    main()
