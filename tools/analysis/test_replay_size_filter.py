#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path

from replay_size_filter import parse_motion_tracks, replay


class ReplaySizeFilterTest(unittest.TestCase):
    def test_replay_matches_logged_baseline(self) -> None:
        lines = [
            "[MotionTrack] frame=1 pts_ms=100 id=7 class_id=41 dt_ms=0 "
            "raw=0,0,100,100 smooth=0,0,100,100 deadzone=0 reset=1 "
            "was_active=0 active=1 center_alpha=1 size_alpha=1",
            "[MotionTrack] frame=2 pts_ms=150 id=7 class_id=41 dt_ms=50 "
            "raw=0,0,110,100 smooth=2.75,0,107.25,100 deadzone=0 reset=0 "
            "was_active=1 active=1 center_alpha=0.5 size_alpha=0.45",
            "[MotionTrack] frame=3 pts_ms=200 id=7 class_id=41 dt_ms=50 "
            "raw=0,0,111,100 smooth=3.25,0,107.75,100 deadzone=1 reset=0 "
            "was_active=1 active=1 center_alpha=0 size_alpha=0",
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tracks.log"
            path.write_text("\n".join(lines) + "\n")
            samples = parse_motion_tracks(path)
        baseline = replay(samples, 0.45)
        candidate = replay(samples, 0.30)
        self.assertLess(baseline["replay_width_error_p99_px"], 1e-6)
        self.assertLess(baseline["replay_height_error_p99_px"], 1e-6)
        self.assertLess(
            candidate["all_edge_step_p95_pct_diag"],
            baseline["all_edge_step_p95_pct_diag"],
        )


if __name__ == "__main__":
    unittest.main()
