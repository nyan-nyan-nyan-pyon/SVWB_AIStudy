#pragma once

#include "search.hpp"
#include "wb_cards.hpp"

namespace svwb::wb {

[[nodiscard]] Features extract_features(const GameState& state);

struct RankedTurn {
    std::vector<Action> actions;
    int visits;
    std::optional<float> value;
    bool selected;
};

struct Decision {
    GameState next;
    std::vector<PolicySample> policy;
    int visits = 0;
    std::vector<RankedTurn> ranked;
};

class Search {
public:
    Search(const Mlp& first, const Mlp& second, std::mt19937& rng, SearchOptions options);
    ~Search();
    [[nodiscard]] Decision choose(const GameState& state, bool policy);

private:
    struct Node;
    void expand(Node& node, std::size_t width);
    [[nodiscard]] std::array<float, kPlayers> simulate(Node& node, const float* root_priors = nullptr);

    std::array<const Mlp*, kPlayers> models_;
    std::mt19937& rng_;
    SearchOptions options_;
    std::unique_ptr<Node> root_;
};

// Evaluate each mask over common sampled replacement decks, without inspecting the actual next cards.
[[nodiscard]] Action choose_mulligan(const GameState& state, const Mlp& model, std::mt19937& rng);
[[nodiscard]] float outcome_value(Outcome outcome, int seat);

} // namespace svwb::wb
