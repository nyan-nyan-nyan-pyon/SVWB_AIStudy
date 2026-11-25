"""Train the value and policy heads from the C++ replay files (MLP and Attention v2 / v3-G / v3-GD)."""

import argparse
from dataclasses import asdict, dataclass
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
ATTN_VAL_LOG_MAGIC = b"SVWBAVAL"
ATTN_POL_LOG_MAGIC = b"SVWBAPOL"
ATTN_FORMAT_VERSION_LEGACY = 1
ATTN_FORMAT_VERSION_V3 = 2
ATTN_LOG_FORMAT_VERSION = 3
ATTN_LOG_HEADER_WORDS = 15
ATTN_LOG_HEADER_BYTES = 8 + 4 * ATTN_LOG_HEADER_WORDS  # 68

ATTN_VOCAB = 600
ATTN_CARD_EMBED = 32
ATTN_ATTR_DIM = 32
ATTN_TOKEN_INPUT = ATTN_CARD_EMBED + ATTN_ATTR_DIM
ATTN_GLOBAL_DIM_V2 = 32
ATTN_GLOBAL_DIM = 64
ATTN_MODEL_DIM = 128
ATTN_HEADS = 4
ATTN_HEAD_DIM = ATTN_MODEL_DIM // ATTN_HEADS
ATTN_FFN_DIM = 512
ATTN_LAYERS = 2
ATTN_CANDIDATE_DIM = 64
ATTN_DIFF_ZONES = 5
ATTN_CANDIDATE_EMBDIFF_DIM = ATTN_DIFF_ZONES * ATTN_CARD_EMBED  # 160
ATTN_ZONE_MAX_DIFF = (18, 10, 10, 10, 10)
ATTN_ZONE_SLICE_OFFSETS = (0, 18, 28, 38, 48, 58)
ATTN_MAX_SPARSE_DIFF = 58
ATTN_VALUE_HIDDEN = 64
ATTN_POLICY_HIDDEN = 128
ATTN_POLICY_INPUT_V2 = ATTN_MODEL_DIM + ATTN_CANDIDATE_DIM  # 192
ATTN_POLICY_INPUT = ATTN_MODEL_DIM + ATTN_CANDIDATE_DIM + ATTN_CANDIDATE_EMBDIFF_DIM  # 352
ATTN_CARD_TOKENS = 29
ATTN_MAX_TOKENS = ATTN_CARD_TOKENS + 1
ATTN_ACTIVATION_RELU = 1
ATTN_CARD_VOCAB_HASH = 0x6680F613
ATTN_RULES_VERSION = 1
ATTN_REWARD_WIN_HALF_BOTH_LOSE = 1

ATTN_PARAM_COUNT_V2 = 461_826
ATTN_PARAM_COUNT_V3G = 465_922
ATTN_PARAM_COUNT_V3GD = 486_402
ATTN_PARAM_COUNT = ATTN_PARAM_COUNT_V3GD

ATTN_HEADER_WORDS_LEGACY = 18
ATTN_HEADER_BYTES_LEGACY = len(ATTN_MAGIC) + 4 * ATTN_HEADER_WORDS_LEGACY  # 80
ATTN_HEADER_WORDS_V3 = 26
ATTN_HEADER_BYTES_V3 = len(ATTN_MAGIC) + 4 * ATTN_HEADER_WORDS_V3          # 112
ATTN_HEADER_BYTES = ATTN_HEADER_BYTES_V3

ATTN_WEIGHT_BYTES_LEGACY = ATTN_HEADER_BYTES_LEGACY + 4 * ATTN_PARAM_COUNT_V2
ATTN_WEIGHT_BYTES_V3G = ATTN_HEADER_BYTES_V3 + 4 * ATTN_PARAM_COUNT_V3G
ATTN_WEIGHT_BYTES = ATTN_HEADER_BYTES_V3 + 4 * ATTN_PARAM_COUNT_V3GD

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

# Column mapping W_v3[:, dst] = W_v2[:, src] (v2[27] is split 0.5f each into v3[37] and v3[38]).
GLOBAL_V2_TO_V3_MAP = (
    (0, 56), (1, 57), (2, 3), (3, 4), (4, 5), (5, 6), (6, 7), (7, 9),
    (8, 10), (9, 11), (10, 0), (11, 1), (12, 2), (13, 14), (14, 15), (15, 16),
    (16, 17), (17, 18), (18, 19), (19, 20), (20, 21), (21, 22), (22, 23),
    (23, 28), (24, 29), (25, 30), (26, 32), (28, 42), (29, 44), (30, 47), (31, 49),
)

ATTN_POLICY_V3_DTYPE = np.dtype([
    ("features", "<f4", (ATTN_CANDIDATE_DIM,)),
    ("diff_ids", "<u2", (ATTN_MAX_SPARSE_DIFF,)),
    ("diff_signs", "<i1", (ATTN_MAX_SPARSE_DIFF,)),
    ("zone_counts", "<u1", (ATTN_DIFF_ZONES + 1,)),
    ("target", "<f4"),
])
assert ATTN_POLICY_V3_DTYPE.itemsize == 440

ATTN_POLICY_V2_FLOATS = ATTN_CANDIDATE_DIM + 1
ATTN_POLICY_V2_BYTES = 4 * ATTN_POLICY_V2_FLOATS


def compute_attention_param_count(global_dim: int, candidate_dim: int, embdiff_dim: int) -> int:
    layer_params = (
        2 * ATTN_MODEL_DIM
        + (3 * ATTN_MODEL_DIM * ATTN_MODEL_DIM + 3 * ATTN_MODEL_DIM)
        + (ATTN_MODEL_DIM * ATTN_MODEL_DIM + ATTN_MODEL_DIM)
        + 2 * ATTN_MODEL_DIM
        + (ATTN_MODEL_DIM * ATTN_FFN_DIM + ATTN_FFN_DIM)
        + (ATTN_FFN_DIM * ATTN_MODEL_DIM + ATTN_MODEL_DIM)
    )
    pol_in = ATTN_MODEL_DIM + candidate_dim + embdiff_dim
    return (
        ATTN_VOCAB * ATTN_CARD_EMBED
        + (ATTN_TOKEN_INPUT * ATTN_MODEL_DIM + ATTN_MODEL_DIM)
        + (global_dim * ATTN_MODEL_DIM + ATTN_MODEL_DIM)
        + ATTN_MODEL_DIM
        + ATTN_LAYERS * layer_params
        + 2 * ATTN_MODEL_DIM
        + (ATTN_MODEL_DIM * ATTN_VALUE_HIDDEN + ATTN_VALUE_HIDDEN)
        + (ATTN_VALUE_HIDDEN + 1)
        + (pol_in * ATTN_POLICY_HIDDEN + ATTN_POLICY_HIDDEN)
        + (ATTN_POLICY_HIDDEN + 1)
    )


