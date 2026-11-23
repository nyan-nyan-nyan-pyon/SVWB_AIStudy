import argparse
import math
from pathlib import Path
import re
import struct
import subprocess
import tempfile


SAMPLE = struct.Struct("<257f")
HEADER = struct.Struct("<i")


def read_exact(file, size):
    data = file.read(size)
    if len(data) != size:
        raise ValueError(f"Truncated record in {file.name}")
    return data


def close(a, b):
    return math.isclose(a, b, rel_tol=0.0, abs_tol=1e-6)


def verify_logs(directory, expected_games=None):
    value_path = directory / "val_log.bin"
    policy_path = directory / "pol_log.bin"
    values_bytes = value_path.stat().st_size
    policies_bytes = policy_path.stat().st_size
    turns, remainder = divmod(values_bytes, SAMPLE.size)
    if not turns or remainder:
        raise ValueError("Invalid value log size")

    games = 0
    candidates = 0
    previous_target = None
    with value_path.open("rb") as values, policy_path.open("rb") as policies:
        for turn in range(turns):
            *features, target = SAMPLE.unpack(read_exact(values, SAMPLE.size))
            if target not in (0.0, 0.5, 1.0) or not all(map(math.isfinite, features)):
                raise ValueError(f"Invalid value record at turn {turn}")
            if close(features[240], 1 / 40) and features[122] == 0.0:
                games += 1
            elif previous_target is None or target != 1.0 - previous_target:
                raise ValueError(f"Value perspective did not alternate at turn {turn}")
            previous_target = target

            count, = HEADER.unpack(read_exact(policies, HEADER.size))
            if not 1 <= count <= 18:
                raise ValueError(f"Invalid candidate count {count} at turn {turn}")
            probabilities = []
            for _ in range(count):
                *decision, probability = SAMPLE.unpack(read_exact(policies, SAMPLE.size))
                if not 0.0 <= probability <= 1.0 or not all(map(math.isfinite, decision)):
                    raise ValueError(f"Invalid policy sample at turn {turn}")
                if (not close(features[240], decision[240])
                        or not close(features[2], decision[2])
                        or not close(features[122], decision[122])
                        or decision[1] > features[1]
                        or decision[0] < features[0]
                        or decision[120] > features[120]):
                    raise ValueError(f"Policy candidate uses a different turn or perspective at turn {turn}")
                probabilities.append(probability)
            if not close(math.fsum(probabilities), 1.0):
                raise ValueError(f"Policy probabilities do not sum to one at turn {turn}")
            candidates += count
        if policies.read(1):
            raise ValueError("Unexpected trailing bytes in policy log")

    if policies_bytes != turns * HEADER.size + candidates * SAMPLE.size:
        raise ValueError("Invalid policy log size")
    if expected_games is not None and games != expected_games:
        raise ValueError(f"Expected {expected_games} games, found {games}")
    print(
        f"PASS games={games} turns={turns} candidates={candidates} "
        f"val_bytes={values_bytes} pol_bytes={policies_bytes}"
    )
    return turns, candidates


def verify_cli(exe, directory, games):
    result = subprocess.run(
        [exe, "--games", str(games), "--threads", "2", "--seconds", "30",
         "--deck-a", "A", "--deck-b", "A", "--seed", "123", "--opponent-seed", "456",
         "--simulations", "4", "--log-dir", directory],
        check=True, capture_output=True, text=True, timeout=45,
    )
    print(result.stdout, end="")
    expected_seats = f"A_first={(games + 1) // 2} A_second={games // 2}"
    if expected_seats not in result.stdout:
        raise ValueError("CLI did not alternate seats")
    turns, candidates = verify_logs(directory, games)
    if f"value_samples={turns} policy_turns={turns} policy_samples={candidates}" not in result.stdout:
        raise ValueError("CLI log counters disagree with decoded samples")

    weights = directory / "cli_weights.bin"
    weights.write_bytes(bytes(74376))
    timed = subprocess.run(
        [exe, "--seconds", "1", "--threads", "2", "--simulations", "1", "--weights", weights],
        check=True, capture_output=True, text=True, timeout=15,
    )
    match = re.search(r"^games=([0-9]+) threads=2\b", timed.stdout)
    if match is None or int(match.group(1)) == 0:
        raise ValueError("Time-limited CLI did not complete any games")
    print("PASS CLI weight loading, time and game limits")

    for opponent in (["--random-opponent"], ["--opponent-weights", weights]):
        common = [exe, "--evaluate", "--weights", weights, *opponent,
                  "--games", "4", "--simulations", "4", "--seed", "731"]
        summaries = []
        for threads in ("1", "2"):
            evaluation = subprocess.run(
                [*common, "--threads", threads],
                check=True, capture_output=True, text=True, timeout=15,
            )
            if "mode=evaluate" not in evaluation.stdout:
                raise ValueError("CLI did not report evaluation mode")
            summary = re.search(r"A_first=2 A_second=2 A_wins=(\d+) B_wins=(\d+) draws=(\d+)",
                                evaluation.stdout)
            if summary is None or sum(map(int, summary.groups())) != 4:
                raise ValueError("Evaluation has incorrect seat or result counts")
            summaries.append(summary.groups())
        if summaries[0] != summaries[1]:
            raise ValueError("Evaluation results changed when the thread count changed")
    print("PASS seat-balanced, seeded evaluation against random moves and another model")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path, nargs="?")
    parser.add_argument("--games", type=int)
    parser.add_argument("--exe", type=Path)
    args = parser.parse_args()
    if args.exe:
        if args.directory:
            verify_cli(args.exe.resolve(), args.directory, args.games or 6)
        else:
            with tempfile.TemporaryDirectory(prefix="svwb-log-test-") as directory:
                verify_cli(args.exe.resolve(), Path(directory), args.games or 6)
    elif args.directory:
        verify_logs(args.directory, args.games)
    else:
        parser.error("specify a log directory or --exe")


if __name__ == "__main__":
    main()
