from __future__ import annotations

import csv
import math
import wave
from collections import Counter
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont

import inspect_sd_events as events_lib


OUTPUT = events_lib.OUTPUT
FRAME_DIR = OUTPUT / "video-frames"
WIDTH = 1080
HEIGHT = 1920
DAY_DATE = datetime.strptime(events_lib.DAY, "%Y%m%d")
DAY_LABEL = f"{DAY_DATE.year} 年 {DAY_DATE.month} 月 {DAY_DATE.day} 日"
DAY_ISO = DAY_DATE.strftime("%Y-%m-%d")
DAY_DOTTED = DAY_DATE.strftime("%Y.%m.%d")


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    candidates = [
        Path(r"C:\Windows\Fonts\msyhbd.ttc") if bold else Path(r"C:\Windows\Fonts\msyh.ttc"),
        Path(r"C:\Windows\Fonts\simhei.ttf"),
        Path(r"C:\Windows\Fonts\arial.ttf"),
    ]
    for candidate in candidates:
        if candidate.exists():
            return ImageFont.truetype(str(candidate), size)
    return ImageFont.load_default()


def fit_cover(image: Image.Image, size: tuple[int, int]) -> Image.Image:
    ratio = max(size[0] / image.width, size[1] / image.height)
    resized = image.resize(
        (math.ceil(image.width * ratio), math.ceil(image.height * ratio)),
        Image.Resampling.LANCZOS,
    )
    left = (resized.width - size[0]) // 2
    top = (resized.height - size[1]) // 2
    return resized.crop((left, top, left + size[0], top + size[1]))


def fit_contain(image: Image.Image, size: tuple[int, int]) -> Image.Image:
    result = image.copy()
    result.thumbnail(size, Image.Resampling.LANCZOS)
    return result


def gradient_card() -> Image.Image:
    top = np.array([13, 34, 25], dtype=np.float32)
    bottom = np.array([4, 12, 9], dtype=np.float32)
    rows = np.linspace(0, 1, HEIGHT, dtype=np.float32)[:, None]
    rgb = top[None, :] * (1 - rows) + bottom[None, :] * rows
    array = np.repeat(rgb[:, None, :], WIDTH, axis=1).astype(np.uint8)
    return Image.fromarray(array, "RGB")