assert compute_attention_param_count(32, 64, 0) == ATTN_PARAM_COUNT_V2
assert compute_attention_param_count(64, 64, 0) == ATTN_PARAM_COUNT_V3G
assert compute_attention_param_count(64, 64, 160) == ATTN_PARAM_COUNT_V3GD


@dataclass(frozen=True)
class AttentionMetadata:
    model_version: int = 3
    global_dim: int = ATTN_GLOBAL_DIM
    candidate_dim: int = ATTN_CANDIDATE_DIM
    candidate_embdiff_dim: int = ATTN_CANDIDATE_EMBDIFF_DIM
    token_schema_version: int = 3
    card_vocab_hash: int = ATTN_CARD_VOCAB_HASH
    rules_version: int = ATTN_RULES_VERSION
    search_version: int = 2
    reward_definition: int = ATTN_REWARD_WIN_HALF_BOTH_LOSE

    @staticmethod
    def for_variant(variant: str, search_version: int = 2) -> "AttentionMetadata":
        if variant == "v2":
            return AttentionMetadata(
                model_version=2 if search_version >= 2 else 1,
                global_dim=ATTN_GLOBAL_DIM_V2,
                candidate_dim=ATTN_CANDIDATE_DIM,
                candidate_embdiff_dim=0,
                token_schema_version=1,
                card_vocab_hash=ATTN_CARD_VOCAB_HASH,
                rules_version=ATTN_RULES_VERSION,
                search_version=search_version,
                reward_definition=ATTN_REWARD_WIN_HALF_BOTH_LOSE if search_version >= 2 else 0,
            )
        if variant == "v3-g":
            return AttentionMetadata(
                model_version=3,
                global_dim=ATTN_GLOBAL_DIM,
                candidate_dim=ATTN_CANDIDATE_DIM,
                candidate_embdiff_dim=0,
                token_schema_version=3,
                card_vocab_hash=ATTN_CARD_VOCAB_HASH,
                rules_version=ATTN_RULES_VERSION,
                search_version=search_version,
                reward_definition=ATTN_REWARD_WIN_HALF_BOTH_LOSE,
            )
        if variant in ("v3-gd", "v3"):
            return AttentionMetadata(
                model_version=3,
                global_dim=ATTN_GLOBAL_DIM,
                candidate_dim=ATTN_CANDIDATE_DIM,
                candidate_embdiff_dim=ATTN_CANDIDATE_EMBDIFF_DIM,
                token_schema_version=3,
                card_vocab_hash=ATTN_CARD_VOCAB_HASH,
                rules_version=ATTN_RULES_VERSION,
                search_version=search_version,
                reward_definition=ATTN_REWARD_WIN_HALF_BOTH_LOSE,
            )
        raise ValueError(f"Unknown Attention variant: {variant!r}")

    def validate(self) -> str:
        if self.candidate_dim != ATTN_CANDIDATE_DIM:
            raise ValueError(f"Unsupported candidate_dim={self.candidate_dim}")
        if self.card_vocab_hash not in (0, ATTN_CARD_VOCAB_HASH):
            raise ValueError(
                f"card_vocab_hash mismatch: expected 0x{ATTN_CARD_VOCAB_HASH:08X}, got 0x{self.card_vocab_hash:08X}"
            )
        dims = (self.global_dim, self.candidate_dim, self.candidate_embdiff_dim)
        if dims == (32, 64, 0):
            if self.token_schema_version != 1:
                raise ValueError(f"Legacy v2 (32,64,0) requires token_schema_version=1, got {self.token_schema_version}")
            return "v2"
        if dims == (64, 64, 0):
            if self.token_schema_version != 3:
                raise ValueError(f"v3-G (64,64,0) requires token_schema_version=3, got {self.token_schema_version}")
            return "v3-g"
        if dims == (64, 64, 160):
            if self.token_schema_version != 3:
                raise ValueError(f"v3-GD (64,64,160) requires token_schema_version=3, got {self.token_schema_version}")
            return "v3-gd"
        raise ValueError(f"Unsupported Attention dimension tuple: {dims}")

    @property
    def variant(self) -> str:
        return self.validate()

    @property
    def parameter_count(self) -> int:
        self.validate()
        return compute_attention_param_count(self.global_dim, self.candidate_dim, self.candidate_embdiff_dim)


def value_dtype_for(global_dim: int) -> np.dtype:
    return np.dtype([
        ("global_feat", "<f4", (global_dim,)),
        ("vocab", "<i4", (ATTN_CARD_TOKENS,)),
        ("attrs", "<f4", (ATTN_CARD_TOKENS, ATTN_ATTR_DIM)),
        ("target", "<f4"),
    ])


