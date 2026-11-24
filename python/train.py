"""Train the value and policy heads from the C++ replay files."""

import argparse
from dataclasses import dataclass
import json
import math
import mmap
import os
from pathlib import Path
import struct
import sys
import zlib

_DEPS = Path(__file__).resolve().parents[1] / "build/phase3-deps"
if _DEPS.is_dir() and str(_DEPS) not in sys.path:
    sys.path.append(str(_DEPS))

import numpy as np
import torch
from torch import nn
from torch.nn import functional as F

from run_storage import replace_file

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

ATTN_MAGIC = b"SVWBATTN"
ATTN_FORMAT_VERSION = 1
ATTN_VOCAB = 600
ATTN_CARD_EMBED = 32
ATTN_ATTR_DIM = 32
ATTN_TOKEN_INPUT = ATTN_CARD_EMBED + ATTN_ATTR_DIM
ATTN_GLOBAL_DIM = 32
ATTN_MODEL_DIM = 128
ATTN_HEADS = 4
ATTN_HEAD_DIM = ATTN_MODEL_DIM // ATTN_HEADS
ATTN_FFN_DIM = 512
ATTN_LAYERS = 2
ATTN_CANDIDATE_DIM = 64
ATTN_VALUE_HIDDEN = 64
ATTN_POLICY_HIDDEN = 128
ATTN_POLICY_INPUT = ATTN_MODEL_DIM + ATTN_CANDIDATE_DIM
ATTN_CARD_TOKENS = 29
ATTN_MAX_TOKENS = ATTN_CARD_TOKENS + 1
ATTN_ACTIVATION_RELU = 1
ATTN_PARAM_COUNT = 461_826
ATTN_HEADER_WORDS = 18
ATTN_HEADER_BYTES = len(ATTN_MAGIC) + 4 * ATTN_HEADER_WORDS
ATTN_WEIGHT_BYTES = ATTN_HEADER_BYTES + 4 * ATTN_PARAM_COUNT

ATTN_PARAMETERS = (
    "card_embed.weight",
    "token_proj.weight", "token_proj.bias",
    "global_proj.weight", "global_proj.bias",
    "cls_embed",
    "layers.0.norm1.weight", "layers.0.norm1.bias",
    "layers.0.qkv.weight", "layers.0.qkv.bias",
    "layers.0.out_proj.weight", "layers.0.out_proj.bias",
    "layers.0.norm2.weight", "layers.0.norm2.bias",
    "layers.0.ffn1.weight", "layers.0.ffn1.bias",
    "layers.0.ffn2.weight", "layers.0.ffn2.bias",
    "layers.1.norm1.weight", "layers.1.norm1.bias",
    "layers.1.qkv.weight", "layers.1.qkv.bias",
    "layers.1.out_proj.weight", "layers.1.out_proj.bias",
    "layers.1.norm2.weight", "layers.1.norm2.bias",
    "layers.1.ffn1.weight", "layers.1.ffn1.bias",
    "layers.1.ffn2.weight", "layers.1.ffn2.bias",
    "final_norm.weight", "final_norm.bias",
    "value_fc1.weight", "value_fc1.bias",
    "value_fc2.weight", "value_fc2.bias",
    "policy_fc1.weight", "policy_fc1.bias",
    "policy_fc2.weight", "policy_fc2.bias",
)

ATTN_VALUE_DTYPE = np.dtype([
    ("global_feat", "<f4", (ATTN_GLOBAL_DIM,)),
    ("vocab", "<i4", (ATTN_CARD_TOKENS,)),
    ("attrs", "<f4", (ATTN_CARD_TOKENS, ATTN_ATTR_DIM)),
    ("target", "<f4"),
])
ATTN_POLICY_FLOATS = ATTN_CANDIDATE_DIM + 1
ATTN_POLICY_BYTES = 4 * ATTN_POLICY_FLOATS


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


