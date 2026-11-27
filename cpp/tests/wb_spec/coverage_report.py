"""Write the reproducible card, effect, and test result inventory."""

import argparse
import csv
import json
import re
from collections import Counter, defaultdict
from pathlib import Path
from xml.etree import ElementTree

from spec_data import DATA, HERE, ROOT, cases, inventory, read, sections


def case_results(path):
    results = {}
    summary = None
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if found := re.fullmatch(r"(PASS|FAIL) (\S+)(?: \| (.*))?", line):
            status, name, error = found.groups()
            if name in results:
                raise ValueError(f"Duplicate result {name}")
            results[name] = {"status": status, "error": error}
        elif found := re.fullmatch(r"SUMMARY pass=(\d+) fail=(\d+)", line):
            summary = {"PASS": int(found[1]), "FAIL": int(found[2])}
    totals = Counter(value["status"] for value in results.values())
    if summary is None or any(totals[key] != count for key, count in summary.items()):
        raise ValueError("Test run is missing its final summary or results are incomplete")
    return results


def manual_mappings(executed):
    by_entity = defaultdict(list)
    fields = ["entity_id", "effect_or_branch", "case_name", "source", "note"]
    for fragment in sorted((ROOT / "docs/spec-verification").glob("*.tsv")):
        with fragment.open(encoding="utf-8-sig", newline="") as stream:
            reader = csv.DictReader(stream, delimiter="\t", quoting=csv.QUOTE_NONE)
            if reader.fieldnames != fields:
                raise ValueError(f"Unexpected columns in {fragment.name}: {reader.fieldnames}")
            for row in reader:
                if None in row or any(value is None for value in row.values()):
                    raise ValueError(f"Incomplete row in {fragment.name}:{reader.line_num}")
                for entity in row["entity_id"].split(","):
                    by_entity[entity.strip()].append({**row, "fragment": fragment.name,
                                                      "line": reader.line_num,
                                                      "status": status(row["case_name"], executed)})
    return by_entity


def status(name, executed):
    return executed.get(name, {"status": "NO RESULT", "error": None})["status"]


def legacy_mentions():
    path = ROOT / "cpp/tests/test_wb.cpp"
    matches = defaultdict(set)
    scope = None
    for line_number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        if function := re.match(r"(?:void|int) (\w+)\(", line):
            scope = function[1]
        for mentioned in re.findall(r"(?:official|find_official_card)\((\d{8})\)", line):
            matches[mentioned].add((scope or "global", line_number))
    return {key: [{"function": fn, "line": number} for fn, number in sorted(found)]
            for key, found in matches.items()}


def legacy_play_results(path):
    if path is None:
        return {}
    groups = {
        "rotation_card": "wb_rotation_101_unit",
        "elf_royal_card": "wb_elf_royal_123_unit",
        "dragon_nightmare_card": "wb_dragon_nightmare_119_unit",
        "witch_nemesis_card": "wb_witch_nemesis_126_unit",
        "high_cost_card": "wb_rotation_high_cost_47_unit",
    }
    passed = {}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if match := re.fullmatch(r"PASS (\w+_card) (\d{8})", line):
            if match[1] not in groups or match[2] in passed:
                raise ValueError(f"Unexpected legacy play result: {line}")
            passed[match[2]] = {"ctest_group": groups[match[1]], "status": "PASS"}
    return passed


CLAUSE_TAG = re.compile(r"\[clauses:\s*([^\]]+)\]")
BOUNDARY_SUFFIXES = {
    "-no-target": "no_target", "-board-cap": "board_cap", "-hand-cap": "hand_cap", "-heal-cap": "heal_cap",
}
CONDITIONAL = re.compile(
    r"なら|なければ|ときは|以上のとき|以下のとき|につき1回|"
    r"【(?:コンボ_\d+|(?:連携|覚醒|土の秘術|ネクロマンス|エンハンス|奥義|解放奥義)(?:_\d+)?)】"
)


