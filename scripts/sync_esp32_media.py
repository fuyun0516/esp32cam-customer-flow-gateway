from __future__ import annotations

import argparse
import bisect
import csv
import io
import json
import os
import sys
import time
from datetime import datetime
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ROOT = PROJECT_ROOT / "media"


class ResourceNotFoundError(RuntimeError):
    pass


def request_bytes(url: str, timeout: float, attempts: int = 3) -> bytes:
    error: Exception | None = None
    for attempt in range(attempts):
        try:
            request = Request(url, headers={"User-Agent": "ESP32-CAM-Daily-Video/1.0"})
            with urlopen(request, timeout=timeout) as response:
                return response.read()
        except HTTPError as current:
            if current.code == 404:
                raise ResourceNotFoundError(f"resource not found: {url}") from current
            error = current
            if attempt + 1 < attempts:
                time.sleep(1.0 + attempt)
        except (URLError, TimeoutError, OSError) as current:
            error = current
            if attempt + 1 < attempts:
                time.sleep(1.0 + attempt)
    raise RuntimeError(f"request failed: {url}: {error}")


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".part")
    temporary.write_bytes(data)
    os.replace(temporary, path)


def load_state(path: Path) -> dict[str, int]:
    if not path.exists():
        return {"last_index": 0}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return {"last_index": max(0, int(data.get("last_index", 0)))}
    except (OSError, ValueError, TypeError):
        return {"last_index": 0}


def save_state(path: Path, state: dict[str, int]) -> None:
    atomic_write(path, (json.dumps(state, indent=2) + "\n").encode("utf-8"))


def load_flow_events(path: Path, day: str) -> list[int]:
    events: list[int] = []
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            try:
                stamp = datetime.fromisoformat(row["timestamp"])
            except (KeyError, ValueError):
                continue
            if stamp.strftime("%Y%m%d") == day:
                events.append(int(stamp.timestamp()))
    return sorted(events)


def near_event(epoch: int, events: list[int], radius_seconds: int = 20) -> bool:
    position = bisect.bisect_left(events, epoch)
    return any(
        abs(events[index] - epoch) <= radius_seconds
        for index in (position - 1, position)
        if 0 <= index < len(events)
    )


def parse_catalog(data: bytes) -> list[tuple[int, int]]:
    records: list[tuple[int, int]] = []
    text = data.decode("utf-8-sig")
    for row in csv.DictReader(io.StringIO(text)):
        try:
            records.append((int(row["index"]), int(row["epoch"])))
        except (KeyError, ValueError):
            continue
    return records


def sync(device: str, root: Path, day: str, timeout: float, all_photos: bool) -> dict[str, int]:
    base_url = device.rstrip("/")
    target_date = datetime.strptime(day, "%Y%m%d").date()
    state_path = root / ".sync-state.json"
    state = load_state(state_path)

    flow_path = root / "data" / "flow.csv"
    atomic_write(flow_path, request_bytes(f"{base_url}/api/flow.csv", timeout))
    events = load_flow_events(flow_path, day)

    catalog_url = f"{base_url}/api/catalog?after={state['last_index']}"
    records = parse_catalog(request_bytes(catalog_url, timeout))
    downloaded = 0
    skipped = 0
    missing = 0
    target_records = 0
    background_buckets: set[int] = set()

    for index, epoch in records:
        stamp = datetime.fromtimestamp(epoch)
        record_date = stamp.date()
        if record_date > target_date:
            break
        if record_date < target_date:
            state["last_index"] = index
            continue

        target_records += 1
        bucket = epoch // 300
        selected = all_photos or near_event(epoch, events) or bucket not in background_buckets
        background_buckets.add(bucket)
        if selected:
            destination = (
                root
                / "timelapse"
                / stamp.strftime("%Y%m%d%H")
                / f"{stamp:%H%M%S}.jpg"
            )
            if not destination.exists() or destination.stat().st_size < 1024:
                try:
                    photo = request_bytes(
                        f"{base_url}/api/photo?index={index}", timeout
                    )
                except ResourceNotFoundError:
                    missing += 1
                    print(
                        f"warning: photo index {index} is no longer available; skipping",
                        file=sys.stderr,
                        flush=True,
                    )
                else:
                    if len(photo) < 1024 or not photo.startswith(b"\xff\xd8"):
                        raise RuntimeError(f"invalid JPEG returned for image {index}")
                    atomic_write(destination, photo)
                    downloaded += 1
            else:
                skipped += 1
        state["last_index"] = index
        if target_records % 250 == 0:
            save_state(state_path, state)

    save_state(state_path, state)
    return {
        "events": len(events),
        "catalog_records": target_records,
        "downloaded": downloaded,
        "existing": skipped,
        "missing": missing,
        "last_index": state["last_index"],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Sync ESP32-CAM media for one day")
    parser.add_argument("--device", default="http://esp32cam.local")
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--day", required=True, help="Date in YYYYMMDD format")
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument(
        "--all-photos",
        action="store_true",
        help="Download every photo for the day instead of event clips and background samples",
    )
    args = parser.parse_args()
    result = sync(args.device, args.root.resolve(), args.day, args.timeout, args.all_photos)
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
