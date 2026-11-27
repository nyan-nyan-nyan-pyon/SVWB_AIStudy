"""Prepare the test oracle and inventory from separate sources."""

import argparse
import json
import re
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
DATA = ROOT / "data/cards"
HERE = Path(__file__).resolve().parent
OVERLAYS = (
    "engine_cards", "phase5b1_cards", "phase5b2_cards", "phase5b3_cards",
    "phase5b4_cards", "rotation_high_cost_cards",
)
CREST_IDS = {"Testimony": 11063312, "Rituel": 10864122, "Rapid": 11064112}
KINDS = {"フォロワー": "Follower", "アミュレット": "Amulet", "スペル": "Spell", "信仰": "Faith"}
RAW_KINDS = {1: "Follower", 2: "Amulet", 3: "Amulet", 4: "Spell"}
RAW_CLASSES = ("ニュートラル", "エルフ", "ロイヤル", "ウィッチ", "ドラゴン", "ナイトメア", "ビショップ", "ネメシス")
OFFICIAL_RULES = "https://shadowverse-wb.com/ja/help?tab=tab0"
OFFICIAL_PAGE = "https://shadowverse-wb.com/ja/deck/cardslist/card/?card_id="
KEYWORDS = {
    "守護": "FLAG_WARD", "疾走": "FLAG_STORM", "突進": "FLAG_RUSH", "必殺": "kBane",
    "ドレイン": "kDrain", "潜伏": "kAmbush", "威圧": "kIntimidate", "オーラ": "kAura",
}
TRIGGERS = {
    "ファンファーレ": "Play", "ラストワード": "Death", "進化時": "Evolve", "超進化時": "Super",
}
PLAIN_TRIGGERS = {
    "これが場に出たとき、": "Enter",
    "自分のターン終了時、": "TurnEnd",
    "自分のターン開始時、": "TurnStart",
}
OPERATIONS = (
    ("Draw", r"自分のデッキから([1-9][0-9]*)枚を引く。"),
    ("Heal", r"自分のリーダーを([1-9][0-9]*)回復。"),
    ("FaceDamage", r"相手のリーダーに([1-9][0-9]*)ダメージ。"),
    ("ChosenDamage", r"相手の場のフォロワー1枚を選ぶ。それに([1-9][0-9]*)ダメージ。"),
    ("AllEnemyDamage", r"相手の場のフォロワーすべてに([1-9][0-9]*)ダメージ。"),
    ("AddHand", r"『([^』]+)』([1-9][0-9]*)枚を自分の手札に加える。"),
    ("Summon", r"『([^』]+)』([1-9][0-9]*)枚を自分の場に出す。"),
)


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def catalog():
    rotation = read(DATA / "svwb_rotation_ja.json")
    official = {
        row["card_id"]: dict(row, spec_kind=KINDS[row["type"]], rotation=True)
        for row in rotation
    }
    for row in read(DATA / "svwb_missing_tokens.json"):
        if row["definition"]:
            card = row["definition"]
            official[card["card_id"]] = dict(card, spec_kind=KINDS[card["type"]], rotation=False)
    for row in read(HERE / "official_supplement.json"):
        if row["card_id"] not in official and row["type_id"] in RAW_KINDS:
            official[row["card_id"]] = dict(
                row, spec_kind=RAW_KINDS[row["type_id"]], rotation=False,
                **{"class": RAW_CLASSES[row["class_id"]]},
            )
    return official


def inventory():
    official = catalog()
    research = {row["special_effect_id"]: row for row in read(DATA / "svwb_special_effect_research.json")["records"]}
    forms = {row["source_card_id"]: row for row in research.values()
             if row["kind"] in {"crystallize", "accelerate"}}
    entities = []
    for overlay in OVERLAYS:
        data = read(DATA / f"{overlay}.json")
        for row in data["cards"]:
            form = forms.get(row.get("base_card_id"))
            number = form["special_effect_id"] if form else row.get("card_id")
            faith = research.get(number)
            reference = official.get(number)
            if form:
                entity_type = form["kind"]
                source = form.get("effect_text_source") or form.get("research_source_url")
            elif (faith and faith["kind"] == "faith") or row.get("kind") == "Faith":
                entity_type = "faith"
                source = faith["research_source_url"] if faith else None
            else:
                entity_type = "card" if reference and reference["rotation"] else "token"
                source = OFFICIAL_PAGE + str(number) if number else None
            entities.append({
                "id": str(number) if number else f"Engine:{row['id']}",
                "engine_id": row["id"], "entity_type": entity_type,
                "name": (reference or {}).get("name") or (form or faith or {}).get("special_name") or row.get("name"),
                "source": source, "official": reference, "research": form or faith,
                "inventory_file": f"data/cards/{overlay}.json",
                "engine_branches": row.get("effects", []),
                "additional_engine_fields": {k: v for k, v in row.items()
                                             if k not in {"id", "card_id", "name", "effects"}},
            })
        for row in data["crests"]:
            number = row.get("special_effect_id") or CREST_IDS[row["id"]]
            record = research[number]
            entities.append({
                "id": str(number), "crest_enum": row["id"], "entity_type": "crest",
                "name": record["special_name"], "research": record,
                "source": record.get("effect_text_source") or record.get("research_source_url"),
                "inventory_file": f"data/cards/{overlay}.json", "engine_branches": row.get("effects", []),
                "additional_engine_fields": {"clock": row["clock"]},
            })
    ids = Counter(row["id"] for row in entities)
    if any(count != 1 for count in ids.values()):
        raise ValueError(f"duplicate entities: {[(key, n) for key, n in ids.items() if n != 1]}")
    return sorted(entities, key=lambda row: (row["entity_type"], row["id"]))


