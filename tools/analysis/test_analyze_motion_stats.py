#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path

from analyze_motion_stats import parse_log, parse_runtime_stats, summarize


class MotionStatsAnalysisTest(unittest.TestCase):
    def test_stationary_jitter_and_moving_lag(self) -> None:
        lines = []
        raw_x = 100.0
        smooth_x = 100.0
        for index in range(80):
            moving = index >= 40
            raw_dx = 5.0 if moving else (1.0 if index % 2 == 0 else -1.0)
            smooth_dx = 5.0 if moving else (0.2 if index % 2 == 0 else -0.2)
            raw_x += raw_dx
            smooth_x = raw_x - 10.0 if moving else smooth_x + smooth_dx
            raw_step_ratio = abs(raw_dx) / 100.0
            smooth_step_ratio = abs(smooth_dx) / 100.0
            lines.append(
                "[MotionStats] "
                f"frame={index + 1} pts_ms={(index + 1) * 50} matched_active=1 stable_tracks=1 "
                f"raw_center={raw_x},100 smooth_center={smooth_x},100 "
                "raw_size=80,60 smooth_size=80,60 "
                f"raw_delta={raw_dx},0 smooth_delta={smooth_dx},0 dt_ms=50 "
                "center_gap=10 center_gap_ratio=0.1 delta_gap=0 "
                f"raw_step_ratio={raw_step_ratio} smooth_step_ratio={smooth_step_ratio} "
                "raw_size_step=0.01 smooth_size_step=0.002 raw_reversals=0 "
                "created=0 lost=0 active_ids=7 stable_ids=7"
            )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "motion.log"
            path.write_text("\n".join(lines) + "\n")
            report = summarize(parse_log(path), stationary_speed=0.35, moving_speed=0.75)

        self.assertEqual(report["stationary_samples"], 40)
        self.assertEqual(report["moving_samples"], 40)
        self.assertAlmostEqual(report["static_center_smoothing_reduction_pct"], 80.0, places=4)
        self.assertAlmostEqual(report["static_size_smoothing_reduction_pct"], 80.0, places=4)
        self.assertAlmostEqual(report["moving_equivalent_lag_p50_ms"], 100.0, places=4)

    def test_runtime_stats(self) -> None:
        content = "\n".join([
            "[PullStats] input_fps=30 decoded=300 enc_push=298 enc_drop=2 infer_push=300 infer_drop=90 queues=0/1",
            "[EncodeStats] input_fps=29.9 packet_fps=29.8 bitrate_kbps=17950 avg_process_ms=18 put_fail=0 out_drop=1 queue=0 pts_delta_ms=20",
            "[StreamStats] output_fps=29.8 bitrate_kbps=17950 write_fail=0 queue=0",
        ])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "runtime.log"
            path.write_text(content + "\n")
            report = parse_runtime_stats(path)
        self.assertEqual(report["enc_drop"], 2.0)
        self.assertEqual(report["infer_drop"], 90.0)
        self.assertEqual(report["out_drop"], 1.0)
        self.assertEqual(report["encode_fps_p50"], 29.8)
        self.assertEqual(report["pts_delta_ms_p90"], 20.0)


if __name__ == "__main__":
    unittest.main()
