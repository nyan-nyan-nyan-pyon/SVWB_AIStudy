#pragma once

// Small card-token Transformer used as an alternative to the 256->64->32 MLP.
// Supports Legacy v2 (32,64,0), v3-G (64,64,0), and v3-GD (64,64,160) with
// strict metadata validation and warm-start weight migration.

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace svwb::attn {

inline constexpr int kVocab = 600;          // 0 = empty slot, cards 1..587, reserved up to 599
inline constexpr int kCardEmbed = 32;
inline constexpr int kAttrDim = 32;
inline constexpr int kTokenInput = kCardEmbed + kAttrDim;
inline constexpr int kGlobalDimV2 = 32;
inline constexpr int kGlobalDim = 64;
inline constexpr int kModel = 128;
inline constexpr int kHeads = 4;
inline constexpr int kHeadDim = kModel / kHeads;
inline constexpr int kFfn = 512;
inline constexpr int kLayers = 2;
inline constexpr int kCandidateDim = 64;
inline constexpr int kDiffZones = 5;
inline constexpr int kCandidateEmbDiffDim = kDiffZones * kCardEmbed; // 160
inline constexpr int kValueHidden = 64;
inline constexpr int kPolicyHidden = 128;
inline constexpr int kPolicyInputV2 = kModel + kCandidateDim;                           // 192
inline constexpr int kPolicyInput = kModel + kCandidateDim + kCandidateEmbDiffDim;      // 352

inline constexpr int kHandTokens = 9;
inline constexpr int kBoardTokens = 5;
inline constexpr int kCrestTokens = 10;
inline constexpr int kHandOffset = 0;
inline constexpr int kOwnBoardOffset = kHandOffset + kHandTokens;
inline constexpr int kEnemyBoardOffset = kOwnBoardOffset + kBoardTokens;
inline constexpr int kCrestOffset = kEnemyBoardOffset + kBoardTokens;
inline constexpr int kCardTokens = kCrestOffset + kCrestTokens; // 29
inline constexpr int kMaxTokens = kCardTokens + 1;              // + CLS

inline constexpr std::array<int, kDiffZones> kZoneMaxDiff{18, 10, 10, 10, 10};
inline constexpr std::array<int, kDiffZones + 1> kZoneDiffSliceOffsets{0, 18, 28, 38, 48, 58};
inline constexpr int kMaxSparseDiffEntries = 2 * kCardTokens; // 58

inline constexpr std::uint32_t kFormatVersionLegacy = 1;
inline constexpr std::uint32_t kFormatVersionV3 = 2;
inline constexpr std::uint32_t kActivationRelu = 1;
inline constexpr std::uint32_t kCardVocabHash = 0x6680F613U;
inline constexpr std::uint32_t kRulesVersion = 1;
inline constexpr std::uint32_t kRewardWinHalfBothLose = 1;

// Parameter counts in PyTorch layout, in the order written to attn_weights.bin.
inline constexpr std::size_t kLayerParameters =
    2 * kModel + (3 * kModel * kModel + 3 * kModel) + (kModel * kModel + kModel) + 2 * kModel +
    (kModel * kFfn + kFfn) + (kFfn * kModel + kModel);

constexpr std::size_t compute_parameter_count(int global_dim, int candidate_dim, int embdiff_dim) {
    const std::size_t policy_in = static_cast<std::size_t>(kModel + candidate_dim + embdiff_dim);
    return static_cast<std::size_t>(kVocab) * kCardEmbed +
           static_cast<std::size_t>(kTokenInput) * kModel + kModel +
           static_cast<std::size_t>(global_dim) * kModel + kModel +
           kModel +
           kLayers * kLayerParameters +
           2 * kModel +
           (static_cast<std::size_t>(kModel) * kValueHidden + kValueHidden) + (kValueHidden + 1) +
           (policy_in * kPolicyHidden + kPolicyHidden) + (kPolicyHidden + 1);
}

inline constexpr std::size_t kParameterCountV2 = compute_parameter_count(kGlobalDimV2, kCandidateDim, 0);
inline constexpr std::size_t kParameterCountV3G = compute_parameter_count(kGlobalDim, kCandidateDim, 0);
inline constexpr std::size_t kParameterCountV3GD = compute_parameter_count(kGlobalDim, kCandidateDim, kCandidateEmbDiffDim);
inline constexpr std::size_t kParameterCount = kParameterCountV3GD;

static_assert(kParameterCountV2 == 461'826);
static_assert(kParameterCountV3G == 465'922);
static_assert(kParameterCountV3GD == 486'402);

enum class ModelType : std::uint8_t { Mlp, Attention };

enum class ArchVariant : std::uint8_t {
    LegacyV2,        // global=32, cand=64, embdiff=0
    V3Global,        // global=64, cand=64, embdiff=0
    V3GlobalEmbDiff, // global=64, cand=64, embdiff=160
};

enum class PolicyScorerKernel : std::uint8_t {
    PreprojectedTable, // Method B: projected_emb[5][600][128] vector addition
    DirectMatMul,      // Method A: materialize 160-dim diff + AVX2 matmul
};

struct ModelMetadata {
    std::uint32_t model_version = 3;
    std::uint32_t global_dim = kGlobalDim;
    std::uint32_t candidate_dim = kCandidateDim;
    std::uint32_t candidate_embdiff_dim = kCandidateEmbDiffDim;
    std::uint32_t token_schema_version = 3;
    std::uint32_t card_vocab_hash = kCardVocabHash;
    std::uint32_t rules_version = kRulesVersion;
    std::uint32_t search_version = 2;
    std::uint32_t reward_definition = kRewardWinHalfBothLose;

    [[nodiscard]] static ModelMetadata for_variant(ArchVariant v, std::uint32_t search_ver = 2);
    [[nodiscard]] ArchVariant validate_and_resolve() const;
    [[nodiscard]] std::size_t parameter_count() const;

    bool operator==(const ModelMetadata&) const = default;
};

// Observation of one position from the acting player's side. Fixed size, trivially copyable.
// Slots: hand [0,9), own board [9,14), enemy board [14,19), crests/faiths [19,29).
struct Tokens {
    std::array<float, kGlobalDim> global;
    std::array<std::int32_t, kCardTokens> vocab; // 0 marks an empty slot (masked out)
    std::array<float, kCardTokens * kAttrDim> attrs;

    bool operator==(const Tokens&) const = default;
};
static_assert(sizeof(Tokens) == (kGlobalDim + kCardTokens + kCardTokens * kAttrDim) * sizeof(float));

using CandidateFeatures = std::array<float, kCandidateDim>;
using CandidateEmbDiff = std::array<float, kCandidateEmbDiffDim>;

struct SparseDiffPair {
    std::uint16_t vocab_id = 0;
    std::int8_t signed_count = 0;
    std::uint8_t pad = 0;

    bool operator==(const SparseDiffPair&) const = default;
};
static_assert(sizeof(SparseDiffPair) == 4);

struct CandidateSparseDiff {
    std::array<std::uint8_t, kDiffZones + 1> zone_offsets{};
    std::array<SparseDiffPair, kMaxSparseDiffEntries> entries{};

    [[nodiscard]] int total_entries() const noexcept { return zone_offsets[kDiffZones]; }
    bool operator==(const CandidateSparseDiff&) const = default;
};

// Root encoding reused by every candidate scored against the same position.
struct RootContext {
    alignas(32) std::array<float, kModel> cls;
    alignas(32) std::array<float, kPolicyHidden> policy_base; // policy_fc1(cls part) + bias
};

struct DebugStageOutputs {
    std::array<float, kModel> global_cls_init{};
    std::array<float, kCardTokens * kModel> token_proj{};
    std::array<float, kModel> final_cls{};
    float value_logit = 0.0F;
    float value_prob = 0.5F;
};

class AttentionModel {
public:
    [[nodiscard]] static AttentionModel load(const std::filesystem::path& file);
    [[nodiscard]] static AttentionModel random(std::uint32_t seed,
                                               ArchVariant variant = ArchVariant::V3GlobalEmbDiff);
    [[nodiscard]] static AttentionModel from_parameters(
        std::vector<float> ordered,
        ModelMetadata meta = ModelMetadata::for_variant(ArchVariant::V3GlobalEmbDiff));
    [[nodiscard]] static AttentionModel migrate_v2_to_v3(
        const AttentionModel& v2_model,
        ArchVariant target = ArchVariant::V3GlobalEmbDiff);
    void save(const std::filesystem::path& file, bool legacy_format = false) const;

    [[nodiscard]] const ModelMetadata& metadata() const noexcept { return meta_; }
    [[nodiscard]] ArchVariant variant() const noexcept { return variant_; }
    [[nodiscard]] int global_dim() const noexcept { return static_cast<int>(meta_.global_dim); }
    [[nodiscard]] int candidate_embdiff_dim() const noexcept { return static_cast<int>(meta_.candidate_embdiff_dim); }
    [[nodiscard]] PolicyScorerKernel scorer_kernel() const noexcept { return kernel_; }
    void set_scorer_kernel(PolicyScorerKernel kernel) noexcept { kernel_ = kernel; }

    // Win probability of the acting player.
    [[nodiscard]] float value(const Tokens& tokens) const;
    // Raw value logit before the sigmoid.
    [[nodiscard]] float value_logit(const Tokens& tokens) const;
    // Value probability, plus the CLS encoding and policy precomputation for candidate scoring.
    [[nodiscard]] float evaluate_root(const Tokens& tokens, RootContext& context) const;

    [[nodiscard]] CandidateEmbDiff materialize_embdiff(const CandidateSparseDiff& diff) const;
    [[nodiscard]] float policy_logit(const RootContext& context, const float* candidate,
                                     const CandidateSparseDiff* diff = nullptr) const;
    [[nodiscard]] float policy_logit_direct(const RootContext& context, const float* candidate,
                                            const CandidateSparseDiff* diff = nullptr) const;
    [[nodiscard]] float policy_logit_preprojected(const RootContext& context, const float* candidate,
                                                  const CandidateSparseDiff* diff = nullptr) const;

    void debug_encode(const Tokens& tokens, DebugStageOutputs& out) const;
    void debug_policy(const RootContext& context, const float* candidate, const CandidateSparseDiff* diff,
                      CandidateEmbDiff& dense_diff, std::array<float, kPolicyHidden>& h1_relu,
                      float& out_logit) const;

    [[nodiscard]] const std::vector<float>& parameters() const noexcept { return ordered_; }

private:
    AttentionModel() = default;
    void prepare();
    float encode(const Tokens& tokens, float* cls, DebugStageOutputs* debug = nullptr) const;

    ModelMetadata meta_{};
    ArchVariant variant_ = ArchVariant::V3GlobalEmbDiff;
    PolicyScorerKernel kernel_ = PolicyScorerKernel::PreprojectedTable;
    std::vector<float> ordered_; // PyTorch layout, as stored in the file
    std::vector<float> packed_;  // inference layout (input-major weights + preprojected tables)
    struct Offsets {
        std::size_t token_table;   // [vocab][model]: embedding part of token_proj plus its bias (row 0 = bias only)
        std::size_t attr_weight;   // [attr][model]
        std::size_t global_weight; // [global_dim][model]
        std::size_t global_bias;   // includes the CLS embedding
        struct Layer {
            std::size_t norm1_w, norm1_b, qkv_w, qkv_b, out_w, out_b, norm2_w, norm2_b, ffn1_w, ffn1_b, ffn2_w, ffn2_b;
        };
        std::array<Layer, kLayers> layers;
        std::size_t final_w, final_b;
        std::size_t value1_w, value1_b, value2_w, value2_b;
        std::size_t policy_cls_w, policy_candidate_w, policy_embdiff_w;
        std::size_t projected_emb; // [5][vocab][policy_hidden] when candidate_embdiff_dim == 160
        std::size_t policy1_b, policy2_w, policy2_b;
    } offsets_{};
};

[[nodiscard]] std::uint32_t crc32(const void* data, std::size_t bytes, std::uint32_t crc = 0);
[[nodiscard]] bool is_attention_weights_file(const std::filesystem::path& file);

} // namespace svwb::attn
