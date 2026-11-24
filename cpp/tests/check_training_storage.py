import errno
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from match_runner import seat_win_rates, win_counters
from run_storage import publish_checkpoint, retry_windows_lock


def locked(code=5):
    error = PermissionError(errno.EACCES, "transient Windows lock")
    error.winerror = code
    return error


class TrainingStorage(unittest.TestCase):
    def test_atomic_directory_retry(self):
        with tempfile.TemporaryDirectory() as temp:
            initial = Path(temp) / "generation_011"
            initial.mkdir()
            (initial / "weights.bin").write_bytes(b"previous")
            stage = Path(temp) / ".generation-stage"
            stage.mkdir()
            (stage / "weights.bin").write_bytes(b"new")
            next_generation = Path(temp) / "generation_012"
            rename = Path.rename
            attempts = []

            def staged_rename(source, target):
                attempts.append(target)
                if len(attempts) <= 2:
                    raise locked()
                return rename(source, target)

            with patch.object(Path, "rename", staged_rename), patch("run_storage.time.sleep") as sleep:
                publish_checkpoint(stage, next_generation)
            self.assertEqual([call.args[0] for call in sleep.call_args_list], [0.2, 0.4])
            self.assertEqual((initial / "weights.bin").read_bytes(), b"previous")
            self.assertEqual((next_generation / "weights.bin").read_bytes(), b"new")
            self.assertFalse(stage.exists())
            with self.assertRaises(FileExistsError):
                publish_checkpoint(stage, next_generation)

    def test_retry_limit_and_unrelated_errors(self):
        with patch("run_storage.time.sleep") as sleep:
            with self.assertRaises(PermissionError):
                retry_windows_lock(lambda: (_ for _ in ()).throw(locked(32)))
            self.assertEqual([call.args[0] for call in sleep.call_args_list], [x / 5 for x in range(1, 11)])
            sleep.reset_mock()
            with self.assertRaises(PermissionError):
                retry_windows_lock(lambda: (_ for _ in ()).throw(locked(3)))
            sleep.assert_not_called()

    def test_seat_denominators(self):
        counts = win_counters({
            "games": "5", "A_first": "3", "A_second": "2", "A_wins": "3", "draws": "1",
            "A_first_wins": "2", "A_second_wins": "1", "first_wins": "3", "second_wins": "1",
        })
        self.assertEqual(seat_win_rates(counts, "previous"), {
            "previous_a_first_win_rate": 2 / 3,
            "previous_a_second_win_rate": 1 / 2,
            "previous_first_seat_win_rate": 3 / 5,
        })
        counts["A_first_wins"] = 4
        with self.assertRaisesRegex(RuntimeError, "inconsistent"):
            win_counters(counts)


if __name__ == "__main__":
    unittest.main(verbosity=2)
