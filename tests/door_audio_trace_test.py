import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_door_audio import analyze


def fixture():
    return [dict(frame=i, seconds=i/60, state=11 if 60 <= i < 100 else 8,
                 room=0x91f8 if i < 70 else 0x92fd, produced=i*534,
                 underflows=42, missing_frames=32000, output_rate=32000)
            for i in range(230)]


class DoorTraceTest(unittest.TestCase):
    def test_title_screen_is_not_a_pass(self):
        rows = fixture()
        for row in rows:
            row["state"] = 1
        with self.assertRaisesRegex(ValueError, "No gameplay-to-door"):
            analyze(rows)

    def test_incomplete_transition_is_not_a_pass(self):
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            analyze(fixture()[:90])

    def test_boot_starvation_is_excluded(self):
        result = analyze(fixture())[0]
        self.assertEqual(result["missing_ms"], 0)
        self.assertEqual(result["post_missing_ms"], 0)
        self.assertAlmostEqual(result["post_fps"], 60)

    def test_transition_and_exit_gaps_are_measured(self):
        rows = fixture()
        for row in rows:
            if row["frame"] >= 80:
                row["missing_frames"] += 320
                row["underflows"] += 1
            if row["frame"] >= 110:
                row["missing_frames"] += 160
        result = analyze(rows)[0]
        self.assertEqual(result["missing_ms"], 10)
        self.assertEqual(result["post_missing_ms"], 5)
        self.assertEqual(result["underflows"], 1)

    def test_dropped_audio_is_measured_without_boot_drops(self):
        rows = fixture()
        for row in rows:
            row["dropped"] = 1000 + (100 if row["frame"] >= 80 else 0)
            row["dropped"] += 20 if row["frame"] >= 110 else 0
        result = analyze(rows)[0]
        self.assertEqual(result["dropped_samples"], 100)
        self.assertEqual(result["post_dropped_samples"], 20)


if __name__ == "__main__":
    unittest.main()
