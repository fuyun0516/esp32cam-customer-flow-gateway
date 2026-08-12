from __future__ import annotations

import json
import re
from datetime import datetime
from pathlib import Path
from typing import Any
from urllib.request import Request, urlopen


DOUYIN_HOT_URL = (
    "https://www.douyin.com/aweme/v1/web/hot/search/list/"
    "?device_platform=webapp&aid=6383&channel=channel_pc_web"
)
DOUYIN_HOT_PAGE = "https://www.douyin.com/hot"

# These subjects are never suitable for either shop promotion or an automated
# safety reminder. Weather alerts are handled separately below.
HARD_BLOCKED_TERMS = (
    "遇害",
    "死亡",
    "去世",
    "逝世",
    "遇难",
    "伤亡",
    "地震",
    "灾情",
    "事故",
    "坠毁",
    "爆炸",
    "火灾",
    "嫌犯",
    "嫌疑人",
    "公安",
    "警方",
    "举报",
    "辟谣",
    "失踪",
    "战争",
    "冲突",
    "袭击",
    "病亡",
    "医院",
    "离婚",
    "出轨",
    "封杀",
    "塌房",
    "争议",
    "道歉",
)

# These topics must never be blended into promotional copy. They may only
# produce a short closing safety notice when the headline names the configured
# shop region and contains none of the hard-blocked terms above.
WEATHER_ALERT_TERMS = (
    "台风",
    "暴雨",
    "强降雨",
    "洪水",
    "防汛",
    "内涝",
    "雷暴",
    "雷雨",
    "强对流",
    "大风",
    "高温",
    "寒潮",
    "暴雪",
    "冰冻",
    "道路结冰",
)

# Backward-compatible combined name for callers that only need to know whether
# a title is unsuitable for routine marketing.
UNSAFE_TERMS = HARD_BLOCKED_TERMS + WEATHER_ALERT_TERMS

# Higher values mean the topic can be connected to real shop footage without
# forcing an unrelated news story into the narration.
RELEVANCE_WEIGHTS = {
    "实拍": 32,
    "记录": 30,
    "镜头": 28,
    "高光": 26,
    "日常": 24,
    "日记": 22,
    "生活": 18,
    "城市": 16,
    "创意": 14,
    "夏天": 14,
    "暑假": 12,
    "周末": 10,
    "秋": 10,
    "AI": 8,
}


def fetch_douyin_topics(timeout: float = 12.0) -> list[dict[str, Any]]:
    request = Request(
        DOUYIN_HOT_URL,
        headers={
            "User-Agent": (
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 Chrome/138.0.0.0 Safari/537.36"
            ),
            "Referer": DOUYIN_HOT_PAGE,
            "Accept": "application/json,text/plain,*/*",
        },
    )
    with urlopen(request, timeout=timeout) as response:
        raw = response.read()
    payload = json.loads(raw.decode("utf-8"))
    if payload.get("status_code") not in (None, 0):
        raise ValueError(f"Douyin returned status_code={payload['status_code']}")
    word_list = payload.get("data", {}).get("word_list", [])
    if not isinstance(word_list, list):
        raise ValueError("Douyin response does not contain data.word_list")

    topics: list[dict[str, Any]] = []
    for fallback_rank, item in enumerate(word_list, 1):
        if not isinstance(item, dict):
            continue
        title = re.sub(r"\s+", " ", str(item.get("word", ""))).strip()
        if not title:
            continue
        topics.append(
            {
                "title": title,
                "rank": _integer(item.get("position"), fallback_rank),
                "hot_value": _integer(item.get("hot_value"), 0),
            }
        )
    if not topics:
        raise ValueError("Douyin hot list is empty")
    return topics


def _integer(value: object, default: int) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def topic_relevance(title: str) -> int:
    return sum(weight for term, weight in RELEVANCE_WEIGHTS.items() if term in title)


def topic_is_hard_blocked(title: str) -> bool:
    return any(term in title for term in HARD_BLOCKED_TERMS)


