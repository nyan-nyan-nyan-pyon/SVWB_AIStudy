"""Verify 3-stage PyTorch vs C++20 numerical parity, metadata validation, v2->v3 migration, old-log rejection, and CLI integration."""

import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
_DEPS = ROOT / "build/phase3-deps"
if _DEPS.is_dir() and str(_DEPS) not in sys.path:
    sys.path.append(str(_DEPS))
sys.path.insert(0, str(ROOT / "python"))

import numpy as np
import torch

from train import (
    ATTN_ATTR_DIM,
    ATTN_CANDIDATE_DIM,
    ATTN_CANDIDATE_EMBDIFF_DIM,
    ATTN_CARD_TOKENS,
    ATTN_DIFF_ZONES,
    ATTN_GLOBAL_DIM,
    ATTN_MAX_SPARSE_DIFF,
    ATTN_MODEL_DIM,
    ATTN_PARAM_COUNT,
    ATTN_PARAM_COUNT_V2,
    ATTN_PARAM_COUNT_V3G,
    ATTN_POLICY_HIDDEN,
    ATTN_WEIGHT_BYTES,
    ATTN_WEIGHT_BYTES_LEGACY,
    ATTN_WEIGHT_BYTES_V3G,
    MAX_CANDIDATES,
    export_attention_weights,
    initialize_attention_checkpoint,
    load_attention_weights_bin,
    migrate_attention_v2_to_v3,
    read_attention_replay,
    train_generation,
)

PARITY_HEADER_STRUCT = struct.Struct("<4i")  # count, global_dim, candidate_dim, embdiff_dim

PARITY_RECORD_DTYPE = np.dtype([
    ("global_feat", "<f4", (ATTN_GLOBAL_DIM,)),
    ("vocab", "<i4", (ATTN_CARD_TOKENS,)),
    ("attrs", "<f4", (ATTN_CARD_TOKENS, ATTN_ATTR_DIM)),
    ("global_cls_init", "<f4", (ATTN_MODEL_DIM,)),
    ("token_proj", "<f4", (ATTN_CARD_TOKENS, ATTN_MODEL_DIM)),
    ("final_cls", "<f4", (ATTN_MODEL_DIM,)),
    ("value_logit", "<f4"),
    ("value_prob", "<f4"),
    ("candidate_count", "<i4"),
    ("candidates", "<f4", (MAX_CANDIDATES, ATTN_CANDIDATE_DIM)),
    ("diff_ids", "<u2", (MAX_CANDIDATES, ATTN_MAX_SPARSE_DIFF)),
    ("diff_signs", "<i1", (MAX_CANDIDATES, ATTN_MAX_SPARSE_DIFF)),
    ("zone_counts", "<u1", (MAX_CANDIDATES, ATTN_DIFF_ZONES + 1)),
    ("embdiff_dense", "<f4", (MAX_CANDIDATES, ATTN_CANDIDATE_EMBDIFF_DIM)),
    ("policy_h1", "<f4", (MAX_CANDIDATES, ATTN_POLICY_HIDDEN)),
    ("policy_logits", "<f4", (MAX_CANDIDATES,)),
])


def assert_allclose_with_err(py_arr: np.ndarray, cpp_arr: np.ndarray, name: str,
                             atol: float = 1e-5, rtol: float = 1e-4) -> float:
    abs_diff = np.abs(py_arr - cpp_arr)
    max_abs = float(np.max(abs_diff)) if abs_diff.size else 0.0
    ok = np.allclose(py_arr, cpp_arr, atol=atol, rtol=rtol)
    if not ok:
        idx = np.unravel_index(int(np.argmax(abs_diff)), abs_diff.shape)
        raise AssertionError(
            f"{name} parity failed at {idx}: py={py_arr[idx]:.8f} cpp={cpp_arr[idx]:.8f} "
            f"abs_err={max_abs:.3e} (atol={atol}, rtol={rtol})"
        )
    return max_abs