ATTN_VALUE_DTYPE = value_dtype_for(ATTN_GLOBAL_DIM)
ATTN_VALUE_DTYPE_V2 = value_dtype_for(ATTN_GLOBAL_DIM_V2)


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
    def __init__(self, dropout: float = 0.0, global_dim: int = ATTN_GLOBAL_DIM,
                 candidate_dim: int = ATTN_CANDIDATE_DIM,
                 candidate_embdiff_dim: int = ATTN_CANDIDATE_EMBDIFF_DIM):
        super().__init__()
        self.dropout = dropout
        self.global_dim = global_dim
        self.candidate_dim = candidate_dim
        self.candidate_embdiff_dim = candidate_embdiff_dim
        pol_in = ATTN_MODEL_DIM + candidate_dim + candidate_embdiff_dim

        self.card_embed = nn.Embedding(ATTN_VOCAB, ATTN_CARD_EMBED)
        self.token_proj = nn.Linear(ATTN_TOKEN_INPUT, ATTN_MODEL_DIM)
        self.global_proj = nn.Linear(global_dim, ATTN_MODEL_DIM)
        self.cls_embed = nn.Parameter(torch.zeros(ATTN_MODEL_DIM))
        self.layers = nn.ModuleList([AttentionEncoderLayer(dropout=dropout) for _ in range(ATTN_LAYERS)])
        self.final_norm = nn.LayerNorm(ATTN_MODEL_DIM, eps=1e-5)
        self.value_fc1 = nn.Linear(ATTN_MODEL_DIM, ATTN_VALUE_HIDDEN)
        self.value_fc2 = nn.Linear(ATTN_VALUE_HIDDEN, 1)
        self.policy_fc1 = nn.Linear(pol_in, ATTN_POLICY_HIDDEN)
        self.policy_fc2 = nn.Linear(ATTN_POLICY_HIDDEN, 1)

    def compute_embdiff(self, diff_ids: torch.Tensor, diff_signs: torch.Tensor) -> torch.Tensor:
        # Explicit vocab_id != 0 zero-masking (never relies on padding_idx=0).
        safe_ids = diff_ids.clamp(0, ATTN_VOCAB - 1).long()
        emb = self.card_embed(safe_ids)
        mask = diff_ids.ne(0).unsqueeze(-1).to(emb.dtype)
        weighted = emb * mask * diff_signs.to(emb.dtype).unsqueeze(-1)
        zones = [
            weighted[..., ATTN_ZONE_SLICE_OFFSETS[z]:ATTN_ZONE_SLICE_OFFSETS[z + 1], :].sum(dim=-2)
            for z in range(ATTN_DIFF_ZONES)
        ]
        return torch.cat(zones, dim=-1)

    def encode_cls(self, global_feat: torch.Tensor, vocab_ids: torch.Tensor, token_attrs: torch.Tensor) -> torch.Tensor:
        batch_size = global_feat.size(0)
        g_in = global_feat[:, :self.global_dim]
        cls_token = (self.global_proj(g_in) + self.cls_embed).unsqueeze(1)
        safe_ids = vocab_ids.clamp(0, ATTN_VOCAB - 1).long()
        emb = self.card_embed(safe_ids)
        card_tokens = self.token_proj(torch.cat([emb, token_attrs], dim=-1))
        x = torch.cat([cls_token, card_tokens], dim=1)
        cls_valid = torch.zeros((batch_size, 1), dtype=torch.bool, device=vocab_ids.device)
        pad_mask = torch.cat([cls_valid, vocab_ids <= 0], dim=1)
        for layer in self.layers:
            x = layer(x, pad_mask)
        return self.final_norm(x[:, 0])

    def score_candidates(self, cls: torch.Tensor, candidate_feats: torch.Tensor,
                         diff_ids: torch.Tensor | None = None,
                         diff_signs: torch.Tensor | None = None) -> torch.Tensor:
        parts = [candidate_feats]
        if self.candidate_embdiff_dim > 0:
            if diff_ids is not None and diff_signs is not None:
                parts.append(self.compute_embdiff(diff_ids, diff_signs))
            elif candidate_feats.size(-1) == self.candidate_dim:
                raise ValueError("v3-GD model requires diff_ids and diff_signs (or pre-concatenated 224-dim features)")
        feat_cat = torch.cat(parts, dim=-1) if len(parts) > 1 else parts[0]
        if feat_cat.dim() == 2:
            pol_in = torch.cat([cls, feat_cat], dim=-1)
        else:
            cls_expanded = cls.unsqueeze(1).expand(-1, feat_cat.size(1), -1)
            pol_in = torch.cat([cls_expanded, feat_cat], dim=-1)
        pol_h = F.relu(self.policy_fc1(pol_in))
        if self.dropout > 0.0 and self.training:
            pol_h = F.dropout(pol_h, p=self.dropout, training=True)
        return self.policy_fc2(pol_h).squeeze(-1)

    def forward(self, global_feat: torch.Tensor, vocab_ids: torch.Tensor, token_attrs: torch.Tensor,
                candidate_feats: torch.Tensor | None = None,
                diff_ids: torch.Tensor | None = None,
                diff_signs: torch.Tensor | None = None):
        cls = self.encode_cls(global_feat, vocab_ids, token_attrs)
        val_h = F.relu(self.value_fc1(cls))
        if self.dropout > 0.0 and self.training:
            val_h = F.dropout(val_h, p=self.dropout, training=True)
        value_logit = self.value_fc2(val_h).squeeze(-1)
        if candidate_feats is None:
            return value_logit, cls
        return value_logit, self.score_candidates(cls, candidate_feats, diff_ids, diff_signs)

    @torch.inference_mode()
    def inspect_stages(self, global_feat: torch.Tensor, vocab_ids: torch.Tensor, token_attrs: torch.Tensor,
                       candidate_feats: torch.Tensor, diff_ids: torch.Tensor | None = None,
                       diff_signs: torch.Tensor | None = None):
        batch_size = global_feat.size(0)
        g_in = global_feat[:, :self.global_dim]
        global_cls_init = self.global_proj(g_in) + self.cls_embed
        safe_ids = vocab_ids.clamp(0, ATTN_VOCAB - 1).long()
        emb = self.card_embed(safe_ids)
        raw_token_proj = self.token_proj(torch.cat([emb, token_attrs], dim=-1))
        occupied = (vocab_ids > 0).unsqueeze(-1).to(raw_token_proj.dtype)
        token_proj_masked = raw_token_proj * occupied

        x = torch.cat([global_cls_init.unsqueeze(1), raw_token_proj], dim=1)
        cls_valid = torch.zeros((batch_size, 1), dtype=torch.bool, device=vocab_ids.device)
        pad_mask = torch.cat([cls_valid, vocab_ids <= 0], dim=1)
        for layer in self.layers:
            x = layer(x, pad_mask)
        final_cls = self.final_norm(x[:, 0])
        val_h = F.relu(self.value_fc1(final_cls))
        value_logit = self.value_fc2(val_h).squeeze(-1)
        value_prob = torch.sigmoid(value_logit)

        if self.candidate_embdiff_dim > 0 and diff_ids is not None and diff_signs is not None:
            embdiff_dense = self.compute_embdiff(diff_ids, diff_signs)
            feat_cat = torch.cat([candidate_feats, embdiff_dense], dim=-1)
        else:
            embdiff_dense = torch.zeros(
                (*candidate_feats.shape[:-1], ATTN_CANDIDATE_EMBDIFF_DIM),
                dtype=candidate_feats.dtype, device=candidate_feats.device,
            )
            feat_cat = candidate_feats
        cls_expanded = final_cls.unsqueeze(1).expand(-1, feat_cat.size(1), -1)
        pol_in = torch.cat([cls_expanded, feat_cat], dim=-1)
        policy_h1 = F.relu(self.policy_fc1(pol_in))
        policy_logits = self.policy_fc2(policy_h1).squeeze(-1)
        return {
            "global_cls_init": global_cls_init,
            "token_proj": token_proj_masked,
            "final_cls": final_cls,
            "embdiff_dense": embdiff_dense,
            "policy_h1": policy_h1,
            "value_logit": value_logit,
            "value_prob": value_prob,
            "policy_logits": policy_logits,
        }


