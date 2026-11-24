"""Import a Shadowverse: Worlds Beyond Deck Portal URL or hash as deck JSON."""

import argparse
from collections import Counter
import json
from pathlib import Path
import re
from urllib.parse import parse_qs, urlsplit


CATALOG = Path(__file__).resolve().parents[1] / "data/cards/svwb_rotation_ja.json"
CLASSES = ("ニュートラル", "エルフ", "ロイヤル", "ウィッチ", "ドラゴン", "ナイトメア", "ビショップ", "ネメシス")
DIGITS = {char: i for i, char in enumerate("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_")}
DECK_SIZE = 40


def read_hash(source):
    source = source.strip()
    if source.startswith(("hash=", "?hash=")):
        query = source.removeprefix("?")
    else:
        url = urlsplit(source)
        if not url.scheme and not url.netloc and not url.query and not url.fragment:
            return source
        if url.scheme not in ("http", "https") or url.hostname not in ("shadowverse-wb.com", "www.shadowverse-wb.com"):
            raise ValueError("Expected an official Deck Portal URL or a hash")
        query = url.query
    values = parse_qs(query, keep_blank_values=True).get("hash", [])
    if len(values) != 1 or not values[0]:
        raise ValueError("Expected exactly one nonempty hash parameter")
    return values[0]


def decode_card(token):
    if len(token) != 4 or any(char not in DIGITS for char in token):
        raise ValueError(f"Invalid card token {token!r}. Expected four characters from the Deck Portal alphabet.")
    card_id = 0
    for char in token:
        card_id = card_id * 64 + DIGITS[char]
    return card_id


def import_deck(source, catalog_path=CATALOG, name=None):
    deck_hash = read_hash(source)
    header = deck_hash.split(".", 2)
    if len(header) != 3 or not re.fullmatch(r"[0-9]+", header[0]) or not re.fullmatch(r"[1-7]", header[1]):
        raise ValueError("Expected a numeric format/version followed by a class ID from 1 through 7")
    version_field, class_field, encoded_cards = header
    version, class_id = int(version_field), int(class_field)
    class_name = CLASSES[class_id]
    ids = [decode_card(token) for token in encoded_cards.split(".")]
    copies = Counter(ids)
    catalog = {
        card["card_id"]: card for card in json.loads(catalog_path.read_text(encoding="utf-8-sig"))
        if card["rotation"] and not card["token"]
    }
    errors = []
    if len(ids) != DECK_SIZE:
        errors.append(f"Expected {DECK_SIZE} cards, got {len(ids)}")
    unknown = sorted(copies.keys() - catalog.keys())
    if unknown:
        errors.append("Unregistered rotation card IDs: " + ", ".join(f"{card_id} x{copies[card_id]}" for card_id in unknown))
    names = Counter()
    for card_id in sorted(copies.keys() & catalog.keys()):
        card = catalog[card_id]
        if card["class"] not in (CLASSES[0], class_name):
            errors.append(f"{card_id} {card['name']} is {card['class']}, outside {class_name} + {CLASSES[0]}")
        names[card["name"]] += copies[card_id]
    errors.extend(f"{card} has {count} copies by name (maximum 3)" for card, count in sorted(names.items()) if count > 3)
    if errors:
        raise ValueError("\n".join(errors))
    return {
        "name": name or class_name,
        "class": class_name,
        "class_id": class_id,
        "format_version": version,
        "source_hash": deck_hash,
        "cards": [{"card_id": card_id, "copies": count} for card_id, count in sorted(copies.items())],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="Official URL, hash= query, or raw hash")
    parser.add_argument("--output", "-o", type=Path, help="Deck JSON destination (default: stdout)")
    parser.add_argument("--catalog", type=Path, default=CATALOG)
    parser.add_argument("--name", help="Display name (default: class name)")
    args = parser.parse_args()
    try:
        deck = import_deck(args.source, args.catalog, args.name)
        contents = json.dumps(deck, ensure_ascii=False, indent=2) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(contents, encoding="utf-8")
        else:
            print(contents, end="")
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
