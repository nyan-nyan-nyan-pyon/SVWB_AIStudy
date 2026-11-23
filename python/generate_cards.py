"""Validate the supplied card catalog and compile the effect overlays into C++ tables."""

import argparse
import json
from pathlib import Path
import re


KINDS = {"フォロワー": "Follower", "アミュレット": "Amulet", "スペル": "Spell", "信仰": "Faith"}
SUPPLEMENTS = {"tokens": "missing_tokens", "crests": "crests", "forms": "special_forms"}
CUSTOM = {"EvolveWithAlly", "BanishLowestAndEvolve"}
CREST_SOURCE_IDS = {"Testimony": 11063312, "Rituel": 10864122, "Rapid": 11064112}
FLAGS = {"FLAG_WARD", "FLAG_STORM", "FLAG_RUSH", "kBane", "kBarrier", "kAura",
         "kAmbush", "kIntimidate", "kAttackBlocked", "kDrain", "kDoubleStrike"}
EFFECT_FIELDS = {
    "trigger": "Trigger", "operation": "Operation", "target": "Target", "amount": int,
    "card": int, "health_modifier": int, "mode": int, "crest": "CrestId",
    "granted_abilities": FLAGS, "transformed_evolution": "Evolution",
    "condition": "Condition", "threshold": int, "handler": "CardHandler", "spellboost_multiplier": int,
    "filter": "DeckFilter",
}


def read(directory, name):
    return json.loads((directory / name).read_text(encoding="utf-8-sig"))


def literal(value):
    return json.dumps(value, ensure_ascii=False)


def cpp(value, kind):
    if isinstance(kind, set):
        masks = value if isinstance(value, list) else [value]
        if not all(flag in kind for flag in masks):
            raise ValueError(f"Unknown ability flag: {value}")
        return " | ".join(masks) or "0"
    if kind is str or kind is bool:
        if type(value) is not kind:
            raise ValueError(f"Expected {kind.__name__}, got {value!r}")
        return literal(value)
    if kind is int:
        if type(value) is not int:
            raise ValueError(f"Expected integer, got {value!r}")
        return str(value)
    if kind == "CrestId" and type(value) is int and 1 <= value <= 255:
        return f"static_cast<CrestId>({value})"
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z]\w*", value, re.ASCII):
        raise ValueError(f"Expected {kind} identifier, got {value!r}")
    return f"{kind}::{value}"


def effect_list(steps, ids, implemented):
    if len(steps) > 8:
        raise ValueError("At most eight effect steps can be attached to one definition")
    rows = []
    for item in steps:
        step = dict(item)
        if step.get("operation") in CUSTOM:
            step["handler"] = step["operation"]
            step["operation"] = "Custom"
        if "card_id" in step:
            step["card"] = ids[step.pop("card_id")]
        if step.get("card", 0) and step["card"] not in implemented:
            raise ValueError(f"Effect references a card without engine rules: {step['card']}")
        if not step.get("operation") or not step.get("trigger"):
            raise ValueError(f"Effect requires a trigger and an operation: {step}")
        fields = [f".{name}={cpp(step[name], kind)}" for name, kind in EFFECT_FIELDS.items() if name in step]
        if step.keys() - EFFECT_FIELDS.keys():
            raise ValueError(f"Unrecognized effect fields: {step.keys() - EFFECT_FIELDS.keys()}")
        rows.append("{" + ", ".join(fields) + "}")
    return "{{" + ", ".join(rows) + "}}"