@dataclass
class AttentionReplay:
    metadata: AttentionMetadata
    global_feat: np.ndarray
    vocab: np.ndarray
    attrs: np.ndarray
    targets: np.ndarray
    policies: np.ndarray
    diff_ids: np.ndarray | None
    diff_signs: np.ndarray | None
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


def _parse_attention_log_header(raw_bytes: bytes, expected_magic: bytes, path: Path) -> AttentionMetadata:
    if len(raw_bytes) < ATTN_LOG_HEADER_BYTES or raw_bytes[:8] != expected_magic:
        raise ValueError(
            f"Legacy v2 or headerless Attention replay log rejected ({path}): "
            f"missing {expected_magic!r} metadata header (F06/AC-D-3)"
        )
    words = struct.unpack_from(f"<{ATTN_LOG_HEADER_WORDS}I", raw_bytes, 8)
    fmt_ver, hdr_bytes, model_ver, g_dim, vocab_sz, card_toks, attr_dim, c_dim, d_dim, tok_ver, v_hash, r_ver, s_ver, rew_def, _ = words
    if fmt_ver != ATTN_LOG_FORMAT_VERSION or hdr_bytes != ATTN_LOG_HEADER_BYTES:
        raise ValueError(f"Unsupported Attention replay log format ({fmt_ver}, {hdr_bytes}) in {path}")
    if vocab_sz != ATTN_VOCAB or card_toks != ATTN_CARD_TOKENS or attr_dim != ATTN_ATTR_DIM:
        raise ValueError(f"Attention replay token constants mismatch in {path}")
    meta = AttentionMetadata(
        model_version=model_ver,
        global_dim=g_dim,
        candidate_dim=c_dim,
        candidate_embdiff_dim=d_dim,
        token_schema_version=tok_ver,
        card_vocab_hash=v_hash,
        rules_version=r_ver,
        search_version=s_ver,
        reward_definition=rew_def,
    )
    meta.validate()
    if meta.search_version < 2 or meta.reward_definition != ATTN_REWARD_WIN_HALF_BOTH_LOSE:
        raise ValueError(
            f"Replay log {path} was generated with legacy search_version={meta.search_version} "
            f"or reward_definition={meta.reward_definition}; P0/v3 training requires P0 search logs"
        )
    return meta


def read_attention_replay(directory: Path, expected_meta: AttentionMetadata | None = None) -> AttentionReplay:
    value_file = directory / "val_log.bin"
    policy_file = directory / "pol_log.bin"
    val_size = value_file.stat().st_size
    pol_size = policy_file.stat().st_size
    if val_size <= ATTN_LOG_HEADER_BYTES or pol_size <= ATTN_LOG_HEADER_BYTES:
        raise ValueError(f"Truncated or legacy Attention replay log in {directory}")

    with value_file.open("rb") as vf:
        val_hdr_raw = vf.read(ATTN_LOG_HEADER_BYTES)
    with policy_file.open("rb") as pf:
        pol_hdr_raw = pf.read(ATTN_LOG_HEADER_BYTES)

    val_meta = _parse_attention_log_header(val_hdr_raw, ATTN_VAL_LOG_MAGIC, value_file)
    pol_meta = _parse_attention_log_header(pol_hdr_raw, ATTN_POL_LOG_MAGIC, policy_file)
    if val_meta != pol_meta:
        raise ValueError(f"Mismatched val_log.bin and pol_log.bin headers in {directory}")

    if expected_meta is not None:
        if (val_meta.global_dim, val_meta.candidate_dim, val_meta.candidate_embdiff_dim, val_meta.token_schema_version) != (
            expected_meta.global_dim, expected_meta.candidate_dim, expected_meta.candidate_embdiff_dim, expected_meta.token_schema_version
        ):
            raise ValueError(
                f"Replay schema ({val_meta.global_dim},{val_meta.candidate_dim},{val_meta.candidate_embdiff_dim}) "
                f"does not match model ({expected_meta.global_dim},{expected_meta.candidate_dim},{expected_meta.candidate_embdiff_dim}) in {directory}"
            )
        if val_meta.card_vocab_hash != expected_meta.card_vocab_hash:
            raise ValueError(f"Replay card_vocab_hash mismatch in {directory}")

    vdtype = value_dtype_for(val_meta.global_dim)
    turns, remainder = divmod(val_size - ATTN_LOG_HEADER_BYTES, vdtype.itemsize)
    if turns == 0 or remainder:
        raise ValueError(f"Invalid attention value log size in {value_file}")
    with value_file.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as vraw:
        records = np.frombuffer(vraw, dtype=vdtype, count=turns, offset=ATTN_LOG_HEADER_BYTES)
        global_feat = records["global_feat"].astype(np.float32, copy=True)
        vocab = records["vocab"].astype(np.int32, copy=True)
        attrs = records["attrs"].astype(np.float32, copy=True)
        targets = records["target"].astype(np.float32, copy=True)
        del records

    if (not np.isfinite(global_feat).all() or not np.isfinite(attrs).all()
            or not np.isfinite(targets).all() or not np.isin(targets, [0.0, 0.5, 1.0]).all()
            or np.any((vocab < 0) | (vocab >= ATTN_VOCAB))):
        raise ValueError(f"Invalid attention value samples in {value_file}")

    pol_payload_bytes = pol_size - ATTN_LOG_HEADER_BYTES
    use_embdiff = val_meta.candidate_embdiff_dim > 0
    rec_bytes = ATTN_POLICY_V3_DTYPE.itemsize if use_embdiff else ATTN_POLICY_V2_BYTES

    candidate_total, remainder = divmod(pol_payload_bytes - turns * COUNT.size, rec_bytes)
    if not turns <= candidate_total <= turns * MAX_CANDIDATES or remainder:
        raise ValueError(f"Invalid attention policy log size in {policy_file}")

    candidates = np.empty((candidate_total, ATTN_POLICY_V2_FLOATS), dtype=np.float32)
    diff_ids = np.zeros((candidate_total, ATTN_MAX_SPARSE_DIFF), dtype=np.int16) if use_embdiff else None
    diff_signs = np.zeros((candidate_total, ATTN_MAX_SPARSE_DIFF), dtype=np.int8) if use_embdiff else None
    offsets = np.zeros(turns + 1, dtype=np.int64)
    cursor = ATTN_LOG_HEADER_BYTES

    with policy_file.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as raw:
        for turn in range(turns):
            if cursor + COUNT.size > pol_size:
                raise ValueError(f"Missing attention policy header at turn {turn} in {policy_file}")
            count, = COUNT.unpack_from(raw, cursor)
            cursor += COUNT.size
            end = cursor + rec_bytes * count
            start_candidate = offsets[turn]
            stop_candidate = start_candidate + count
            if not 1 <= count <= MAX_CANDIDATES or end > pol_size or stop_candidate > candidate_total:
                raise ValueError(f"Invalid attention policy group at turn {turn} in {policy_file}")
            if use_embdiff:
                grp = np.frombuffer(raw, dtype=ATTN_POLICY_V3_DTYPE, count=count, offset=cursor)
                candidates[start_candidate:stop_candidate, :ATTN_CANDIDATE_DIM] = grp["features"]
                candidates[start_candidate:stop_candidate, -1] = grp["target"]
                if np.any(grp["diff_ids"] >= ATTN_VOCAB):
                    del grp
                    raise ValueError(f"Out-of-range diff vocab_id in {policy_file}")
                diff_ids[start_candidate:stop_candidate] = grp["diff_ids"].astype(np.int16, copy=False)
                diff_signs[start_candidate:stop_candidate] = grp["diff_signs"]
                del grp
            else:
                candidates[start_candidate:stop_candidate] = np.frombuffer(
                    raw, dtype="<f4", count=count * ATTN_POLICY_V2_FLOATS, offset=cursor
                ).reshape(count, ATTN_POLICY_V2_FLOATS)
            offsets[turn + 1] = stop_candidate
            cursor = end

    probabilities = candidates[:, -1]
    if (cursor != pol_size or offsets[-1] != candidate_total or not np.isfinite(candidates).all()
            or np.any((probabilities < 0) | (probabilities > 1))
            or not np.allclose(np.add.reduceat(probabilities, offsets[:-1]), 1, rtol=0, atol=1e-6)):
        raise ValueError(f"Invalid attention policy samples in {policy_file}")
    return AttentionReplay(val_meta, global_feat, vocab, attrs, targets, candidates, diff_ids, diff_signs, offsets)


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


