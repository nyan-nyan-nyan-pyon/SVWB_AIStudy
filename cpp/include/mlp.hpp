#pragma once

#include "state.hpp"

#include <array>
#include <filesystem>

namespace svwb {

inline constexpr std::size_t kFeatureDim = 256;
inline constexpr std::size_t kHidden1 = 64;
inline constexpr std::size_t kHidden2 = 32;
inline constexpr std::size_t kWeightFloats = kFeatureDim * kHidden1 + kHidden1 +
    kHidden1 * kHidden2 + kHidden2 + 2 * (kHidden2 + 1);

using Features = std::array<float, kFeatureDim>;

struct Prediction {
    float value; // Probability for the player represented by the first feature block.
    float policy_logit;
};

[[nodiscard]] Features extract_features(const GameState& state);

class Mlp {
public:
    [[nodiscard]] static Mlp load(const std::filesystem::path& file);
    [[nodiscard]] static Mlp random(std::uint32_t seed);
    [[nodiscard]] Prediction predict(const Features& features) const;

private:
    explicit Mlp(const std::array<float, kWeightFloats>& ordered_weights);

    // Inference stores input-major weights so the output lanes are contiguous.
    alignas(32) std::array<float, kFeatureDim * kHidden1> fc1_;
    alignas(32) std::array<float, kHidden1> bias1_;
    alignas(32) std::array<float, kHidden1 * kHidden2> fc2_;
    alignas(32) std::array<float, kHidden2> bias2_;
    std::array<float, kHidden2> value_weights_;
    float value_bias_;
    std::array<float, kHidden2> policy_weights_;
    float policy_bias_;
};

} // namespace svwb
