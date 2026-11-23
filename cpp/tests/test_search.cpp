#include "card_db.hpp"
#include "engine.hpp"
#include "search.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace svwb;

void check(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        throw std::runtime_error(std::string(location.file_name()) + ":" + std::to_string(location.line()));
    }
}

void approximately(float actual, float expected, std::source_location location = std::source_location::current()) {
    check(std::abs(actual - expected) <= 1e-6F, location);
}

void write_weight_file(const std::filesystem::path& path, std::span<const float> values, bool extra = false) {
    std::ofstream file(path, std::ios::binary);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    for (const float value : values) {
        const auto word = std::bit_cast<std::uint32_t>(value);
        for (unsigned i = 0; i < 4; ++i) {
            file.put(static_cast<char>((word >> (8 * i)) & 0xffU));
        }
    }
    if (extra) {
        file.put('\0');
    }
    file.close();
}

void reject_weights(const std::filesystem::path& path) {
    try {
        static_cast<void>(Mlp::load(path));
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("Invalid weights were accepted");
}

void test_weights(const std::filesystem::path& directory) {
    const auto file = directory / "weights.bin";
    // Offsets are taken from DESIGN.md in PyTorch's output-major order.
    std::array<float, 18594> weights{};
    weights[1 * 256 + 2] = 2.0F; // fc1.weight
    weights[3 * 256 + 5] = -2.0F;
    weights[16384 + 1] = 0.25F; // fc1.bias
    weights[16384 + 3] = 0.3F;
    weights[16448 + 4 * 64 + 1] = 3.0F; // fc2.weight
    weights[16448 + 4 * 64 + 3] = 17.0F;
    weights[16448 + 2 * 64 + 1] = -2.0F;
    weights[18496 + 4] = 0.5F; // fc2.bias
    weights[18528 + 4] = 0.5F; // head_val.weight
    weights[18528 + 2] = 10.0F;
    weights[18560] = -0.7F; // head_val.bias
    weights[18561 + 4] = -0.25F; // head_pol.weight
    weights[18593] = 0.8F; // head_pol.bias
    write_weight_file(file, weights);
    check(std::filesystem::file_size(file) == 74376);
    Features features{};
    features[2] = 0.5F;
    features[5] = 0.5F;
    const auto prediction = Mlp::load(file).predict(features);
    approximately(prediction.value, 1.0F / (1.0F + std::exp(-1.425F)));
    approximately(prediction.policy_logit, -0.2625F);

    write_weight_file(file, std::span(weights).first(weights.size() - 1));
    reject_weights(file);
    write_weight_file(file, weights, true);
    reject_weights(file);
    weights[0] = std::numeric_limits<float>::quiet_NaN();
    write_weight_file(file, weights);
    reject_weights(file);
    std::cout << "PASS weight byte layout, both heads, ReLU, and malformed files\n";
}

GameState fixture(std::mt19937& rng) {
    Deck first{};
    Deck second{};
    first.fill(1);
    second.fill(2);
    auto state = new_game(first, second, rng);
    state.turn_number = 5;
    return state;
}

void test_seat_features() {
    std::mt19937 rng(73);
    auto state = fixture(rng);
    auto& a = state.players[0];
    auto& b = state.players[1];
    a.leader_hp = 7;
    b.leader_hp = 16;
    a.pp = 3;
    b.pp = 8;
    a.board[0] = {3, 1, 3, 3, 0, FLAG_WARD};
    a.board[1] = {1, 1, 2, 2, 0, FLAG_NONE};
    a.board_count = 2;
    const auto features = extract_features(state);
    approximately(features[0], 7.0F / 20.0F);
    approximately(features[120], 16.0F / 20.0F);
    approximately(features[1], 0.3F);
    approximately(features[121], 0.8F);
    approximately(features[65], 5.0F / 9.0F); // Own hand, goblins.
    approximately(features[120 + 66], 4.0F / 9.0F); // Opponent hand, fighters.
    approximately(features[87], 35.0F / 40.0F); // Own remaining deck, goblins.
    approximately(features[98], 1.0F); // Own next draw, goblin.
    approximately(features[240], 5.0F / 40.0F);
    std::swap(a.board[0], a.board[1]);
    check(features == extract_features(state));
    state.turn_player = 1;
    const auto reversed = extract_features(state);
    check(std::equal(features.begin(), features.begin() + 120, reversed.begin() + 120));
    check(std::equal(features.begin() + 120, features.begin() + 240, reversed.begin()));
    std::cout << "PASS current-turn feature blocks, zones, and board permutation invariance\n";
}

void test_beam_dedup_and_search() {
    std::mt19937 rng(19);
    auto state = fixture(rng);
    auto& a = state.players[0];
    a.hand = {1, 2, 1};
    a.hand_count = 3;
    a.pp = a.max_pp = 3;
    a.ep = 0;
    const auto model = Mlp::random(7);
    const auto candidates = beam_search(state, model);
    check(candidates.size() == 5);
    for (const auto& candidate : candidates) {
        check(candidate.decision.turn_player == 0 && candidate.next.turn_player == 1);
        check(candidate.next.players[1].deck_idx == state.players[1].deck_idx + 1);
    }

    // Test terminal value handling when the acting player is the second seat.
    state.turn_player = 1;
    a.leader_hp = 3;
    auto& b = state.players[1];
    b.hand = {9};
    b.hand_count = 1;
    b.pp = b.max_pp = 5;
    constexpr int simulations = 64;
    TurnSearch search(model, model, rng, {simulations, false});
    const auto chosen = search.choose_turn(state, true);
    check(chosen.next.winner == 1 && chosen.next.players[0].leader_hp <= 0);
    check(chosen.policy.size() == 2);
    float total = 0.0F;
    for (const auto& sample : chosen.policy) {
        approximately(sample.features[0], 1.0F);
        check(sample.probability >= 0.0F && sample.probability <= 1.0F);
        approximately(sample.probability * simulations, std::round(sample.probability * simulations));
        total += sample.probability;
    }
    approximately(total, 1.0F);
    std::cout << "PASS commuting plays deduplicated and PUCT finds second-seat lethal\n";
}

} // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
            ("svwb-weight-test-" + std::to_string(stamp));
        check(std::filesystem::create_directory(directory));
        test_weights(directory);
        test_seat_features();
        test_beam_dedup_and_search();
        std::filesystem::remove(directory / "weights.bin");
        std::filesystem::remove(directory);
        std::cout << "PASS Phase 2 unit suite\n";
    } catch (const std::exception& error) {
        std::cerr << "Phase 2 unit test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
