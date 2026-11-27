"""Review baseline ledger gaps with explicitly dated verification evidence."""

import argparse
from collections import Counter, defaultdict, deque
import json
from pathlib import Path


CARD_OUTPUTS = {
    "AddHand", "AddHandModified", "Summon", "SummonModified", "SummonEvolved", "SummonEnemy",
    "ShuffleIntoDeck", "Transform", "TransformNamed", "UpgradeFaith", "Custom",
}
KEYWORDS = {"守護", "必殺", "潜伏", "ドレイン", "威圧", "疾走", "突進", "オーラ", "バリア"}


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def reachable(deck_dir, entities):
    direct = defaultdict(dict)
    for path in sorted(deck_dir.glob("*.json")):
        for item in read(path)["cards"]:
            direct[str(item["card_id"])][path.stem] = item["copies"]
    if not direct:
        raise ValueError("no deck manifests were loaded")
    by_crest = {row["crest_enum"]: row["id"] for row in entities.values() if "crest_enum" in row}
    forms = defaultdict(set)
    for row in entities.values():
        base = row["additional_engine_fields"].get("base_card_id")
        if base:
            forms[str(base)].add(row["id"])
    paths = {card: [card] for card in direct}
    pending = deque(sorted(direct))
    while pending:
        card = pending.popleft()
        row = entities[card]
        outputs = set(forms[card])
        for effect in row["engine_branches"]:
            if effect["operation"] in CARD_OUTPUTS:
                outputs.update(str(effect[key]) for key in ("card_id", "alternate_card_id") if effect.get(key))
            if effect.get("crest") in by_crest:
                outputs.add(by_crest[effect["crest"]])
        fields = row["additional_engine_fields"]
        outputs.update(str(item) for item in fields.get("fusion_results", []))
        if fields.get("faith_id"):
            outputs.add(str(fields["faith_id"]))
        for produced in sorted(outputs):
            if produced in entities and produced not in paths:
                paths[produced] = [*paths[card], produced]
                pending.append(produced)
    return direct, paths


def priority(row, text, direct, paths):
    if row["id"] in direct:
        return 1
    if row["id"] in paths:
        return 2
    if text.strip("【】") in KEYWORDS:
        return 3
    if row["entity_type"] in {"crest", "faith", "crystallize"} or row.get("kind") == "Amulet":
        return 4
    if "モード" in text or "なら" in text or "とき" in text or "【" in text:
        return 5
    return 6


def current_status(cases):
    statuses = {case["status"] for case in cases}
    if "FAIL" in statuses:
        return "EXECUTED_FAILURE"
    if "PASS" in statuses:
        return "EXECUTED_PASS"
    return "IMPLEMENTED_NOT_RUN" if cases else "NO_LINKED_TEST"


def reviewed_status(cases, historical):
    current = current_status(cases)
    if current in {"EXECUTED_PASS", "EXECUTED_FAILURE"}:
        return current
    return "VERIFIED_IN_PHASE2" if historical else current


def result(baseline, updated, deck_dir):
    rows = {row["id"]: row for row in updated["entities"]}
    direct, paths = reachable(deck_dir, rows)
    clauses = []
    branches = []
    for old in baseline["entities"]:
        row = rows[old["id"]]
        current = {clause["text"]: clause for clause in row["clause_coverage"]}
        for clause in old["clause_coverage"]:
            text = clause["text"]
            new = current[text]
            key = {"entity_id": old["id"], "name": old["name"], "priority": priority(row, text, direct, paths),
                   "clause_index": clause["index"], "clause": text, "source": row["source"],
                   "deck_adoption": direct.get(row["id"], {}), "static_reference_path": paths.get(row["id"], [])}
            old_links = clause["cases"]
            new_links = new["cases"]
            historical = new.get("historical_cases", {})
            flattened = [item for cases in old_links.values() for item in cases]
            if current_status(flattened) not in {"EXECUTED_PASS", "EXECUTED_FAILURE"}:
                cases = [item for cases in new_links.values() for item in cases]
                past = [item for cases in historical.values() for item in cases]
                clauses.append({**key, "phase3_status": reviewed_status(cases, past), "cases": cases + past})
            for branch in clause["branches_in_metric"]:
                if current_status(old_links.get(branch, [])) in {"EXECUTED_PASS", "EXECUTED_FAILURE"}:
                    continue
                if branch not in new["branches_in_metric"]:
                    branches.append({**key, "branch": branch, "phase3_status": "NOT_APPLICABLE", "cases": [],
                                     "reason": "This branch is absent from the reviewed branch criteria."})
                    continue
                cases = new_links.get(branch, [])
                past = historical.get(branch, [])
                branches.append({**key, "branch": branch, "phase3_status": reviewed_status(cases, past),
                                 "cases": cases + past})
    for collection in (clauses, branches):
        collection.sort(key=lambda item: (item["priority"], item["entity_id"], item["clause_index"], item.get("branch", "")))
    return {
        "scope": "Items flagged by the original Phase 2 ledger, with reviewed historical evidence and separate Phase 3 results",
        "reach_model": "Ordered traversal of explicit effect operands, forms, crests and fusion outputs; static references may over-approximate reach and do not cover every dynamic handler.",
        "direct_deck_cards": len(direct), "explicit_reference_cards_and_specials": len(paths) - len(direct),
        "phase2_clause_gaps": len(clauses), "phase2_branch_gaps": len(branches),
        "clauses_by_status": dict(Counter(row["phase3_status"] for row in clauses)),
        "branches_by_status": dict(Counter(row["phase3_status"] for row in branches)),
        "clauses_by_priority": dict(Counter(row["priority"] for row in clauses)),
        "branches_by_priority": dict(Counter(row["priority"] for row in branches)),
        "clauses": clauses, "branches": branches,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("baseline", "current", "decks", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    backlog = result(read(args.baseline), read(args.current), args.decks)
    args.output.write_text(json.dumps(backlog, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in backlog.items() if key not in {"clauses", "branches"}}, indent=2))


if __name__ == "__main__":
    main()