def phase2_evidence(spec_log, legacy_junit):
    spec_cases = case_results(spec_log) if spec_log else {}
    legacy_passes = set()
    if legacy_junit:
        for group in ElementTree.parse(legacy_junit).iter("testcase"):
            if group.get("status") != "run" or any(group.find(tag) is not None for tag in ("failure", "error", "skipped")):
                continue
            legacy_passes.update((group.get("name"), line.removeprefix("PASS "))
                                 for line in group.findtext("system-out", "").splitlines()
                                 if line.startswith("PASS "))

    reviewed = defaultdict(list)
    with (HERE / "phase2-reviewed-clauses.tsv").open(encoding="utf-8", newline="") as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            name, group = row["case"], row["ctest_group"]
            log = legacy_junit if group else spec_log
            observed = (group, name) in legacy_passes if group else status(name, spec_cases) == "PASS"
            if log is not None and not observed:
                raise ValueError(f"Reviewed Phase 2 case has no passing result: {name} in {log}")
            if observed:
                key = (row["entity_id"], row["clause"], row["branch"])
                reviewed[key].append({"case": name, "status": "PASS", "run": "Phase 2", "log": str(log),
                                      "source": row["source"], "assertion": row["assertion"],
                                      **({"ctest_group": group} if group else {})})
    return reviewed, spec_cases


def add_phase2_evidence(key, clauses, generated, historical):
    reviewed, spec_cases, spec_log = historical
    for clause in clauses:
        links = {branch: list(reviewed.get((key, clause["text"], branch), []))
                 for branch in clause["branches_in_metric"]}
        for case in generated:
            if case["checks"] != "countdown-advance" or case.get("official_clause_index") != clause["index"]:
                continue
            if status(case["case"], spec_cases) == "PASS":
                links["active"].append({"case": case["case"], "status": "PASS", "run": "Phase 2",
                                        "log": str(spec_log), "source": "cpp/tests/wb_spec/catalog.cpp",
                                        "assertion": "Checks each countdown value through expiry."})
        if any(links.values()):
            clause["historical_cases"] = {branch: cases for branch, cases in links.items() if cases}


def clause_links(key, texts, generated, manual, executed):
    unique = {}
    for index, text in enumerate(texts, 1):
        unique.setdefault(text, {"index": index, "text": text, "cases": defaultdict(set)})
    for entry in generated:
        if "official_clause_index" not in entry:
            continue
        clause = unique[texts[entry["official_clause_index"] - 1]]
        outcome = next((branch for suffix, branch in BOUNDARY_SUFFIXES.items()
                        if entry["case"].endswith(suffix)), "active")
        clause["cases"][outcome].add(entry["case"])
    for entry in manual:
        if entry["entity_id"] != key:
            continue
        for label in CLAUSE_TAG.findall(entry["note"]):
            for mapping in label.split(";"):
                if not mapping.strip():
                    continue
                if "=" not in mapping:
                    raise ValueError(f'{entry["fragment"]}:{entry["line"]}: malformed branch {mapping}')
                ordinal, outcomes = mapping.strip().split("=", 1)
                index = int(ordinal)
                if not 1 <= index <= len(texts):
                    raise ValueError(f'{entry["fragment"]}:{entry["line"]}: unknown clause index {index} for {key}')
                for outcome in map(str.strip, outcomes.split(",")):
                    if outcome not in {"active", "inactive", "partial", *BOUNDARY_SUFFIXES.values()}:
                        raise ValueError(f'{entry["fragment"]}:{entry["line"]}: unknown branch {outcome}')
                    unique[texts[index - 1]]["cases"][outcome].add(entry["case_name"])
    for row in unique.values():
        text = row["text"]
        expected = {"active"}
        if CONDITIONAL.search(text) or row["cases"].get("inactive"):
            expected.add("inactive")
        if "選ぶ。" in text or row["cases"].get("no_target"):
            expected.add("no_target")
        expected.update(outcome for outcome in ("board_cap", "hand_cap", "heal_cap") if row["cases"].get(outcome))
        bindings = {outcome: [{"case": name, "status": status(name, executed)} for name in sorted(names)]
                    for outcome, names in sorted(row["cases"].items())}
        yield {**row, "branches_in_metric": sorted(expected), "cases": bindings}


