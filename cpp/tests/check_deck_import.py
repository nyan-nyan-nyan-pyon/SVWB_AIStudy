import argparse
from itertools import combinations_with_replacement
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))
from import_deck_url import CATALOG, DIGITS, decode_card, import_deck
from play_ui import EngineManager, list_choices


DECKS = ROOT / "data/decks/phase5c"
PORTAL = "https://shadowverse-wb.com/ja/deck/detail/?hash="
ALPHABET = "".join(DIGITS)


def token(card_id):
    return "".join(ALPHABET[(card_id >> shift) & 63] for shift in (18, 12, 6, 0))


class DeckImport(unittest.TestCase):
    def setUp(self):
        self.elf_hash = json.loads((DECKS / "elf.json").read_text(encoding="utf-8"))["source_hash"]

    def test_input_formats(self):
        self.assertEqual(decode_card("e4Gg"), 10503210)
        self.assertEqual(decode_card("ZZ-_"), int("100011100011111110111111", 2))
        expected = import_deck(self.elf_hash)
        escaped = self.elf_hash.replace(".", "%2E")
        for source in ("hash=" + escaped, "?hash=" + escaped, PORTAL + escaped + "&lang=ja#deck"):
            self.assertEqual(import_deck(source), expected)

    def test_malformed_inputs(self):
        prefix = self.elf_hash.rsplit(".", 1)[0]
        for source, diagnosis in (
            (self.elf_hash + ".e4Gg", r"got 41"),
            (prefix, r"got 39"),
            (prefix + ".@@@@", r"Invalid card token"),
            (prefix + ".0000", r"Unregistered rotation card IDs: 0 x1"),
            (self.elf_hash.replace("1.1.", "1.4.", 1), r"outside"),
            (self.elf_hash.replace("1.1.", "1.8.", 1), r"class ID"),
            ("https://example.org/?hash=" + self.elf_hash, r"official Deck Portal"),
            (PORTAL + self.elf_hash + "&hash=" + self.elf_hash, r"exactly one"),
        ):
            with self.subTest(source=source), self.assertRaisesRegex(ValueError, diagnosis):
                import_deck(source)

    def test_duplicate_name_across_ids(self):
        witch = [
            card["card_id"] for card in json.loads(CATALOG.read_text(encoding="utf-8"))
            if card["class"] == "ウィッチ" and card["name"] != "ストームブラスト"
        ]
        codes = [token(card) for card in [10131320, 10131320, 10831310, 10831310, *witch[:36]]]
        with self.assertRaisesRegex(ValueError, r"4 copies by name"):
            import_deck("1.3." + ".".join(codes))


def check_registered(stress_cli, play_cli):
    paths = sorted(DECKS.glob("*.json"))
    assert len(paths) >= 2
    names = {card["card_id"]: card["name"] for card in json.loads(CATALOG.read_text(encoding="utf-8"))}
    decks = {}
    for path in paths:
        deck = json.loads(path.read_text(encoding="utf-8"))
        assert import_deck(PORTAL + deck["source_hash"], name=deck["name"]) == deck, path
        decks[path] = {names[entry["card_id"]] for entry in deck["cards"]}

    with tempfile.TemporaryDirectory(prefix="svwb-deck-import-") as scratch:
        weights = Path(scratch) / "weights.bin"
        weights.write_bytes(bytes(74376))
        choices, _ = list_choices(ROOT / "data/decks", "**/*.json", None, "Default")
        ids = {path: key for key, path in choices.items() if path is not None}
        engine = EngineManager(play_cli, 1, 2026, {"default": weights}, choices)
        try:
            for a, b in combinations_with_replacement(paths, 2):
                reply = engine.request({"command": "new_game", "seat": "first", "debug": True,
                                        "deck_a": ids[a], "deck_b": ids[b]})
                assert reply["ok"], reply
                state = reply["state"]
                assert state["deck_a"] == ids[a] and state["deck_b"] == ids[b]
                for role, path in (("human", a), ("opponent", b)):
                    hand = state[role]["hand"]
                    assert len(hand) == 4 and {card["name"] for card in hand} <= decks[path]
                completed = subprocess.run(
                    [stress_cli, "--deck-pair-stress", a, b, "1000"], check=True, capture_output=True,
                    encoding="utf-8", timeout=20,
                )
                assert "games=1000" in completed.stdout and "invariant_failures=0" in completed.stdout, completed.stdout
                print(a.stem, "vs", b.stem, completed.stdout.splitlines()[1], flush=True)
        finally:
            engine.close()


def main():
    parser = argparse.ArgumentParser(description="Test portal import and registered deck play")
    parser.add_argument("--stress-cli", type=Path, required=True)
    parser.add_argument("--play-cli", type=Path, required=True)
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(DeckImport)
    if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful():
        raise SystemExit(1)
    check_registered(args.stress_cli.resolve(), args.play_cli.resolve())


if __name__ == "__main__":
    main()
