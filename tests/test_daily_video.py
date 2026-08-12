from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import run_daily_video


class DailyVideoTests(unittest.TestCase):
    def test_splits_narration_into_complete_subtitle_sentences(self) -> None:
        sentences = run_daily_video.split_sentences("第一句。第二句！最后一句？")
        self.assertEqual(sentences, ["第一句。", "第二句！", "最后一句？"])
        self.assertEqual(run_daily_video.srt_time(65.432), "00:01:05,432")

    def test_extends_only_body_frames(self) -> None:
        content = """file 'C:/opening-flow-test/00001-opening-120000.jpg'
duration 0.200
file 'C:/opening-flow-test/00002-title.jpg'
duration 1.200
file 'C:/opening-flow-test/00003-120005.jpg'
duration 0.200
file 'C:/opening-flow-test/00004-summary.jpg'
duration 3.000
file 'C:/opening-flow-test/00004-summary.jpg'
"""
        with tempfile.TemporaryDirectory() as temporary:
            concat = Path(temporary) / "concat.txt"
            concat.write_text(content, encoding="utf-8")
            run_daily_video.extend_body_frames(concat, 6.0)
            durations = [
                float(line.split()[1])
                for line in concat.read_text(encoding="utf-8").splitlines()
                if line.startswith("duration ")
            ]
        self.assertEqual(durations, [0.2, 1.2, 1.6, 3.0])

if __name__ == "__main__":
    unittest.main()
