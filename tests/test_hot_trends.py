from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import hot_trends


class HotTrendTests(unittest.TestCase):
    def test_selects_relevant_topic_and_rejects_sensitive_news(self) -> None:
        topics = [
            {"title": "某地地震已致多人遇难", "rank": 1, "hot_value": 12_000_000},
            {"title": "一组数据看新产业动能强劲", "rank": 2, "hot_value": 11_000_000},
            {"title": "开封夜市好吃到停不下来", "rank": 5, "hot_value": 10_000_000},
            {"title": "给AI时代一点手搓实拍的小震撼", "rank": 9, "hot_value": 9_000_000},
        ]

        selected = hot_trends.select_topic(topics)

        self.assertIsNotNone(selected)
        self.assertEqual(selected["title"], "给AI时代一点手搓实拍的小震撼")

    def test_returns_no_topic_when_only_news_is_available(self) -> None:
        topics = [
            {"title": "警方回应案件传闻", "rank": 1, "hot_value": 1},
            {"title": "一组数据看产业发展", "rank": 2, "hot_value": 1},
        ]

        self.assertIsNone(hot_trends.select_topic(topics))

    def test_weather_alert_never_becomes_a_marketing_topic(self) -> None:
        topics = [
            {"title": "台风实拍记录福建风雨", "rank": 1, "hot_value": 10_000_000},
            {"title": "把普通日常拍成高光", "rank": 8, "hot_value": 8_000_000},
        ]

        selected = hot_trends.select_topic(topics)

        self.assertIsNotNone(selected)
        self.assertEqual(selected["title"], "把普通日常拍成高光")

    def test_selects_weather_notice_only_when_store_region_matches(self) -> None:
        topics = [
            {"title": "台风将给福建带来强降雨", "rank": 2, "hot_value": 9_000_000},
            {"title": "广东发布高温预警", "rank": 3, "hot_value": 8_000_000},
        ]

        selected = hot_trends.select_safety_alert(topics, "福建省福州市")

        self.assertIsNotNone(selected)
        self.assertEqual(selected["alert_type"], "台风天气")
        self.assertIn("福建省福州市可能受台风天气影响", selected["notice"])
        self.assertIn("注意出行安全", selected["notice"])

    def test_does_not_select_an_out_of_region_weather_notice(self) -> None:
        topics = [
            {"title": "台风将影响广东沿海", "rank": 1, "hot_value": 9_000_000},
        ]

        self.assertIsNone(
            hot_trends.select_safety_alert(topics, "福建省福州市")
        )

    def test_weather_title_with_casualties_remains_fully_blocked(self) -> None:
        topics = [
            {
                "title": "台风在福建造成多人伤亡",
                "rank": 1,
                "hot_value": 9_000_000,
            },
        ]

        self.assertIsNone(
            hot_trends.select_safety_alert(topics, "福建省福州市")
        )

    def test_manual_topic_writes_review_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            result = hot_trends.prepare_hot_topic(
                output,
                "20260810",
                manual_topic="把普通日常拍成高光",
            )
            stored = json.loads((output / "hot-topic.json").read_text(encoding="utf-8"))
            report = (output / "hot-trends-used.txt").read_text(encoding="utf-8")

        self.assertEqual(result["status"], "manual")
        self.assertEqual(stored["topic"], "把普通日常拍成高光")
        self.assertIn("把普通日常拍成高光", stored["narration_hook"])
        self.assertIn("发布前请再次确认", report)

    def test_network_failure_falls_back_without_stopping_video_job(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            with patch.object(
                hot_trends,
                "fetch_douyin_topics",
                side_effect=TimeoutError("test timeout"),
            ):
                result = hot_trends.prepare_hot_topic(output, "20260810")

        self.assertEqual(result["status"], "fallback")
        self.assertIsNone(result["topic"])
        self.assertEqual(result["narration_hook"], "今天的门店高光，先睹为快。")
        self.assertIn("TimeoutError", result["selection_note"])

    def test_prepare_writes_separate_local_safety_alert_and_candidates(self) -> None:
        topics = [
            {"title": "把普通日常拍成高光", "rank": 5, "hot_value": 8_000_000},
            {"title": "台风将影响福州沿海", "rank": 2, "hot_value": 9_000_000},
            {"title": "广东发布暴雨预警", "rank": 3, "hot_value": 7_000_000},
        ]
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            with patch.object(hot_trends, "fetch_douyin_topics", return_value=topics):
                result = hot_trends.prepare_hot_topic(
                    output,
                    "20260810",
                    store_region="福建省福州市",
                )
            safety = json.loads(
                (output / "safety-alert.json").read_text(encoding="utf-8")
            )
            candidates = json.loads(
                (output / "hot-trend-candidates.json").read_text(encoding="utf-8")
            )

        self.assertEqual(result["topic"], "把普通日常拍成高光")
        self.assertEqual(safety["status"], "selected")
        self.assertEqual(safety["source_title"], "台风将影响福州沿海")
        self.assertEqual(len(candidates["weather_alert_candidates"]), 2)
        self.assertTrue(
            candidates["weather_alert_candidates"][0]["matches_store_region"]
        )

    def test_story_uses_real_video_facts_to_connect_the_topic(self) -> None:
        topic = "给AI时代一点手搓实拍的小震撼"
        facts = {
            "event_count": 53,
            "photo_count": 84,
            "first_time": "17点28分",
            "last_time": "22点19分",
            "peak_hour": 19,
            "peak_count": 21,
            "recorded_hours": 5,
            "peak_anchor_time": "19点13分",
            "peak_group_count": 5,
            "peak_photo_count": 5,
        }

        story = hot_trends.content_story(topic, facts)
        narration = "".join(story["sentences"])

        self.assertEqual(story["angle"], "ai_and_real")
        self.assertIn("从傍晚到深夜", narration)
        self.assertIn("19点到20点", narration)
        self.assertIn("手搓实拍", narration)
        self.assertIn("真实发生", narration)
        self.assertNotIn("系统通过连续照片", narration)
        self.assertNotIn("连续5条客流记录", narration)
        self.assertNotIn("照片", narration)

    def test_story_places_safety_notice_at_the_very_end(self) -> None:
        facts = {
            "event_count": 10,
            "peak_hour": 19,
            "peak_count": 4,
            "peak_group_count": 2,
        }
        notice = "福州市可能受台风天气影响，请关注官方预警并注意安全。"

        story = hot_trends.content_story(None, facts, safety_notice=notice)

        self.assertEqual(story["sentences"][-1], notice)
        self.assertEqual(story["safety_notice"], notice)

    def test_story_uses_the_actual_recording_period(self) -> None:
        facts = {
            "event_count": 34,
            "first_hour": 1,
            "last_hour": 16,
            "peak_hour": 2,
            "peak_count": 19,
            "peak_group_count": 4,
        }

        story = hot_trends.content_story(None, facts)
        narration = "".join(story["sentences"])

        self.assertIn("从凌晨到下午", narration)
        self.assertIn("凌晨的客流相对集中", narration)
        self.assertNotIn("从傍晚到夜里", narration)

    def test_does_not_force_an_unverified_food_topic_into_generic_footage(self) -> None:
        topics = [
            {"title": "开封夜市好吃到停不下来", "rank": 1, "hot_value": 10_000_000}
        ]

        self.assertIsNone(hot_trends.select_topic(topics))


if __name__ == "__main__":
    unittest.main()