def sections(text):
    return [line.strip() for block in (text or "").split("------")
            for line in block.splitlines() if line.strip()]


def exact_effects(card, known_names):
    # Only full lines with an unambiguous trigger and a single, unconditional operation.
    # A bare spell must consist of this one line (a preceding condition might scope it).
    if any(marker in (card.get("text") or "") for marker in
           ("反復", "エンハンス", "プレイできない", "これのコスト", "【モード】", "【融合】")):
        return
    clauses = sections(card.get("text")) + sections(card.get("evolved_text"))
    for index, clause in enumerate(clauses):
        if clause in clauses[:index]:
            continue
        match = re.match(r"^【([^】]+)】(.*)", clause)
        if match and match[1] in TRIGGERS:
            trigger, body = TRIGGERS[match[1]], match[2]
        elif len(clauses) == 1 and card["spec_kind"] == "Spell":
            trigger, body = "Play", clause
        else:
            prefix = next((prefix for prefix in PLAIN_TRIGGERS if clause.startswith(prefix)), None)
            if not prefix:
                continue
            trigger, body = PLAIN_TRIGGERS[prefix], clause[len(prefix):]
            # A natural turn-start draw would confound the oracle for these two zone changes.
            if trigger == "TurnStart" and (body.endswith("枚を引く。") or "手札に加える。" in body):
                continue
        for operation, pattern in OPERATIONS:
            found = re.fullmatch(pattern, body)
            if not found:
                continue
            if operation in {"AddHand", "Summon"}:
                recipient = known_names.get(found[1], [])
                if len(recipient) != 1:
                    continue
                related, amount = recipient[0], int(found[2])
            else:
                related, amount = 0, int(found[1])
            yield index, trigger, operation, amount, related


def cases(entities):
    names = {}
    forms_by_base = {}
    for row in entities:
        if row.get("official") and "engine_id" in row:
            names.setdefault(row["official"]["name"], []).append(int(row["id"]))
        if row["entity_type"] in {"crystallize", "accelerate"}:
            record = row["research"]
            if record["play_cost"] is not None:
                forms_by_base[record["source_card_id"]] = record
    for row in entities:
        specs = []
        if row["entity_type"] == "crest":
            specs.append(("registry", 0))
            if row["research"]["countdown_status"] in {"present", "not_printed"} and (
                row["research"].get("effect_text_status") == "captured"
            ):
                specs.append(("printed-countdown", 0))
        else:
            specs.append(("registry", 0))
            card = row.get("official")
            if row["entity_type"] in {"crystallize", "accelerate"} and row["research"]["play_cost"] is not None:
                specs.append(("form-data", 0))
                specs.append(("form-play", 0))
            if card and row["entity_type"] in {"card", "token"}:
                specs.append(("printed-data", 0))
                text = card.get("text") or ""
                form = forms_by_base.get(int(row["id"]))
                if form and form["play_cost"] > 0:
                    specs.append(("below-form-cost", form["play_cost"]))
                elif card["cost"] > 0 and not any(
                    marker in text for marker in ("【結晶", "【アクセラレート")
                ):
                    specs.append(("below-printed-cost", card["cost"]))
                if countdown := re.search(r"【カウントダウン_(\d+)】", text):
                    specs.append(("printed-amulet-countdown", int(countdown[1])))
                    if card["spec_kind"] == "Amulet" and not any(
                        marker in text for marker in (
                            "カウントを", "ターン終了時", "ターン開始時", "エンハンス",
                            "【モード】", "プレイできない", "プレイしたとき",
                        )
                    ):
                        specs.append(("countdown-advance", int(countdown[1])))
                if card["spec_kind"] == "Amulet":
                    specs.append(("amulet-combat", 0))
                if card["spec_kind"] in {"Follower", "Amulet"} and "プレイできない" not in text:
                    specs.extend((("full-board", 0), ("play", 0)))
                elif "プレイできない" in text:
                    specs.append(("cannot-play", 0))
                elif card["spec_kind"] == "Spell" and (
                    "選ぶ" not in text or "相手の場のフォロワー1枚を選ぶ。" in text
                ):
                    specs.append(("play-spell", 0))
                clauses = sections(text) + sections(card.get("evolved_text"))
                if card["spec_kind"] == "Follower":
                    for index, clause in enumerate(sections(text)):
                        keyword = re.fullmatch(r"【([^】]+)】", clause)
                        if keyword and keyword[1] in KEYWORDS and clause not in clauses[:index]:
                            flag = KEYWORDS[keyword[1]]
                            specs.append((f"keyword-{index + 1}-{flag}", (index, flag)))
                if card["spec_kind"] == "Follower" and all(
                    re.fullmatch("【(?:" + "|".join(KEYWORDS) + ")】", clause) for clause in clauses
                ):
                    specs.append(("attack-window", 0))
                    specs.append(("intimidate" if "【威圧】" in clauses else "combat-destruction", 0))
                for effect in exact_effects(card, names):
                    index, trigger, operation, amount, related = effect
                    prefix = f"effect-{index + 1}-{trigger}-{operation}"
                    specs.append((prefix, effect))
                    if trigger == "Play" and operation == "ChosenDamage":
                        specs.append((prefix + "-no-target", effect))
                    if trigger == "Play" and operation == "Summon":
                        specs.append((prefix + "-board-cap", effect))
                    if trigger == "Play" and operation in {"AddHand", "Draw"} and amount > 1:
                        specs.append((prefix + "-hand-cap", effect))
                    if trigger == "Play" and operation == "Heal" and amount > 1:
                        specs.append((prefix + "-heal-cap", effect))
        yield row, specs


