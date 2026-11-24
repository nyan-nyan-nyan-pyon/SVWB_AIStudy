"""Verify PyTorch vs C++20 numerical parity, binary format, and CLI integration for the Attention model."""

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
    ATTN_CARD_TOKENS,
    ATTN_GLOBAL_DIM,
    ATTN_PARAM_COUNT,
    ATTN_WEIGHT_BYTES,
    MAX_CANDIDATES,
    initialize_attention_checkpoint,
    load_attention_weights_bin,
    read_attention_replay,
    train_generation,
)

PARITY_RECORD_DTYPE = np.dtype([
    ("global_feat", "<f4", (ATTN_GLOBAL_DIM,)),
    ("vocab", "<i4", (ATTN_CARD_TOKENS,)),
    ("attrs", "<f4", (ATTN_CARD_TOKENS, ATTN_ATTR_DIM)),
    ("value_logit", "<f4"),
    ("value_prob", "<f4"),
    ("candidate_count", "<i4"),
    ("candidates", "<f4", (MAX_CANDIDATES, ATTN_CANDIDATE_DIM)),
    ("policy_logits", "<f4", (MAX_CANDIDATES,)),
])


@torch.inference_mode()
def check_parity_dump(test_wb: Path, weights_path: Path, dump_path: Path, label: str):
    subprocess.run(
        [str(test_wb), "--attention-parity-dump", str(weights_path), str(dump_path)],
        check=True, capture_output=True, text=True, timeout=30,
    )
    raw = dump_path.read_bytes()
    count, = struct.unpack_from("<i", raw, 0)
    assert count >= 16, f"Expected at least 16 positions, got {count}"
    records = np.frombuffer(raw, dtype=PARITY_RECORD_DTYPE, count=count, offset=4)

    model = load_attention_weights_bin(weights_path, device="cpu").eval()
    total_params = sum(p.numel() for p in model.parameters())
    assert total_params == ATTN_PARAM_COUNT, f"Expected {ATTN_PARAM_COUNT} params, got {total_params}"

    g = torch.from_numpy(records["global_feat"].copy())
    v = torch.from_numpy(records["vocab"].copy())
    a = torch.from_numpy(records["attrs"].copy())
    c = torch.from_numpy(records["candidates"].copy())
    py_val_logits, py_pol_logits = model(g, v, a, c)
    py_val_probs = torch.sigmoid(py_val_logits)

    cpp_val_logits = records["value_logit"]
    cpp_val_probs = records["value_prob"]
    max_val_logit_err = float(np.max(np.abs(py_val_logits.numpy() - cpp_val_logits)))
    max_val_prob_err = float(np.max(np.abs(py_val_probs.numpy() - cpp_val_probs)))

    max_pol_logit_err = 0.0
    total_candidates = 0
    for i in range(count):
        k = int(records["candidate_count"][i])
        assert 1 <= k <= MAX_CANDIDATES
        total_candidates += k
        py_row = py_pol_logits[i, :k].numpy()
        cpp_row = records["policy_logits"][i, :k]
        err = float(np.max(np.abs(py_row - cpp_row)))
        if err > max_pol_logit_err:
            max_pol_logit_err = err

    print(
        f"PARITY [{label}] positions={count} candidates={total_candidates} "
        f"max_val_prob_err={max_val_prob_err:.2e} max_val_logit_err={max_val_logit_err:.2e} "
        f"max_pol_logit_err={max_pol_logit_err:.2e}"
    )
    assert max_val_prob_err < 1e-4, f"{label} value prob error {max_val_prob_err} >= 1e-4"
    assert max_val_logit_err < 1e-4, f"{label} value logit error {max_val_logit_err} >= 1e-4"
    assert max_pol_logit_err < 1e-4, f"{label} policy logit error {max_pol_logit_err} >= 1e-4"
    return max(max_val_prob_err, max_val_logit_err, max_pol_logit_err)


def check_corrupted_weights(match_cli: Path, valid_weights: Path, scratch: Path):
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
    with tempfile.TemporaryDirectory(prefix="svwb-attn-test-", dir=scratch_parent) as tmp:
        root = Path(tmp)
        gen0 = root / "generation_000"
        initialize_attention_checkpoint(gen0, seed=2026)
        random_weights = gen0 / "attn_weights.bin"
        assert random_weights.stat().st_size == ATTN_WEIGHT_BYTES

        # 1. Parity test with random weights
        err_random = check_parity_dump(args.test_wb.resolve(), random_weights, root / "parity_random.bin", "random")

        # 2. Generate self-play replay logs with C++ Attention model and train 1 generation in PyTorch
        gen1 = root / "generation_001"
        res = subprocess.run(
            [str(args.match_cli.resolve()), "--rules", "wb", "--model-type", "attention",
             "--games", "4", "--threads", "2", "--simulations", "8", "--seed", "9001",
             "--weights", str(random_weights), "--log-dir", str(gen1)],
            check=True, capture_output=True, text=True, timeout=30,
        )
        assert "feature_schema=wb-attention-v1" in res.stdout and "model_type=attention" in res.stdout
        replay = read_attention_replay(gen1)
        assert len(replay.targets) >= 20

        metrics = train_generation(
            [(1, gen1)], gen0 / "model.pt", gen1, gen1 / "evaluation.npz", 1,
            device=torch.device("cpu"), epochs=3, batch_size=16, seed=9002, model_type="attention",
        )
        assert metrics["model_type"] == "attention"
        trained_weights = gen1 / "attn_weights.bin"
        assert trained_weights.stat().st_size == ATTN_WEIGHT_BYTES

        # 3. Parity test with trained weights
        err_trained = check_parity_dump(args.test_wb.resolve(), trained_weights, root / "parity_trained.bin", "trained")

        # 4. Header/CRC check and play_cli integration check
        check_corrupted_weights(args.match_cli.resolve(), trained_weights, root)
        check_play_cli_attention(args.play_cli.resolve(), trained_weights)

        print(f"PASS Attention PyTorch vs C++20 parity (max_err={max(err_random, err_trained):.2e} < 1e-4), logs, and CLI")


if __name__ == "__main__":
    main()
