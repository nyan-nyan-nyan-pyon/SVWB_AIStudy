#include "mlp.hpp"

#include "card_db.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>
#include <tuple>

namespace svwb {
namespace {

constexpr int kCardFeatures = 11;
constexpr int kFollowerFeatures = 11;
constexpr int kPlayerFeatures = 120;
constexpr int kPlayerScalars = 10;
constexpr int kZoneOffset = kPlayerScalars + kBoardLimit * kFollowerFeatures;
static_assert(kZoneOffset + 5 * kCardFeatures == kPlayerFeatures);
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

void add_card(float* zone, CardId id, float scale) {
    if (id >= 1 && id <= kCardFeatures) {
        zone[id - 1] += scale;
    }
}

void encode_player(float* out, const GameState& state, int seat) {
    const auto& player = state.players[seat];
    out[0] = static_cast<float>(std::max(0, static_cast<int>(player.leader_hp))) / kLeaderMaxHp;
    out[1] = static_cast<float>(player.pp) / kMaxPp;
    out[2] = static_cast<float>(player.max_pp) / kMaxPp;
    out[3] = static_cast<float>(player.ep) / 3.0F;
    out[4] = static_cast<float>(std::min(64, static_cast<int>(player.shadows))) / 64.0F;
    out[5] = static_cast<float>(player.hand_count) / kHandLimit;
    out[6] = static_cast<float>(player.board_count) / kBoardLimit;
    out[7] = static_cast<float>(kDeckSize - player.deck_idx) / kDeckSize;
    out[8] = static_cast<float>(player.evolved_this_turn);
    out[9] = state.turn_number >= 5 - seat && player.ep > 0 && !player.evolved_this_turn ? 1.0F : 0.0F;

    auto board = player.board;
    std::sort(board.begin(), board.begin() + player.board_count, [](const Follower& a, const Follower& b) {
        return std::tie(a.card_id, a.atk, a.hp, a.max_hp, a.flags) <
               std::tie(b.card_id, b.atk, b.hp, b.max_hp, b.flags);
    });
    auto* zone = out + kZoneOffset;
    for (int i = 0; i < player.hand_count; ++i) {
        add_card(zone, player.hand[i], 1.0F / kHandLimit);
    }
    for (int i = 0; i < player.board_count; ++i) {
        const auto& follower = board[i];
        auto* slot = out + kPlayerScalars + i * kFollowerFeatures;
        slot[0] = static_cast<float>(follower.card_id) / kCardFeatures;
        slot[1] = static_cast<float>(follower.atk) / 10.0F;
        slot[2] = static_cast<float>(std::max(0, static_cast<int>(follower.hp))) / 10.0F;
        slot[3] = static_cast<float>(follower.max_hp) / 10.0F;
        for (int bit = 0; bit < 7; ++bit) {
            slot[4 + bit] = (follower.flags & (1U << bit)) ? 1.0F : 0.0F;
        }
        add_card(zone + kCardFeatures, follower.card_id, 1.0F / kBoardLimit);
    }
    for (int i = player.deck_idx; i < kDeckSize; ++i) {
        add_card(zone + 2 * kCardFeatures, player.deck[i], 1.0F / kDeckSize);
    }
    for (int i = 0; i < 2 && player.deck_idx + i < kDeckSize; ++i) {
        add_card(zone + (3 + i) * kCardFeatures, player.deck[player.deck_idx + i], 1.0F);
    }
}

template<std::size_t Inputs, std::size_t Outputs>
void transpose_layer(const float*& ordered, std::array<float, Inputs * Outputs>& weights,
                     std::array<float, Outputs>& bias) {
    for (std::size_t output = 0; output < Outputs; ++output) {
        for (std::size_t input = 0; input < Inputs; ++input) {
            weights[input * Outputs + output] = *ordered++;
        }
    }
    std::copy_n(ordered, Outputs, bias.begin());
    ordered += Outputs;
}

template<std::size_t Inputs, std::size_t Outputs>
void relu_layer(const std::array<float, Inputs>& input, const std::array<float, Inputs * Outputs>& weights,
                std::array<float, Outputs>& output) {
    for (std::size_t i = 0; i < input.size(); ++i) {
        const float value = input[i];
        if (value == 0.0F) {
            continue;
        }
        const float* row = weights.data() + i * Outputs;
        for (std::size_t j = 0; j < Outputs; ++j) {
            output[j] += value * row[j];
        }
    }
    for (auto& value : output) {
        value = std::max(0.0F, value);
    }
}

} // namespace

Features extract_features(const GameState& state) {
    Features features{};
    encode_player(features.data(), state, state.turn_player);
    encode_player(features.data() + kPlayerFeatures, state, 1 - state.turn_player);
    features[2 * kPlayerFeatures] = static_cast<float>(state.turn_number) / kDeckSize;
    return features;
}

Mlp::Mlp(const std::array<float, kWeightFloats>& ordered_weights) {
    const float* ordered = ordered_weights.data();
    transpose_layer<kFeatureDim, kHidden1>(ordered, fc1_, bias1_);
    transpose_layer<kHidden1, kHidden2>(ordered, fc2_, bias2_);
    std::copy_n(ordered, kHidden2, value_weights_.begin());
    ordered += kHidden2;
    value_bias_ = *ordered++;
    std::copy_n(ordered, kHidden2, policy_weights_.begin());
    ordered += kHidden2;
    policy_bias_ = *ordered;
}

Mlp Mlp::load(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    std::array<float, kWeightFloats> weights;
    if (!stream.read(reinterpret_cast<char*>(weights.data()), sizeof(weights)) ||
        stream.peek() != std::char_traits<char>::eof() || stream.bad()) {
        throw std::runtime_error("weights.bin must contain exactly 74376 bytes: " + file.string());
    }
    if constexpr (std::endian::native != std::endian::little) {
        for (auto& value : weights) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
            std::uint32_t word = 0;
            for (unsigned i = 0; i < 4; ++i) {
                word |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
            }
            value = std::bit_cast<float>(word);
        }
    }
    if (!std::all_of(weights.begin(), weights.end(), [](float value) { return std::isfinite(value); })) {
        throw std::runtime_error("weights.bin contains a non-finite float: " + file.string());
    }
    return Mlp(weights);
}