def generate(output, entities):
    lines = ["// Generated from official snapshots and implementation inventory."]
    card_count = sum(row["entity_type"] != "crest" for row in entities)
    lines.append(f"add_inventory(suite, {card_count}, {len(entities) - card_count});")
    for row, specs in cases(entities):
        number = int(row["id"]) if row["id"].isdigit() else 0
        key = f'catalog.{row["id"]}'
        if row["entity_type"] == "crest":
            crest = row["crest_enum"]
            countdown = row["research"]["countdown"] or 0
            lines.append(f'add_crest(suite, "{key}", CrestId::{crest}, {number}, {countdown}, '
                         f'{str(any(name == "printed-countdown" for name, _ in specs)).lower()});')
            continue
        lines.append(f'add_registry(suite, "{key}", {row["engine_id"]}, {number});')
        card = row.get("official")
        if row["entity_type"] in {"crystallize", "accelerate"} and row["research"]["play_cost"] is not None:
            record = row["research"]
            kind = "Amulet" if row["entity_type"] == "crystallize" else "Spell"
            lines.append(f'add_form(suite, "{key}", {number}, {record["source_card_id"]}, '
                         f'Kind::{kind}, {record["play_cost"]}, {record["countdown"] or 0});')
        if not card or row["entity_type"] not in {"card", "token"}:
            continue
        text = card.get("text") or ""
        flags = " | ".join(flag for jp, flag in KEYWORDS.items()
                           if any(re.fullmatch(rf"(?:【[^】]+】)*【{jp}】(?:【[^】]+】)*", clause)
                                  for clause in sections(text))) or "0"
        flags = ", ".join((flags, f'{card["cost"]}', f'{card.get("attack") or 0}',
                           f'{card.get("defense") or 0}'))
        kind = card["spec_kind"]
        rotation = str(card["rotation"]).lower()
        lines.append(f'const Printed p{number}{{{number}, Kind::{kind}, {flags}, {rotation}}};')
        for name, data in specs[1:]:
            if name.startswith("keyword-"):
                _, flag = data
                lines.append(f'add_keyword(suite, "{key}.{name}", p{number}, {flag});')
            elif name.startswith("effect-"):
                _, trigger, operation, amount, related = data
                if name.endswith("-no-target"):
                    lines.append(f'add_no_target(suite, "{key}.{name}", p{number});')
                elif name.endswith("-board-cap"):
                    lines.append(f'add_summon_board_cap(suite, "{key}.{name}", p{number});')
                elif name.endswith("-hand-cap"):
                    lines.append(f'add_hand_cap(suite, "{key}.{name}", p{number}, Check::{operation}, {amount}, {related});')
                elif name.endswith("-heal-cap"):
                    lines.append(f'add_heal_cap(suite, "{key}.{name}", p{number});')
                else:
                    lines.append(
                        f'add_effect(suite, "{key}.{name}", p{number}, Event::{trigger}, '
                        f'Check::{operation}, {amount}, {related});'
                    )
            elif name in {"below-printed-cost", "below-form-cost"}:
                lines.append(f'add_below_cost(suite, "{key}.{name}", p{number}, {data});')
            elif name in {"printed-amulet-countdown", "countdown-advance"}:
                lines.append(f'add_{name.replace("-", "_")}(suite, "{key}.{name}", p{number}, {data});')
            else:
                lines.append(f'add_{name.replace("-", "_")}(suite, "{key}.{name}", p{number});')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.output, inventory())


if __name__ == "__main__":
    main()