def generate(directory):
    rotation = read(directory, "svwb_rotation_ja.json")
    manifest = read(directory, "svwb_supplements_manifest.json")
    supplements = {key: read(directory, f"svwb_{label}.json") for key, label in SUPPLEMENTS.items()}
    for key, rows in supplements.items():
        if manifest[key] != rows:
            raise ValueError(f"Supplement manifest does not match the {key} file")
    catalog = {}
    for card in rotation + [row["definition"] for row in supplements["tokens"] if row["definition"]]:
        official = card["card_id"]
        if type(official) is not int or not 0 < official <= 2**32 - 1 or official in catalog:
            raise ValueError(f"Invalid or duplicate official ID: {official}")
        catalog[official] = card
    overlay = read(directory, "engine_cards.json")
    extension = read(directory, "phase5b1_cards.json")
    research = {row["special_effect_id"]: row for row in read(directory, "svwb_special_effect_research.json")["records"]}
    if overlay["schema"] != 1 or extension["schema"] != 1:
        raise ValueError("Unknown card engine schema version")
    rules = overlay["cards"] + extension["cards"]
    forms = {row["source_card_id"]: row for row in research.values() if row["kind"] in ("crystallize", "accelerate")}
    for row in rules:
        if "base_card_id" in row:
            form = forms[row["base_card_id"]]
            row["card_id"] = form["special_effect_id"]
    if len({row["id"] for row in rules}) != len(rules):
        raise ValueError("Duplicate engine IDs")
    fixed = {row["card_id"]: row["id"] for row in rules if "card_id" in row}
    if len(fixed) != sum("card_id" in row for row in rules):
        raise ValueError("Duplicate official IDs in engine overlays")
    scope = {card["card_id"] for card in rotation if card["class"] in ("ニュートラル", "ビショップ")}
    if scope - fixed.keys():
        raise ValueError(f"Missing rotation implementations: {sorted(scope - fixed.keys())}")
    fixed_ids = {row["id"] for row in rules}
    next_ids = iter(number for number in range(101, 65536) if number not in fixed_ids)
    ids = dict(fixed)
    for official in sorted(catalog.keys() - fixed.keys()):
        ids[official] = next(next_ids)
    entries = [(ids[official], official, card, {}) for official, card in catalog.items()]
    entries.extend((row["id"], row.get("card_id", 0), catalog.get(row.get("card_id"), {}), row) for row in rules)
    entries = sorted({item[0]: item for item in entries}.values())
    if any(id != 101 + offset for offset, (id, _, _, _) in enumerate(entries)):
        raise ValueError("Card slots must be contiguous from 101")
    out = ["// Generated by generate_cards.py from the checked-in card data."]
    relations = {}
    for id, official, metadata, rules in entries:
        related = sorted({ids[other] for other in metadata.get("related_card_ids", []) if other in ids})
        if related:
            relations[id] = f"related_{id}"
            out.append(f"constexpr CardId {relations[id]}[]{{{', '.join(map(str, related))}}};")
    implemented = fixed_ids
    out.append("constexpr CardDef cards[]{")
    for id, official, metadata, rules in entries:
        form = forms[rules["base_card_id"]] if "base_card_id" in rules else None
        official_data = catalog[rules["base_card_id"]] if form else metadata
        text = (form["effect_text"] or form["effect_summary"]) if form else official_data.get("text", rules.get("text", ""))
        evolved_text = official_data.get("evolved_text", "") or ""
        keywords = sorted(set(re.findall(r"【([^】_\n]+)(?:_[^】]*)?】", text + "\n" + evolved_text)))
        kind = (form["form_type"] or ("スペル" if form["kind"] == "accelerate" else "アミュレット")) if form else official_data.get("type")
        values = {
            "id": id, "name": rules.get("name", official_data.get("name")), "kind": rules.get("kind", KINDS.get(kind)),
            "cost": form["play_cost"] if form else rules.get("cost", official_data.get("cost")),
            "atk": rules.get("atk", 0 if form else official_data.get("attack", 0)),
            "hp": rules.get("hp", 0 if form else official_data.get("defense", 0)),
            "abilities": rules.get("abilities", []), "countdown": (form["countdown"] or 0) if form else rules.get("countdown", 0),
            "act_cost": rules.get("act_cost", -1), "modes": rules.get("modes", 0),
            "enhance_cost": rules.get("enhance_cost", 0),
            "crystal_form": ids[rules["crystal_form_id"]] if "crystal_form_id" in rules else rules.get("crystal_form", 0),
            "clue": rules.get("clue", False),
        }
        types = {"name": str, "kind": "Kind", "abilities": FLAGS, "clue": bool}
        fields = [f".{key}={cpp(value, types.get(key, int))}" for key, value in values.items()]
        if values["crystal_form"] and values["crystal_form"] not in implemented:
            raise ValueError(f"Invalid crystal form on {id}")
        if rules.get("effects"):
            fields.append(f".effects={effect_list(rules['effects'], ids, implemented)}")
        fields.append(f".official_id={official}")
        strings = {"class_name": official_data.get("class", rules.get("class_name", "")),
                   "traits": "\n".join(official_data.get("traits") or []), "keywords": "\n".join(keywords),
                   "text": text, "evolved_text": evolved_text}
        fields.extend(f".{key}={literal(value)}" for key, value in strings.items() if value)
        if id in relations:
            fields.append(f".related={relations[id]}")
        if rules:
            fields.extend((".implemented=true", f".deck_eligible={literal(rules.get('deck_eligible', not form and bool(metadata)))}"))
            optional = {"earth": bool, "spellboost": bool, "spellboost_cost_reduction": int,
                        "fusion": "FusionMaterial", "fusion_trait": str}
            fields.extend(f".{key}={cpp(rules[key], kind)}" for key, kind in optional.items() if key in rules)
            if "accelerate_form_id" in rules:
                fields.append(f".accelerate_form={ids[rules['accelerate_form_id']]}")
            if form:
                fields.append(f".base_form={ids[rules['base_card_id']]}")
            for key, kind in {"all_modes_with_high_cost": bool, "super_evolve_hand_discount": int}.items():
                if key in rules:
                    fields.append(f".{key}={cpp(rules[key], kind)}")
        out.append("    {" + ", ".join(fields) + "},")
    out.append("};\nconstexpr CrestDef crests[]{")
    for row in overlay["crests"] + extension["crests"]:
        record = research[row.get("special_effect_id", CREST_SOURCE_IDS.get(row["id"]))]
        out.append(f"    {{{cpp(row['id'], 'CrestId')}, {cpp(record['countdown'] or 0, int)}, "
                   f"{cpp(row['clock'], 'CrestClock')}, {effect_list(row['effects'], ids, implemented)}, "
                   f"{literal(record['special_name'])}, {record['special_effect_id']}}},")
    out.append("};\nconstexpr OfficialCard official_cards[]{")
    out.extend(f"    {{{official}, {id}}}," for official, id in sorted(ids.items()))
    out.append("};\nconstexpr CardId rotation_batch[]{" + ", ".join(str(ids[id]) for id in sorted(scope)) + "};\n")
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generate(args.data), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
