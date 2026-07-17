#!/usr/bin/env python3
"""在同一组 MotionTrack 原始测量上重放多个尺寸滤波参数。"""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class TrackSample:
    frame: int
    pts_ms: int
    track_id: int
    class_id: int
    dt_ms: int
    raw: tuple[float, float, float, float]
    smooth: tuple[float, float, float, float]
    deadzone: bool
    reset: bool
    was_active: bool
    active: bool
    logged_size_alpha: float


@dataclass
class ReplayState:
    raw: tuple[float, float, float, float]
    width: float
    height: float
    filtered_center_motion: float
    filtered_size_motion: float
    active: bool
    pts_ms: int


def percentile(values: Iterable[float], q: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    position = (len(ordered) - 1) * q
    lower, upper = math.floor(position), math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] * (upper - position) + ordered[upper] * (position - lower)


def rms(values: Iterable[float]) -> float:
    items = list(values)
    return math.sqrt(sum(value * value for value in items) / len(items)) if items else math.nan


def box_size(box: tuple[float, float, float, float]) -> tuple[float, float]:
    return max(1.0, box[2] - box[0]), max(1.0, box[3] - box[1])


def box_center(box: tuple[float, float, float, float]) -> tuple[float, float]:
    return (box[0] + box[2]) * 0.5, (box[1] + box[3]) * 0.5


def parse_bool(value: str) -> bool:
    if value not in ("0", "1"):
        raise ValueError(f"非法布尔值 {value!r}")
    return value == "1"


def parse_box(value: str) -> tuple[float, float, float, float]:
    parts = tuple(float(item) for item in value.split(","))
    if len(parts) != 4:
        raise ValueError(f"框必须包含四个坐标: {value!r}")
    return parts


def parse_motion_tracks(path: Path) -> list[TrackSample]:
    samples: list[TrackSample] = []
    required = {
        "frame", "pts_ms", "id", "class_id", "dt_ms", "raw", "smooth", "deadzone",
        "reset", "was_active", "active", "size_alpha",
    }
    for line_number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        marker = line.find("[MotionTrack]")
        if marker < 0:
            continue
        fields = dict(re.findall(r"([a-z_]+)=([^\s]+)", line[marker:]))
        missing = sorted(required - fields.keys())
        if missing:
            raise ValueError(f"{path}:{line_number}: MotionTrack 缺少字段: {', '.join(missing)}")
        try:
            samples.append(TrackSample(
                frame=int(fields["frame"]),
                pts_ms=int(fields["pts_ms"]),
                track_id=int(fields["id"]),
                class_id=int(fields["class_id"]),
                dt_ms=int(fields["dt_ms"]),
                raw=parse_box(fields["raw"]),
                smooth=parse_box(fields["smooth"]),
                deadzone=parse_bool(fields["deadzone"]),
                reset=parse_bool(fields["reset"]),
                was_active=parse_bool(fields["was_active"]),
                active=parse_bool(fields["active"]),
                logged_size_alpha=float(fields["size_alpha"]),
            ))
        except ValueError as exc:
            raise ValueError(f"{path}:{line_number}: MotionTrack 字段格式错误: {exc}") from exc
    if not samples:
        raise ValueError(f"{path}: 未找到 [MotionTrack]；请使用支持逐 Track 诊断的新二进制采集")
    return samples


def smoothstep(value: float) -> float:
    value = min(1.0, max(0.0, value))
    return value * value * (3.0 - 2.0 * value)