def metadata_for_model(model: AttentionModel, search_version: int = 2) -> AttentionMetadata:
    dims = (model.global_dim, model.candidate_dim, model.candidate_embdiff_dim)
    if dims == (32, 64, 0):
        return AttentionMetadata.for_variant("v2", search_version=search_version)
    if dims == (64, 64, 0):
        return AttentionMetadata.for_variant("v3-g", search_version=search_version)
    if dims == (64, 64, 160):
        return AttentionMetadata.for_variant("v3-gd", search_version=search_version)
    raise ValueError(f"Unsupported AttentionModel dimensions: {dims}")


def export_attention_weights(model: AttentionModel, metadata: AttentionMetadata | None = None,
                             legacy_format: bool = False) -> bytes:
    meta = metadata or metadata_for_model(model)
    meta.validate()
    state = model.state_dict()
    arrays = []
    for key in ATTN_PARAMETERS:
        arr = state[key].detach().cpu().contiguous().numpy().astype("<f4", copy=True)
        if key == "card_embed.weight" and not legacy_format:
            arr[0] = 0.0
        arrays.append(arr.tobytes())
    payload = b"".join(arrays)
    expected_params = meta.parameter_count
    if len(payload) != 4 * expected_params:
        raise ValueError(f"Unexpected Attention weight payload size: {len(payload)} != {4 * expected_params}")
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    if legacy_format:
        if meta.variant != "v2":
            raise ValueError("legacy_format=True is only valid for v2 (32,64,0)")
        header = ATTN_MAGIC + struct.pack(
            "<18I",
            ATTN_FORMAT_VERSION_LEGACY, ATTN_HEADER_BYTES_LEGACY, ATTN_VOCAB, ATTN_CARD_EMBED, ATTN_ATTR_DIM,
            ATTN_GLOBAL_DIM_V2, ATTN_MODEL_DIM, ATTN_HEADS, ATTN_FFN_DIM, ATTN_LAYERS,
            ATTN_CANDIDATE_DIM, ATTN_VALUE_HIDDEN, ATTN_POLICY_HIDDEN, ATTN_MAX_TOKENS,
            ATTN_ACTIVATION_RELU, ATTN_PARAM_COUNT_V2, crc, 0,
        )
    else:
        header = ATTN_MAGIC + struct.pack(
            "<26I",
            ATTN_FORMAT_VERSION_V3, ATTN_HEADER_BYTES_V3, ATTN_VOCAB, ATTN_CARD_EMBED, ATTN_ATTR_DIM,
            meta.global_dim, ATTN_MODEL_DIM, ATTN_HEADS, ATTN_FFN_DIM, ATTN_LAYERS,
            meta.candidate_dim, ATTN_VALUE_HIDDEN, ATTN_POLICY_HIDDEN, ATTN_MAX_TOKENS,
            ATTN_ACTIVATION_RELU, expected_params, crc,
            meta.model_version, meta.candidate_embdiff_dim, meta.token_schema_version,
            meta.card_vocab_hash, meta.rules_version, meta.search_version, meta.reward_definition, 0, 0,
        )
    return header + payload