def clause_metrics(rows):
    counts = Counter()
    for row in rows:
        for clause in row["clause_coverage"]:
            counts["unique_clauses"] += 1
            required = set(clause["branches_in_metric"])
            executed = {branch for branch, cases in clause["cases"].items()
                        if any(case["status"] in {"PASS", "FAIL"} for case in cases)}
            passed = {branch for branch, cases in clause["cases"].items()
                      if any(case["status"] == "PASS" for case in cases)}
            mapped = set(clause["cases"])
            counts["clauses_with_explicit_mapping"] += bool(mapped)
            counts["clauses_with_all_tracked_branches_linked"] += required <= mapped
            counts["clauses_linked_to_an_executed_case"] += bool(executed)
            counts["clauses_with_complete_branch_execution"] += required <= executed
            counts["clauses_with_all_tracked_branches_passing"] += required <= passed
            counts["tracked_branches"] += len(required)
            counts["branches_with_explicit_mapping"] += len(required & mapped)
            counts["branches_exercised"] += len(required & executed)
            counts["branches_passing"] += len(required & passed)
    return dict(counts)


def registration_entries(path):
    if path is None:
        return None
    entries = {}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        name, site = line.split("\t")
        if name in entries:
            raise ValueError(f"duplicate registration for {name}")
        entries[name] = site
    return entries


def registration_metrics(entries, executed):
    if entries is None:
        return None
    instances = Counter(entries.values())
    return {"definition_sites": len(instances), "registered_instances": len(entries),
            "execution_instances": len(executed),
            "parameterized_sites": sum(n > 1 for n in instances.values()),
            "definitions": [{"site": site, "instances": count} for site, count in sorted(instances.items())]}


