"""Generate the opt-in Phase 2 catalog without building or mutating any files."""

import argparse
import itertools
import json
from collections import defaultdict, deque
from pathlib import Path

from spec_data import DATA, HERE, OVERLAYS, read


AMOUNTS = {"Damage", "Heal", "Draw", "AddHand", "Summon", "Buff", "RestorePp", "IncreaseMaxPp"}
TARGETS = {"EnemyLeader": "OwnLeader", "OwnLeader": "EnemyLeader",
           "ChosenEnemyFollower": "RandomEnemyFollower", "AllEnemyFollowers": "AllOwnFollowers"}
TIMINGS = {"OnPlay": "TurnEnd", "TurnEnd": "TurnStart", "OnEvolve": "OnSuperEvolve",
           "OnSuperEvolve": "OnEvolve", "LastWords": "OnPlay"}
OPS = {"Destroy": "Banish", "Banish": "Destroy"}
EXACT_TRIGGER = {"OnPlay": "Play", "OnEvolve": "Evolve", "OnSuperEvolve": "Super",
                 "LastWords": "Death", "OnEnter": "Enter", "TurnEnd": "TurnEnd", "TurnStart": "TurnStart"}


def probes(report):
    by_id = defaultdict(list)
    for entity in report["entities"]:
        key = entity["id"]
        for case in entity["generated_cases"]:
            if case["status"] == "PASS" and case["checks"].startswith("effect-"):
                by_id[key].append((case["case"], "exact"))
        for branch in entity["manual_branches"]:
            if branch["status"] == "PASS" and "Observation" not in branch["entity_id"]:
                by_id[key].append((branch["case_name"], "card-level"))
    return by_id


def candidates(report):
    known = probes(report)
    for overlay in OVERLAYS:
        for card in read(DATA / f"{overlay}.json")["cards"]:
            official = card.get("card_id")
            available = known.get(str(official))
            if not available:
                continue
            steps = card.get("effects", [])
            base = {"file": f"data/cards/{overlay}.json", "section": "cards",
                    "entity": {"card_id": official}}
            for index, step in enumerate(steps):
                operation = step["operation"]
                amount = step.get("amount")
                target = step.get("target")
                trigger = step["trigger"]
                options = []
                if operation in AMOUNTS and isinstance(amount, int) and 0 < amount <= 10:
                    options.extend((("amount_up", "amount", amount + 1), ("amount_down", "amount", amount - 1)))
                if isinstance(step.get("threshold"), int) and 0 < step["threshold"] < 30:
                    options.append(("threshold_up", "threshold", step["threshold"] + 1))
                if step.get("condition") not in (None, "Always", "CostPaid"):
                    options.append(("remove_condition", "condition", "Always"))
                if trigger in TIMINGS:
                    options.append(("timing", "trigger", TIMINGS[trigger]))
                if target in TARGETS:
                    options.append(("target_scope", "target", TARGETS[target]))
                if operation in OPS:
                    options.append(("removal_kind", "operation", OPS[operation]))
                keyword = EXACT_TRIGGER.get(trigger)
                exact = [value for value in available if value[1] == "exact" and keyword
                         and f"-{keyword}-" in value[0]
                         and (operation in value[0] or operation == "Damage" and "Damage" in value[0])
                         and not value[0].endswith(("-no-target", "-board-cap", "-hand-cap", "-heal-cap"))]
                case, confidence = exact[0] if exact else next(
                    (value for value in available if value[1] == "card-level"), available[0])
                if not exact:
                    confidence = "card-level"
                for family, field, after in options:
                    yield {**base, "name": f"phase2_{official}_{index + 1}_{family}",
                           "family": family, "effect_index": index, "original_effect": step,
                           "field": field, "before": step.get(field), "after": after,
                           "case": case, "probe_scope": confidence}
                if index + 1 < len(steps) and step != steps[index + 1] and trigger == steps[index + 1]["trigger"]:
                    next_step = steps[index + 1]
                    if {operation, next_step["operation"]} & {"Destroy", "Banish", "Draw", "Summon", "Heal"}:
                        yield {**base, "name": f"phase2_{official}_{index + 1}_effect_order",
                               "family": "effect_order", "swap_effects": [index, index + 1],
                               "original_effects": [step, next_step], "case": case, "probe_scope": "card-level"}


def balanced(rows, size):
    families = defaultdict(deque)
    for row in sorted(rows, key=lambda r: (r["probe_scope"] != "exact", r["name"])):
        families[row["family"]].append(row)
    groups = [families[key] for key in sorted(families)]
    if not groups:
        raise ValueError("no mutation candidates")
    for index in itertools.cycle(range(len(groups))):
        if size == 0:
            return
        if groups[index]:
            yield groups[index].popleft()
            size -= 1
        elif not any(groups):
            raise ValueError("too few mutation candidates")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--coverage", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--count", type=int, default=180)
    args = parser.parse_args()
    seeds = read(HERE / "mutations.json") + read(HERE / "phase2_mutation_smoke.json")
    if not 100 <= args.count <= 300:
        parser.error("--count must be between 100 and 300")
    rows = seeds + list(balanced(candidates(read(args.coverage)), args.count - len(seeds)))
    names = {row["name"] for row in rows}
    if len(names) != len(rows):
        raise ValueError("duplicate mutation names")
    args.output.write_text(json.dumps(rows, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
