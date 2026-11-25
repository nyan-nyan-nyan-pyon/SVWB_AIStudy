#pragma once

#include "attention.hpp"
#include "mlp.hpp"

#include <bit>
#include <cassert>
#include <cstring>
#include <fstream>
#include <span>
#include <stdexcept>
#include <vector>

namespace svwb {

struct TrainingSample {
    Features features;
    float target;
};

static_assert(sizeof(TrainingSample) == (kFeatureDim + 1) * sizeof(float));

struct GameLog {
    std::vector<TrainingSample> values;
    std::vector<TrainingSample> policies;
    std::vector<std::int32_t> policy_sizes;
};

struct AttentionValueSample {
    attn::Tokens tokens;
    float target;
};

static_assert(sizeof(AttentionValueSample) == sizeof(attn::Tokens) + sizeof(float));

struct AttentionPolicyRecord {
    attn::CandidateFeatures features;
    attn::CandidateSparseDiff emb_diff;
    float target;
};

// Packed on-disk v3 policy record (440 bytes, 4-byte aligned, zone-sliced sparse diff without truncation).
struct AttentionPolicyDiskV3 {
    attn::CandidateFeatures features{};
    std::array<std::uint16_t, attn::kMaxSparseDiffEntries> diff_ids{};
    std::array<std::int8_t, attn::kMaxSparseDiffEntries> diff_signs{};
    std::array<std::uint8_t, attn::kDiffZones + 1> zone_counts{};
    float target = 0.0F;
};
static_assert(sizeof(AttentionPolicyDiskV3) == 440);

struct AttentionPolicyDiskV2 {
    attn::CandidateFeatures features{};
    float target = 0.0F;
};
static_assert(sizeof(AttentionPolicyDiskV2) == (attn::kCandidateDim + 1) * sizeof(float));

struct AttentionGameLog {
    std::vector<AttentionValueSample> values;
    std::vector<AttentionPolicyRecord> policies;
    std::vector<std::int32_t> policy_sizes;
};

inline constexpr char kAttnValLogMagic[8] = {'S', 'V', 'W', 'B', 'A', 'V', 'A', 'L'};
inline constexpr char kAttnPolLogMagic[8] = {'S', 'V', 'W', 'B', 'A', 'P', 'O', 'L'};
inline constexpr std::uint32_t kAttnLogFormatVersion = 3;
inline constexpr std::uint32_t kAttnLogHeaderWords = 15;
inline constexpr std::uint32_t kAttnLogHeaderBytes = 8 + 4 * kAttnLogHeaderWords; // 68 bytes

inline void write_word(std::ofstream& output, std::uint32_t word) {
    std::array<unsigned char, 4> bytes{};
    for (unsigned i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<unsigned char>(word >> (8 * i));
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

inline void write_samples(std::ofstream& output, std::span<const TrainingSample> samples) {
    if constexpr (std::endian::native == std::endian::little) {
        output.write(reinterpret_cast<const char*>(samples.data()),
                     static_cast<std::streamsize>(samples.size_bytes()));
    } else {
        for (const auto& sample : samples) {
            for (const float feature : sample.features) {
                write_word(output, std::bit_cast<std::uint32_t>(feature));
            }
            write_word(output, std::bit_cast<std::uint32_t>(sample.target));
        }
    }
}

class TrainingLogs {
public:
    explicit TrainingLogs(const std::filesystem::path& directory) {
        std::filesystem::create_directories(directory);
        values_.exceptions(std::ios::badbit | std::ios::failbit);
        policies_.exceptions(std::ios::badbit | std::ios::failbit);
        values_.open(directory / "val_log.bin", std::ios::binary | std::ios::trunc);
        policies_.open(directory / "pol_log.bin", std::ios::binary | std::ios::trunc);
    }

    // The caller holds the match output lock so a whole game's two streams stay in order.
    void append(const GameLog& log) {
        write_samples(values_, log.values);
        std::span<const TrainingSample> remaining(log.policies);
        for (const auto size : log.policy_sizes) {
            write_word(policies_, static_cast<std::uint32_t>(size));
            write_samples(policies_, remaining.first(size));
            remaining = remaining.subspan(size);
        }
    }

    void finish() {
        values_.close();
        policies_.close();
    }

private:
    std::ofstream values_;
    std::ofstream policies_;
};

inline AttentionPolicyDiskV3 pack_policy_disk_v3(const AttentionPolicyRecord& rec) {
    AttentionPolicyDiskV3 disk{};
    disk.features = rec.features;
    disk.target = rec.target;
    int total = 0;
    for (int z = 0; z < attn::kDiffZones; ++z) {
        const int begin = rec.emb_diff.zone_offsets[z];
        const int end = rec.emb_diff.zone_offsets[z + 1];
        const int z_count = end - begin;
        assert(z_count >= 0 && z_count <= attn::kZoneMaxDiff[z]);
        if (z_count < 0 || z_count > attn::kZoneMaxDiff[z]) {
            throw std::logic_error("CandidateSparseDiff exceeded zone capacity");
        }
        disk.zone_counts[z] = static_cast<std::uint8_t>(z_count);
        const int dst_base = attn::kZoneDiffSliceOffsets[z];
        for (int i = 0; i < z_count; ++i) {
            const auto entry = rec.emb_diff.entries[begin + i];
            assert(entry.vocab_id > 0 && entry.vocab_id < attn::kVocab && entry.signed_count != 0);
            disk.diff_ids[dst_base + i] = entry.vocab_id;
            disk.diff_signs[dst_base + i] = entry.signed_count;
        }
        total += z_count;
    }
    assert(total <= attn::kMaxSparseDiffEntries);
    disk.zone_counts[attn::kDiffZones] = static_cast<std::uint8_t>(total);
    return disk;
}

class AttentionTrainingLogs {
public:
    explicit AttentionTrainingLogs(
        const std::filesystem::path& directory,
        attn::ModelMetadata meta = attn::ModelMetadata::for_variant(attn::ArchVariant::V3GlobalEmbDiff))
        : meta_(meta) {
        static_cast<void>(meta_.validate_and_resolve());
        std::filesystem::create_directories(directory);
        values_.exceptions(std::ios::badbit | std::ios::failbit);
        policies_.exceptions(std::ios::badbit | std::ios::failbit);
        values_.open(directory / "val_log.bin", std::ios::binary | std::ios::trunc);
        policies_.open(directory / "pol_log.bin", std::ios::binary | std::ios::trunc);
        write_header(values_, kAttnValLogMagic);
        write_header(policies_, kAttnPolLogMagic);
    }

    void append(const AttentionGameLog& log) {
        const int g_dim = static_cast<int>(meta_.global_dim);
        for (const auto& sample : log.values) {
            if constexpr (std::endian::native == std::endian::little) {
                values_.write(reinterpret_cast<const char*>(sample.tokens.global.data()),
                              static_cast<std::streamsize>(g_dim * sizeof(float)));
                values_.write(reinterpret_cast<const char*>(sample.tokens.vocab.data()),
                              static_cast<std::streamsize>(sizeof(sample.tokens.vocab)));
                values_.write(reinterpret_cast<const char*>(sample.tokens.attrs.data()),
                              static_cast<std::streamsize>(sizeof(sample.tokens.attrs)));
                values_.write(reinterpret_cast<const char*>(&sample.target), sizeof(float));
            } else {
                for (int i = 0; i < g_dim; ++i) {
                    write_word(values_, std::bit_cast<std::uint32_t>(sample.tokens.global[i]));
                }
                for (const auto id : sample.tokens.vocab) {
                    write_word(values_, static_cast<std::uint32_t>(id));
                }
                for (const float attr : sample.tokens.attrs) {
                    write_word(values_, std::bit_cast<std::uint32_t>(attr));
                }
                write_word(values_, std::bit_cast<std::uint32_t>(sample.target));
            }
        }

        std::span<const AttentionPolicyRecord> remaining(log.policies);
        for (const auto size : log.policy_sizes) {
            write_word(policies_, static_cast<std::uint32_t>(size));
            const auto group = remaining.first(size);
            if (meta_.candidate_embdiff_dim > 0) {
                for (const auto& rec : group) {
                    const auto disk = pack_policy_disk_v3(rec);
                    policies_.write(reinterpret_cast<const char*>(&disk), sizeof(disk));
                }
            } else {
                for (const auto& rec : group) {
                    const AttentionPolicyDiskV2 disk{rec.features, rec.target};
                    policies_.write(reinterpret_cast<const char*>(&disk), sizeof(disk));
                }
            }
            remaining = remaining.subspan(size);
        }
    }

    void finish() {
        values_.close();
        policies_.close();
    }

private:
    void write_header(std::ofstream& out, const char (&magic)[8]) {
        out.write(magic, 8);
        const std::array<std::uint32_t, kAttnLogHeaderWords> words{
            kAttnLogFormatVersion,
            kAttnLogHeaderBytes,
            meta_.model_version,
            meta_.global_dim,
            attn::kVocab,
            attn::kCardTokens,
            attn::kAttrDim,
            meta_.candidate_dim,
            meta_.candidate_embdiff_dim,
            meta_.token_schema_version,
            meta_.card_vocab_hash,
            meta_.rules_version,
            meta_.search_version,
            meta_.reward_definition,
            0U,
        };
        for (const auto word : words) write_word(out, word);
    }

    attn::ModelMetadata meta_;
    std::ofstream values_;
    std::ofstream policies_;
};

} // namespace svwb