class AttentionEncoderLayer(nn.Module):
    def __init__(self, dropout: float = 0.0):
        super().__init__()
        self.dropout = dropout
        self.norm1 = nn.LayerNorm(ATTN_MODEL_DIM, eps=1e-5)
        self.qkv = nn.Linear(ATTN_MODEL_DIM, 3 * ATTN_MODEL_DIM)
        self.out_proj = nn.Linear(ATTN_MODEL_DIM, ATTN_MODEL_DIM)
        self.norm2 = nn.LayerNorm(ATTN_MODEL_DIM, eps=1e-5)
        self.ffn1 = nn.Linear(ATTN_MODEL_DIM, ATTN_FFN_DIM)
        self.ffn2 = nn.Linear(ATTN_FFN_DIM, ATTN_MODEL_DIM)

    def forward(self, x: torch.Tensor, key_padding_mask: torch.Tensor) -> torch.Tensor:
        batch_size, seq_len, _ = x.shape
        h = self.norm1(x)
        qkv = self.qkv(h).view(batch_size, seq_len, 3, ATTN_HEADS, ATTN_HEAD_DIM)
        q = qkv[:, :, 0].transpose(1, 2)
        k = qkv[:, :, 1].transpose(1, 2)
        v = qkv[:, :, 2].transpose(1, 2)
        scores = torch.matmul(q, k.transpose(-2, -1)) * (1.0 / math.sqrt(ATTN_HEAD_DIM))
        scores = scores.masked_fill(key_padding_mask[:, None, None, :], -torch.inf)
        probs = torch.softmax(scores, dim=-1)
        att = torch.matmul(probs, v).transpose(1, 2).contiguous().view(batch_size, seq_len, ATTN_MODEL_DIM)
        att_out = self.out_proj(att)
        if self.dropout > 0.0 and self.training:
            att_out = F.dropout(att_out, p=self.dropout, training=True)
        x = x + att_out
        ffn_h = F.relu(self.ffn1(self.norm2(x)))
        if self.dropout > 0.0 and self.training:
            ffn_h = F.dropout(ffn_h, p=self.dropout, training=True)
        ffn_out = self.ffn2(ffn_h)
        if self.dropout > 0.0 and self.training:
            ffn_out = F.dropout(ffn_out, p=self.dropout, training=True)
        x = x + ffn_out
        return x


class AttentionModel(nn.Module):
    def __init__(self, dropout: float = 0.0):
        super().__init__()
        self.dropout = dropout
        self.card_embed = nn.Embedding(ATTN_VOCAB, ATTN_CARD_EMBED)
        self.token_proj = nn.Linear(ATTN_TOKEN_INPUT, ATTN_MODEL_DIM)
        self.global_proj = nn.Linear(ATTN_GLOBAL_DIM, ATTN_MODEL_DIM)
        self.cls_embed = nn.Parameter(torch.zeros(ATTN_MODEL_DIM))
        self.layers = nn.ModuleList([AttentionEncoderLayer(dropout=dropout) for _ in range(ATTN_LAYERS)])
        self.final_norm = nn.LayerNorm(ATTN_MODEL_DIM, eps=1e-5)
        self.value_fc1 = nn.Linear(ATTN_MODEL_DIM, ATTN_VALUE_HIDDEN)
        self.value_fc2 = nn.Linear(ATTN_VALUE_HIDDEN, 1)
        self.policy_fc1 = nn.Linear(ATTN_POLICY_INPUT, ATTN_POLICY_HIDDEN)
        self.policy_fc2 = nn.Linear(ATTN_POLICY_HIDDEN, 1)

    def encode_cls(self, global_feat: torch.Tensor, vocab_ids: torch.Tensor, token_attrs: torch.Tensor) -> torch.Tensor:
        batch_size = global_feat.size(0)
        cls_token = (self.global_proj(global_feat) + self.cls_embed).unsqueeze(1)
        safe_ids = vocab_ids.clamp(0, ATTN_VOCAB - 1).long()
        emb = self.card_embed(safe_ids)
        card_tokens = self.token_proj(torch.cat([emb, token_attrs], dim=-1))
        x = torch.cat([cls_token, card_tokens], dim=1)
        cls_valid = torch.zeros((batch_size, 1), dtype=torch.bool, device=vocab_ids.device)
        pad_mask = torch.cat([cls_valid, vocab_ids <= 0], dim=1)
        for layer in self.layers:
            x = layer(x, pad_mask)
        return self.final_norm(x[:, 0])

    def score_candidates(self, cls: torch.Tensor, candidate_feats: torch.Tensor) -> torch.Tensor:
        if candidate_feats.dim() == 2:
            pol_in = torch.cat([cls, candidate_feats], dim=-1)
        else:
            cls_expanded = cls.unsqueeze(1).expand(-1, candidate_feats.size(1), -1)
            pol_in = torch.cat([cls_expanded, candidate_feats], dim=-1)
        pol_h = F.relu(self.policy_fc1(pol_in))
        if self.dropout > 0.0 and self.training:
            pol_h = F.dropout(pol_h, p=self.dropout, training=True)
        return self.policy_fc2(pol_h).squeeze(-1)

    def forward(self, global_feat: torch.Tensor, vocab_ids: torch.Tensor, token_attrs: torch.Tensor,
                candidate_feats: torch.Tensor | None = None):
        cls = self.encode_cls(global_feat, vocab_ids, token_attrs)
        val_h = F.relu(self.value_fc1(cls))
        if self.dropout > 0.0 and self.training:
            val_h = F.dropout(val_h, p=self.dropout, training=True)
        value_logit = self.value_fc2(val_h).squeeze(-1)
        if candidate_feats is None:
            return value_logit, cls
        return value_logit, self.score_candidates(cls, candidate_feats)


