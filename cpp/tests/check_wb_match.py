import argparse
import csv
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import tempfile


SAMPLE = struct.Struct("<257f")
HEADER = struct.Struct("<i")


def bounded_features(row):
    return all(math.isfinite(x) for x in row)


def check_replay(directory):
    values = (directory / "val_log.bin").read_bytes()
    policy = (directory / "pol_log.bin").read_bytes()
    assert len(values) % SAMPLE.size == 0
    cursor = 0
    starts = 0
    previous = None
    for row in SAMPLE.iter_unpack(values):
        assert bounded_features(row) and row[-1] in (0, 1)
        assert row[13] in (0, 1) and row[133] == 1 - row[13]
        if math.isclose(row[240], 1 / 40, abs_tol=1e-8) and row[13] == 1:
            starts += 1
            assert row[2] > 0 and row[122] == 0
        else:
            assert row[13] != previous
        previous = row[13]
        count, = HEADER.unpack_from(policy, cursor)
        cursor += HEADER.size
        assert 1 <= count <= 18
        total = 0
        for _ in range(count):
            choice = SAMPLE.unpack_from(policy, cursor)
            cursor += SAMPLE.size
            assert bounded_features(choice) and choice[13] == row[13] and choice[240] == row[240]
            assert 0 <= choice[-1] <= 1
            total += choice[-1]
        assert math.isclose(total, 1, abs_tol=1e-6)
    assert starts == 4 and cursor == len(policy)


def check_exports(path):
    trace = json.loads((path / "turn_trace.json").read_text(encoding="utf-8"))
    assert trace["game"] == 2 and trace["competitor_a_seat"] == 2
    for opening in trace["opening_hands"]:
        assert len(opening["offered"]) == len(opening["after_mulligan"]) == 4
        kept = [card for index, card in enumerate(opening["offered"]) if not opening["mulligan_mask"] & (1 << index)]
        assert opening["kept"] == kept
    for turn in trace["turns"]:
        assert turn["selected_actions"]
        if turn["candidates"]:
            visits = turn["total_visits"]
            assert sum(item["visits"] for item in turn["candidates"]) == visits
            chosen = turn["candidates"][turn["selected_rank"] - 1]
            assert chosen["actions"] == turn["selected_actions"]
            if turn["forced_lethal"]:
                assert visits == 0 and len(turn["candidates"]) == 1 and chosen["value"] == 1
            else:
                assert visits >= 4
        else:
            assert turn["selected_rank"] is None
    with (path / "opening_win_rates.csv").open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
    assert len(rows) == 45
    ids = {int(row["card_id"]) for row in rows}
    assert len(ids) == 15 and sum(row["official_card_id"] == "11063110" for row in rows) == 3
    for seat, games in (("first", 2), ("second", 2), ("combined", 4)):
        group = [row for row in rows if row["seat"] == seat]
        assert len(group) == 15
        for row in group:
            for with_key, without_key in (("kept", "not_kept"), ("present", "absent")):
                assert int(row[f"{with_key}_games"]) + int(row[f"{without_key}_games"]) == games


def check_custom_decks(command, root):
    decks = Path(__file__).resolve().parents[2] / "data/decks"
    mirror, monk = decks / "bishop-mirror.json", decks / "bishop-monk.json"
    mappings = []
    for a, b in ((mirror, monk), (monk, mirror)):
        logs = root / a.stem
        result = subprocess.run([*command, "--deck-a", a, "--deck-b", b, "--log-dir", logs],
                                check=True, capture_output=True, encoding="utf-8", timeout=30)
        assert "feature_schema=wb-matchup-v2" in result.stdout
        mapping = json.loads((logs / "matchup_features.json").read_text(encoding="utf-8"))
        assert mapping["input_dim"] == 256
        cards = mapping["cards"]
        assert [card["local_id"] for card in cards] == list(range(len(cards)))
        assert {11061120, 90061140, 90001210} <= {card["official_id"] for card in cards}
        rows = list(SAMPLE.iter_unpack((logs / "val_log.bin").read_bytes()))
        assert rows and all(row[241] == 1 and bounded_features(row) for row in rows)
        check_replay(logs)
        mappings.append(mapping)
    assert mappings[0] == mappings[1]

    unsupported = root / "unsupported.json"
    unsupported.write_text(json.dumps({"cards": [{"card_id": 4294967295, "copies": 1}]}), encoding="utf-8")
    failed = subprocess.run([*command, "--deck-a", unsupported], capture_output=True, encoding="utf-8", timeout=30)
    assert failed.returncode != 0 and "lacks playable engine rules" in failed.stderr