def load_attention_weights_bin(path: Path, device: torch.device | str = "cpu",
                               return_metadata: bool = False):
    data = path.read_bytes()
    if len(data) < ATTN_HEADER_BYTES_LEGACY or data[:8] != ATTN_MAGIC:
        raise ValueError(f"Invalid attn_weights.bin magic or truncated file: {path}")
    fmt_ver, hdr_bytes = struct.unpack_from("<2I", data, 8)

    if fmt_ver == ATTN_FORMAT_VERSION_LEGACY:
        if hdr_bytes != ATTN_HEADER_BYTES_LEGACY or len(data) != ATTN_WEIGHT_BYTES_LEGACY:
            raise ValueError(f"Invalid v1 attn_weights.bin header or size: {path}")
        words = struct.unpack_from("<18I", data, 8)
        payload = data[ATTN_HEADER_BYTES_LEGACY:]
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        expected_words = (
            ATTN_FORMAT_VERSION_LEGACY, ATTN_HEADER_BYTES_LEGACY, ATTN_VOCAB, ATTN_CARD_EMBED, ATTN_ATTR_DIM,
            ATTN_GLOBAL_DIM_V2, ATTN_MODEL_DIM, ATTN_HEADS, ATTN_FFN_DIM, ATTN_LAYERS,
            ATTN_CANDIDATE_DIM, ATTN_VALUE_HIDDEN, ATTN_POLICY_HIDDEN, ATTN_MAX_TOKENS,
            ATTN_ACTIVATION_RELU, ATTN_PARAM_COUNT_V2, crc, 0,
        )
        if words != expected_words:
            raise ValueError(f"attn_weights.bin v1 header or CRC mismatch in {path}")
        meta = AttentionMetadata.for_variant("v2", search_version=1)
    elif fmt_ver == ATTN_FORMAT_VERSION_V3:
        if hdr_bytes != ATTN_HEADER_BYTES_V3 or len(data) < ATTN_HEADER_BYTES_V3:
            raise ValueError(f"Invalid v3 attn_weights.bin header size: {path}")
        w = struct.unpack_from("<26I", data, 8)
        if (w[2], w[3], w[4], w[6], w[7], w[8], w[9], w[11], w[12], w[13], w[14]) != (
            ATTN_VOCAB, ATTN_CARD_EMBED, ATTN_ATTR_DIM, ATTN_MODEL_DIM, ATTN_HEADS,
            ATTN_FFN_DIM, ATTN_LAYERS, ATTN_VALUE_HIDDEN, ATTN_POLICY_HIDDEN, ATTN_MAX_TOKENS, ATTN_ACTIVATION_RELU
        ):
            raise ValueError(f"attn_weights.bin v3 fixed architecture field mismatch in {path}")
        meta = AttentionMetadata(
            model_version=w[17],
            global_dim=w[5],
            candidate_dim=w[10],
            candidate_embdiff_dim=w[18],
            token_schema_version=w[19],
            card_vocab_hash=w[20],
            rules_version=w[21],
            search_version=w[22],
            reward_definition=w[23],
        )
        meta.validate()
        expected_params = meta.parameter_count
        if w[15] != expected_params or len(data) != ATTN_HEADER_BYTES_V3 + 4 * expected_params:
            raise ValueError(f"attn_weights.bin v3 parameter count or file size mismatch in {path}")
        payload = data[ATTN_HEADER_BYTES_V3:]
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        if w[16] != crc:
            raise ValueError(f"attn_weights.bin v3 checksum mismatch in {path}")
    else:
        raise ValueError(f"Unsupported attn_weights.bin format version {fmt_ver} in {path}")

    floats = np.frombuffer(payload, dtype="<f4").astype(np.float32, copy=True)
    if not np.isfinite(floats).all():
        raise ValueError(f"attn_weights.bin contains non-finite values: {path}")
    model = AttentionModel(
        global_dim=meta.global_dim,
        candidate_dim=meta.candidate_dim,
        candidate_embdiff_dim=meta.candidate_embdiff_dim,
    )
    state = model.state_dict()
    cursor = 0
    for key in ATTN_PARAMETERS:
        target = state[key]
        count = target.numel()
        state[key] = torch.from_numpy(floats[cursor:cursor + count].reshape(target.shape))
        cursor += count
    model.load_state_dict(state)
    model = model.to(device)
    return (model, meta) if return_metadata else model


def validate_attention_weights_file(path: Path) -> AttentionMetadata:
    _, meta = load_attention_weights_bin(path, device="cpu", return_metadata=True)
    return meta


def save_attention_model(model: AttentionModel, directory: Path, generation: int, optimizer=None,
                         metadata: AttentionMetadata | None = None):
    directory.mkdir(parents=True, exist_ok=True)
    with torch.no_grad():
        model.card_embed.weight[0].zero_()
    meta = metadata or metadata_for_model(model, search_version=2)
    meta.validate()
    state = model.state_dict()
    binary = export_attention_weights(model, metadata=meta)
    expected_bytes = ATTN_HEADER_BYTES_V3 + 4 * meta.parameter_count
    if len(binary) != expected_bytes:
        raise ValueError("Unexpected Attention weight layout")
    attn_tmp = directory / "attn_weights.bin.tmp"
    weights_tmp = directory / "weights.bin.tmp"
    checkpoint_tmp = directory / "model.pt.tmp"
    attn_tmp.write_bytes(binary)
    weights_tmp.write_bytes(binary)
    torch.save({
        "generation": generation,
        "model_type": "attention",
        "metadata": asdict(meta),
        "model": state,
        "optimizer": optimizer.state_dict() if optimizer is not None else None,
    }, checkpoint_tmp)
    replace_file(checkpoint_tmp, directory / "model.pt")
    replace_file(attn_tmp, directory / "attn_weights.bin")
    replace_file(weights_tmp, directory / "weights.bin")


def initialize_attention_checkpoint(directory: Path, seed: int, variant: str = "v3-gd"):
    meta = AttentionMetadata.for_variant(variant, search_version=2)
    torch.manual_seed(seed)
    model = AttentionModel(
        global_dim=meta.global_dim,
        candidate_dim=meta.candidate_dim,
        candidate_embdiff_dim=meta.candidate_embdiff_dim,
    )
    nn.init.normal_(model.card_embed.weight, mean=0.0, std=0.2)
    with torch.no_grad():
        model.card_embed.weight[0].zero_()
    nn.init.normal_(model.cls_embed, mean=0.0, std=0.05)
    for module in model.modules():
        if isinstance(module, nn.Linear):
            nn.init.xavier_uniform_(module.weight)
            nn.init.zeros_(module.bias)
    save_attention_model(model, directory, 0, optimizer=None, metadata=meta)


