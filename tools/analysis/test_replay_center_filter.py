#!/usr/bin/env python3

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from replay_center_filter import global_motion, replay
from replay_size_filter import TrackSample


def sample(frame: int, track_id: int, x: float, *, smooth_x: float | None = None,
           reset: bool = False, dt_ms: int = 50) -> TrackSample:
    displayed_x = x if smooth_x is None else smooth_x
    return TrackSample(
        frame=frame,
        pts_ms=frame * 50,
        track_id=track_id,
        class_id=1,
        dt_ms=0 if reset else dt_ms,
        raw=(x, 0.0, x + 100.0, 100.0),
        smooth=(displayed_x, 0.0, displayed_x + 100.0, 100.0),
        deadzone=False,
        reset=reset,
        was_active=not reset,
        active=True,
        logged_size_alpha=1.0 if reset else 0.12,
    )


class ReplayCenterFilterTest(unittest.TestCase):
    def test_global_motion_uses_component_median(self):
        samples = []
        for track_id, delta in ((1, 10.0), (2, 12.0), (3, 100.0)):
            samples.extend((sample(1, track_id, 0.0, reset=True), sample(2, track_id, delta)))
        motion = global_motion(samples, 0.25)
        self.assertEqual(motion[(2, 100)], (12.0, 0.0))

    def test_weight_one_replays_production_center(self):
        samples = [sample(1, 1, 0.0, reset=True), sample(2, 1, 10.0, smooth_x=9.0)]
        report = replay(samples, 1.0)
        self.assertLess(float(report["baseline_center_error_p99_px"]), 1e-6)


if __name__ == "__main__":
    unittest.main()
