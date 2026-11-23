"""Run the self-play executable and validate its match counters."""

import re
import subprocess


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