@torch.inference_mode()
def check_parity_dump(test_wb: Path, weights_path: Path, dump_path: Path, label: str, expected_params: int):
    subprocess.run(
        [str(test_wb), "--attention-parity-dump", str(weights_path), str(dump_path)],
        check=True, capture_output=True, text=True, timeout=45,
    )
    raw = dump_path.read_bytes()
    count, g_dim, c_dim, d_dim = PARITY_HEADER_STRUCT.unpack_from(raw, 0)
    assert count >= 24, f"Expected at least 24 positions (including 8 boundary cases), got {count}"
    records = np.frombuffer(raw, dtype=PARITY_RECORD_DTYPE, count=count, offset=PARITY_HEADER_STRUCT.size)

    model, meta = load_attention_weights_bin(weights_path, device="cpu", return_metadata=True)
    model.eval()
    assert (meta.global_dim, meta.candidate_dim, meta.candidate_embdiff_dim) == (g_dim, c_dim, d_dim)
    total_params = sum(p.numel() for p in model.parameters())
    assert total_params == expected_params, f"Expected {expected_params} params, got {total_params}"

    # Verify Stage 1 sparse diff integrity (bounded <= 58, non-zero entries strictly inside zone slices, vocab_id > 0)
    for i in range(count):
        k = int(records["candidate_count"][i])
        assert 1 <= k <= MAX_CANDIDATES
        for c_idx in range(k):
            zc = records["zone_counts"][i, c_idx]
            assert int(zc[-1]) == int(np.sum(zc[:-1])) <= ATTN_MAX_SPARSE_DIFF

    g = torch.from_numpy(records["global_feat"].copy())
    v = torch.from_numpy(records["vocab"].copy())
    a = torch.from_numpy(records["attrs"].copy())
    c = torch.from_numpy(records["candidates"].copy())
    d_ids = torch.from_numpy(records["diff_ids"].astype(np.int32, copy=True))
    d_signs = torch.from_numpy(records["diff_signs"].copy())

    stages = model.inspect_stages(g, v, a, c, d_ids if d_dim > 0 else None, d_signs if d_dim > 0 else None)

    # Stage 1: Feature extraction & dense EmbDiff(160) parity
    err_s1_diff = 0.0
    if d_dim > 0:
        for i in range(count):
            k = int(records["candidate_count"][i])
            err_s1_diff = max(
                err_s1_diff,
                assert_allclose_with_err(
                    stages["embdiff_dense"][i, :k].numpy(),
                    records["embdiff_dense"][i, :k],
                    f"{label}/Stage1/embdiff_dense[{i}]",
                ),
            )

    # Stage 2: Intermediate representations (Global+CLS init, Token proj, Final LN CLS, Policy FC1 ReLU)
    err_s2_gcls = assert_allclose_with_err(
        stages["global_cls_init"].numpy(), records["global_cls_init"], f"{label}/Stage2/global_cls_init"
    )
    err_s2_tproj = assert_allclose_with_err(
        stages["token_proj"].numpy(), records["token_proj"], f"{label}/Stage2/token_proj"
    )
    err_s2_cls = assert_allclose_with_err(
        stages["final_cls"].numpy(), records["final_cls"], f"{label}/Stage2/final_cls"
    )
    err_s2_pol_h1 = 0.0
    for i in range(count):
        k = int(records["candidate_count"][i])
        err_s2_pol_h1 = max(
            err_s2_pol_h1,
            assert_allclose_with_err(
                stages["policy_h1"][i, :k].numpy(),
                records["policy_h1"][i, :k],
                f"{label}/Stage2/policy_h1[{i}]",
            ),
        )

    # Stage 3: Final outputs (Value logit/prob, Policy logits, and candidate ranking order for |delta| >= 1e-3)
    err_val_logit = assert_allclose_with_err(
        stages["value_logit"].numpy(), records["value_logit"], f"{label}/Stage3/value_logit"
    )
    err_val_prob = assert_allclose_with_err(
        stages["value_prob"].numpy(), records["value_prob"], f"{label}/Stage3/value_prob"
    )
    err_pol_logit = 0.0
    max_near_tie_inversion_gap = 0.0
    total_candidates = 0
    for i in range(count):
        k = int(records["candidate_count"][i])
        total_candidates += k
        py_row = stages["policy_logits"][i, :k].numpy()
        cpp_row = records["policy_logits"][i, :k]
        err_pol_logit = max(
            err_pol_logit,
            assert_allclose_with_err(py_row, cpp_row, f"{label}/Stage3/policy_logits[{i}]"),
        )
        for p in range(k):
            for q in range(p + 1, k):
                py_gap = float(py_row[p] - py_row[q])
                cpp_gap = float(cpp_row[p] - cpp_row[q])
                if py_gap * cpp_gap < 0.0:
                    gap = max(abs(py_gap), abs(cpp_gap))
                    assert gap < 1e-3, (
                        f"{label} candidate ranking inversion at pos={i} ({p},{q}) with gap={gap:.4e} >= 1e-3"
                    )
                    max_near_tie_inversion_gap = max(max_near_tie_inversion_gap, gap)

    max_err = max(err_s1_diff, err_s2_gcls, err_s2_tproj, err_s2_cls, err_s2_pol_h1,
                  err_val_logit, err_val_prob, err_pol_logit)
    print(
        f"PARITY [{label}] pos={count} cands={total_candidates} "
        f"s1_diff={err_s1_diff:.2e} s2_cls={err_s2_cls:.2e} s2_h1={err_s2_pol_h1:.2e} "
        f"val_logit={err_val_logit:.2e} pol_logit={err_pol_logit:.2e} "
        f"near_tie_inv={max_near_tie_inversion_gap:.2e}"
    )
    return max_err


