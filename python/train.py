"""Train the value and policy heads from the C++ replay files."""

import argparse
from dataclasses import dataclass
import json
import math
import mmap
import os
from pathlib import Path
import struct

import numpy as np
import torch
from torch import nn
from torch.nn import functional as F


FEATURES = 256
SAMPLE_FLOATS = FEATURES + 1
SAMPLE_BYTES = 4 * SAMPLE_FLOATS
MAX_CANDIDATES = 18
WEIGHT_BYTES = 74376
PARAMETERS = (
    "fc1.weight", "fc1.bias", "fc2.weight", "fc2.bias",
    "head_val.weight", "head_val.bias", "head_pol.weight", "head_pol.bias",
)
COUNT = struct.Struct("<i")


class Model(nn.Module):
    def __init__(self):
        super().__init__()
        self.fc1 = nn.Linear(FEATURES, 64)
        self.fc2 = nn.Linear(64, 32)
        self.head_val = nn.Linear(32, 1)
        self.head_pol = nn.Linear(32, 1)

    def forward(self, features):
        hidden = F.relu(self.fc2(F.relu(self.fc1(features))))
        return self.head_val(hidden).squeeze(-1), self.head_pol(hidden).squeeze(-1)


@dataclass
class Replay:
    values: np.ndarray
    policies: np.ndarray
    offsets: np.ndarray


def read_replay(directory: Path) -> Replay:
    value_file = directory / "val_log.bin"
    policy_file = directory / "pol_log.bin"
    turns, remainder = divmod(value_file.stat().st_size, SAMPLE_BYTES)
    if turns == 0 or remainder:
        raise ValueError(f"Invalid value log size in {value_file}")
    values = np.fromfile(value_file, dtype="<f4").reshape(turns, SAMPLE_FLOATS).astype(np.float32, copy=False)
    if not np.isfinite(values).all() or not np.isin(values[:, -1], [0.0, 0.5, 1.0]).all():
        raise ValueError(f"Invalid value samples in {value_file}")

    size = policy_file.stat().st_size
    candidate_total, remainder = divmod(size - turns * COUNT.size, SAMPLE_BYTES)
    if not turns <= candidate_total <= turns * MAX_CANDIDATES or remainder:
        raise ValueError(f"Invalid policy log size in {policy_file}")
    candidates = np.empty((candidate_total, SAMPLE_FLOATS), dtype=np.float32)
    offsets = np.zeros(turns + 1, dtype=np.int64)
    cursor = 0
    with policy_file.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as raw:
        for turn in range(turns):
            if cursor + COUNT.size > size:
                raise ValueError(f"Missing policy header at turn {turn} in {policy_file}")
            count, = COUNT.unpack_from(raw, cursor)
            cursor += COUNT.size
            end = cursor + SAMPLE_BYTES * count
            start_candidate = offsets[turn]
            stop_candidate = start_candidate + count
            if not 1 <= count <= MAX_CANDIDATES or end > size or stop_candidate > candidate_total:
                raise ValueError(f"Invalid policy group at turn {turn} in {policy_file}")
            candidates[start_candidate:stop_candidate] = np.frombuffer(
                raw, dtype="<f4", count=count * SAMPLE_FLOATS, offset=cursor
            ).reshape(count, SAMPLE_FLOATS)
            offsets[turn + 1] = stop_candidate
            cursor = end
    probabilities = candidates[:, -1]
    if (cursor != size or offsets[-1] != candidate_total or not np.isfinite(candidates).all()
            or np.any((probabilities < 0) | (probabilities > 1))
            or not np.allclose(np.add.reduceat(probabilities, offsets[:-1]), 1, rtol=0, atol=1e-6)):
        raise ValueError(f"Invalid policy samples in {policy_file}")
    return Replay(values, candidates, offsets)


def pick_device(requested: str) -> torch.device:
    available = torch.cuda.is_available()
    if requested == "cuda" and not available:
        raise RuntimeError("CUDA was requested but PyTorch cannot access a CUDA device")
    if requested == "cuda" or (requested == "auto" and available):
        return torch.device("cuda")
    torch.set_num_threads(min(4, os.cpu_count() or 1))
    return torch.device("cpu")