def topic_is_weather_alert(title: str) -> bool:
    return any(term in title for term in WEATHER_ALERT_TERMS)


def topic_is_safe(title: str) -> bool:
    return (
        4 <= len(title) <= 40
        and not topic_is_hard_blocked(title)
        and not topic_is_weather_alert(title)
    )


def _marketing_candidates(
    topics: list[dict[str, Any]],
) -> list[tuple[int, int, int, dict[str, Any]]]:
    candidates = []
    for topic in topics:
        title = str(topic.get("title", "")).strip()
        relevance = topic_relevance(title)
        if relevance <= 0 or not topic_is_safe(title):
            continue
        rank = _integer(topic.get("rank"), 999)
        hot_value = _integer(topic.get("hot_value"), 0)
        score = relevance * 100 - rank
        candidates.append((score, hot_value, -rank, topic))
    return candidates


def select_topic(topics: list[dict[str, Any]]) -> dict[str, Any] | None:
    candidates = _marketing_candidates(topics)
    return (
        max(candidates, default=None, key=lambda item: item[:3])[3]
        if candidates
        else None
    )


def region_terms(store_region: str | None) -> tuple[str, ...]:
    if not store_region:
        return ()
    normalized = re.sub(r"[，,、/;；|]+", " ", store_region).strip()
    terms: set[str] = set()
    for piece in normalized.split():
        if len(piece) >= 2:
            terms.add(piece)
        stripped = re.sub(
            r"(?:特别行政区|自治区|自治州|省|市|地区|盟|县|区)$", "", piece
        )
        if len(stripped) >= 2:
            terms.add(stripped)
    for match in re.finditer(
        r"([\u4e00-\u9fff]{2,12}?)(?:特别行政区|自治区|自治州|省|市|地区|盟|县|区)",
        normalized,
    ):
        if len(match.group(1)) >= 2:
            terms.add(match.group(1))
    return tuple(sorted(terms, key=lambda term: (-len(term), term)))


def title_matches_region(title: str, store_region: str | None) -> bool:
    return any(term in title for term in region_terms(store_region))


def _weather_alert_type(title: str) -> str:
    if "台风" in title:
        return "台风天气"
    if any(term in title for term in ("暴雨", "强降雨", "洪水", "防汛", "内涝")):
        return "强降雨天气"
    if any(term in title for term in ("雷暴", "雷雨", "强对流", "大风")):
        return "强对流天气"
    if "高温" in title:
        return "高温天气"
    if any(term in title for term in ("寒潮", "暴雪", "冰冻", "道路结冰")):
        return "低温雨雪天气"
    return "恶劣天气"


def safety_notice_for(
    title: str, store_region: str, store_name: str | None = None
) -> str:
    alert_type = _weather_alert_type(title)
    if alert_type == "高温天气":
        advice = "关注官方预警，注意防暑补水和出行安全"
    elif alert_type == "低温雨雪天气":
        advice = "关注官方预警，注意保暖和道路出行安全"
    else:
        advice = "关注官方预警，减少不必要外出，注意出行安全"
    speaker = (store_name or "").strip()
    reminder = f"{speaker}提醒大家" if speaker else "请大家"
    return f"{store_region}可能受{alert_type}影响，{reminder}{advice}。"


def select_safety_alert(
    topics: list[dict[str, Any]], store_region: str | None,
    store_name: str | None = None,
) -> dict[str, Any] | None:
    if not region_terms(store_region):
        return None
    candidates: list[tuple[int, int, dict[str, Any]]] = []
    for topic in topics:
        title = str(topic.get("title", "")).strip()
        if (
            not topic_is_weather_alert(title)
            or topic_is_hard_blocked(title)
            or not title_matches_region(title, store_region)
        ):
            continue
        rank = _integer(topic.get("rank"), 999)
        hot_value = _integer(topic.get("hot_value"), 0)
        candidates.append((-rank, hot_value, topic))
    if not candidates:
        return None
    selected = max(candidates, key=lambda item: item[:2])[2]
    title = str(selected.get("title", "")).strip()
    return {
        **selected,
        "alert_type": _weather_alert_type(title),
        "notice": safety_notice_for(
            title, str(store_region).strip(), store_name=store_name
        ),
    }