@dataclass
class AttentionReplay:
    global_feat: np.ndarray
    vocab: np.ndarray
    attrs: np.ndarray
    targets: np.ndarray
    policies: np.ndarray
    offsets: np.ndarray


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


def read_attention_replay(directory: Path) -> AttentionReplay:
    value_file = directory / "val_log.bin"
    policy_file = directory / "pol_log.bin"
    turns, remainder = divmod(value_file.stat().st_size, ATTN_VALUE_DTYPE.itemsize)
    if turns == 0 or remainder:
        raise ValueError(f"Invalid attention value log size in {value_file}")
    records = np.fromfile(value_file, dtype=ATTN_VALUE_DTYPE)
    global_feat = records["global_feat"].astype(np.float32, copy=True)
    vocab = records["vocab"].astype(np.int32, copy=True)
    attrs = records["attrs"].astype(np.float32, copy=True)
    targets = records["target"].astype(np.float32, copy=True)
    if (not np.isfinite(global_feat).all() or not np.isfinite(attrs).all()
            or not np.isfinite(targets).all() or not np.isin(targets, [0.0, 0.5, 1.0]).all()
            or np.any((vocab < 0) | (vocab >= ATTN_VOCAB))):
        raise ValueError(f"Invalid attention value samples in {value_file}")

    size = policy_file.stat().st_size
    candidate_total, remainder = divmod(size - turns * COUNT.size, ATTN_POLICY_BYTES)
    if not turns <= candidate_total <= turns * MAX_CANDIDATES or remainder:
        raise ValueError(f"Invalid attention policy log size in {policy_file}")
    candidates = np.empty((candidate_total, ATTN_POLICY_FLOATS), dtype=np.float32)
    offsets = np.zeros(turns + 1, dtype=np.int64)
    cursor = 0
    with policy_file.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as raw:
        for turn in range(turns):
            if cursor + COUNT.size > size:
                raise ValueError(f"Missing attention policy header at turn {turn} in {policy_file}")
            count, = COUNT.unpack_from(raw, cursor)
            cursor += COUNT.size
            end = cursor + ATTN_POLICY_BYTES * count
            start_candidate = offsets[turn]
            stop_candidate = start_candidate + count
            if not 1 <= count <= MAX_CANDIDATES or end > size or stop_candidate > candidate_total:
                raise ValueError(f"Invalid attention policy group at turn {turn} in {policy_file}")
            candidates[start_candidate:stop_candidate] = np.frombuffer(
                raw, dtype="<f4", count=count * ATTN_POLICY_FLOATS, offset=cursor
            ).reshape(count, ATTN_POLICY_FLOATS)
            offsets[turn + 1] = stop_candidate
            cursor = end
    probabilities = candidates[:, -1]
    if (cursor != size or offsets[-1] != candidate_total or not np.isfinite(candidates).all()
            or np.any((probabilities < 0) | (probabilities > 1))
            or not np.allclose(np.add.reduceat(probabilities, offsets[:-1]), 1, rtol=0, atol=1e-6)):
        raise ValueError(f"Invalid attention policy samples in {policy_file}")
    return AttentionReplay(global_feat, vocab, attrs, targets, candidates, offsets)


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
    replace_file(checkpoint_tmp, directory / "model.pt")
    replace_file(weights_tmp, directory / "weights.bin")


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


