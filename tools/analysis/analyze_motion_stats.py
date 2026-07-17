#!/usr/bin/env python3
"""量化 AlertGateway [MotionStats] 日志中的框抖动、跳变与等效滞后。"""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


PAIR_KEYS = {"raw_center", "smooth_center", "raw_size", "smooth_size", "raw_delta", "smooth_delta"}
INT_KEYS = {"frame", "pts_ms", "matched_active", "stable_tracks", "dt_ms", "raw_reversals", "created", "lost"}
FLOAT_KEYS = {
    "center_gap", "center_gap_ratio", "delta_gap", "raw_step_ratio", "smooth_step_ratio",
    "raw_size_step", "smooth_size_step",
}


@dataclass(frozen=True)
class Sample:
    frame: int
    pts_ms: int
    dt_ms: int
    stable_tracks: int
    raw_center: tuple[float, float]
    smooth_center: tuple[float, float]
    raw_delta: tuple[float, float]
    smooth_delta: tuple[float, float]
    raw_step_ratio: float
    smooth_step_ratio: float
    raw_size_step: float
    smooth_size_step: float
    raw_reversals: int
    created: int
    lost: int
    active_ids: tuple[int, ...]
    stable_ids: tuple[int, ...]


def percentile(values: Iterable[float], q: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    position = (len(ordered) - 1) * q
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return ordered[lower]
    return ordered[lower] * (upper - position) + ordered[upper] * (position - lower)


def rms(values: Iterable[float]) -> float:
    items = list(values)
    return math.sqrt(sum(value * value for value in items) / len(items)) if items else math.nan


def safe_change(before: float, after: float) -> float:
    if not math.isfinite(before) or abs(before) < 1e-12:
        return math.nan
    return (after - before) / before * 100.0


def parse_ids(value: str) -> tuple[int, ...]:
    return tuple(int(item) for item in value.split(",") if item)


def parse_log(path: Path) -> list[Sample]:
    samples: list[Sample] = []
    for line_number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        marker = line.find("[MotionStats]")
        if marker < 0:
            continue
        fields = dict(re.findall(r"([a-z_]+)=([^\s]*)", line[marker:]))
        required = {
            "frame", "pts_ms", "dt_ms", "stable_tracks", "raw_center", "smooth_center",
            "raw_delta", "smooth_delta", "raw_step_ratio", "smooth_step_ratio",
            "raw_size_step", "smooth_size_step", "raw_reversals", "created", "lost",
            "active_ids", "stable_ids",
        }
        missing = sorted(required - fields.keys())
        if missing:
            raise ValueError(f"{path}:{line_number}: MotionStats 缺少字段: {', '.join(missing)}")

        parsed: dict[str, object] = {}
        try:
            for key in PAIR_KEYS & fields.keys():
                first, second = fields[key].split(",", 1)
                parsed[key] = (float(first), float(second))
            for key in INT_KEYS & fields.keys():
                parsed[key] = int(fields[key])
            for key in FLOAT_KEYS & fields.keys():
                parsed[key] = float(fields[key])
            parsed["active_ids"] = parse_ids(fields["active_ids"])
            parsed["stable_ids"] = parse_ids(fields["stable_ids"])
            samples.append(Sample(**{key: parsed[key] for key in Sample.__dataclass_fields__}))
        except (TypeError, ValueError) as exc:
            raise ValueError(f"{path}:{line_number}: MotionStats 字段格式错误: {exc}") from exc
    if not samples:
        raise ValueError(f"{path}: 未找到可用的 [MotionStats]；请启用 track_motion_stats_logging")
    return samples


def parse_runtime_stats(path: Path) -> dict[str, float]:
    encode_fps: list[float] = []
    bitrates: list[float] = []
    pts_deltas: list[float] = []
    counters = {key: 0.0 for key in ("enc_drop", "put_fail", "out_drop", "write_fail", "infer_drop")}
    for line in path.read_text(errors="replace").splitlines():
        for marker in ("[PullStats]", "[EncodeStats]", "[StreamStats]"):
            start = line.find(marker)
            if start < 0:
                continue
            end = line.find("[", start + len(marker))
            section = line[start:end if end >= 0 else None]
            fields = dict(re.findall(r"([a-z_]+)=(-?[0-9]+(?:\.[0-9]+)?)", section))
            if marker == "[EncodeStats]":
                if "packet_fps" in fields:
                    encode_fps.append(float(fields["packet_fps"]))
                if "bitrate_kbps" in fields:
                    bitrates.append(float(fields["bitrate_kbps"]))
                if "pts_delta_ms" in fields:
                    pts_deltas.append(float(fields["pts_delta_ms"]))
            for key in counters.keys() & fields.keys():
                counters[key] += float(fields[key])
    return {
        "encode_fps_p50": percentile(encode_fps, 0.50),
        "bitrate_kbps_p50": percentile(bitrates, 0.50),
        "pts_delta_ms_p90": percentile(pts_deltas, 0.90),
        **counters,
    }


def direction_reversal_rate(samples: list[Sample], smooth: bool) -> float:
    reversals = 0
    eligible = 0
    previous: tuple[float, float] | None = None
    previous_ids: tuple[int, ...] | None = None
    for sample in samples:
        delta = sample.smooth_delta if smooth else sample.raw_delta
        ids = sample.stable_ids
        magnitude = math.hypot(*delta)
        if previous is not None and ids == previous_ids and magnitude > 0.5 and math.hypot(*previous) > 0.5:
            eligible += 1
            if delta[0] * previous[0] + delta[1] * previous[1] < 0.0:
                reversals += 1
        previous, previous_ids = delta, ids
    return reversals / eligible * 100.0 if eligible else math.nan


def summarize(samples: list[Sample], stationary_speed: float, moving_speed: float,
              runtime: dict[str, float] | None = None) -> dict[str, object]:
    # 过滤启动/退出时的异常短间隔以及断流重连造成的长间隔，避免把生命周期事件算成运动。
    valid = [sample for sample in samples if 10 <= sample.dt_ms <= 250 and sample.stable_tracks > 0]
    if not valid:
        raise ValueError("MotionStats 中没有 dt_ms > 0 的连续稳定轨迹样本")

    speeds = [sample.raw_step_ratio * 1000.0 / sample.dt_ms for sample in valid]
    stationary = [sample for sample, speed in zip(valid, speeds) if speed <= stationary_speed]
    moving = [sample for sample, speed in zip(valid, speeds) if speed >= moving_speed]

    raw_static = [sample.raw_step_ratio for sample in stationary]
    smooth_static = [sample.smooth_step_ratio for sample in stationary]
    raw_size_static = [sample.raw_size_step for sample in stationary]
    smooth_size_static = [sample.smooth_size_step for sample in stationary]

    equivalent_lag_ms: list[float] = []
    for sample in moving:
        raw_step = math.hypot(*sample.raw_delta)
        if raw_step <= 1e-6:
            continue
        offset_x = sample.raw_center[0] - sample.smooth_center[0]
        offset_y = sample.raw_center[1] - sample.smooth_center[1]
        along_motion = (offset_x * sample.raw_delta[0] + offset_y * sample.raw_delta[1]) / raw_step
        if along_motion > 0.0:
            equivalent_lag_ms.append(along_motion / raw_step * sample.dt_ms)

    raw_static_rms = rms(raw_static)
    smooth_static_rms = rms(smooth_static)
    raw_size_rms = rms(raw_size_static)
    smooth_size_rms = rms(smooth_size_static)
    duration_sec = max(0.0, (valid[-1].pts_ms - valid[0].pts_ms) / 1000.0)
    minutes = max(duration_sec / 60.0, 1e-9)
    track_set_changes = sum(
        current.active_ids != previous.active_ids for previous, current in zip(valid, valid[1:])
    )

    warnings: list[str] = []
    if len(stationary) < 20:
        warnings.append(f"低速样本仅 {len(stationary)} 个，静止抖动指标可信度不足")
    if len(moving) < 20:
        warnings.append(f"运动样本仅 {len(moving)} 个，滞后指标可信度不足")
    if duration_sec < 20.0:
        warnings.append(f"有效时长仅 {duration_sec:.1f}s，建议同一片段至少采集 60s")
    runtime = runtime or {}
    if runtime.get("enc_drop", 0.0) > 0:
        warnings.append(f"编码输入丢帧累计 {runtime['enc_drop']:.0f}，两组推理采样可能不完全一致")
    failures = sum(runtime.get(key, 0.0) for key in ("put_fail", "out_drop", "write_fail"))
    if failures > 0:
        warnings.append(f"编码/推流失败计数累计 {failures:.0f}")

    return {
        "samples": len(valid),
        "discarded_timing_samples": len(samples) - len(valid),
        "duration_sec": duration_sec,
        "stationary_samples": len(stationary),
        "moving_samples": len(moving),
        "stationary_speed_threshold_diag_per_sec": stationary_speed,
        "moving_speed_threshold_diag_per_sec": moving_speed,
        "static_center_raw_rms_pct_diag": raw_static_rms * 100.0,
        "static_center_smooth_rms_pct_diag": smooth_static_rms * 100.0,
        "static_center_smoothing_reduction_pct": -safe_change(raw_static_rms, smooth_static_rms),
        "static_center_smooth_p90_pct_diag": percentile(smooth_static, 0.90) * 100.0,
        "static_size_raw_rms_pct": raw_size_rms * 100.0,
        "static_size_smooth_rms_pct": smooth_size_rms * 100.0,
        "static_size_smoothing_reduction_pct": -safe_change(raw_size_rms, smooth_size_rms),
        "moving_equivalent_lag_p50_ms": percentile(equivalent_lag_ms, 0.50),
        "moving_equivalent_lag_p90_ms": percentile(equivalent_lag_ms, 0.90),
        "moving_smooth_step_p90_pct_diag": percentile(
            (sample.smooth_step_ratio for sample in moving), 0.90
        ) * 100.0,
        "overall_smooth_step_p95_pct_diag": percentile(
            (sample.smooth_step_ratio for sample in valid), 0.95
        ) * 100.0,
        "raw_direction_reversal_rate_pct": direction_reversal_rate(valid, False),
        "smooth_direction_reversal_rate_pct": direction_reversal_rate(valid, True),
        "track_set_change_rate_pct": track_set_changes / max(1, len(valid) - 1) * 100.0,
        "created_tracks_per_min": sum(sample.created for sample in valid) / minutes,
        "lost_tracks_per_min": sum(sample.lost for sample in valid) / minutes,
        "mean_stable_tracks": statistics.fmean(sample.stable_tracks for sample in valid),
        **runtime,
        "warnings": warnings,
    }


def fmt(value: object, digits: int = 2) -> str:
    if not isinstance(value, (int, float)) or not math.isfinite(float(value)):
        return "N/A"
    return f"{float(value):.{digits}f}"


def render_report(labels: list[str], reports: list[dict[str, object]]) -> str:
    lines = ["# 检测框运动统计", ""]
    rows = [
        ("有效样本", "samples", ""),
        ("有效时长", "duration_sec", "s"),
        ("低速/运动样本", None, ""),
        ("低速中心抖动 RMS", "static_center_smooth_rms_pct_diag", "%框对角线"),
        ("低速中心抖动 P90", "static_center_smooth_p90_pct_diag", "%框对角线"),
        ("相对原始框中心抑噪", "static_center_smoothing_reduction_pct", "%"),
        ("低速尺寸抖动 RMS", "static_size_smooth_rms_pct", "%"),
        ("相对原始框尺寸抑噪", "static_size_smoothing_reduction_pct", "%"),
        ("运动等效滞后 P50", "moving_equivalent_lag_p50_ms", "ms"),
        ("运动等效滞后 P90", "moving_equivalent_lag_p90_ms", "ms"),
        ("运动单步跳变 P90", "moving_smooth_step_p90_pct_diag", "%框对角线"),
        ("全段单步跳变 P95", "overall_smooth_step_p95_pct_diag", "%框对角线"),
        ("平滑框方向反转率", "smooth_direction_reversal_rate_pct", "%"),
        ("轨迹集合变化率", "track_set_change_rate_pct", "%"),
        ("新建轨迹", "created_tracks_per_min", "次/min"),
        ("丢失轨迹", "lost_tracks_per_min", "次/min"),
        ("编码 FPS P50", "encode_fps_p50", "FPS"),
        ("编码码率 P50", "bitrate_kbps_p50", "kbps"),
        ("框/视频 PTS差 P90", "pts_delta_ms_p90", "ms"),
        ("编码输入丢帧", "enc_drop", "帧"),
    ]
    lines.append("| 指标 | " + " | ".join(labels) + " |")
    lines.append("|---|" + "---:|" * len(labels))
    for title, key, unit in rows:
        values = []
        for report in reports:
            if key is None:
                value = f"{report['stationary_samples']}/{report['moving_samples']}"
            elif key == "samples":
                value = str(report[key])
            else:
                value = fmt(report[key]) + (f" {unit}" if unit else "")
            values.append(value)
        lines.append(f"| {title} | " + " | ".join(values) + " |")

    if len(reports) == 2:
        baseline, candidate = reports
        lines.extend(["", "## 候选相对基线", ""])
        comparisons = [
            ("低速中心抖动", "static_center_smooth_rms_pct_diag", True),
            ("低速尺寸抖动", "static_size_smooth_rms_pct", True),
            ("运动等效滞后 P90", "moving_equivalent_lag_p90_ms", True),
            ("运动单步跳变 P90", "moving_smooth_step_p90_pct_diag", True),
            ("轨迹集合变化率", "track_set_change_rate_pct", True),
            ("新建轨迹率", "created_tracks_per_min", True),
        ]
        for title, key, lower_is_better in comparisons:
            change = safe_change(float(baseline[key]), float(candidate[key]))
            if not math.isfinite(change):
                lines.append(f"- {title}：N/A")
                continue
            improvement = -change if lower_is_better else change
            verdict = "改善" if improvement > 0 else "退化"
            lines.append(f"- {title}：{verdict} {abs(improvement):.2f}%")
        duration_gap = abs(float(candidate["duration_sec"]) - float(baseline["duration_sec"])) / max(
            float(baseline["duration_sec"]), 1e-9
        )
        if duration_gap > 0.05:
            lines.append("- 警告：两组有效时长相差超过 5%，不属于严格同源等时长 A/B。")
        for key, title in (("stationary_samples", "低速"), ("moving_samples", "运动")):
            count_gap = abs(float(candidate[key]) - float(baseline[key])) / max(float(baseline[key]), 1.0)
            if count_gap > 0.10:
                lines.append(f"- 警告：两组{title}样本数相差 {count_gap * 100.0:.1f}%，参数结论需复测。")

    all_warnings = [(label, warning) for label, report in zip(labels, reports) for warning in report["warnings"]]
    if all_warnings:
        lines.extend(["", "## 数据质量提示", ""])
        lines.extend(f"- {label}：{warning}" for label, warning in all_warnings)
    lines.extend([
        "",
        "> 等效滞后由“原始中心与平滑中心沿运动方向的距离 ÷ 当前整体中心速度”估算，"
        "它适合严格 A/B，不等同于摄像头到播放器的端到端延迟。原始 YOLO 框也不是人工真值，"
        "因此最终保留候选仍需同源画面人工确认。",
    ])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path, help="一个日志生成单组报告；两个日志执行 A/B")
    parser.add_argument("--labels", nargs="+", help="与日志对应的显示名称")
    parser.add_argument("--stationary-speed", type=float, default=0.35, help="低速阈值，框对角线/秒")
    parser.add_argument("--moving-speed", type=float, default=0.75, help="运动阈值，框对角线/秒")
    parser.add_argument("--json-out", type=Path, help="可选 JSON 报告路径")
    parser.add_argument("--markdown-out", type=Path, help="可选 Markdown 报告路径")
    args = parser.parse_args()
    if len(args.logs) not in (1, 2):
        parser.error("只支持一个日志或基线/候选两个日志")
    labels = args.labels or (["当前"] if len(args.logs) == 1 else ["基线", "候选"])
    if len(labels) != len(args.logs):
        parser.error("--labels 数量必须与日志数量一致")
    if args.stationary_speed < 0 or args.moving_speed <= args.stationary_speed:
        parser.error("运动阈值必须大于低速阈值，且阈值不能为负")

    reports = [
        summarize(parse_log(path), args.stationary_speed, args.moving_speed, parse_runtime_stats(path))
        for path in args.logs
    ]
    markdown = render_report(labels, reports)
    print(markdown)
    if args.markdown_out:
        args.markdown_out.write_text(markdown + "\n")
    if args.json_out:
        payload = {label: report for label, report in zip(labels, reports)}
        args.json_out.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