def centered(draw: ImageDraw.ImageDraw, y: int, text: str, text_font: ImageFont.ImageFont, fill: str) -> None:
    box = draw.textbbox((0, 0), text, font=text_font)
    draw.text(((WIDTH - (box[2] - box[0])) // 2, y), text, font=text_font, fill=fill)


def save_title(path: Path, first: datetime, last: datetime) -> None:
    image = gradient_card()
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((110, 280, 970, 1450), radius=42, fill="#102b20", outline="#3c7658", width=3)
    centered(draw, 390, "门店客流的一天", font(76, True), "white")
    centered(draw, 505, DAY_LABEL, font(38), "#b6d9c4")
    draw.line((245, 610, 835, 610), fill="#65b889", width=5)
    centered(draw, 720, f"{first:%H:%M} — {last:%H:%M}", font(72, True), "#82d6a6")
    centered(draw, 835, "延时摄影 · 客流片段", font(36), "#d7e8de")
    centered(draw, 1255, "ESP32 智能客流网关", font(29), "#91a99b")
    image.save(path, quality=94)


def save_hour_card(path: Path, hour: int, count: int, peak_hour: int) -> None:
    image = gradient_card()
    draw = ImageDraw.Draw(image)
    centered(draw, 530, f"{hour:02d}:00 — {hour + 1:02d}:00", font(80, True), "white")
    centered(draw, 700, f"系统记录约 {count} 人次", font(46), "#a9d9bd")
    if hour == peak_hour:
        centered(draw, 825, "今日客流高峰", font(42, True), "#ffd47a")
    draw.rounded_rectangle((190, 1020, 890, 1038), radius=9, fill="#2d5d44")
    width = min(700, max(30, int(700 * count / 21)))
    draw.rounded_rectangle((190, 1020, 190 + width, 1038), radius=9, fill="#66c18c")
    centered(draw, 1260, "数据来自设备客流估算", font(28), "#829b8c")
    image.save(path, quality=94)


def subject_crop(source: Image.Image, source_path: Path, background_gray: np.ndarray) -> Image.Image:
    current = events_lib.small_gray(source_path)
    brightness_delta = np.median(current - background_gray)
    mask = np.abs(current - background_gray - brightness_delta) >= 18.0
    coordinates = np.argwhere(mask)
    center_x = float(np.median(coordinates[:, 1])) if len(coordinates) >= 20 else 40.0
    center_x = center_x * source.width / 80.0
    crop_width = min(source.width, int(source.height * 0.80))
    left = int(round(center_x - crop_width / 2))
    left = max(0, min(source.width - crop_width, left))
    return source.crop((left, 0, left + crop_width, source.height))


def save_photo_frame(
    path: Path,
    source_path: Path,
    stamp: datetime,
    hourly_count: int,
    background_gray: np.ndarray,
    badge: str | None = None,
) -> None:
    with Image.open(source_path) as source:
        source = ImageEnhance.Color(source.convert("RGB")).enhance(0.92)
        background = fit_cover(source, (WIDTH, HEIGHT)).filter(ImageFilter.GaussianBlur(30))
        background = ImageEnhance.Brightness(background).enhance(0.33)
        foreground = subject_crop(source, source_path, background_gray).resize(
            (1000, 1250), Image.Resampling.LANCZOS
        )

    image = background
    draw = ImageDraw.Draw(image, "RGBA")
    draw.rectangle((0, 0, WIDTH, HEIGHT), fill=(2, 13, 8, 62))
    draw.rounded_rectangle((30, 42, 1050, 224), radius=30, fill=(5, 20, 13, 215), outline=(105, 184, 137, 130), width=2)
    draw.text((68, 73), "门店客流记录", font=font(38, True), fill="white")
    draw.text((68, 137), DAY_LABEL, font=font(27), fill="#a9c8b5")
    time_text = stamp.strftime("%H:%M:%S")
    time_box = draw.textbbox((0, 0), time_text, font=font(52, True))
    draw.text((1010 - (time_box[2] - time_box[0]), 92), time_text, font=font(52, True), fill="#8ee0ae")

    x = (WIDTH - foreground.width) // 2
    y = 270
    draw.rounded_rectangle((x - 9, y - 9, x + foreground.width + 9, y + foreground.height + 9), radius=24, fill=(230, 245, 236, 220))
    image.paste(foreground, (x, y))
    if badge:
        badge_box = draw.textbbox((0, 0), badge, font=font(27, True))
        badge_width = badge_box[2] - badge_box[0] + 56
        draw.rounded_rectangle((50, 286, 50 + badge_width, 350), radius=24, fill=(236, 177, 74, 238))
        draw.text((78, 301), badge, font=font(27, True), fill="#172018")

    draw.rounded_rectangle((40, 1565, 1040, 1875), radius=34, fill=(5, 22, 14, 225))
    draw.text((82, 1610), f"{stamp.hour:02d}:00—{stamp.hour + 1:02d}:00", font=font(36, True), fill="#b8d8c4")
    count_text = f"约 {hourly_count} 人次"
    count_box = draw.textbbox((0, 0), count_text, font=font(54, True))
    draw.text((998 - (count_box[2] - count_box[0]), 1590), count_text, font=font(54, True), fill="white")
    bar_width = min(800, max(30, int(800 * hourly_count / 21)))
    draw.rounded_rectangle((82, 1722, 882, 1744), radius=11, fill="#294f3b")
    draw.rounded_rectangle((82, 1722, 82 + bar_width, 1744), radius=11, fill="#66c18c")
    draw.text((82, 1790), "照片每 5 秒记录 · 画面按时间连续播放", font=font(24), fill="#81998a")
    image.save(path, quality=91)


def save_summary(path: Path, hourly: Counter[int], total: int, peak_hour: int) -> None:
    image = gradient_card()
    draw = ImageDraw.Draw(image)
    centered(draw, 165, "今日客流总结", font(64, True), "white")
    centered(draw, 275, f"系统记录约 {total} 人次", font(42), "#a8d3b8")

    chart_left, chart_top, chart_right, chart_bottom = 105, 560, 975, 1230
    draw.rounded_rectangle((60, 450, 1020, 1395), radius=40, fill="#102a1f", outline="#315c46", width=2)
    hours = list(range(min(hourly), max(hourly) + 1))
    max_count = max(hourly.values())
    gap = 26
    bar_w = (chart_right - chart_left - gap * (len(hours) - 1)) // len(hours)
    for index, hour in enumerate(hours):
        count = hourly.get(hour, 0)
        bar_h = int((chart_bottom - chart_top) * count / max_count) if max_count else 0
        x = chart_left + index * (bar_w + gap)
        color = "#f0bb58" if hour == peak_hour else "#65bd88"
        draw.rounded_rectangle((x, chart_bottom - bar_h, x + bar_w, chart_bottom), radius=14, fill=color)
        label = str(count)
        label_box = draw.textbbox((0, 0), label, font=font(30, True))
        draw.text((x + (bar_w - (label_box[2] - label_box[0])) // 2, chart_bottom - bar_h - 52), label, font=font(30, True), fill="white")
        hour_label = f"{hour}点"
        box = draw.textbbox((0, 0), hour_label, font=font(24))
        draw.text((x + (bar_w - (box[2] - box[0])) // 2, chart_bottom + 20), hour_label, font=font(24), fill="#a4b9ab")

    centered(draw, 1480, f"高峰期：{peak_hour:02d}:00—{peak_hour + 1:02d}:00", font(50, True), "#ffd375")
    centered(draw, 1565, f"该时段系统记录约 {hourly[peak_hour]} 人次", font(34), "#cde0d4")
    centered(draw, 1750, "客流数据为设备估算，仅供运营参考", font(26), "#80988a")
    image.save(path, quality=94)


def synthesize_music(path: Path, seconds: float) -> None:
    sample_rate = 48000
    length = int(seconds * sample_rate)
    t = np.arange(length, dtype=np.float32) / sample_rate
    music = np.zeros(length, dtype=np.float32)
    chords = [
        (130.81, 164.81, 196.00),
        (110.00, 146.83, 174.61),
        (87.31, 130.81, 164.81),
        (98.00, 146.83, 196.00),
    ]
    segment = 4.0
    for index, chord in enumerate(chords * (math.ceil(seconds / (segment * len(chords))) + 1)):
        start = int(index * segment * sample_rate)
        if start >= length:
            break
        end = min(length, int((index + 1) * segment * sample_rate))
        local_t = np.arange(end - start, dtype=np.float32) / sample_rate
        envelope = np.minimum(1.0, local_t / 0.7) * np.minimum(1.0, (segment - local_t) / 0.7)
        pad = sum(np.sin(2 * np.pi * frequency * local_t) for frequency in chord) / len(chord)
        music[start:end] += 0.16 * pad * np.clip(envelope, 0, 1)
    beat_interval = 0.5
    for beat in np.arange(0, seconds, beat_interval):
        start = int(beat * sample_rate)
        end = min(length, start + int(0.12 * sample_rate))
        local_t = np.arange(end - start, dtype=np.float32) / sample_rate
        kick = np.sin(2 * np.pi * (70 - 25 * local_t) * local_t) * np.exp(-30 * local_t)
        music[start:end] += 0.13 * kick
    fade = min(length // 2, sample_rate * 2)
    music[:fade] *= np.linspace(0, 1, fade, dtype=np.float32)
    music[-fade:] *= np.linspace(1, 0, fade, dtype=np.float32)
    music = np.clip(music, -0.9, 0.9)
    stereo = np.column_stack((music, music))
    pcm = (stereo * 32767).astype("<i2")
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(pcm.tobytes())


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    FRAME_DIR.mkdir(parents=True, exist_ok=True)
    events = events_lib.load_events()
    groups = events_lib.group_events(events)
    photos = events_lib.load_photos()
    backgrounds = events_lib.hourly_backgrounds(photos)
    hourly = Counter(stamp.hour for stamp in events)
    peak_hour = max(hourly, key=hourly.get)
    strongest = [events_lib.strongest_photo(group, photos, backgrounds) for group in groups]
    selected_groups = [
        (group, item)
        for group, item in zip(groups, strongest, strict=True)
        if events_lib.foreground_score(item[1], backgrounds[item[0].hour]) >= 0.05
    ]

    sequence: list[tuple[Path, float]] = []
    frame_number = 0

    def add_frame(image_path: Path, duration: float) -> None:
        sequence.append((image_path, duration))

    title_path = FRAME_DIR / "00000-title.jpg"
    save_title(title_path, events[0], events[-1])
    add_frame(title_path, 3.4)

    current_hour: int | None = None
    used_photos: set[Path] = set()
    for group, (strong_stamp, _) in selected_groups:
        if strong_stamp.hour != current_hour:
            current_hour = strong_stamp.hour
            frame_number += 1
            hour_path = FRAME_DIR / f"{frame_number:05d}-hour-{current_hour:02d}.jpg"
            save_hour_card(hour_path, current_hour, hourly[current_hour], peak_hour)
            add_frame(hour_path, 1.0)

        window_start = strong_stamp - timedelta(seconds=10)
        window_end = strong_stamp + timedelta(seconds=10)
        clip = [(stamp, path) for stamp, path in photos if window_start <= stamp <= window_end]
        previous_output: Path | None = None
        for stamp, source in clip:
            if source in used_photos:
                continue
            used_photos.add(source)
            frame_number += 1
            output_path = FRAME_DIR / f"{frame_number:05d}-{stamp:%H%M%S}.jpg"
            save_photo_frame(
                output_path,
                source,
                stamp,
                hourly[stamp.hour],
                backgrounds[stamp.hour],
            )
            if previous_output is not None:
                frame_number += 1
                blend_path = FRAME_DIR / f"{frame_number:05d}-blend.jpg"
                with Image.open(previous_output) as before, Image.open(output_path) as after:
                    Image.blend(before.convert("RGB"), after.convert("RGB"), 0.5).save(blend_path, quality=88)
                add_frame(blend_path, 0.07)
            add_frame(output_path, 0.20)
            previous_output = output_path

    frame_number += 1
    summary_path = FRAME_DIR / f"{frame_number:05d}-summary.jpg"
    save_summary(summary_path, hourly, len(events), peak_hour)
    add_frame(summary_path, 6.0)

    concat_path = OUTPUT / "video-concat.txt"
    with concat_path.open("w", encoding="utf-8", newline="\n") as stream:
        for image_path, duration in sequence:
            escaped = image_path.as_posix().replace("'", "'\\''")
            stream.write(f"file '{escaped}'\n")
            stream.write(f"duration {duration:.3f}\n")
        stream.write(f"file '{sequence[-1][0].as_posix()}'\n")

    duration = sum(item[1] for item in sequence)
    narration = (
        f"今天是{DAY_LABEL}。"
        f"从{events[0]:%H点%M分}到{events[-1]:%H点%M分}，系统共记录约{len(events)}人次客流。"
        "这些延时片段，记录了门店在不同时段的人员经过情况。"
        f"{peak_hour}点到{peak_hour + 1}点，客流达到今日高峰，系统记录约{hourly[peak_hour]}人次。"
        "以上数据来自设备客流估算，仅供门店运营参考。"
    )
    (OUTPUT / "narration.txt").write_text(narration, encoding="utf-8")
    (OUTPUT / "video-duration.txt").write_text(f"{duration:.3f}\n", encoding="ascii")
    synthesize_music(OUTPUT / "background-music.wav", duration)

    report_path = OUTPUT / "video-report.csv"
    with report_path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["date", "log_events", "selected_groups", "peak_period", "peak_count", "duration_seconds"])
        writer.writerow([DAY_ISO, len(events), len(selected_groups), f"{peak_hour:02d}:00-{peak_hour + 1:02d}:00", hourly[peak_hour], f"{duration:.2f}"])
    print(f"selected_groups={len(selected_groups)} frames={len(sequence)} duration={duration:.2f}")
    print(concat_path)
    print(OUTPUT / "narration.txt")
    print(OUTPUT / "background-music.wav")


if __name__ == "__main__":
    main()