def check_elf_royal_decks(command, root):
    decks = {
        "elf": [10614120, 10011120, 10011210, 10511310, 10512110, 10514110, 10612110,
                10814120, 10714110, 10912310, 10912120, 10813310, 11012310, 11012110],
        "royal": [10624120, 10021110, 10622310, 10623310, 10522310, 10621110, 10721120,
                  10921110, 10922110, 11021110, 11024120, 10724110, 10624110, 11023310],
    }
    for faction, ids in decks.items():
        (root / f"{faction}.json").write_text(json.dumps({
            "cards": [{"card_id": id, "copies": 3 if index < 13 else 1} for index, id in enumerate(ids)]
        }), encoding="utf-8")
    logs = root / "elf-royal"
    result = subprocess.run(
        [*command, "--deck-a", root / "elf.json", "--deck-b", root / "royal.json", "--log-dir", logs],
        check=True, capture_output=True, encoding="utf-8", timeout=30,
    )
    assert "games=4" in result.stdout and "feature_schema=wb-matchup-v2" in result.stdout
    mapping = json.loads((logs / "matchup_features.json").read_text(encoding="utf-8"))
    assert {10614120, 10624120, 10614122, 10624122, 90011110, 90021120, 90014330, 90024320, 90021320} <= {
        card["official_id"] for card in mapping["cards"]
    }
    rows = list(SAMPLE.iter_unpack((logs / "val_log.bin").read_bytes()))
    assert rows and all(0 < row[242] <= 1 and 0 < row[249] <= 1 for row in rows)
    check_replay(logs)


def check_witch_nemesis_decks(command, root):
    catalog = json.loads(
        (Path(__file__).resolve().parents[2] / "data/cards/svwb_rotation_ja.json").read_text(encoding="utf-8")
    )
    decks = {
        "witch": ("ウィッチ", [10131320, 10831310]),
        "nemesis": ("ネメシス", [10071110, 10072110, 10671110, 10672110, 10673110]),
    }
    for name, (faction, required) in decks.items():
        ids = required + [
            card["card_id"] for card in catalog
            if card["class"] == faction and card["card_id"] not in required
        ][:14 - len(required)]
        deck = [{"card_id": card_id, "copies": 3 if index < 13 else 1}
                for index, card_id in enumerate(ids)]
        (root / f"{name}.json").write_text(json.dumps({"cards": deck}), encoding="utf-8")
    logs = root / "witch-nemesis"
    subprocess.run(
        [*command, "--deck-a", root / "witch.json", "--deck-b", root / "nemesis.json", "--log-dir", logs],
        check=True, capture_output=True, encoding="utf-8", timeout=30,
    )
    mapping = json.loads((logs / "matchup_features.json").read_text(encoding="utf-8"))
    assert mapping["input_dim"] == 256
    official = {card["official_id"]: card["local_id"] for card in mapping["cards"]}
    assert official[10131320] != official[10831310]
    assert {90071210, 90071220, 90072110, 90072120, 90073110, 90073120, 90073130, 90074110} <= official.keys()
    check_replay(logs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("exe", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="svwb-wb-match-") as scratch:
        root = Path(scratch)
        weights = root / "weights.bin"
        weights.write_bytes(bytes(74376))
        command = [args.exe.resolve(), "--rules", "wb", "--games", "4", "--threads", "2",
                   "--simulations", "4", "--seed", "8787", "--weights", weights]
        subprocess.run([*command, "--log-dir", root], check=True, capture_output=True, timeout=30)
        check_replay(root)
        mirror_layout = json.loads((root / "matchup_features.json").read_text(encoding="utf-8"))
        assert mirror_layout["schema"] == "wb-mirror-v2"
        for row in SAMPLE.iter_unpack((root / "val_log.bin").read_bytes()):
            assert not any(row[194:230]) and not any(row[113:115]) and not any(row[233:235])
        result = subprocess.run(
            [*command, "--evaluate", "--random-opponent", "--analysis-dir", root, "--trace-top", "18",
             "--trace-game", "2"],
            check=True, capture_output=True, encoding="utf-8", timeout=30,
        )
        assert re.search(r"A_first=2 A_second=2 A_wins=\d+ B_wins=\d+ draws=\d+", result.stdout)
        check_exports(root)
        check_custom_decks(command, root)
        check_elf_royal_decks(command, root)
        check_witch_nemesis_decks(command, root)
        snapshot = subprocess.run(
            [*command, "--games", "10", "--snapshot-weights", weights, "--snapshot-bps", "2000"],
            check=True, capture_output=True, encoding="utf-8", timeout=30,
        )
        assert re.search(r"^games=10 .*mode=selfplay", snapshot.stdout)
        assert "snapshot_games=2" in snapshot.stdout and "A_first=5 A_second=5" in snapshot.stdout
    print("PASS WB logs, balanced exports and seat-invariant local IDs")


if __name__ == "__main__":
    main()