def save_model(model, directory: Path, generation: int, optimizer=None):
    directory.mkdir(parents=True, exist_ok=True)
    state = model.state_dict()
    weights = b"".join(
        state[key].detach().cpu().contiguous().numpy().astype("<f4", copy=False).tobytes()
        for key in PARAMETERS
    )
    if len(weights) != WEIGHT_BYTES:
        raise ValueError("Unexpected C++ weight layout")
    weights_tmp = directory / "weights.bin.tmp"
    checkpoint_tmp = directory / "model.pt.tmp"
    weights_tmp.write_bytes(weights)
    torch.save({
        "generation": generation,
        "model": state,
        "optimizer": optimizer.state_dict() if optimizer is not None else None,
    }, checkpoint_tmp)
    checkpoint_tmp.replace(directory / "model.pt")
    weights_tmp.replace(directory / "weights.bin")


def initialize_checkpoint(directory: Path, seed: int):
    torch.manual_seed(seed)
    model = Model()
    for layer in (model.fc1, model.fc2, model.head_val, model.head_pol):
        nn.init.xavier_uniform_(layer.weight)
        nn.init.zeros_(layer.bias)
    save_model(model, directory, 0)


def load_checkpoint(path: Path, device: torch.device):
    checkpoint = torch.load(path, map_location=device, weights_only=True)
    model = Model().to(device)
    model.load_state_dict(checkpoint["model"])
    return model, checkpoint


