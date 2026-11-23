import argparse
import csv
import json
import math
from pathlib import Path
import re
import subprocess
import tempfile


def check_trace(trace, game):
    assert trace["game"] == game and trace["seed"] == 31415
    assert [seat["seat"] for seat in trace["opening_hands"]] == [1, 2]
    assert trace["opening_hands"][0]["competitor"] == ("A" if game % 2 else "B")
    assert all(len(seat["cards"]) == 4 for seat in trace["opening_hands"])
    assert trace["winner_seat"] in (1, 2, None)
    assert trace["turns"]
    for index, turn in enumerate(trace["turns"]):
        assert turn["seat"] == 1 + index % 2 and turn["round"] == 1 + index // 2
        actions = turn["selected_actions"]
        assert actions and all(action["type"] in ("play_card", "evolve", "attack", "end_turn") for action in actions)
        assert all(action["type"] != "end_turn" for action in actions[:-1])
        if index + 1 != len(trace["turns"]):
            assert actions[-1]["type"] == "end_turn"
        for action in actions:
            if action["type"] != "end_turn":
                assert action["source"]["name"] and action["source"]["slot"] >= 1
            if "target" in action:
                assert action["target"]["seat"] == 3 - turn["seat"]
        if turn["controller"] == "random":
            assert not turn["candidates"] and turn["total_visits"] == 0 and turn["selected_rank"] is None
            continue
        assert turn["controller"] == "search" and turn["total_visits"] >= 4
        candidates = turn["candidates"]
        assert [item["rank"] for item in candidates] == list(range(1, len(candidates) + 1))
        assert [item["visits"] for item in candidates] == sorted(
            (item["visits"] for item in candidates), reverse=True
        )
        assert sum(item["visits"] for item in candidates) == turn["total_visits"]
        for candidate in candidates:
            assert math.isclose(candidate["visit_share"], candidate["visits"] / turn["total_visits"],
                                rel_tol=1e-8, abs_tol=1e-9)
            value = candidate["value"]
            assert (value is None) if candidate["visits"] == 0 else 0 <= value <= 1
        selected, = (item for item in candidates if item["selected"])
        assert selected["rank"] == turn["selected_rank"] and selected["actions"] == actions


def check_opening_table(rows, traces):
    expected = {"A": {1, 2, 4, 5, 6, 7, 9}, "B": {2, 3, 5, 6, 8, 9, 10}}
    assert {(row["competitor"], int(row["card_id"])) for row in rows} == {
        (label, card) for label, cards in expected.items() for card in cards
    } and len(rows) == 14
    for row in rows:
        competitor = row["competitor"]
        assert row["deck"] == competitor and row["card_name"]
        card_id = int(row["card_id"])
        buckets = {"with": [], "without": []}
        for trace in traces:
            owner = next(seat for seat in trace["opening_hands"] if seat["competitor"] == competitor)
            ids = {card["card_id"] for card in owner["cards"]}
            buckets["with" if card_id in ids else "without"].append(
                (trace["winner_seat"] == owner["seat"], trace["winner_seat"] is None)
            )
        rates = {}
        for cohort, outcomes in buckets.items():
            assert int(row[f"{cohort}_games"]) == len(outcomes)
            wins = sum(won for won, _ in outcomes)
            assert int(row[f"{cohort}_wins"]) == wins
            assert int(row[f"{cohort}_draws"]) == sum(draw for _, draw in outcomes)
            if outcomes:
                rates[cohort] = wins / len(outcomes)
                assert math.isclose(float(row[f"{cohort}_win_rate"]), rates[cohort], abs_tol=1e-8)
            else:
                assert row[f"{cohort}_win_rate"] == ""
        if len(rates) == 2:
            assert math.isclose(float(row["win_rate_diff_pp"]), 100 * (rates["with"] - rates["without"]),
                                abs_tol=1e-8)
        else:
            assert row["win_rate_diff_pp"] == ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("exe", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="svwb-analysis-test-") as scratch:
        directory = Path(scratch)
        weights = directory / "weights.bin"
        weights.write_bytes(bytes(74376))
        command = [args.exe.resolve(), "--evaluate", "--weights", weights, "--random-opponent", "--games", "4",
                   "--threads", "2", "--seed", "31415", "--deck-a", "A", "--deck-b", "B", "--simulations", "4"]
        baseline = subprocess.run(command, check=True, capture_output=True, encoding="utf-8", timeout=20)
        result_pattern = r"games=4 threads=2 turns=\d+[^\n]*\nA_first=2 A_second=2 A_wins=\d+ B_wins=\d+ draws=\d+"
        before = re.search(result_pattern, baseline.stdout)
        assert before is not None
        traces = []
        for game in range(1, 5):
            output = directory / f"game-{game}"
            run = subprocess.run(
                [*command, "--analysis-dir", output, "--trace-game", str(game), "--trace-top", "18"],
                check=True, capture_output=True, encoding="utf-8", timeout=20,
            )
            with (output / "turn_trace.json").open(encoding="utf-8") as source:
                trace = json.load(source)
            check_trace(trace, game)
            assert re.search(result_pattern, run.stdout).group() == before.group()
            traces.append(trace)
        for game in range(1, 5):
            with (directory / f"game-{game}" / "opening_win_rates.csv").open(encoding="utf-8", newline="") as source:
                check_opening_table(list(csv.DictReader(source)), traces)
    print("PASS replayable turn traces, stable match results, and independently counted opening cohorts")


if __name__ == "__main__":
    main()