def narration_hook(title: str) -> str:
    if any(term in title for term in ("实拍", "记录", "镜头", "日记", "高光")):
        return f"抖音热榜正在聊《{title}》，今天我们也把镜头留给门店里真实发生的每一刻。"
    if "AI" in title:
        return f"抖音热榜正在聊《{title}》，AI负责打开想象，镜头负责留下门店今天的真实人气。"
    if any(term in title for term in ("城市", "夜市", "美食", "好吃", "旅行")):
        return f"抖音热榜正在聊《{title}》，城市的烟火气，也藏在每一次真实到店里。"
    if any(term in title for term in ("夏天", "暑假", "秋", "周末")):
        return f"抖音热榜正在聊《{title}》，属于这个时节的门店日常，也被镜头认真记录下来。"
    return f"抖音热榜正在聊《{title}》，今天也用我们的方式，记录门店一天的真实高光。"


def compact_topic(title: str, max_chars: int = 18) -> str:
    """Keep repeated on-screen references short enough for a vertical frame."""
    normalized = re.sub(r"\s+", " ", title).strip()
    if len(normalized) <= max_chars:
        return normalized
    return normalized[: max_chars - 1] + "…"


def topic_bridge(title: str | None, angle: str | None) -> str:
    """Return the small idea used in speech, while keeping the full title in the report."""
    if angle in {"ai_and_real", "real_capture"}:
        return "手搓实拍"
    if angle == "daily_rhythm":
        return "普通日常"
    if angle == "seasonal_daily":
        return compact_topic(title or "当下这个时节", 10)
    if angle == "creative_record":
        return "创意记录"
    return compact_topic(title or "", 10)


def topic_angle(title: str | None) -> str | None:
    if not title:
        return None
    real_terms = ("实拍", "记录", "镜头", "日记", "高光")
    if "AI" in title and any(term in title for term in real_terms):
        return "ai_and_real"
    if any(term in title for term in real_terms):
        return "real_capture"
    if any(term in title for term in ("日常", "生活", "城市")):
        return "daily_rhythm"
    if any(term in title for term in ("夏天", "暑假", "秋", "周末")):
        return "seasonal_daily"
    if "AI" in title or "创意" in title:
        return "creative_record"
    return None


def _hour_fact(
    facts: dict[str, object], hour_key: str, time_key: str, default: int
) -> int:
    try:
        return max(0, min(23, int(facts[hour_key])))
    except (KeyError, TypeError, ValueError):
        match = re.match(r"\s*(\d{1,2})", str(facts.get(time_key) or ""))
        return max(0, min(23, int(match.group(1)))) if match else default


def day_period(hour: int) -> str:
    if hour <= 5:
        return "凌晨"
    if hour <= 9:
        return "早上"
    if hour <= 11:
        return "上午"
    if hour <= 13:
        return "中午"
    if hour <= 16:
        return "下午"
    if hour <= 18:
        return "傍晚"
    if hour <= 21:
        return "晚上"
    return "深夜"