def replay(samples: list[TrackSample], size_alpha_max: float, *, size_alpha_min: float = 0.12,
           motion_full_response: float = 1.20, motion_smoothing_alpha: float = 0.35,
           stationary_speed: float = 0.35, moving_speed: float = 0.75,
           center_gated: bool = False, high_motion_size_alpha_max: float = 0.45) -> dict[str, object]:
    states: dict[int, ReplayState] = {}
    static_size_steps: list[float] = []
    static_edge_steps: list[float] = []
    moving_size_errors: list[float] = []
    moving_edge_errors: list[float] = []
    moving_edge_steps: list[float] = []
    all_edge_steps: list[float] = []
    baseline_width_errors: list[float] = []
    baseline_height_errors: list[float] = []
    worst_replay_error = (0.0, 0, 0, "")
    replayed_alphas: list[float] = []
    active_samples = 0

    for sample in samples:
        raw_width, raw_height = box_size(sample.raw)
        logged_width, logged_height = box_size(sample.smooth)
        previous = states.get(sample.track_id)
        state_continuous = previous is not None and sample.dt_ms > 0
        metric_timing_valid = previous is not None and 10 <= sample.dt_ms <= 250
        force_reset = sample.reset or not state_continuous

        if force_reset:
            width, height = raw_width, raw_height
            filtered_center_motion = 0.0
            filtered_size_motion = 0.0
            alpha = 1.0
        elif sample.deadzone:
            width, height = previous.width, previous.height
            filtered_center_motion = previous.filtered_center_motion * (1.0 - motion_smoothing_alpha)
            filtered_size_motion = previous.filtered_size_motion * (1.0 - motion_smoothing_alpha)
            alpha = 0.0
        else:
            last_raw_width, last_raw_height = box_size(previous.raw)
            last_raw_center = box_center(previous.raw)
            raw_center = box_center(sample.raw)
            dt_sec = sample.dt_ms / 1000.0
            reference_diagonal = max(32.0, math.hypot(
                (last_raw_width + raw_width) * 0.5,
                (last_raw_height + raw_height) * 0.5,
            ))
            raw_center_motion = math.hypot(
                raw_center[0] - last_raw_center[0], raw_center[1] - last_raw_center[1]
            ) / reference_diagonal / dt_sec
            raw_size_motion = math.hypot(
                math.log(raw_width / last_raw_width),
                math.log(raw_height / last_raw_height),
            ) / dt_sec
            filtered_center_motion = (
                motion_smoothing_alpha * raw_center_motion
                + (1.0 - motion_smoothing_alpha) * previous.filtered_center_motion
            )
            filtered_size_motion = (
                motion_smoothing_alpha * raw_size_motion
                + (1.0 - motion_smoothing_alpha) * previous.filtered_size_motion
            )
            center_response = smoothstep(
                max(raw_center_motion, filtered_center_motion) / max(0.01, motion_full_response)
            )
            size_response = smoothstep(
                max(raw_size_motion, filtered_size_motion) / max(0.01, motion_full_response)
            )
            effective_max = size_alpha_max
            if center_gated:
                effective_max += (high_motion_size_alpha_max - size_alpha_max) * center_response
            alpha = min(1.0, max(0.0, size_alpha_min + (effective_max - size_alpha_min) * size_response))
            width = alpha * raw_width + (1.0 - alpha) * previous.width
            height = alpha * raw_height + (1.0 - alpha) * previous.height

        states[sample.track_id] = ReplayState(
            raw=sample.raw,
            width=width,
            height=height,
            filtered_center_motion=filtered_center_motion,
            filtered_size_motion=filtered_size_motion,
            active=sample.active,
            pts_ms=sample.pts_ms,
        )
        replayed_alphas.append(alpha)
        width_error = abs(width - logged_width)
        height_error = abs(height - logged_height)
        baseline_width_errors.append(width_error)
        baseline_height_errors.append(height_error)
        sample_error = max(width_error, height_error)
        if sample_error > worst_replay_error[0]:
            path = "reset" if force_reset else ("deadzone" if sample.deadzone else "adaptive")
            worst_replay_error = (sample_error, sample.frame, sample.track_id, path)

        if (not sample.active or previous is None or not previous.active
                or not metric_timing_valid or force_reset):
            continue
        active_samples += 1
        previous_raw_center = box_center(previous.raw)
        raw_center = box_center(sample.raw)
        raw_diagonal = max(1.0, math.hypot(raw_width, raw_height))
        center_speed = math.hypot(
            raw_center[0] - previous_raw_center[0], raw_center[1] - previous_raw_center[1]
        ) / raw_diagonal / (sample.dt_ms / 1000.0)
        size_step = math.hypot(
            math.log(width / previous.width), math.log(height / previous.height)
        )
        edge_step = 0.5 * math.hypot(width - previous.width, height - previous.height) / raw_diagonal
        size_error = math.hypot(math.log(width / raw_width), math.log(height / raw_height))
        edge_error = 0.5 * math.hypot(width - raw_width, height - raw_height) / raw_diagonal
        all_edge_steps.append(edge_step)
        if center_speed <= stationary_speed:
            static_size_steps.append(size_step)
            static_edge_steps.append(edge_step)
        if center_speed >= moving_speed:
            moving_size_errors.append(size_error)
            moving_edge_errors.append(edge_error)
            moving_edge_steps.append(edge_step)

    return {
        "size_alpha_max": size_alpha_max,
        "center_gated": center_gated,
        "high_motion_size_alpha_max": high_motion_size_alpha_max,
        "track_samples": len(samples),
        "active_transitions": active_samples,
        "stationary_transitions": len(static_size_steps),
        "moving_transitions": len(moving_size_errors),
        "static_size_step_rms_pct": rms(static_size_steps) * 100.0,
        "static_size_step_p90_pct": percentile(static_size_steps, 0.90) * 100.0,
        "static_edge_step_rms_pct_diag": rms(static_edge_steps) * 100.0,
        "moving_size_error_p90_pct": percentile(moving_size_errors, 0.90) * 100.0,
        "moving_edge_error_p90_pct_diag": percentile(moving_edge_errors, 0.90) * 100.0,
        "moving_edge_step_p90_pct_diag": percentile(moving_edge_steps, 0.90) * 100.0,
        "all_edge_step_p95_pct_diag": percentile(all_edge_steps, 0.95) * 100.0,
        "replay_width_error_p99_px": percentile(baseline_width_errors, 0.99),
        "replay_height_error_p99_px": percentile(baseline_height_errors, 0.99),
        "replay_error_max_px": worst_replay_error[0],
        "replay_error_max_frame": worst_replay_error[1],
        "replay_error_max_track_id": worst_replay_error[2],
        "replay_error_max_path": worst_replay_error[3],
        "mean_size_alpha": statistics.fmean(replayed_alphas),
    }


