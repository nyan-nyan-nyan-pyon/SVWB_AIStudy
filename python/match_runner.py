"""Run the self-play executable and validate its match counters."""

import re
import subprocess


WIN_COUNTERS = ("games", "A_first", "A_second", "A_wins", "draws",
                "A_first_wins", "A_second_wins", "first_wins", "second_wins")


def win_counters(result):
    try:
        counts = {key: int(result[key]) for key in WIN_COUNTERS}
    except (KeyError, ValueError) as error:
        raise RuntimeError("The match executable must emit per-seat wins") from error
    if (any(count < 0 for count in counts.values()) or
            counts["A_first_wins"] > min(counts["A_first"], counts["first_wins"]) or
            counts["A_second_wins"] > min(counts["A_second"], counts["second_wins"]) or
            counts["A_first_wins"] + counts["A_second_wins"] != counts["A_wins"] or
            counts["first_wins"] + counts["second_wins"] + counts["draws"] != counts["games"]):
        raise RuntimeError("C++ match produced inconsistent per-seat wins")
    return counts


def seat_win_rates(counts, prefix):
    def rate(wins, games):
        return counts[wins] / counts[games] if counts[games] else None

    return {
        f"{prefix}_a_first_win_rate": rate("A_first_wins", "A_first"),
        f"{prefix}_a_second_win_rate": rate("A_second_wins", "A_second"),
        f"{prefix}_first_seat_win_rate": rate("first_wins", "games"),
    }


def invoke(executable, arguments):
    command = [str(executable), *(str(value) for value in arguments)]
    completed = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if completed.returncode:
        raise RuntimeError(f"C++ match exited with {completed.returncode}: {completed.stderr.strip()}")
    counters = dict(re.findall(r"(?:^|\s)([A-Za-z_]+)=([^\s]+)", completed.stdout))
    required = ("games", "A_first", "A_second", "A_wins", "B_wins", "draws", "wall_seconds")
    if any(key not in counters for key in required):
        raise RuntimeError(f"C++ match did not emit counters: {completed.stdout.strip()}")
    games, first, second, wins, losses, draws = (int(counters[key]) for key in required[:-1])
    if not games or first + second != games or wins + losses + draws != games:
        raise RuntimeError("C++ match produced invalid game counts")
    return counters
