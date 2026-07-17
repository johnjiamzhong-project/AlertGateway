#!/usr/bin/env python3
"""在同一组 MotionTrack 测量上重放中心响应的瞬时运动权重。"""

from __future__ import annotations

import argparse
import json
import math
import statistics
from dataclasses import dataclass
from pathlib import Path

from replay_size_filter import (
    TrackSample,
    box_center,
    box_size,
    parse_motion_tracks,
    percentile,
    rms,
    smoothstep,
)


@dataclass
class State:
    raw: tuple[float, float, float, float]
    center: tuple[float, float]
    filtered_motion: float
    active: bool
    last_step: tuple[float, float] | None = None


def global_motion(samples: list[TrackSample], smoothing_alpha: float,
                  min_tracks: int = 3) -> dict[tuple[int, int], tuple[float, float]]:
    previous_by_track: dict[int, TrackSample] = {}
    grouped: dict[tuple[int, int], list[tuple[float, float]]] = {}
    for sample in samples:
        previous = previous_by_track.get(sample.track_id)
        previous_by_track[sample.track_id] = sample
        if (previous is None or not previous.active or not sample.active or sample.reset
                or not 10 <= sample.dt_ms <= 250):
            continue
        previous_center = box_center(previous.raw)
        center = box_center(sample.raw)
        grouped.setdefault((sample.frame, sample.pts_ms), []).append((
            center[0] - previous_center[0], center[1] - previous_center[1]
        ))

    result: dict[tuple[int, int], tuple[float, float]] = {}
    filtered = (0.0, 0.0)
    valid = False
    for key in sorted(grouped, key=lambda item: (item[1], item[0])):
        deltas = grouped[key]
        if len(deltas) < min_tracks:
            continue
        measured = (
            statistics.median(delta[0] for delta in deltas),
            statistics.median(delta[1] for delta in deltas),
        )
        if not valid:
            filtered = measured
            valid = True
        else:
            filtered = (
                smoothing_alpha * measured[0] + (1.0 - smoothing_alpha) * filtered[0],
                smoothing_alpha * measured[1] + (1.0 - smoothing_alpha) * filtered[1],
            )
        result[key] = filtered
    return result


def replay(samples: list[TrackSample], instant_weight: float, *, alpha_min: float = 0.18,
           alpha_max: float = 0.90, full_response: float = 1.20,
           motion_smoothing_alpha: float = 0.35, max_correction_px: float = 120.0,
           stationary_speed: float = 0.35, moving_speed: float = 0.75,
           global_motion_alpha: float | None = None) -> dict[str, object]:
    states: dict[int, State] = {}
    global_deltas = (global_motion(samples, global_motion_alpha)
                     if global_motion_alpha is not None else {})
    static_steps: list[float] = []
    static_roughness: list[float] = []
    static_errors: list[float] = []
    moving_errors: list[float] = []
    moving_steps: list[float] = []
    all_steps: list[float] = []
    baseline_errors: list[float] = []
    alphas: list[float] = []

    for sample in samples:
        raw_center = box_center(sample.raw)
        raw_width, raw_height = box_size(sample.raw)
        diagonal = max(1.0, math.hypot(raw_width, raw_height))
        previous = states.get(sample.track_id)
        continuous = previous is not None and sample.dt_ms > 0
        metric_valid = previous is not None and 10 <= sample.dt_ms <= 250
        reset = sample.reset or not continuous

        if reset:
            center = raw_center
            filtered_motion = 0.0
            alpha = 1.0
            step = None
        elif sample.deadzone:
            center = previous.center
            filtered_motion = previous.filtered_motion * (1.0 - motion_smoothing_alpha)
            alpha = 0.0
            step = (0.0, 0.0)
        else:
            previous_raw_center = box_center(previous.raw)
            previous_width, previous_height = box_size(previous.raw)
            reference_diagonal = max(32.0, math.hypot(
                (previous_width + raw_width) * 0.5,
                (previous_height + raw_height) * 0.5,
            ))
            raw_motion = math.hypot(
                raw_center[0] - previous_raw_center[0],
                raw_center[1] - previous_raw_center[1],
            ) / reference_diagonal / max(0.001, sample.dt_ms / 1000.0)
            filtered_motion = (
                motion_smoothing_alpha * raw_motion
                + (1.0 - motion_smoothing_alpha) * previous.filtered_motion
            )
            response = smoothstep(
                max(raw_motion * instant_weight, filtered_motion) / max(0.01, full_response)
            )
            alpha = min(1.0, max(0.0, alpha_min + (alpha_max - alpha_min) * response))
            global_delta = global_deltas.get((sample.frame, sample.pts_ms), (0.0, 0.0))
            predicted_center = (
                previous.center[0] + global_delta[0], previous.center[1] + global_delta[1]
            )
            dx = global_delta[0] + alpha * (raw_center[0] - predicted_center[0])
            dy = global_delta[1] + alpha * (raw_center[1] - predicted_center[1])
            distance = math.hypot(dx, dy)
            if distance > max_correction_px:
                scale = max_correction_px / distance
                dx *= scale
                dy *= scale
            center = previous.center[0] + dx, previous.center[1] + dy
            step = (dx, dy)

        logged_center = box_center(sample.smooth)
        baseline_errors.append(math.hypot(
            center[0] - logged_center[0], center[1] - logged_center[1]
        ))
        alphas.append(alpha)

        if (previous is not None and previous.active and sample.active and metric_valid
                and not reset and step is not None):
            previous_raw_center = box_center(previous.raw)
            raw_speed = math.hypot(
                raw_center[0] - previous_raw_center[0],
                raw_center[1] - previous_raw_center[1],
            ) / diagonal / (sample.dt_ms / 1000.0)
            normalized_step = math.hypot(*step) / diagonal
            normalized_error = math.hypot(
                center[0] - raw_center[0], center[1] - raw_center[1]
            ) / diagonal
            all_steps.append(normalized_step)
            if raw_speed <= stationary_speed:
                static_steps.append(normalized_step)
                static_errors.append(normalized_error)
                if previous.last_step is not None:
                    static_roughness.append(math.hypot(
                        step[0] - previous.last_step[0], step[1] - previous.last_step[1]
                    ) / diagonal)
            if raw_speed >= moving_speed:
                moving_errors.append(normalized_error)
                moving_steps.append(normalized_step)

        states[sample.track_id] = State(
            raw=sample.raw,
            center=center,
            filtered_motion=filtered_motion,
            active=sample.active,
            last_step=step,
        )

    return {
        "instant_motion_weight": instant_weight,
        "global_motion_alpha": global_motion_alpha,
        "track_samples": len(samples),
        "static_center_step_rms_pct_diag": rms(static_steps) * 100.0,
        "static_center_roughness_rms_pct_diag": rms(static_roughness) * 100.0,
        "static_center_error_p90_pct_diag": percentile(static_errors, 0.90) * 100.0,
        "moving_center_error_p90_pct_diag": percentile(moving_errors, 0.90) * 100.0,
        "moving_center_step_p90_pct_diag": percentile(moving_steps, 0.90) * 100.0,
        "all_center_step_p95_pct_diag": percentile(all_steps, 0.95) * 100.0,
        "mean_center_alpha": sum(alphas) / len(alphas),
        "baseline_center_error_p99_px": percentile(baseline_errors, 0.99),
        "baseline_center_error_max_px": max(baseline_errors),
    }


