#pragma once

#include "search.hpp"
#include "wb_cards.hpp"

namespace svwb::wb {

class MatchupFeatures {
public:
    MatchupFeatures() = default;
    MatchupFeatures(const Deck& first, const Deck& second);
    [[nodiscard]] int local_id(CardId id) const;
    [[nodiscard]] std::span<const CardId> cards() const { return cards_; }
    [[nodiscard]] bool legacy_mirror() const { return legacy_; }

private:
    std::vector<CardId> cards_;
    std::vector<int> by_card_;
    bool legacy_ = true;
};

[[nodiscard]] Features extract_features(const GameState& state, const MatchupFeatures* matchup = nullptr);
[[nodiscard]] inline const char* feature_schema(const MatchupFeatures& matchup) {
    return matchup.legacy_mirror() ? "wb-mirror-v2" : "wb-matchup-v2";
}

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
    bool forced = false;
    std::vector<RankedTurn> ranked;
};

class Search {
public:
    Search(const Mlp& first, const Mlp& second, std::mt19937& rng, SearchOptions options,
           const MatchupFeatures* matchup = nullptr);
    ~Search();
    [[nodiscard]] Decision choose(const GameState& state, bool policy);

private:
    struct Node;
    void expand(Node& node, std::size_t width);
    [[nodiscard]] std::array<float, kPlayers> simulate(Node& node, const float* root_priors = nullptr);

    std::array<const Mlp*, kPlayers> models_;
    std::mt19937& rng_;
    SearchOptions options_;
    const MatchupFeatures* matchup_;
    std::unique_ptr<Node> root_;
};

// Evaluate each mask over common sampled replacement decks, without inspecting the actual next cards.
[[nodiscard]] Action choose_mulligan(const GameState& state, const Mlp& model, std::mt19937& rng,
                                     const MatchupFeatures* matchup = nullptr);
[[nodiscard]] float outcome_value(Outcome outcome, int seat);

} // namespace svwb::wb