def build_report(executed, legacy_played, registrations=None, partial=False, historical=None):
    mapped = manual_mappings(executed)
    legacy = legacy_mentions()
    rows = []
    represented = set()
    for row, generated in cases(inventory()):
        key = row["id"]
        represented.add(key)
        if "crest_enum" in row:
            represented.add(f"Crest:{row['crest_enum']}")
        generated = [{"case": f"catalog.{key}.{name}", "checks": name,
                      **({"official_clause_index": data[0] + 1} if name.startswith(("effect-", "keyword-")) else {})}
                     for name, data in generated]
        manual = mapped.get(key, []) + mapped.get(f"Crest:{row.get('crest_enum')}", [])
        official = row.get("official") or {}
        research = row.get("research") or {}
        text = official.get("text") or research.get("effect_text") or research.get("effect_summary")
        clauses = []
        all_clauses = sections(text) + sections(official.get("evolved_text"))
        for entry in generated:
            if entry["checks"] == "countdown-advance":
                indices = {all_clauses.index(clause) + 1 for clause in all_clauses
                           if re.fullmatch(r"【カウントダウン_\d+】", clause)}
                if len(indices) == 1:
                    entry["official_clause_index"] = next(iter(indices))
        mapped_clauses = list(clause_links(key, all_clauses, generated, manual, executed))
        if historical is not None:
            add_phase2_evidence(key, mapped_clauses, generated, historical)
        for index, clause in enumerate(all_clauses, 1):
            canonical_index = all_clauses.index(clause) + 1
            exact = [entry["case"] for entry in generated if entry.get("official_clause_index") == canonical_index]
            clauses.append({"index": index, "text": clause, "exact_cases": exact,
                            "status": "explicit automated mapping" if exact else "requires manual review of linked branches"})
        tests = [{"case": entry["case"], "status": status(entry["case"], executed),
                  "checks": entry["checks"]} for entry in generated]
        details = {field: value for field, value in row.items() if field not in {"official", "research"}}
        rows.append({
            **details, "class": official.get("class"), "kind": official.get("spec_kind"),
            "printed_source_available": bool(official),
            "source_strength": (research.get("effect_text_status") if research else
                                "official snapshot" if official else "no captured card detail"),
            "official_clauses": clauses, "generated_cases": tests, "manual_branches": manual,
            "clause_coverage": mapped_clauses,
            "legacy_review_candidates": legacy.get(key, []),
            "legacy_play_smoke": legacy_played.get(key),
        })
    manifested = {entry["case_name"] for values in mapped.values() for entry in values}
    expected = {entry["case"] for row in rows for entry in row["generated_cases"]} | manifested | {"catalog.inventory"}
    unresolved = [{"id": str(row["card_id"]), "source": row["source_url"]}
                  for row in read(DATA / "svwb_missing_tokens.json")
                  if str(row["card_id"]) not in represented]
    def has_behavior(row):
        return any(case["checks"].startswith(("effect-", "keyword-")) for case in row["generated_cases"]) or any(
            not case["case_name"].startswith("special.TokenCatalog.") for case in row["manual_branches"])
    registered = set(registrations) if registrations is not None else set(executed)
    return {
        "run_scope": "targeted" if partial else "full",
        "entity_totals": dict(Counter(row["entity_type"] for row in rows)),
        "registered_entities": sum(any(case["checks"] == "registry" and case["status"] == "PASS"
                                       for case in row["generated_cases"]) for row in rows),
        "ordinary_cards_with_legacy_play_smoke": sum(row["legacy_play_smoke"] is not None
                                                      for row in rows if row["entity_type"] == "card"),
        "entities_with_behavior_mapping": dict(Counter(row["entity_type"] for row in rows if has_behavior(row))),
        "abilities_and_branches": {
            "rotation_cards": clause_metrics(row for row in rows if row["entity_type"] == "card"),
            "all_entities": clause_metrics(rows),
        },
        "registrations": registration_metrics(registrations, executed),
        "test_totals_by_group": {group: dict(Counter(
            item["status"] for name, item in executed.items() if name.split(".")[0] == group))
            for group in sorted({name.split(".")[0] for name in executed})},
        "entities": rows,
        "cross_entity_branches": {key: values for key, values in mapped.items() if key not in represented},
        "unimplemented_supplement_references": unresolved,
        "expected_case_missing_from_run": sorted(expected - executed.keys()),
        "executed_case_missing_from_ledger": sorted(executed.keys() - expected),
        "expected_case_not_registered": sorted(expected - registered),
        "registered_case_missing_from_ledger": sorted(registered - expected),
        "executed_case_not_registered": sorted(executed.keys() - registered),
        "failures": {key: value["error"] for key, value in executed.items() if value["status"] == "FAIL"},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-log", type=Path, required=True)
    parser.add_argument("--legacy-play-log", type=Path)
    parser.add_argument("--registration-log", type=Path)
    parser.add_argument("--phase2-spec-log", type=Path, help="archived Phase 2 spec results for reviewed mappings")
    parser.add_argument("--phase2-legacy-junit", type=Path, help="archived Phase 2 CTest output for reviewed mappings")
    parser.add_argument("--allow-partial", action="store_true",
                        help="record a targeted execution; all cases still need to be registered and mapped")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.allow_partial and args.registration_log is None:
        parser.error("--allow-partial requires --registration-log")
    executed = case_results(args.run_log)
    reviewed, historical_cases = phase2_evidence(args.phase2_spec_log, args.phase2_legacy_junit)
    report = build_report(executed, legacy_play_results(args.legacy_play_log),
                          registration_entries(args.registration_log), args.allow_partial,
                          (reviewed, historical_cases, args.phase2_spec_log))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    summary = {key: report[key] for key in ("run_scope", "entity_totals", "registered_entities",
                                            "ordinary_cards_with_legacy_play_smoke", "entities_with_behavior_mapping",
                                            "test_totals_by_group", "abilities_and_branches")}
    audit_keys = ("executed_case_missing_from_ledger", "expected_case_not_registered",
                  "registered_case_missing_from_ledger", "executed_case_not_registered")
    summary.update({key: report[key] for key in audit_keys})
    summary["expected_case_missing_from_run"] = len(report["expected_case_missing_from_run"])
    print(json.dumps(summary, indent=2))
    return any(report[key] for key in audit_keys) or (not args.allow_partial and bool(report["expected_case_missing_from_run"]))


if __name__ == "__main__":
    raise SystemExit(main())