def safe_improvement(baseline: float, candidate: float) -> float:
    return (baseline - candidate) / baseline * 100.0 if baseline > 1e-12 else math.nan


def fmt(value: object, digits: int = 3) -> str:
    if not isinstance(value, (int, float)) or not math.isfinite(float(value)):
        return "N/A"
    return f"{float(value):.{digits}f}"


def recommend(reports: list[dict[str, object]]) -> tuple[float, str]:
    baseline = reports[0]
    baseline_jitter = float(baseline["static_edge_step_rms_pct_diag"])
    baseline_motion_error = float(baseline["moving_edge_error_p90_pct_diag"])
    eligible: list[dict[str, object]] = []
    for report in reports:
        jitter_gain = safe_improvement(baseline_jitter, float(report["static_edge_step_rms_pct_diag"]))
        motion_change = -safe_improvement(
            baseline_motion_error, float(report["moving_edge_error_p90_pct_diag"])
        )
        if jitter_gain >= 5.0 and motion_change <= 10.0:
            eligible.append(report)
    if not eligible:
        return float(baseline["size_alpha_max"]), "没有候选同时满足低速边缘抖动改善≥5%且运动边缘误差增加≤10%"
    winner = min(eligible, key=lambda item: float(item["static_edge_step_rms_pct_diag"]))
    return float(winner["size_alpha_max"]), "满足抖动收益与运动跟随门限，且低速边缘抖动最小"