def export_attention_weights(model: AttentionModel) -> bytes:
    state = model.state_dict()
    payload = b"".join(
        state[key].detach().cpu().contiguous().numpy().astype("<f4", copy=False).tobytes()
        for key in ATTN_PARAMETERS
    )
    if len(payload) != 4 * ATTN_PARAM_COUNT:
        raise ValueError("Unexpected Attention weight payload size")
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = ATTN_MAGIC + struct.pack(
        "<18I",
        ATTN_FORMAT_VERSION, ATTN_HEADER_BYTES, ATTN_VOCAB, ATTN_CARD_EMBED, ATTN_ATTR_DIM,
        ATTN_GLOBAL_DIM, ATTN_MODEL_DIM, ATTN_HEADS, ATTN_FFN_DIM, ATTN_LAYERS,
        ATTN_CANDIDATE_DIM, ATTN_VALUE_HIDDEN, ATTN_POLICY_HIDDEN, ATTN_MAX_TOKENS,
        ATTN_ACTIVATION_RELU, ATTN_PARAM_COUNT, crc, 0,
    )
    return header + payload


def load_attention_weights_bin(path: Path, device: torch.device | str = "cpu") -> AttentionModel:
    data = path.read_bytes()
    if len(data) != ATTN_WEIGHT_BYTES or data[:8] != ATTN_MAGIC:
        raise ValueError(f"Invalid attn_weights.bin header or size: {path}")
    words = struct.unpack_from("<18I", data, 8)
    payload = data[ATTN_HEADER_BYTES:]
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    expected_words = (
        ATTN_FORMAT_VERSION, ATTN_HEADER_BYTES, ATTN_VOCAB, ATTN_CARD_EMBED, ATTN_ATTR_DIM,
        ATTN_GLOBAL_DIM, ATTN_MODEL_DIM, ATTN_HEADS, ATTN_FFN_DIM, ATTN_LAYERS,
        ATTN_CANDIDATE_DIM, ATTN_VALUE_HIDDEN, ATTN_POLICY_HIDDEN, ATTN_MAX_TOKENS,
        ATTN_ACTIVATION_RELU, ATTN_PARAM_COUNT, crc, 0,
    )
    if words != expected_words:
        raise ValueError(f"attn_weights.bin header or CRC mismatch in {path}")
    floats = np.frombuffer(payload, dtype="<f4").astype(np.float32, copy=True)
    if not np.isfinite(floats).all():
        raise ValueError(f"attn_weights.bin contains non-finite values: {path}")
    model = AttentionModel()
    state = model.state_dict()
    cursor = 0
    for key in ATTN_PARAMETERS:
        target = state[key]
        count = target.numel()
        state[key] = torch.from_numpy(floats[cursor:cursor + count].reshape(target.shape))
        cursor += count
    model.load_state_dict(state)
    return model.to(device)


def save_attention_model(model: AttentionModel, directory: Path, generation: int, optimizer=None):
    directory.mkdir(parents=True, exist_ok=True)
    state = model.state_dict()
    binary = export_attention_weights(model)
    if len(binary) != ATTN_WEIGHT_BYTES:
        raise ValueError("Unexpected Attention weight layout")
    attn_tmp = directory / "attn_weights.bin.tmp"
    weights_tmp = directory / "weights.bin.tmp"
    checkpoint_tmp = directory / "model.pt.tmp"
    attn_tmp.write_bytes(binary)
    weights_tmp.write_bytes(binary)
    torch.save({
        "generation": generation,
        "model_type": "attention",
        "model": state,
        "optimizer": optimizer.state_dict() if optimizer is not None else None,
    }, checkpoint_tmp)
    replace_file(checkpoint_tmp, directory / "model.pt")
    replace_file(attn_tmp, directory / "attn_weights.bin")
    replace_file(weights_tmp, directory / "weights.bin")