def content_story(
    topic: str | None,
    facts: dict[str, object],
    safety_notice: str | None = None,
) -> dict[str, object]:
    """Use the footage profile internally and return only publishable copy."""
    event_count = int(facts["event_count"])
    peak_hour = int(facts["peak_hour"])
    peak_count = int(facts["peak_count"])
    peak_anchor_time = str(facts.get("peak_anchor_time") or f"{peak_hour}点左右")
    peak_group_count = int(facts.get("peak_group_count") or 0)
    first_hour = _hour_fact(facts, "first_hour", "first_time", peak_hour)
    last_hour = _hour_fact(facts, "last_hour", "last_time", peak_hour)
    first_period = day_period(first_hour)
    last_period = day_period(last_hour)
    peak_period = day_period(peak_hour)
    angle = topic_angle(topic)
    bridge = topic_bridge(topic, angle)

    opening = (
        f"{first_period}时段，门店里的来来往往被镜头陆续记录下来。"
        if first_period == last_period
        else f"从{first_period}到{last_period}，门店里的来来往往被镜头陆续记录下来。"
    )
    activity = (
        f"{peak_period}的客流相对集中，镜头里的人影接连出现。"
        if peak_group_count > 1
        else f"{peak_period}的客流相对集中，门店留下了几次真实经过。"
    )
    peak_summary = f"今天最热闹的时间，是{peak_hour}点到{peak_hour + 1}点。"
    closing = "每一次经过，都是门店今天留下的真实人气。"

    trend_sentence: str | None = None
    if angle == "ai_and_real":
        trend_sentence = (
            f"今天大家在聊“{bridge}”，这里没有设计，"
            "只有门店真实发生的人来人往。"
        )
        reason = "热点中的手搓实拍与高峰段落的连续现场画面直接对应。"
    elif angle == "real_capture":
        trend_sentence = (
            f"今天大家在聊“{bridge}”，镜头里的来来往往，"
            "就是门店自己的真实现场。"
        )
        reason = "热点强调实拍、记录或镜头，并指向本片的高峰连续画面。"
    elif angle == "daily_rhythm":
        trend_sentence = (
            f"今天大家在聊“{bridge}”，镜头里的这些来来往往，"
            "就是这家店自己的日常。"
        )
        reason = "热点关注日常，能够由本片的完整记录时段自然承接。"
    elif angle == "seasonal_daily":
        trend_sentence = (
            f"今天大家在聊“{bridge}”，这个时节的热闹不需要安排，"
            "镜头里的每次经过都是真实发生。"
        )
        reason = "热点属于季节或时间话题，并与本片的实际日期和高峰时段对应。"
    elif angle == "creative_record":
        trend_sentence = (
            f"今天大家在聊“{bridge}”，这条片的内容很简单，"
            "把真实经过认真留下。"
        )
        reason = "热点只作为表达角度，具体叙事由高峰段落和客流事实构成。"
    else:
        reason = "当前热点与视频中可确认的时间、客流和连续画面没有自然联系，未写入成片。"

    sentences = [opening, activity]
    if trend_sentence:
        sentences.append(trend_sentence)
    sentences.extend([peak_summary, closing])
    normalized_notice = (safety_notice or "").strip()
    if normalized_notice:
        sentences.append(normalized_notice)

    return {
        "angle": angle or "no_forced_match",
        "match_reason": reason,
        "sentences": sentences,
        "safety_notice": normalized_notice or None,
        "analysis": {
            "event_count": event_count,
            "peak_hour": peak_hour,
            "peak_count": peak_count,
            "peak_anchor_time": peak_anchor_time,
            "peak_group_count": peak_group_count,
            "first_hour": first_hour,
            "last_hour": last_hour,
        },
    }


