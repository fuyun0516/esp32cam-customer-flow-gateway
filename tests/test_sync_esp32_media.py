from __future__ import annotations

import sys
import tempfile
import unittest
from datetime import datetime, timedelta
from pathlib import Path
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import sync_esp32_media


class SyncEsp32MediaTests(unittest.TestCase):
    def test_downloads_event_frames_and_background_samples(self) -> None:
        day = "20260809"
        base = datetime(2026, 8, 9, 10, 0, 0)
        epochs = [
            int((base + timedelta(seconds=offset)).timestamp())
            for offset in (0, 5, 120, 300)
        ]
        future = int(datetime(2026, 8, 10, 0, 0, 0).timestamp())
        flow = b"timestamp,count\n2026-08-09T10:02:00,1\n"
        catalog = (
            "index,epoch\n"
            + "".join(f"{index + 1},{epoch}\n" for index, epoch in enumerate(epochs))
            + f"5,{future}\n"
        ).encode()
        jpeg = b"\xff\xd8" + b"x" * 2048 + b"\xff\xd9"

        def fake_request(url: str, timeout: float, attempts: int = 3) -> bytes:
            if url.endswith("/api/flow.csv"):
                return flow
            if "/api/catalog?" in url:
                return catalog
            if "/api/photo?" in url:
                return jpeg
            raise AssertionError(url)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.object(sync_esp32_media, "request_bytes", side_effect=fake_request):
                result = sync_esp32_media.sync(
                    "http://device", root, day, timeout=1.0, all_photos=False
                )
            photos = list((root / "timelapse").rglob("*.jpg"))
            self.assertEqual(result["downloaded"], 3)
            self.assertEqual(result["missing"], 0)
            self.assertEqual(result["last_index"], 4)
            self.assertEqual(len(photos), 3)

    def test_skips_a_missing_photo_and_continues_with_later_records(self) -> None:
        day = "20260809"
        base = datetime(2026, 8, 9, 10, 0, 0)
        catalog = (
            "index,epoch\n"
            + "".join(
                f"{index},{int((base + timedelta(seconds=index)).timestamp())}\n"
                for index in (1, 2, 3)
            )
        ).encode()
        flow = b"timestamp,count\n"
        jpeg = b"\xff\xd8" + b"x" * 2048 + b"\xff\xd9"
        requested_photos: list[int] = []

        def fake_request(url: str, timeout: float, attempts: int = 3) -> bytes:
            if url.endswith("/api/flow.csv"):
                return flow
            if "/api/catalog?" in url:
                return catalog
            if "/api/photo?" in url:
                index = int(url.rsplit("=", 1)[1])
                requested_photos.append(index)
                if index == 2:
                    raise sync_esp32_media.ResourceNotFoundError(url)
                return jpeg
            raise AssertionError(url)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.object(sync_esp32_media, "request_bytes", side_effect=fake_request):
                result = sync_esp32_media.sync(
                    "http://device", root, day, timeout=1.0, all_photos=True
                )
            photos = list((root / "timelapse").rglob("*.jpg"))

        self.assertEqual(requested_photos, [1, 2, 3])
        self.assertEqual(result["downloaded"], 2)
        self.assertEqual(result["missing"], 1)
        self.assertEqual(result["last_index"], 3)
        self.assertEqual(len(photos), 2)


if __name__ == "__main__":
    unittest.main()
