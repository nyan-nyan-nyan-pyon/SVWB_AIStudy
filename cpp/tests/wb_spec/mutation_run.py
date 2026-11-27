"""Run one isolated, single-process native build per targeted mutation."""

import argparse
import json
import os
import re
import shutil
import subprocess
import tempfile
from collections import Counter
from pathlib import Path

from spec_data import HERE, ROOT, read


def copy_source(destination):
    destination.mkdir()
    shutil.copy2(ROOT / "CMakeLists.txt", destination)
    for folder in ("cpp", "python", "data/cards", "data/decks/phase5c"):
        shutil.copytree(ROOT / folder, destination / folder, ignore=shutil.ignore_patterns("__pycache__"))


def run(args, log, environment, cwd, timeout=240):
    with log.open("ab") as stream:
        stream.write(("\n" + subprocess.list2cmdline([str(arg) for arg in args]) + "\n").encode())
        stream.flush()
        try:
            return subprocess.run(args, stdout=stream, stderr=subprocess.STDOUT,
                                  env=environment, cwd=cwd, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            stream.write(b"\nTIMEOUT\n")
            return 124


def modify(path, mutation):
    if "before" in mutation and "section" not in mutation:
        original = path.read_text(encoding="utf-8")
        before = mutation["before"]
        if original.count(before) != 1:
            raise ValueError("text anchor must occur exactly once")
        path.write_text(original.replace(before, mutation["after"], 1), encoding="utf-8")
        return
    contents = read(path)
    cards = [row for row in contents[mutation["section"]]
             if all(row.get(key) == value for key, value in mutation["entity"].items())]
    if len(cards) != 1:
        raise ValueError("entity selector must match exactly once")
    selected = cards[0]
    if "swap_effects" in mutation:
        left, right = mutation["swap_effects"]
        steps = selected["effects"]
        if [steps[left], steps[right]] != mutation["original_effects"]:
            raise ValueError("effect order anchor changed")
        steps[left], steps[right] = steps[right], steps[left]
    elif "effect_index" in mutation:
        index = mutation["effect_index"]
        steps = selected.get("effects", [])
        if not 0 <= index < len(steps) or steps[index] != mutation["original_effect"]:
            raise ValueError("indexed effect anchor changed")
        selected = steps[index]
    elif "effect" in mutation:
        steps = [step for step in selected["effects"]
                 if all(step.get(key) == value for key, value in mutation["effect"].items())]
        if len(steps) != 1:
            raise ValueError("effect selector must match exactly once")
        selected = steps[0]
    if "swap_effects" not in mutation:
        changes = mutation.get("changes")
        if changes is None:
            changes = {mutation["field"]: {"before": mutation.get("before"), "after": mutation["after"]}}
        for field, change in changes.items():
            if selected.get(field) != change["before"]:
                raise ValueError(f"mutation's expected original {field} has changed")
            selected[field] = change["after"]
    path.write_text(json.dumps(contents, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def select_mutations(args):
    mutations = [row for row in read(args.manifest) if not args.only or row["name"] in args.only]
    if args.only and set(args.only) != {row["name"] for row in mutations}:
        raise ValueError("unknown --only mutation name")
    if not mutations:
        raise ValueError("no mutations selected")
    if len({mutation["name"] for mutation in mutations}) != len(mutations):
        raise ValueError("duplicate mutation names")
    if any(not re.fullmatch(r"[a-z0-9_-]+", mutation["name"]) for mutation in mutations):
        raise ValueError("invalid mutation name")
    for mutation in mutations:
        relative = Path(mutation["file"])
        if relative.is_absolute() or not (ROOT / relative).resolve().is_relative_to(ROOT):
            raise ValueError("mutation path must refer to a source file")
    return mutations


def validate_mutations(mutations, output):
    with tempfile.TemporaryDirectory(prefix="validation-", dir=output) as folder:
        temporary = Path(folder)
        for mutation in mutations:
            path = temporary / Path(mutation["file"]).name
            original = (ROOT / mutation["file"]).read_bytes()
            path.write_bytes(original)
            modify(path, mutation)
            if path.suffix == ".json":
                before = json.loads(original)
                if before == read(path):
                    raise ValueError(f'no semantic change for {mutation["name"]}')
            elif path.read_bytes() == original:
                raise ValueError(f'no file change for {mutation["name"]}')
    print(f"validated {len(mutations)} mutation anchors and edit operations; no native builds")


def all_mutations(args, output, mutations):
    env = os.environ.copy()
    env.update(CMAKE_BUILD_PARALLEL_LEVEL="1", OMP_NUM_THREADS="1", MKL_NUM_THREADS="1",
               CUDA_VISIBLE_DEVICES="-1", PYTHONDONTWRITEBYTECODE="1")
    with tempfile.TemporaryDirectory(prefix="implementation-", dir=output) as staged:
        work = Path(staged)
        stage = work / "source"
        build = work / "build"
        copy_source(stage)
        config = [args.cmake, "-S", str(stage), "-B", str(build)]
        if args.generator:
            config.extend(("-G", args.generator))
        config.append(f"-DCMAKE_BUILD_TYPE={args.config}")
        compile_command = [args.cmake, "--build", str(build), "--config", args.config, "--target", "test_wb_spec"]
        if run(config, output / "configure.log", env, work):
            raise RuntimeError("mutation configure failed; see configure.log")
        if run(compile_command, output / "baseline.log", env, work):
            raise RuntimeError("mutation baseline build failed; see baseline.log")
        extension = ".exe" if os.name == "nt" else ""
        exe = build / args.config / f"test_wb_spec{extension}"
        if not exe.is_file():
            exe = build / f"test_wb_spec{extension}"
        baseline_results = {}
        for mutation in mutations:
            name = mutation["name"]
            case = mutation["case"]
            log = output / f"{name}.log"
            baseline_results[name] = {
                probe: run([str(exe), probe, "--exact"], log, env, work, 30)
                for probe in [case, *mutation.get("additional_cases", [])]
            }
        for mutation in mutations:
            name = mutation["name"]
            case = mutation["case"]
            log = output / f"{name}.log"
            item = {"name": name, "case": case, "file": mutation["file"]}
            if baseline_results[name][case] != 0:
                item["status"] = "baseline did not pass"
                yield item
                continue
            path = (stage / mutation["file"]).resolve()
            if not path.is_relative_to(stage) or not path.is_file():
                raise ValueError("mutation path must identify a file inside the staged source")
            original = path.read_bytes()
            try:
                modify(path, mutation)
                build_code = run(compile_command, log, env, work)
                if build_code:
                    item["status"] = "failed to build" if build_code != 124 else "build timeout"
                else:
                    item["probes"] = []
                    item["status"] = "survived"
                    for probe, baseline in baseline_results[name].items():
                        if baseline != 0:
                            item["probes"].append({"case": probe, "status": "baseline did not pass"})
                            continue
                        test_code = run([str(exe), probe, "--exact"], log, env, work, 30)
                        diagnostic = log.read_text(encoding="utf-8", errors="replace")
                        if test_code == 1 and re.search(rf"^FAIL {re.escape(probe)} \| ", diagnostic, re.M):
                            result = "killed"
                        elif test_code == 0:
                            result = "survived"
                        else:
                            result = "test infrastructure failure"
                        item["probes"].append({"case": probe, "status": result})
                        item["status"] = result
                        if result != "survived":
                            break
            except ValueError as error:
                item["status"] = "invalid mutation"
                item["detail"] = str(error)
            finally:
                path.write_bytes(original)
            # The next incremental build sees both the restored and newly mutated inputs.
            yield item


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--generator")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--manifest", type=Path, default=HERE / "mutations.json")
    parser.add_argument("--only", action="append", help="exact mutation name, repeatable")
    parser.add_argument("--limit", type=int, default=10, help="maximum selected native mutations")
    parser.add_argument("--validate-only", action="store_true", help="validate edits in isolation without compiling")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    mutations = select_mutations(args)
    if not args.validate_only and len(mutations) > args.limit:
        parser.error(f"selected {len(mutations)} mutations; narrow --only or set explicit --limit")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    if args.validate_only:
        validate_mutations(mutations, output)
        return 0
    results = []
    for item in all_mutations(args, output, mutations):
        results.append(item)
        print(f'{item["status"]}: {item["name"]}', flush=True)
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    counts = Counter(item["status"] for item in results)
    count = counts["killed"] + counts["survived"]
    print(f"killed {counts['killed']}/{count} buildable mutations; all outcomes: {dict(counts)}")
    return 0 if count > 0 and len(results) == count and counts["killed"] / count >= .9 else 1


if __name__ == "__main__":
    raise SystemExit(main())