def make_reference(path: Path, replay: Replay, generation: int, seed: int):
    turns = len(replay.values)
    if turns < 2:
        raise ValueError("A fixed holdout requires at least two replay turns")
    chosen = np.random.default_rng(seed).choice(turns, size=min(1024, max(1, turns // 10)), replace=False)
    starts, ends = replay.offsets[chosen, None], replay.offsets[chosen + 1, None]
    indices = starts + np.arange(MAX_CANDIDATES)
    valid = indices < ends
    policy = replay.policies[np.where(valid, indices, 0), :FEATURES]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as stream:
        np.savez_compressed(stream, generation=generation, heldout=chosen,
                            values=replay.values[chosen, :FEATURES], policies=policy, valid=valid)


def masked_log_softmax(logits, valid):
    return F.log_softmax(logits.masked_fill(~valid, -torch.inf), dim=-1).masked_fill(~valid, 0.0)


@torch.inference_mode()
def measure_drift(model, previous, reference, device):
    value_total = torch.zeros((), device=device)
    policy_total = torch.zeros((), device=device)
    size = len(reference["values"])
    for start in range(0, size, 256):
        batch = slice(start, start + 256)
        values = torch.as_tensor(reference["values"][batch], device=device)
        policies = torch.as_tensor(reference["policies"][batch], device=device)
        valid = torch.as_tensor(reference["valid"][batch], device=device)
        value_delta = (model(values)[0].sigmoid() - previous(values)[0].sigmoid()).abs()
        new_logit = model(policies)[1].masked_fill(~valid, -torch.inf)
        old_logit = previous(policies)[1].masked_fill(~valid, -torch.inf)
        value_total += value_delta.sum()
        policy_total += 0.5 * (new_logit.softmax(-1) - old_logit.softmax(-1)).abs().sum()
    return float(value_total / size), float(policy_total / size)


def train_generation(replay_dirs, previous_path: Path, output: Path, reference_path: Path,
                     generation: int, *, device: torch.device, epochs: int, batch_size: int, seed: int,
                     learning_rate=None):
    if not replay_dirs or epochs < 1 or batch_size < 1:
        raise ValueError("Replay directories, positive epochs and positive batch size are required")
    if learning_rate is not None and (not math.isfinite(learning_rate) or learning_rate <= 0):
        raise ValueError("Learning rate must be finite and positive")
    torch.manual_seed(seed)
    replays = [read_replay(path) for _, path in replay_dirs]
    if not reference_path.exists():
        make_reference(reference_path, replays[0], replay_dirs[0][0], seed)
    with np.load(reference_path) as source:
        reference = {name: source[name] for name in source.files}

    size_bytes = sum(r.values.nbytes + r.policies.nbytes + r.offsets.nbytes for r in replays)
    storage = torch.device("cpu")
    if device.type == "cuda" and size_bytes < 0.35 * torch.cuda.mem_get_info(device)[0]:
        storage = device
    values = torch.as_tensor(np.concatenate([r.values for r in replays]), device=storage)
    policies = torch.as_tensor(np.concatenate([r.policies for r in replays]), device=storage)
    offsets = [0]
    eligible = []
    row = 0
    for (source_generation, _), replay in zip(replay_dirs, replays):
        offsets.extend(replay.offsets[1:] + offsets[-1])
        present = np.arange(len(replay.values))
        if source_generation == int(reference["generation"]):
            present = np.delete(present, reference["heldout"])
        eligible.append(present + row)
        row += len(replay.values)
    del replays
    offsets = torch.as_tensor(offsets, device=storage, dtype=torch.long)
    turns = torch.as_tensor(np.concatenate(eligible), device=storage)
    columns = torch.arange(MAX_CANDIDATES, device=storage)
    if turns.numel() == 0:
        raise ValueError("The replay window has no training samples")

    previous, checkpoint = load_checkpoint(previous_path, device)
    if checkpoint["generation"] != generation - 1:
        raise ValueError("The checkpoint must be from the preceding generation")
    model = Model().to(device)
    model.load_state_dict(previous.state_dict())
    optimizer = torch.optim.Adam(model.parameters(), lr=1e-3, weight_decay=1e-4)
    if checkpoint["optimizer"] is not None:
        optimizer.load_state_dict(checkpoint["optimizer"])
    if learning_rate is not None:
        for group in optimizer.param_groups:
            group["lr"] = learning_rate
    del checkpoint
    totals = torch.zeros(2, device=device)
    for _ in range(epochs):
        value_order = turns[torch.randperm(turns.numel(), device=storage)]
        policy_order = turns[torch.randperm(turns.numel(), device=storage)]
        for value_ids, policy_ids in zip(value_order.split(batch_size), policy_order.split(batch_size)):
            value = values[value_ids].to(device)
            indices = offsets[policy_ids, None] + columns
            valid = indices < offsets[policy_ids + 1, None]
            policy = policies[torch.where(valid, indices, 0)].to(device)
            valid = valid.to(device)

            value_logits, _ = model(value[:, :FEATURES])
            _, policy_logits = model(policy[:, :, :FEATURES])
            value_loss = F.binary_cross_entropy_with_logits(value_logits, value[:, -1])
            targets = policy[:, :, -1].masked_fill(~valid, 0.0)
            policy_loss = -(targets * masked_log_softmax(policy_logits, valid)).sum(-1).mean()
            optimizer.zero_grad(set_to_none=True)
            (value_loss + policy_loss).backward()
            optimizer.step()
            totals += torch.stack((value_loss.detach(), policy_loss.detach())) * len(value_ids)

    val_loss, pol_loss = (totals / (epochs * turns.numel())).tolist()
    delta_v, delta_pi = measure_drift(model.eval(), previous.eval(), reference, device)
    if not all(math.isfinite(x) for x in (val_loss, pol_loss, delta_v, delta_pi)):
        raise RuntimeError("Training produced non-finite metrics")
    save_model(model, output, generation, optimizer)
    metrics = {
        "val_loss": val_loss, "pol_loss": pol_loss, "delta_v": delta_v, "delta_pi": delta_pi,
        "training_turns": turns.numel(), "reference_turns": len(reference["values"]),
        "learning_rate": optimizer.param_groups[0]["lr"],
        "replay_generations": [number for number, _ in replay_dirs],
        "device": torch.cuda.get_device_name(device) if device.type == "cuda" else "CPU",
    }
    (output / "training.json").write_text(json.dumps(metrics, indent=2) + "\n", encoding="utf-8")
    return metrics


def generation_dir(root: Path, generation: int) -> Path:
    return root / f"generation_{generation:03d}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("replay_root", type=Path)
    parser.add_argument("--generation", type=int, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--previous", type=Path, required=True)
    parser.add_argument("--eval-file", type=Path)
    parser.add_argument("--replay-window", type=int, default=5)
    parser.add_argument("--epochs", type=int, default=5)
    parser.add_argument("--learning-rate", type=float, help="Override the checkpoint's Adam learning rate")
    parser.add_argument("--batch-size", type=int, default=256)
    parser.add_argument("--seed", type=int, default=2026)
    parser.add_argument("--device", choices=("auto", "cpu", "cuda"), default="auto")
    args = parser.parse_args()
    genesis = max(1, args.generation - args.replay_window + 1)
    paths = [(number, generation_dir(args.replay_root, number))
             for number in range(genesis, args.generation + 1)]
    metrics = train_generation(paths, args.previous, args.output_dir,
                               args.eval_file or args.replay_root / "evaluation.npz",
                               args.generation, device=pick_device(args.device), epochs=args.epochs,
                               batch_size=args.batch_size, seed=args.seed, learning_rate=args.learning_rate)
    print(json.dumps(metrics, indent=2))


if __name__ == "__main__":
    main()