def render(reports: list[dict[str, object]], baseline_alpha: float) -> str:
    labels = [f"max={float(report['size_alpha_max']):.2f}" for report in reports]
    rows = [
        ("逐 Track 样本", "track_samples", ""),
        ("低速/运动转换", None, ""),
        ("低速尺寸步长 RMS", "static_size_step_rms_pct", "%"),
        ("低速尺寸步长 P90", "static_size_step_p90_pct", "%"),
        ("低速边缘步长 RMS", "static_edge_step_rms_pct_diag", "%框对角线"),
        ("运动尺寸误差 P90", "moving_size_error_p90_pct", "%"),
        ("运动边缘误差 P90", "moving_edge_error_p90_pct_diag", "%框对角线"),
        ("运动边缘步长 P90", "moving_edge_step_p90_pct_diag", "%框对角线"),
        ("全段边缘步长 P95", "all_edge_step_p95_pct_diag", "%框对角线"),
        ("平均尺寸 alpha", "mean_size_alpha", ""),
    ]
    lines = ["# 同序列尺寸滤波重放", "", "| 指标 | " + " | ".join(labels) + " |",
             "|---|" + "---:|" * len(labels)]
    for title, key, unit in rows:
        values = []
        for report in reports:
            if key is None:
                value = f"{report['stationary_transitions']}/{report['moving_transitions']}"
            elif key == "track_samples":
                value = str(report[key])
            else:
                value = fmt(report[key]) + (f" {unit}" if unit else "")
            values.append(value)
        lines.append(f"| {title} | " + " | ".join(values) + " |")

    baseline = min(reports, key=lambda item: abs(float(item["size_alpha_max"]) - baseline_alpha))
    replay_error = max(
        float(baseline["replay_width_error_p99_px"]), float(baseline["replay_height_error_p99_px"])
    )
    lines.extend(["", "## 回归校验", ""])
    lines.append(f"- max={baseline_alpha:.2f} 对日志基线尺寸的 P99 重放误差：{replay_error:.4f}px。")
    lines.append(
        "- 最大误差："
        f"{float(baseline['replay_error_max_px']):.4f}px，"
        f"frame={baseline['replay_error_max_frame']}，track={baseline['replay_error_max_track_id']}，"
        f"path={baseline['replay_error_max_path']}。"
    )
    if replay_error > 0.10:
        lines.append("- 结果无效：重放未能复刻生产基线，不能据此选择参数。")
    else:
        winner, reason = recommend(reports)
        lines.append("- 基线复刻通过（门限 0.10px）。")
        lines.append(f"- 自动建议：`track_size_alpha_max={winner:.2f}`；{reason}。")
    lines.extend([
        "",
        "> 此重放固定使用一次板端关联得到的 Track 与死区/重置决策，只比较显示尺寸滤波。"
        "它能消除 RKNN 跳帧差异，但不能预测参数改变后对轨迹关联 IoU 的二阶影响；胜出参数仍需短时上板和人工观看。",
    ])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--max-alphas", nargs="+", type=float, default=[0.45, 0.35, 0.30])
    parser.add_argument("--baseline-alpha", type=float, default=0.45)
    parser.add_argument("--size-alpha-min", type=float, default=0.12)
    parser.add_argument("--motion-full-response", type=float, default=1.20)
    parser.add_argument("--motion-smoothing-alpha", type=float, default=0.35)
    parser.add_argument("--center-gated", action="store_true",
                        help="把 max-alpha 作为低速上限，并随中心运动连续恢复 high-motion-max")
    parser.add_argument("--high-motion-max", type=float, default=0.45)
    parser.add_argument("--stationary-speed", type=float, default=0.35)
    parser.add_argument("--moving-speed", type=float, default=0.75)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--markdown-out", type=Path)
    args = parser.parse_args()
    if not args.max_alphas or any(alpha < args.size_alpha_min or alpha > 1.0 for alpha in args.max_alphas):
        parser.error("所有 max alpha 必须位于 size-alpha-min 到 1.0 之间")
    if args.baseline_alpha not in args.max_alphas:
        parser.error("baseline-alpha 必须包含在 max-alphas 中")

    samples = parse_motion_tracks(args.log)
    reports = [
        replay(
            samples, alpha,
            size_alpha_min=args.size_alpha_min,
            motion_full_response=args.motion_full_response,
            motion_smoothing_alpha=args.motion_smoothing_alpha,
            stationary_speed=args.stationary_speed,
            moving_speed=args.moving_speed,
            center_gated=args.center_gated,
            high_motion_size_alpha_max=args.high_motion_max,
        )
        for alpha in args.max_alphas
    ]
    markdown = render(reports, args.baseline_alpha)
    print(markdown)
    if args.markdown_out:
        args.markdown_out.write_text(markdown + "\n")
    if args.json_out:
        args.json_out.write_text(json.dumps(reports, ensure_ascii=False, indent=2) + "\n")

    baseline = next(report for report in reports if float(report["size_alpha_max"]) == args.baseline_alpha)
    replay_error = max(
        float(baseline["replay_width_error_p99_px"]), float(baseline["replay_height_error_p99_px"])
    )
    return 2 if replay_error > 0.10 else 0


if __name__ == "__main__":
    raise SystemExit(main())