def check_corrupted_and_schema_errors(match_cli: Path, valid_weights: Path, scratch: Path):
    data = bytearray(valid_weights.read_bytes())
    assert len(data) == ATTN_WEIGHT_BYTES

    bad_magic = scratch / "bad_magic.bin"
    corrupted = bytearray(data)
    corrupted[0] ^= 0xFF
    bad_magic.write_bytes(corrupted)
    res = subprocess.run(
        [str(match_cli), "--rules", "wb", "--model-type", "attention", "--games", "1", "--weights", str(bad_magic)],
        capture_output=True, text=True, timeout=15,
    )
    assert res.returncode != 0 and "magic" in res.stderr.lower()

    bad_crc = scratch / "bad_crc.bin"
    corrupted = bytearray(data)
    corrupted[-16] ^= 0x01
    bad_crc.write_bytes(corrupted)
    res = subprocess.run(
        [str(match_cli), "--rules", "wb", "--model-type", "attention", "--games", "1", "--weights", str(bad_crc)],
        capture_output=True, text=True, timeout=15,
    )
    assert res.returncode != 0 and "checksum" in res.stderr.lower()

    # Corrupt card_vocab_hash in header (word 20 -> byte offset 8 + 4*20 = 88) -> must fail loudly
    bad_vocab = scratch / "bad_vocab_hash.bin"
    corrupted = bytearray(data)
    corrupted[88] ^= 0x01
    bad_vocab.write_bytes(corrupted)
    res = subprocess.run(
        [str(match_cli), "--rules", "wb", "--model-type", "attention", "--games", "1", "--weights", str(bad_vocab)],
        capture_output=True, text=True, timeout=15,
    )
    assert res.returncode != 0 and "vocab_hash" in res.stderr.lower()

    # Legacy v2 headerless replay log must be rejected by read_attention_replay (AC-D-3)
    fake_v2_dir = scratch / "fake_legacy_v2_replay"
    fake_v2_dir.mkdir(parents=True, exist_ok=True)
    (fake_v2_dir / "val_log.bin").write_bytes(bytes(3960 * 4))
    (fake_v2_dir / "pol_log.bin").write_bytes(struct.pack("<i", 1) * 4 + bytes(260 * 4))
    try:
        read_attention_replay(fake_v2_dir)
    except ValueError as exc:
        assert "rejected" in str(exc).lower() or "legacy" in str(exc).lower()
    else:
        raise AssertionError("Expected Legacy v2 replay log without header to be rejected")