def load_attention_checkpoint(path: Path, device: torch.device):
    checkpoint = torch.load(path, map_location=device, weights_only=True)
    state = checkpoint["model"]
    if "metadata" in checkpoint and checkpoint["metadata"] is not None:
        meta = AttentionMetadata(**checkpoint["metadata"])
        meta.validate()
    else:
        g_dim = int(state["global_proj.weight"].shape[1])
        pol_in = int(state["policy_fc1.weight"].shape[1])
        d_dim = pol_in - ATTN_MODEL_DIM - ATTN_CANDIDATE_DIM
        meta = AttentionMetadata(
            model_version=1 if g_dim == 32 else 3,
            global_dim=g_dim,
            candidate_dim=ATTN_CANDIDATE_DIM,
            candidate_embdiff_dim=d_dim,
            token_schema_version=1 if g_dim == 32 else 3,
        )
        meta.validate()
    model = AttentionModel(
        global_dim=meta.global_dim,
        candidate_dim=meta.candidate_dim,
        candidate_embdiff_dim=meta.candidate_embdiff_dim,
    ).to(device)
    model.load_state_dict(state)
    return model, checkpoint, meta


def migrate_attention_v2_to_v3(source_path: Path, output_dir: Path | None = None,
                               target_variant: str = "v3-gd", generation: int = 0) -> tuple[AttentionModel, AttentionMetadata]:
    if source_path.is_dir():
        bin_candidate = source_path / "attn_weights.bin"
        pt_candidate = source_path / "model.pt"
        source_path = bin_candidate if bin_candidate.is_file() else pt_candidate

    if source_path.suffix == ".bin":
        v2_model, src_meta = load_attention_weights_bin(source_path, device="cpu", return_metadata=True)
    else:
        v2_model, _, src_meta = load_attention_checkpoint(source_path, torch.device("cpu"))

    if src_meta.variant != "v2":
        raise ValueError(f"migrate_attention_v2_to_v3 expects a v2 (32,64,0) model, got {src_meta.variant}")
    if src_meta.card_vocab_hash not in (0, ATTN_CARD_VOCAB_HASH):
        raise ValueError("card_vocab_hash mismatch during v2 -> v3 migration")

    dst_meta = AttentionMetadata.for_variant(target_variant, search_version=2)
    dst_model = AttentionModel(
        global_dim=dst_meta.global_dim,
        candidate_dim=dst_meta.candidate_dim,
        candidate_embdiff_dim=dst_meta.candidate_embdiff_dim,
    )
    src_sd = v2_model.state_dict()
    dst_sd = dst_model.state_dict()

    with torch.no_grad():
        for key in ATTN_PARAMETERS:
            if key == "global_proj.weight":
                if dst_meta.global_dim == ATTN_GLOBAL_DIM_V2:
                    dst_sd[key].copy_(src_sd[key])
                else:
                    dst_sd[key].zero_()
                    for s_col, d_col in GLOBAL_V2_TO_V3_MAP:
                        dst_sd[key][:, d_col].copy_(src_sd[key][:, s_col])
                    # v2[27] = (opp.ep + opp.sep) / 4.0f -> 0.5f * v3[37] + 0.5f * v3[38]
                    dst_sd[key][:, 37].copy_(0.5 * src_sd[key][:, 27])
                    dst_sd[key][:, 38].copy_(0.5 * src_sd[key][:, 27])
            elif key == "policy_fc1.weight":
                dst_sd[key].zero_()
                dst_sd[key][:, :ATTN_POLICY_INPUT_V2].copy_(src_sd[key])
            else:
                dst_sd[key].copy_(src_sd[key])
        dst_sd["card_embed.weight"][0].zero_()

    dst_model.load_state_dict(dst_sd)
    if output_dir is not None:
        # Optimizer state is intentionally reset (None) for fair warm-start across B40/C40/D40.
        save_attention_model(dst_model, output_dir, generation, optimizer=None, metadata=dst_meta)
    return dst_model, dst_meta


