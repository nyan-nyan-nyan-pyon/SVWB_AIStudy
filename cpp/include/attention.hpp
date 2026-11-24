#pragma once

// Small card-token Transformer used as an alternative to the 256->64->32 MLP.
// The math here is independent of the game rules: callers fill Tokens and
// candidate feature vectors, the model returns value probabilities and policy logits.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace svwb::attn {

inline constexpr int kVocab = 600;          // 0 = empty slot, cards 1..559, crests 560..599
inline constexpr int kCardEmbed = 32;
inline constexpr int kAttrDim = 32;
inline constexpr int kTokenInput = kCardEmbed + kAttrDim;
inline constexpr int kGlobalDim = 32;
inline constexpr int kModel = 128;
inline constexpr int kHeads = 4;
inline constexpr int kHeadDim = kModel / kHeads;
inline constexpr int kFfn = 512;
inline constexpr int kLayers = 2;
inline constexpr int kCandidateDim = 64;
inline constexpr int kValueHidden = 64;
inline constexpr int kPolicyHidden = 128;
inline constexpr int kPolicyInput = kModel + kCandidateDim;

inline constexpr int kHandTokens = 9;
inline constexpr int kBoardTokens = 5;
inline constexpr int kCrestTokens = 10;
inline constexpr int kHandOffset = 0;
inline constexpr int kOwnBoardOffset = kHandOffset + kHandTokens;
inline constexpr int kEnemyBoardOffset = kOwnBoardOffset + kBoardTokens;
inline constexpr int kCrestOffset = kEnemyBoardOffset + kBoardTokens;
inline constexpr int kCardTokens = kCrestOffset + kCrestTokens; // 29
inline constexpr int kMaxTokens = kCardTokens + 1;              // + CLS

inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::uint32_t kActivationRelu = 1;

// Parameter count in PyTorch layout, in the order written to attn_weights.bin.
inline constexpr std::size_t kLayerParameters =
    2 * kModel + (3 * kModel * kModel + 3 * kModel) + (kModel * kModel + kModel) + 2 * kModel +
    (kModel * kFfn + kFfn) + (kFfn * kModel + kModel);
inline constexpr std::size_t kParameterCount =
    static_cast<std::size_t>(kVocab) * kCardEmbed + (kTokenInput * kModel + kModel) +
    (kGlobalDim * kModel + kModel) + kModel + kLayers * kLayerParameters + 2 * kModel +
    (kModel * kValueHidden + kValueHidden) + (kValueHidden + 1) +
    (kPolicyInput * kPolicyHidden + kPolicyHidden) + (kPolicyHidden + 1);
static_assert(kParameterCount == 461'826);

enum class ModelType : std::uint8_t { Mlp, Attention };

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

// Root encoding reused by every candidate scored against the same position.
struct RootContext {
    alignas(32) std::array<float, kModel> cls;
    alignas(32) std::array<float, kPolicyHidden> policy_base; // policy_fc1(cls part) + bias
};

class AttentionModel {
public:
    [[nodiscard]] static AttentionModel load(const std::filesystem::path& file);
    [[nodiscard]] static AttentionModel random(std::uint32_t seed);
    // Build from PyTorch-ordered parameters (kParameterCount floats).
    [[nodiscard]] static AttentionModel from_parameters(std::vector<float> ordered);
    void save(const std::filesystem::path& file) const;

    // Win probability of the acting player.
    [[nodiscard]] float value(const Tokens& tokens) const;
    // Raw value logit before the sigmoid.
    [[nodiscard]] float value_logit(const Tokens& tokens) const;
    // Value probability, plus the CLS encoding and policy precomputation for candidate scoring.
    [[nodiscard]] float evaluate_root(const Tokens& tokens, RootContext& context) const;
    [[nodiscard]] float policy_logit(const RootContext& context, const float* candidate) const;
    [[nodiscard]] const std::vector<float>& parameters() const { return ordered_; }

private:
    AttentionModel() = default;
    void prepare();
    float encode(const Tokens& tokens, float* cls) const;

    std::vector<float> ordered_; // PyTorch layout, as stored in the file
    std::vector<float> packed_;  // inference layout (input-major weights)
    struct Offsets {
        std::size_t token_table;  // [vocab][model]: embedding part of token_proj plus its bias
        std::size_t attr_weight;  // [attr][model]
        std::size_t global_weight; // [global][model]
        std::size_t global_bias;  // includes the CLS embedding
        struct Layer {
            std::size_t norm1_w, norm1_b, qkv_w, qkv_b, out_w, out_b, norm2_w, norm2_b, ffn1_w, ffn1_b, ffn2_w, ffn2_b;
        };
        std::array<Layer, kLayers> layers;
        std::size_t final_w, final_b;
        std::size_t value1_w, value1_b, value2_w, value2_b;
        std::size_t policy_cls_w, policy_candidate_w, policy1_b, policy2_w, policy2_b;
    } offsets_{};
};

[[nodiscard]] std::uint32_t crc32(const void* data, std::size_t bytes, std::uint32_t crc = 0);
[[nodiscard]] bool is_attention_weights_file(const std::filesystem::path& file);

} // namespace svwb::attn