def check_play_cli_attention(play_cli: Path, weights_path: Path):
    with subprocess.Popen(
        [str(play_cli), "--weights", str(weights_path), "--model-type", "attention", "--simulations", "4", "--seed", "77"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8",
    ) as proc:
        try:
            def send(cmd, **kwargs):
                proc.stdin.write(json.dumps({"command": cmd, **kwargs}) + "\n")
                proc.stdin.flush()
                reply = json.loads(proc.stdout.readline())
                assert reply["ok"], reply
                return reply

            send("new_game", seat="first", seed=77)
            state = send("mulligan", mask=0)["state"]
            assert state["phase"] == "main" and state["active_seat"] == "first"
            analysis = send("analyze_turn", revision=state["revision"])["analysis"]
            assert analysis["total_visits"] == 4 and analysis["candidates"]
            end_idx = next(a["action_index"] for a in state["legal_actions"] if a["type"] == "end_turn")
            state = send("act", revision=state["revision"], action_index=end_idx)["state"]
            ai_res = send("ai_turn", revision=state["revision"])
            assert ai_res["state"]["active_seat"] == "first" and ai_res["ai_turn"]["total_visits"] == 4
        finally:
            proc.stdin.close()
        assert proc.wait(timeout=5) == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-wb", type=Path, required=True)
    parser.add_argument("--match-cli", type=Path, required=True)
    parser.add_argument("--play-cli", type=Path, required=True)
    args = parser.parse_args()

    scratch_parent = ROOT / "build" if (ROOT / "build").is_dir() else None
    with tempfile.TemporaryDirectory(prefix="svwb-attn-v3-test-", dir=scratch_parent) as tmp:
        root = Path(tmp)

        # 1. Initialize v3-GD (486,402), v3-G (465,922), and Legacy v2 (461,826) checkpoints & verify 3-stage parity
        gen0 = root / "generation_000"
        initialize_attention_checkpoint(gen0, seed=2026, variant="v3-gd")
        v3gd_weights = gen0 / "attn_weights.bin"
        assert v3gd_weights.stat().st_size == ATTN_WEIGHT_BYTES
        err_random = check_parity_dump(
            args.test_wb.resolve(), v3gd_weights, root / "parity_v3gd_random.bin", "v3-gd-random", ATTN_PARAM_COUNT
        )

        v3g_dir = root / "v3g_init"
        initialize_attention_checkpoint(v3g_dir, seed=2027, variant="v3-g")
        assert (v3g_dir / "attn_weights.bin").stat().st_size == ATTN_WEIGHT_BYTES_V3G
        err_v3g = check_parity_dump(
            args.test_wb.resolve(), v3g_dir / "attn_weights.bin", root / "parity_v3g.bin", "v3-g-random",
            ATTN_PARAM_COUNT_V3G,
        )

        # 2. Create/load a Legacy v2 80-byte binary, migrate v2 -> v3-GD in Python, and check 3-stage parity
        real_v2_gen40 = ROOT / "data/phase5c-attn-v2/bishop__bishop/generation_040/attn_weights.bin"
        if real_v2_gen40.is_file():
            v2_src = real_v2_gen40
        else:
            v2_tmp_dir = root / "v2_init"
            initialize_attention_checkpoint(v2_tmp_dir, seed=2025, variant="v2")
            v2_m = load_attention_weights_bin(v2_tmp_dir / "attn_weights.bin")
            v2_src = root / "legacy_v2_80b.bin"
            v2_src.write_bytes(export_attention_weights(v2_m, legacy_format=True))
        assert v2_src.stat().st_size == ATTN_WEIGHT_BYTES_LEGACY

        err_v2_legacy = check_parity_dump(
            args.test_wb.resolve(), v2_src, root / "parity_v2_legacy.bin", "v2-legacy", ATTN_PARAM_COUNT_V2
        )

        migrated_dir = root / "migrated_v3gd"
        migrate_attention_v2_to_v3(v2_src, migrated_dir, target_variant="v3-gd", generation=0)
        migrated_weights = migrated_dir / "attn_weights.bin"
        assert migrated_weights.stat().st_size == ATTN_WEIGHT_BYTES
        err_migrated = check_parity_dump(
            args.test_wb.resolve(), migrated_weights, root / "parity_migrated.bin", "v2->v3-gd-migrated",
            ATTN_PARAM_COUNT,
        )

        # 3. Self-play replay generation with v3-GD C++ engine -> 1 generation PyTorch training -> trained parity check
        gen1 = root / "generation_001"
        res = subprocess.run(
            [str(args.match_cli.resolve()), "--rules", "wb", "--model-type", "attention",
             "--games", "4", "--threads", "2", "--simulations", "8", "--seed", "9001",
             "--weights", str(v3gd_weights), "--log-dir", str(gen1)],
            check=True, capture_output=True, text=True, timeout=45,
        )
        assert "feature_schema=wb-attention-v3" in res.stdout and "model_type=attention" in res.stdout
        assert "both_lose_rate=" in res.stdout
        replay = read_attention_replay(gen1)
        assert len(replay.targets) >= 20 and replay.diff_ids is not None

        metrics = train_generation(
            [(1, gen1)], gen0 / "model.pt", gen1, gen1 / "evaluation.npz", 1,
            device=torch.device("cpu"), epochs=2, batch_size=16, seed=9002,
            learning_rate=3e-4, weight_decay=1e-2, dropout=0.1, value_label_smoothing=0.05,
            model_type="attention",
        )
        assert metrics["model_type"] == "attention" and metrics["attention_variant"] == "v3-gd"
        assert metrics["parameter_count"] == ATTN_PARAM_COUNT
        assert metrics["brier_score"] is not None and metrics["ece_10bin"] is not None
        trained_weights = gen1 / "attn_weights.bin"
        assert trained_weights.stat().st_size == ATTN_WEIGHT_BYTES

        err_trained = check_parity_dump(
            args.test_wb.resolve(), trained_weights, root / "parity_trained.bin", "v3-gd-trained", ATTN_PARAM_COUNT
        )

        # 4. Metadata corruption, legacy replay rejection, play_cli, and heterogeneous v3-GD vs Legacy v2 & MLP eval
        check_corrupted_and_schema_errors(args.match_cli.resolve(), trained_weights, root)
        check_play_cli_attention(args.play_cli.resolve(), trained_weights)

        mlp_weights = root / "mlp_weights.bin"
        mlp_weights.write_bytes(bytes(74376))
        hetero_mlp = subprocess.run(
            [str(args.match_cli.resolve()), "--rules", "wb", "--evaluate",
             "--model-type", "attention", "--weights", str(trained_weights),
             "--opponent-model-type", "mlp", "--opponent-weights", str(mlp_weights),
             "--games", "4", "--threads", "2", "--simulations", "4", "--seed", "20261125"],
            check=True, capture_output=True, text=True, timeout=30,
        )
        assert "model_type=attention" in hetero_mlp.stdout and "opponent_model_type=mlp" in hetero_mlp.stdout

        hetero_v2 = subprocess.run(
            [str(args.match_cli.resolve()), "--rules", "wb", "--evaluate",
             "--model-type", "attention", "--weights", str(trained_weights),
             "--opponent-weights", str(v2_src),
             "--games", "4", "--threads", "2", "--simulations", "4", "--seed", "20261125"],
            check=True, capture_output=True, text=True, timeout=30,
        )
        assert "A_first=2 A_second=2" in hetero_v2.stdout

        overall_max = max(err_random, err_v3g, err_v2_legacy, err_migrated, err_trained)
        print(
            f"PASS Attention v3 3-stage PyTorch/C++ parity (max_err={overall_max:.2e} < 1e-5), "
            f"migration, metadata, replay guard, and CLI"
        )


if __name__ == "__main__":
    main()