def make_attention_reference(path: Path, replay: AttentionReplay, generation: int, seed: int):
    turns = len(replay.targets)
    if turns < 2:
        raise ValueError("A fixed holdout requires at least two replay turns")
    chosen = np.random.default_rng(seed).choice(turns, size=min(1024, max(1, turns // 10)), replace=False)
    starts, ends = replay.offsets[chosen, None], replay.offsets[chosen + 1, None]
    indices = starts + np.arange(MAX_CANDIDATES)
    valid = indices < ends
    safe_idx = np.where(valid, indices, 0)
    policy = replay.policies[safe_idx, :ATTN_CANDIDATE_DIM]
    payload = dict(
        generation=generation, heldout=chosen,
        global_feat=replay.global_feat[chosen], vocab=replay.vocab[chosen],
        attrs=replay.attrs[chosen], targets=replay.targets[chosen],
        policies=policy, valid=valid,
    )
    if replay.diff_ids is not None and replay.diff_signs is not None:
        payload["diff_ids"] = replay.diff_ids[safe_idx]
        payload["diff_signs"] = replay.diff_signs[safe_idx]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as stream:
        np.savez_compressed(stream, **payload)


@torch.inference_mode()
def measure_attention_drift_and_calibration(model: AttentionModel, previous: AttentionModel, reference, device):
    value_total = torch.zeros((), device=device)
    policy_total = torch.zeros((), device=device)
    all_probs = []
    size = len(reference["global_feat"])
    has_diff = "diff_ids" in reference and model.candidate_embdiff_dim > 0
    for start in range(0, size, 256):
        batch = slice(start, start + 256)
        g = torch.as_tensor(reference["global_feat"][batch], device=device)
        v = torch.as_tensor(reference["vocab"][batch], device=device)
        a = torch.as_tensor(reference["attrs"][batch], device=device)
        policies = torch.as_tensor(reference["policies"][batch], device=device)
        valid = torch.as_tensor(reference["valid"][batch], device=device)
        d_ids = torch.as_tensor(reference["diff_ids"][batch], device=device) if has_diff else None
        d_signs = torch.as_tensor(reference["diff_signs"][batch], device=device) if has_diff else None

        new_val, new_pol = model(g, v, a, policies, d_ids, d_signs)
        old_val, old_pol = previous(g, v, a, policies, d_ids, d_signs)
        new_prob = new_val.sigmoid()
        all_probs.append(new_prob.detach().cpu().numpy())
        value_delta = (new_prob - old_val.sigmoid()).abs()
        new_logit = new_pol.masked_fill(~valid, -torch.inf)
        old_logit = old_pol.masked_fill(~valid, -torch.inf)
        value_total += value_delta.sum()
        policy_total += 0.5 * (new_logit.softmax(-1) - old_logit.softmax(-1)).abs().sum()

    brier = None
    ece = None
    if "targets" in reference:
        probs_np = np.concatenate(all_probs)
        raw_targets = reference["targets"].astype(np.float64)
        brier = float(np.mean((probs_np - raw_targets) ** 2))
        bins = np.linspace(0.0, 1.0, 11)
        ece_acc = 0.0
        for b_idx in range(10):
            lo, hi = bins[b_idx], bins[b_idx + 1]
            mask = (probs_np >= lo) & (probs_np <= hi if b_idx == 9 else probs_np < hi)
            if np.any(mask):
                ece_acc += (np.sum(mask) / len(probs_np)) * abs(float(np.mean(probs_np[mask]) - np.mean(raw_targets[mask])))
        ece = float(ece_acc)
    return float(value_total / size), float(policy_total / size), brier, ece


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

    previous, checkpoint, prev_meta = load_attention_checkpoint(previous_path, device)
    if checkpoint["generation"] != generation - 1:
        raise ValueError("The checkpoint must be from the preceding generation")

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
    replays = [read_attention_replay(path, expected_meta=prev_meta) for _, path in replay_dirs]
    if not reference_path.exists():
        make_attention_reference(reference_path, replays[0], replay_dirs[0][0], seed)
    with np.load(reference_path) as source:
        reference = {name: source[name] for name in source.files}

    use_embdiff = prev_meta.candidate_embdiff_dim > 0
    size_bytes = sum(
        r.global_feat.nbytes + r.vocab.nbytes + r.attrs.nbytes + r.targets.nbytes + r.policies.nbytes + r.offsets.nbytes
        + (r.diff_ids.nbytes + r.diff_signs.nbytes if r.diff_ids is not None else 0)
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
    diff_ids_all = torch.as_tensor(np.concatenate([r.diff_ids for r in replays]), device=storage) if use_embdiff else None
    diff_signs_all = torch.as_tensor(np.concatenate([r.diff_signs for r in replays]), device=storage) if use_embdiff else None
    both_lose_turn_rate = float((val_targets == 0.5).float().mean().item())

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

    model = AttentionModel(
        dropout=dropout,
        global_dim=prev_meta.global_dim,
        candidate_dim=prev_meta.candidate_dim,
        candidate_embdiff_dim=prev_meta.candidate_embdiff_dim,
    ).to(device)
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
            safe_idx = torch.where(valid, indices, 0)
            policy = policies[safe_idx].to(device)
            d_ids = diff_ids_all[safe_idx].to(device) if use_embdiff else None
            d_signs = diff_signs_all[safe_idx].to(device) if use_embdiff else None
            valid = valid.to(device)

            value_logits, policy_logits = model(
                g_batch, v_batch, a_batch, policy[:, :, :ATTN_CANDIDATE_DIM], d_ids, d_signs
            )
            value_loss = F.binary_cross_entropy_with_logits(value_logits, t_batch)
            targets = policy[:, :, -1].masked_fill(~valid, 0.0)
            policy_loss = -(targets * masked_log_softmax(policy_logits, valid)).sum(-1).mean()
            optimizer.zero_grad(set_to_none=True)
            (value_loss + policy_loss).backward()
            with torch.no_grad():
                model.card_embed.weight[0].zero_()
            optimizer.step()
            with torch.no_grad():
                model.card_embed.weight[0].zero_()
            totals += torch.stack((value_loss.detach(), policy_loss.detach())) * len(turn_ids)

    val_loss, pol_loss = (totals / (epochs * turns.numel())).tolist()
    delta_v, delta_pi, brier, ece = measure_attention_drift_and_calibration(
        model.eval(), previous.eval(), reference, device
    )
    if not all(math.isfinite(x) for x in (val_loss, pol_loss, delta_v, delta_pi)):
        raise RuntimeError("Training produced non-finite metrics")
    out_meta = AttentionMetadata.for_variant(prev_meta.variant, search_version=2)
    save_attention_model(model, output, generation, optimizer, metadata=out_meta)
    metrics = {
        "val_loss": val_loss, "pol_loss": pol_loss, "delta_v": delta_v, "delta_pi": delta_pi,
        "brier_score": brier, "ece_10bin": ece, "both_lose_turn_rate": both_lose_turn_rate,
        "training_turns": turns.numel(), "reference_turns": len(reference["global_feat"]),
        "learning_rate": optimizer.param_groups[0]["lr"],
        "weight_decay": optimizer.param_groups[0]["weight_decay"],
        "dropout": dropout,
        "value_label_smoothing": value_label_smoothing,
        "replay_generations": [number for number, _ in replay_dirs],
        "device": torch.cuda.get_device_name(device) if device.type == "cuda" else "CPU",
        "model_type": "attention",
        "attention_variant": out_meta.variant,
        "parameter_count": out_meta.parameter_count,
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
    parser.add_argument("replay_root", type=Path, nargs="?")
    parser.add_argument("--migrate-v2", type=Path, help="Migrate a Legacy v2 checkpoint/weights to v3")
    parser.add_argument("--attention-variant", choices=("v2", "v3-g", "v3-gd"), default="v3-gd")
    parser.add_argument("--generation", type=int)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--previous", type=Path)
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
    if args.migrate_v2 is not None:
        _, meta = migrate_attention_v2_to_v3(
            args.migrate_v2, args.output_dir, target_variant=args.attention_variant,
            generation=args.generation or 0,
        )
        print(json.dumps({"migrated": str(args.output_dir), "variant": meta.variant,
                          "parameter_count": meta.parameter_count}, indent=2))
        return
    if args.replay_root is None or args.generation is None or args.previous is None:
        parser.error("replay_root, --generation, and --previous are required unless --migrate-v2 is specified")
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