def prepare_hot_topic(
    output: Path,
    video_day: str,
    *,
    manual_topic: str | None = None,
    store_name: str | None = None,
    store_region: str | None = None,
    manual_safety_notice: str | None = None,
    enabled: bool = True,
    timeout: float = 12.0,
) -> dict[str, Any]:
    fetched_at = datetime.now().astimezone().isoformat(timespec="seconds")
    topics: list[dict[str, Any]] = []
    fetch_error: str | None = None
    needs_automatic_marketing = not (manual_topic and manual_topic.strip())
    needs_automatic_safety = bool(region_terms(store_region)) and not (
        manual_safety_notice and manual_safety_notice.strip()
    )
    if enabled and (needs_automatic_marketing or needs_automatic_safety):
        try:
            topics = fetch_douyin_topics(timeout)
        except Exception as error:
            fetch_error = f"{type(error).__name__}: {error}"

    result: dict[str, Any] = {
        "video_day": video_day,
        "source_name": "抖音热榜",
        "source_url": DOUYIN_HOT_PAGE,
        "fetched_at": fetched_at,
    }

    if manual_topic and manual_topic.strip():
        title = manual_topic.strip()
        if topic_is_safe(title):
            result.update(
                {
                    "status": "manual",
                    "topic": title,
                    "angle": topic_angle(title),
                    "rank": None,
                    "hot_value": None,
                    "narration_hook": narration_hook(title),
                    "selection_note": "使用命令行指定并已人工确认的话题。",
                }
            )
        else:
            result.update(
                _fallback("指定话题属于灾害、伤亡、案件或争议内容，未用于营销文案。")
            )
    elif not enabled:
        result.update(_fallback("已关闭热点读取。"))
    elif fetch_error:
        result.update(_fallback(f"热点读取失败：{fetch_error}"))
    else:
        selected = select_topic(topics)
        if selected is None:
            result.update(_fallback("当前热榜没有适合门店宣传的安全生活类话题。"))
        else:
            title = str(selected["title"])
            result.update(
                {
                    "status": "selected",
                    "topic": title,
                    "angle": topic_angle(title),
                    "rank": _integer(selected.get("rank"), 0),
                    "hot_value": _integer(selected.get("hot_value"), 0),
                    "narration_hook": narration_hook(title),
                    "selection_note": (
                        "气象灾害不会用于营销；伤亡、案件和争议话题已完全排除。"
                    ),
                }
            )

    safety_alert = _prepare_safety_alert(
        topics,
        video_day,
        fetched_at,
        store_region=store_region,
        store_name=store_name,
        manual_notice=manual_safety_notice,
        enabled=enabled,
        fetch_error=fetch_error,
    )
    result["safety_alert"] = safety_alert
    candidate_report = _candidate_report(
        topics,
        video_day,
        fetched_at,
        store_region=store_region,
        enabled=enabled,
        fetch_error=fetch_error,
    )

    output.mkdir(parents=True, exist_ok=True)
    (output / "hot-topic.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    (output / "hot-trends-used.txt").write_text(
        _human_readable(result), encoding="utf-8"
    )
    (output / "safety-alert.json").write_text(
        json.dumps(safety_alert, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    (output / "safety-alert.txt").write_text(
        _safety_human_readable(safety_alert), encoding="utf-8"
    )
    (output / "hot-trend-candidates.json").write_text(
        json.dumps(candidate_report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    (output / "hot-trend-candidates.txt").write_text(
        _candidate_human_readable(candidate_report), encoding="utf-8"
    )
    return result


def _normalize_notice(notice: str) -> str:
    normalized = re.sub(r"\s+", " ", notice).strip()
    if normalized and normalized[-1] not in "。！？!?":
        normalized += "。"
    return normalized


def _prepare_safety_alert(
    topics: list[dict[str, Any]],
    video_day: str,
    fetched_at: str,
    *,
    store_region: str | None,
    store_name: str | None,
    manual_notice: str | None,
    enabled: bool,
    fetch_error: str | None,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "video_day": video_day,
        "source_name": "抖音热榜",
        "source_url": DOUYIN_HOT_PAGE,
        "fetched_at": fetched_at,
        "store_region": (store_region or "").strip() or None,
        "source_title": None,
        "rank": None,
        "alert_type": None,
        "notice": None,
        "included_in_video": False,
    }
    if manual_notice and manual_notice.strip():
        notice = _normalize_notice(manual_notice)
        if topic_is_hard_blocked(notice):
            result.update(
                {
                    "status": "rejected",
                    "selection_note": "人工提醒包含伤亡、案件或其他禁止内容，未写入视频。",
                }
            )
        else:
            result.update(
                {
                    "status": "manual",
                    "notice": notice,
                    "selection_note": "使用命令行指定并已人工确认的结尾安全提醒。",
                }
            )
        return result
    if not enabled:
        result.update(
            {
                "status": "disabled",
                "selection_note": "已关闭热榜读取，没有自动生成气象安全提醒。",
            }
        )
        return result
    if fetch_error:
        result.update(
            {
                "status": "unavailable",
                "selection_note": f"热榜读取失败，未生成安全提醒：{fetch_error}",
            }
        )
        return result
    if not region_terms(store_region):
        result.update(
            {
                "status": "needs_region",
                "selection_note": "尚未配置店铺所在省市，气象灾害只列为候选，不会自动播报。",
            }
        )
        return result

    selected = select_safety_alert(topics, store_region, store_name=store_name)
    if selected is None:
        result.update(
            {
                "status": "not_selected",
                "selection_note": "没有发现明确影响店铺所在地区的安全气象话题。",
            }
        )
        return result

    result.update(
        {
            "status": "selected",
            "source_title": str(selected.get("title", "")),
            "rank": _integer(selected.get("rank"), 0),
            "alert_type": selected.get("alert_type"),
            "notice": selected.get("notice"),
            "selection_note": (
                "标题包含店铺地区和气象风险；该内容仅作为结尾安全提醒，不参与营销文案。"
            ),
        }
    )
    return result


def _candidate_report(
    topics: list[dict[str, Any]],
    video_day: str,
    fetched_at: str,
    *,
    store_region: str | None,
    enabled: bool,
    fetch_error: str | None,
) -> dict[str, Any]:
    marketing = []
    for score, _, _, topic in sorted(
        _marketing_candidates(topics), key=lambda item: item[:3], reverse=True
    )[:10]:
        title = str(topic.get("title", "")).strip()
        marketing.append(
            {
                "title": title,
                "rank": _integer(topic.get("rank"), 0),
                "hot_value": _integer(topic.get("hot_value"), 0),
                "relevance": topic_relevance(title),
                "score": score,
            }
        )

    weather = []
    for topic in sorted(topics, key=lambda item: _integer(item.get("rank"), 999)):
        title = str(topic.get("title", "")).strip()
        if not topic_is_weather_alert(title) or topic_is_hard_blocked(title):
            continue
        weather.append(
            {
                "title": title,
                "rank": _integer(topic.get("rank"), 0),
                "hot_value": _integer(topic.get("hot_value"), 0),
                "alert_type": _weather_alert_type(title),
                "matches_store_region": title_matches_region(title, store_region),
            }
        )

    return {
        "video_day": video_day,
        "source_name": "抖音热榜",
        "source_url": DOUYIN_HOT_PAGE,
        "fetched_at": fetched_at,
        "store_region": (store_region or "").strip() or None,
        "status": "disabled" if not enabled else ("unavailable" if fetch_error else "ready"),
        "fetch_error": fetch_error,
        "marketing_candidates": marketing,
        "weather_alert_candidates": weather,
        "review_note": (
            "营销候选与气象安全候选已分开；含伤亡、案件、战争或争议内容的标题不会进入任何候选。"
        ),
    }


def _candidate_human_readable(report: dict[str, Any]) -> str:
    marketing = report.get("marketing_candidates") or []
    weather = report.get("weather_alert_candidates") or []
    lines = [
        f"视频日期：{report['video_day']}",
        f"获取时间：{report['fetched_at']}",
        f"店铺地区：{report.get('store_region') or '未配置'}",
        f"状态：{report['status']}",
        "",
        "营销热点候选：",
    ]
    if marketing:
        lines.extend(
            f"- 第 {item['rank']} 名：{item['title']}" for item in marketing
        )
    else:
        lines.append("- 无")
    lines.extend(["", "气象安全提醒候选（不会用于营销）："])
    if weather:
        for item in weather:
            match = "本地匹配" if item["matches_store_region"] else "未匹配店铺地区"
            lines.append(f"- 第 {item['rank']} 名：{item['title']}（{match}）")
    else:
        lines.append("- 无")
    lines.extend(["", str(report["review_note"])])
    if report.get("fetch_error"):
        lines.append(f"读取错误：{report['fetch_error']}")
    return "\n".join(lines) + "\n"


def _safety_human_readable(result: dict[str, Any]) -> str:
    return (
        f"视频日期：{result['video_day']}\n"
        f"店铺地区：{result.get('store_region') or '未配置'}\n"
        f"状态：{result['status']}\n"
        f"来源标题：{result.get('source_title') or '无'}\n"
        f"结尾提醒：{result.get('notice') or '未启用'}\n"
        f"已写入视频：{'是' if result.get('included_in_video') else '否'}\n"
        f"说明：{result['selection_note']}\n"
    )


def _fallback(reason: str) -> dict[str, Any]:
    return {
        "status": "fallback",
        "topic": None,
        "angle": None,
        "rank": None,
        "hot_value": None,
        "narration_hook": "今天的门店高光，先睹为快。",
        "selection_note": reason,
    }


def _human_readable(result: dict[str, Any]) -> str:
    topic = result.get("topic") or "未使用具体热点，采用通用门店文案"
    rank = result.get("rank")
    rank_text = f"第 {rank} 名" if rank else "无"
    safety = result.get("safety_alert")
    safety_notice = (
        safety.get("notice")
        if isinstance(safety, dict) and safety.get("notice")
        else "未启用"
    )
    content_sentences = result.get("content_sentences")
    actual_copy = (
        "".join(str(sentence) for sentence in content_sentences)
        if isinstance(content_sentences, list) and content_sentences
        else "成片生成后写入"
    )
    return (
        f"视频日期：{result['video_day']}\n"
        f"获取时间：{result['fetched_at']}\n"
        f"来源：{result['source_name']}\n"
        f"来源地址：{result['source_url']}\n"
        f"状态：{result['status']}\n"
        f"内容角度：{result.get('angle') or '不强行匹配'}\n"
        f"采用话题：{topic}\n"
        f"热榜排名：{rank_text}\n"
        f"结尾安全提醒：{safety_notice}\n"
        f"实际视频文案：{actual_copy}\n"
        f"说明：{result['selection_note']}\n"
        "发布前请再次确认话题仍在热榜且与店铺内容相符。\n"
    )


def record_content_story(
    output: Path,
    story: dict[str, object],
    facts: dict[str, object],
) -> None:
    """Persist why the selected trend was or was not used in this edit."""
    path = output / "hot-topic.json"
    try:
        result = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return
    if not isinstance(result, dict):
        return
    safety = result.get("safety_alert")
    if isinstance(safety, dict):
        safety["included_in_video"] = bool(story.get("safety_notice"))
    result.update(
        {
            "content_angle": story.get("angle"),
            "content_match_reason": story.get("match_reason"),
            "content_facts": facts,
            "content_sentences": story.get("sentences"),
        }
    )
    path.write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    (output / "hot-trends-used.txt").write_text(
        _human_readable(result)
        + f"内容匹配说明：{result.get('content_match_reason', '')}\n",
        encoding="utf-8",
    )
    safety_path = output / "safety-alert.json"
    if isinstance(safety, dict):
        safety_path.write_text(
            json.dumps(safety, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        (output / "safety-alert.txt").write_text(
            _safety_human_readable(safety), encoding="utf-8"
        )


def load_hot_topic(output: Path) -> dict[str, Any]:
    path = output / "hot-topic.json"
    if not path.exists():
        return _fallback("未找到热点结果文件。")
    try:
        result = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return _fallback("热点结果文件无法读取。")
    if not isinstance(result, dict) or not result.get("narration_hook"):
        return _fallback("热点结果文件内容不完整。")
    return result


def load_safety_alert(output: Path) -> dict[str, Any]:
    path = output / "safety-alert.json"
    if not path.exists():
        return {"status": "missing", "notice": None}
    try:
        result = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {"status": "invalid", "notice": None}
    if not isinstance(result, dict):
        return {"status": "invalid", "notice": None}
    return result