Mlp Mlp::random(std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::array<float, kWeightFloats> weights{};
    auto* position = weights.data();
    const auto fill_layer = [&](std::size_t inputs, std::size_t outputs) {
        const auto limit = std::sqrt(6.0F / static_cast<float>(inputs + outputs));
        std::uniform_real_distribution<float> uniform(-limit, limit);
        for (std::size_t i = 0; i < inputs * outputs; ++i) {
            *position++ = uniform(rng);
        }
        position += outputs; // Biases are initialized to zero.
    };
    fill_layer(kFeatureDim, kHidden1);
    fill_layer(kHidden1, kHidden2);
    fill_layer(kHidden2, 1);
    fill_layer(kHidden2, 1);
    return Mlp(weights);
}

Prediction Mlp::predict(const Features& features) const {
    alignas(32) auto hidden1 = bias1_;
    alignas(32) auto hidden2 = bias2_;
    relu_layer(features, fc1_, hidden1);
    relu_layer(hidden1, fc2_, hidden2);
    float value = value_bias_;
    float policy = policy_bias_;
    for (std::size_t i = 0; i < kHidden2; ++i) {
        value += hidden2[i] * value_weights_[i];
        policy += hidden2[i] * policy_weights_[i];
    }
    if (!std::isfinite(value) || !std::isfinite(policy)) {
        throw std::overflow_error("MLP prediction overflow");
    }
    return {1.0F / (1.0F + std::exp(-value)), policy};
}

} // namespace svwb