def render(reports: list[dict[str, object]]) -> str:
    columns = [
        (f"weight={r['instant_motion_weight']:.2f}"
         if r["global_motion_alpha"] is None
         else f"global={r['global_motion_alpha']:.2f}")
        for r in reports
    ]
    rows = [
        ("低速中心步长 RMS", "static_center_step_rms_pct_diag"),
        ("低速中心粗糙度 RMS", "static_center_roughness_rms_pct_diag"),
        ("低速中心误差 P90", "static_center_error_p90_pct_diag"),
        ("运动中心误差 P90", "moving_center_error_p90_pct_diag"),
        ("运动中心步长 P90", "moving_center_step_p90_pct_diag"),
        ("全段中心步长 P95", "all_center_step_p95_pct_diag"),
        ("平均中心 alpha", "mean_center_alpha"),
    ]
    lines = ["# 同序列中心响应重放", "", "| 指标 | " + " | ".join(columns) + " |",
             "|---|" + "---:|" * len(columns)]
    for title, key in rows:
        suffix = "" if key == "mean_center_alpha" else "%框对角线"
        lines.append(f"| {title} | " + " | ".join(
            f"{float(r[key]):.3f}{suffix}" for r in reports
        ) + " |")
    baseline = reports[0]
    lines.extend([
        "", "## 回归校验", "",
        f"- weight=1.00 对生产中心的 P99 重放误差：{baseline['baseline_center_error_p99_px']:.4f}px。",
        f"- 最大误差：{baseline['baseline_center_error_max_px']:.4f}px。",
        "",
        "> weight 只压低单帧原始运动尖峰；global 使用同帧至少三个 Track 的中位共同位移，"
        "两者都不外推未来位置。",
    ])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--weights", nargs="+", type=float, default=[1.0, 0.75, 0.5, 0.25, 0.0])
    parser.add_argument("--global-alphas", nargs="*", type=float, default=[])
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--markdown-out", type=Path)
    args = parser.parse_args()
    if not args.weights or any(weight < 0.0 or weight > 1.0 for weight in args.weights):
        parser.error("weights 必须位于 0 到 1")
    if any(alpha < 0.0 or alpha > 1.0 for alpha in args.global_alphas):
        parser.error("global-alphas 必须位于 0 到 1")
    samples = parse_motion_tracks(args.log)
    reports = [replay(samples, weight) for weight in args.weights]
    reports.extend(replay(samples, 1.0, global_motion_alpha=alpha)
                   for alpha in args.global_alphas)
    markdown = render(reports)
    print(markdown)
    if args.json_out:
        args.json_out.write_text(json.dumps(reports, ensure_ascii=False, indent=2) + "\n")
    if args.markdown_out:
        args.markdown_out.write_text(markdown + "\n")
    return 2 if float(reports[0]["baseline_center_error_p99_px"]) > 0.10 else 0


if __name__ == "__main__":
    raise SystemExit(main())