def initialize_attention_checkpoint(directory: Path, seed: int):
    torch.manual_seed(seed)
    model = AttentionModel()
    nn.init.normal_(model.card_embed.weight, mean=0.0, std=0.2)
    with torch.no_grad():
        model.card_embed.weight[0].zero_()
    nn.init.normal_(model.cls_embed, mean=0.0, std=0.05)
    for module in model.modules():
        if isinstance(module, nn.Linear):
            nn.init.xavier_uniform_(module.weight)
            nn.init.zeros_(module.bias)
    save_attention_model(model, directory, 0)


def load_attention_checkpoint(path: Path, device: torch.device):
    checkpoint = torch.load(path, map_location=device, weights_only=True)
    model = AttentionModel().to(device)
    model.load_state_dict(checkpoint["model"])
    return model, checkpoint


def make_attention_reference(path: Path, replay: AttentionReplay, generation: int, seed: int):
    turns = len(replay.targets)
    if turns < 2:
        raise ValueError("A fixed holdout requires at least two replay turns")
    chosen = np.random.default_rng(seed).choice(turns, size=min(1024, max(1, turns // 10)), replace=False)
    starts, ends = replay.offsets[chosen, None], replay.offsets[chosen + 1, None]
    indices = starts + np.arange(MAX_CANDIDATES)
    valid = indices < ends
    policy = replay.policies[np.where(valid, indices, 0), :ATTN_CANDIDATE_DIM]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as stream:
        np.savez_compressed(
            stream, generation=generation, heldout=chosen,
            global_feat=replay.global_feat[chosen], vocab=replay.vocab[chosen],
            attrs=replay.attrs[chosen], policies=policy, valid=valid,
        )


@torch.inference_mode()
def measure_attention_drift(model: AttentionModel, previous: AttentionModel, reference, device):
    value_total = torch.zeros((), device=device)
    policy_total = torch.zeros((), device=device)
    size = len(reference["global_feat"])
    for start in range(0, size, 256):
        batch = slice(start, start + 256)
        g = torch.as_tensor(reference["global_feat"][batch], device=device)
        v = torch.as_tensor(reference["vocab"][batch], device=device)
        a = torch.as_tensor(reference["attrs"][batch], device=device)
        policies = torch.as_tensor(reference["policies"][batch], device=device)
        valid = torch.as_tensor(reference["valid"][batch], device=device)
        new_val, new_pol = model(g, v, a, policies)
        old_val, old_pol = previous(g, v, a, policies)
        value_delta = (new_val.sigmoid() - old_val.sigmoid()).abs()
        new_logit = new_pol.masked_fill(~valid, -torch.inf)
        old_logit = old_pol.masked_fill(~valid, -torch.inf)
        value_total += value_delta.sum()
        policy_total += 0.5 * (new_logit.softmax(-1) - old_logit.softmax(-1)).abs().sum()
    return float(value_total / size), float(policy_total / size)


def train_attention_generation(replay_dirs, previous_path: Path, output: Path, reference_path: Path,
                               generation: int, *, device: torch.device, epochs: int, batch_size: int, seed: int,
                               learning_rate=None, weight_decay=None, dropout: float = 0.0,
                               value_label_smoothing: float = 0.0):
    if not replay_dirs or epochs < 1 or batch_size < 1:
        raise ValueError("Replay directories, positive epochs and positive batch size are required")
    if learning_rate is not None and (not math.isfinite(learning_rate) or learning_rate <= 0):
        raise ValueError("Learning rate must be finite and positive")
    if weight_decay is not None and (not math.isfinite(weight_decay) or weight_decay < 0):
        raise ValueError("Weight decay must be finite and non-negative")
    if not (math.isfinite(dropout) and 0.0 <= dropout < 1.0):
        raise ValueError("Dropout must be in [0.0, 1.0)")
    if not (math.isfinite(value_label_smoothing) and 0.0 <= value_label_smoothing < 0.5):
        raise ValueError("Value label smoothing must be in [0.0, 0.5)")
    matchups = []
    for _, path in replay_dirs:
        layout = path / "matchup_features.json"
        matchups.append(json.loads(layout.read_text(encoding="utf-8")) if layout.is_file() else None)
    if any(layout != matchups[0] for layout in matchups[1:]):
        raise ValueError("Replay directories use different matchup feature layouts")
    if matchups[0] is not None:
        output.mkdir(parents=True, exist_ok=True)
        (output / "matchup_features.json").write_text(
            json.dumps(matchups[0], ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    torch.manual_seed(seed)
    replays = [read_attention_replay(path) for _, path in replay_dirs]
    if not reference_path.exists():
        make_attention_reference(reference_path, replays[0], replay_dirs[0][0], seed)
    with np.load(reference_path) as source:
        reference = {name: source[name] for name in source.files}

    size_bytes = sum(
        r.global_feat.nbytes + r.vocab.nbytes + r.attrs.nbytes + r.targets.nbytes + r.policies.nbytes + r.offsets.nbytes
        for r in replays
    )
    storage = torch.device("cpu")
    if device.type == "cuda" and size_bytes < 0.35 * torch.cuda.mem_get_info(device)[0]:
        storage = device
    global_feat = torch.as_tensor(np.concatenate([r.global_feat for r in replays]), device=storage)
    vocab = torch.as_tensor(np.concatenate([r.vocab for r in replays]), device=storage)
    attrs = torch.as_tensor(np.concatenate([r.attrs for r in replays]), device=storage)
    val_targets = torch.as_tensor(np.concatenate([r.targets for r in replays]), device=storage)
    policies = torch.as_tensor(np.concatenate([r.policies for r in replays]), device=storage)
    offsets = [0]
    eligible = []
    row = 0
    for (source_generation, _), replay in zip(replay_dirs, replays):
        offsets.extend(replay.offsets[1:] + offsets[-1])
        present = np.arange(len(replay.targets))
        if source_generation == int(reference["generation"]):
            present = np.delete(present, reference["heldout"])
        eligible.append(present + row)
        row += len(replay.targets)
    del replays
    offsets = torch.as_tensor(offsets, device=storage, dtype=torch.long)
    turns = torch.as_tensor(np.concatenate(eligible), device=storage)
    columns = torch.arange(MAX_CANDIDATES, device=storage)
    if turns.numel() == 0:
        raise ValueError("The replay window has no training samples")

    previous, checkpoint = load_attention_checkpoint(previous_path, device)
    if checkpoint["generation"] != generation - 1:
        raise ValueError("The checkpoint must be from the preceding generation")
    model = AttentionModel(dropout=dropout).to(device)
    model.load_state_dict(previous.state_dict())
    wd = 1e-4 if weight_decay is None else weight_decay
    opt_cls = torch.optim.AdamW if weight_decay is not None else torch.optim.Adam
    optimizer = opt_cls(model.parameters(), lr=1e-3, weight_decay=wd)
    if checkpoint["optimizer"] is not None:
        optimizer.load_state_dict(checkpoint["optimizer"])
    if learning_rate is not None:
        for group in optimizer.param_groups:
            group["lr"] = learning_rate
    if weight_decay is not None:
        for group in optimizer.param_groups:
            group["weight_decay"] = weight_decay
    del checkpoint
    model.train()
    totals = torch.zeros(2, device=device)
    for _ in range(epochs):
        turn_order = turns[torch.randperm(turns.numel(), device=storage)]
        for turn_ids in turn_order.split(batch_size):
            g_batch = global_feat[turn_ids].to(device)
            v_batch = vocab[turn_ids].to(device)
            a_batch = attrs[turn_ids].to(device)
            t_batch = val_targets[turn_ids].to(device)
            if value_label_smoothing > 0.0:
                t_batch = t_batch * (1.0 - value_label_smoothing) + 0.5 * value_label_smoothing
            indices = offsets[turn_ids, None] + columns
            valid = indices < offsets[turn_ids + 1, None]
            policy = policies[torch.where(valid, indices, 0)].to(device)
            valid = valid.to(device)

            value_logits, policy_logits = model(g_batch, v_batch, a_batch, policy[:, :, :ATTN_CANDIDATE_DIM])
            value_loss = F.binary_cross_entropy_with_logits(value_logits, t_batch)
            targets = policy[:, :, -1].masked_fill(~valid, 0.0)
            policy_loss = -(targets * masked_log_softmax(policy_logits, valid)).sum(-1).mean()
            optimizer.zero_grad(set_to_none=True)
            (value_loss + policy_loss).backward()
            optimizer.step()
            totals += torch.stack((value_loss.detach(), policy_loss.detach())) * len(turn_ids)

    val_loss, pol_loss = (totals / (epochs * turns.numel())).tolist()
    delta_v, delta_pi = measure_attention_drift(model.eval(), previous.eval(), reference, device)
    if not all(math.isfinite(x) for x in (val_loss, pol_loss, delta_v, delta_pi)):
        raise RuntimeError("Training produced non-finite metrics")
    save_attention_model(model, output, generation, optimizer)
    metrics = {
        "val_loss": val_loss, "pol_loss": pol_loss, "delta_v": delta_v, "delta_pi": delta_pi,
        "training_turns": turns.numel(), "reference_turns": len(reference["global_feat"]),
        "learning_rate": optimizer.param_groups[0]["lr"],
        "weight_decay": optimizer.param_groups[0]["weight_decay"],
        "dropout": dropout,
        "value_label_smoothing": value_label_smoothing,
        "replay_generations": [number for number, _ in replay_dirs],
        "device": torch.cuda.get_device_name(device) if device.type == "cuda" else "CPU",
        "model_type": "attention",
    }
    (output / "training.json").write_text(json.dumps(metrics, indent=2) + "\n", encoding="utf-8")
    return metrics


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
                     learning_rate=None, weight_decay=None, dropout: float = 0.0,
                     value_label_smoothing: float = 0.0, model_type: str = "mlp"):
    if model_type == "attention":
        return train_attention_generation(
            replay_dirs, previous_path, output, reference_path, generation,
            device=device, epochs=epochs, batch_size=batch_size, seed=seed,
            learning_rate=learning_rate, weight_decay=weight_decay, dropout=dropout,
            value_label_smoothing=value_label_smoothing,
        )
    if not replay_dirs or epochs < 1 or batch_size < 1:
        raise ValueError("Replay directories, positive epochs and positive batch size are required")
    if learning_rate is not None and (not math.isfinite(learning_rate) or learning_rate <= 0):
        raise ValueError("Learning rate must be finite and positive")
    if weight_decay is not None and (not math.isfinite(weight_decay) or weight_decay < 0):
        raise ValueError("Weight decay must be finite and non-negative")
    if not (math.isfinite(value_label_smoothing) and 0.0 <= value_label_smoothing < 0.5):
        raise ValueError("Value label smoothing must be in [0.0, 0.5)")
    matchups = []
    for _, path in replay_dirs:
        layout = path / "matchup_features.json"
        matchups.append(json.loads(layout.read_text(encoding="utf-8")) if layout.is_file() else None)
    if any(layout != matchups[0] for layout in matchups[1:]):
        raise ValueError("Replay directories use different matchup feature layouts")
    if matchups[0] is not None:
        output.mkdir(parents=True, exist_ok=True)
        (output / "matchup_features.json").write_text(
            json.dumps(matchups[0], ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
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
    parser.add_argument("--learning-rate", "--lr", dest="learning_rate", type=float,
                        help="Override the checkpoint's Adam learning rate")
    parser.add_argument("--weight-decay", type=float, help="Override the checkpoint's weight decay")
    parser.add_argument("--dropout", type=float, default=0.0, help="Training dropout probability for Attention")
    parser.add_argument("--value-label-smoothing", type=float, default=0.0,
                        help="Label smoothing factor for value targets")
    parser.add_argument("--batch-size", type=int, default=256)
    parser.add_argument("--seed", type=int, default=2026)
    parser.add_argument("--device", choices=("auto", "cpu", "cuda"), default="auto")
    parser.add_argument("--model-type", choices=("mlp", "attention"), default="mlp")
    args = parser.parse_args()
    genesis = max(1, args.generation - args.replay_window + 1)
    paths = [(number, generation_dir(args.replay_root, number))
             for number in range(genesis, args.generation + 1)]
    metrics = train_generation(paths, args.previous, args.output_dir,
                               args.eval_file or args.replay_root / "evaluation.npz",
                               args.generation, device=pick_device(args.device), epochs=args.epochs,
                               batch_size=args.batch_size, seed=args.seed, learning_rate=args.learning_rate,
                               weight_decay=args.weight_decay, dropout=args.dropout,
                               value_label_smoothing=args.value_label_smoothing, model_type=args.model_type)
    print(json.dumps(metrics, indent=2))


if __name__ == "__main__":
    main()
