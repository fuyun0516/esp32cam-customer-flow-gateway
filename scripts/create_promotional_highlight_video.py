from __future__ import annotations

import math
import wave
from collections import Counter
from datetime import timedelta
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageEnhance

import create_customer_day_video as base
import hot_trends
import inspect_sd_events as events_lib


OUTPUT = base.OUTPUT
FRAME_DIR = OUTPUT / "promo-frames"


def save_fullscreen_photo(
    path: Path,
    source_path: Path,
    stamp,
    total_count: int,
    background_gray: np.ndarray,
) -> None:
    with Image.open(source_path) as opened:
        source = ImageEnhance.Color(opened.convert("RGB")).enhance(0.94)
        current = events_lib.small_gray(source_path)
        brightness_delta = np.median(current - background_gray)
        mask = np.abs(current - background_gray - brightness_delta) >= 18.0
        coordinates = np.argwhere(mask)
        center_x = float(np.median(coordinates[:, 1])) if len(coordinates) >= 20 else 40.0
        center_x = center_x * source.width / 80.0
        crop_width = min(source.width, int(round(source.height * 9 / 16)))
        left = int(round(center_x - crop_width / 2))
        left = max(0, min(source.width - crop_width, left))
        image = source.crop((left, 0, left + crop_width, source.height)).resize(
            (base.WIDTH, base.HEIGHT), Image.Resampling.LANCZOS
        )

    overlay = Image.new("RGBA", image.size, (0, 0, 0, 0))
    overlay_draw = ImageDraw.Draw(overlay, "RGBA")
    for row in range(390):
        alpha = int(180 * (1 - row / 390) ** 2)
        overlay_draw.rectangle((0, row, base.WIDTH, row + 1), fill=(0, 8, 4, alpha))
    for row in range(520):
        alpha = int(205 * (row / 520) ** 1.6)
        y = base.HEIGHT - 520 + row
        overlay_draw.rectangle((0, y, base.WIDTH, y + 1), fill=(0, 8, 4, alpha))
    image = Image.alpha_composite(image.convert("RGBA"), overlay)
    draw = ImageDraw.Draw(image, "RGBA")

    draw.text(
        (52, 52),
        stamp.strftime("%H:%M:%S"),
        font=base.font(52, True),
        fill="#ffd43b",
        stroke_width=3,
        stroke_fill="#132018",
    )
    draw.text(
        (55, 126),
        base.DAY_DOTTED,
        font=base.font(27, True),
        fill="white",
        stroke_width=2,
        stroke_fill="#132018",
    )
    draw.text(
        (55, 172),
        f"今日客流 {total_count} 人次",
        font=base.font(27, True),
        fill="#72f0a8",
        stroke_width=2,
        stroke_fill="#132018",
    )

    image.convert("RGB").save(path, quality=92)


def synthesize_promo_music(path: Path, seconds: float) -> None:
    sample_rate = 48000
    length = int(seconds * sample_rate)
    music = np.zeros(length, dtype=np.float32)
    bpm = 108.0
    beat = 60.0 / bpm
    chords = [
        (130.81, 164.81, 196.00),
        (110.00, 146.83, 174.61),
        (87.31, 130.81, 164.81),
        (98.00, 146.83, 196.00),
    ]
    chord_seconds = beat * 4
    chord_count = math.ceil(seconds / chord_seconds)
    for index in range(chord_count):
        start = int(index * chord_seconds * sample_rate)
        end = min(length, int((index + 1) * chord_seconds * sample_rate))
        local_t = np.arange(end - start, dtype=np.float32) / sample_rate
        chord = chords[index % len(chords)]
        pad = sum(np.sin(2 * np.pi * frequency * local_t) for frequency in chord) / len(chord)
        pulse = 0.55 + 0.45 * np.sin(2 * np.pi * local_t / beat) ** 2
        music[start:end] += 0.13 * pad * pulse
        root = chord[0] / 2
        music[start:end] += 0.12 * np.sin(2 * np.pi * root * local_t) * (0.55 + 0.45 * np.cos(2 * np.pi * local_t / beat))

    for index, position in enumerate(np.arange(0, seconds, beat / 2)):
        start = int(position * sample_rate)
        if index % 2 == 0:
            end = min(length, start + int(0.14 * sample_rate))
            local_t = np.arange(end - start, dtype=np.float32) / sample_rate
            kick = np.sin(2 * np.pi * (78 - 32 * local_t) * local_t) * np.exp(-27 * local_t)
            music[start:end] += 0.24 * kick
        else:
            end = min(length, start + int(0.055 * sample_rate))
            noise = np.random.default_rng(index).normal(0, 1, end - start).astype(np.float32)
            envelope = np.exp(-70 * np.arange(end - start, dtype=np.float32) / sample_rate)
            music[start:end] += 0.035 * noise * envelope

    for bar in np.arange(beat, seconds, beat * 2):
        start = int(bar * sample_rate)
        end = min(length, start + int(0.16 * sample_rate))
        noise = np.random.default_rng(int(bar * 100)).normal(0, 1, end - start).astype(np.float32)
        envelope = np.exp(-25 * np.arange(end - start, dtype=np.float32) / sample_rate)
        music[start:end] += 0.07 * noise * envelope

    fade = min(length // 2, sample_rate * 2)
    music[:fade] *= np.linspace(0, 1, fade, dtype=np.float32)
    music[-fade:] *= np.linspace(1, 0, fade, dtype=np.float32)
    music = np.clip(music, -0.92, 0.92)
    stereo = np.column_stack((music, music))
    pcm = (stereo * 32767).astype("<i2")
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(pcm.tobytes())


def build_narration(
    events,
    photos,
    hourly: Counter,
    peak_hour: int,
    peak_anchor=None,
    peak_group_count: int = 0,
) -> str:
    hot_topic = hot_trends.load_hot_topic(OUTPUT)
    topic = hot_topic.get("topic") if hot_topic.get("status") != "fallback" else None
    safety_alert = hot_trends.load_safety_alert(OUTPUT)
    safety_notice = (
        str(safety_alert["notice"])
        if safety_alert.get("status") in {"manual", "selected"}
        and safety_alert.get("notice")
        else None
    )
    peak_photo_count = (
        sum(
            1
            for stamp, _ in photos
            if abs((stamp - peak_anchor).total_seconds()) <= 10
        )
        if peak_anchor is not None
        else 0
    )
    facts: dict[str, object] = {
        "event_count": len(events),
        "photo_count": len(photos),
        "first_time": photos[0][0].strftime("%H点%M分"),
        "last_time": photos[-1][0].strftime("%H点%M分"),
        "first_hour": photos[0][0].hour,
        "last_hour": photos[-1][0].hour,
        "peak_hour": peak_hour,
        "peak_count": hourly.get(peak_hour, 0),
        "recorded_hours": len({stamp.hour for stamp, _ in photos}),
        "peak_anchor_time": (
            peak_anchor.strftime("%H点%M分") if peak_anchor is not None else None
        ),
        "peak_group_count": peak_group_count,
        "peak_photo_count": peak_photo_count,
    }
    story = hot_trends.content_story(topic, facts, safety_notice=safety_notice)
    hot_trends.record_content_story(OUTPUT, story, facts)
    return "".join(str(sentence) for sentence in story["sentences"])


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    FRAME_DIR.mkdir(parents=True, exist_ok=True)
    events = events_lib.load_events()
    groups = events_lib.group_events(events)
    photos = events_lib.load_photos()
    if not photos:
        raise RuntimeError("No timelapse photos are available for the selected day")
    backgrounds = events_lib.hourly_backgrounds(photos)
    hourly = Counter(stamp.hour for stamp in events)
    peak_hour = max(hourly, key=hourly.get) if hourly else photos[len(photos) // 2][0].hour

    peak_groups = [
        group for group in groups if any(stamp.hour == peak_hour for stamp in group)
    ]
    peak_group = max(peak_groups, key=lambda group: (len(group), group[0]), default=[])
    peak_anchor = peak_group[len(peak_group) // 2] if peak_group else None

    ranked_groups = []
    for group in groups:
        strongest = events_lib.strongest_photo(group, photos, backgrounds)
        score = events_lib.foreground_score(strongest[1], backgrounds[strongest[0].hour])
        rank = score * (1.0 + min(5, len(group)) / 5.0)
        ranked_groups.append((group, strongest, score, rank))
    candidates = [item for item in ranked_groups if item[2] >= 0.05]
    if not candidates:
        candidates = ranked_groups
    if not candidates:
        step = max(1, len(photos) // 60)
        fallback_photos = photos[::step][:60]
        for stamp, path in fallback_photos:
            score = events_lib.foreground_score(path, backgrounds[stamp.hour])
            candidates.append(([stamp], (stamp, path), score, score))
    selected = sorted(sorted(candidates, key=lambda item: item[3], reverse=True)[:15], key=lambda item: item[1][0])

    sequence: list[tuple[Path, float]] = []
    frame_number = 0
    used: set[Path] = set()

    def add(path: Path, duration: float) -> None:
        sequence.append((path, duration))

    if candidates:
        peak_candidates = [
            item for item in candidates if item[1][0].hour == peak_hour
        ]
        opening_group, (opening_stamp, _), _, _ = max(
            peak_candidates or candidates, key=lambda item: item[3]
        )
        opening_start = opening_stamp - timedelta(seconds=10)
        opening_end = opening_stamp + timedelta(seconds=10)
        opening_clip = [
            (stamp, photo)
            for stamp, photo in photos
            if opening_start <= stamp <= opening_end
        ]
        opening_clip = sorted(
            opening_clip, key=lambda item: abs((item[0] - opening_stamp).total_seconds())
        )[:5]
        opening_clip.sort(key=lambda item: item[0])
        opening_duration = 1.0 / max(1, len(opening_clip))
        for stamp, source in opening_clip:
            used.add(source)
            frame_number += 1
            opening_frame = FRAME_DIR / f"{frame_number:05d}-opening-{stamp:%H%M%S}.jpg"
            save_fullscreen_photo(
                opening_frame,
                source,
                stamp,
                len(events),
                backgrounds[stamp.hour],
            )
            add(opening_frame, opening_duration)

    for group, (strong_stamp, _), _, _ in selected:
        start = strong_stamp - timedelta(seconds=10)
        end = strong_stamp + timedelta(seconds=10)
        clip = [(stamp, photo) for stamp, photo in photos if start <= stamp <= end]
        previous: Path | None = None
        for stamp, source in clip:
            if source in used:
                continue
            used.add(source)
            frame_number += 1
            rendered = FRAME_DIR / f"{frame_number:05d}-{stamp:%H%M%S}.jpg"
            save_fullscreen_photo(
                rendered,
                source,
                stamp,
                len(events),
                backgrounds[stamp.hour],
            )
            if previous is not None:
                frame_number += 1
                blended = FRAME_DIR / f"{frame_number:05d}-blend.jpg"
                with Image.open(previous) as before, Image.open(rendered) as after:
                    Image.blend(before.convert("RGB"), after.convert("RGB"), 0.5).save(blended, quality=89)
                add(blended, 0.07)
            add(rendered, 0.22)
            previous = rendered

    concat = OUTPUT / "promo-video-concat.txt"
    with concat.open("w", encoding="utf-8", newline="\n") as stream:
        for image_path, duration in sequence:
            stream.write(f"file '{image_path.as_posix()}'\n")
            stream.write(f"duration {duration:.3f}\n")
        stream.write(f"file '{sequence[-1][0].as_posix()}'\n")

    duration = sum(item[1] for item in sequence)
    narration = build_narration(
        events,
        photos,
        hourly,
        peak_hour,
        peak_anchor=peak_anchor,
        peak_group_count=len(peak_group),
    )
    (OUTPUT / "promo-narration.txt").write_text(narration, encoding="utf-8")
    (OUTPUT / "narration.txt").write_text(narration, encoding="utf-8")
    (OUTPUT / "promo-duration.txt").write_text(f"{duration:.3f}\n", encoding="ascii")
    synthesize_promo_music(OUTPUT / "promo-background-music.wav", duration)
    print(f"selected={len(selected)} frames={len(sequence)} duration={duration:.2f}")
    print(concat)


if __name__ == "__main__":
    main()
