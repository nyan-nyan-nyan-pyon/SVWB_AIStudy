#pragma once

#include "mlp.hpp"

#include <memory>
#include <optional>
#include <random>
#include <vector>

namespace svwb {

inline constexpr int kBeamWidth = 64;
inline constexpr int kBeamSteps = 20;
inline constexpr int kRankedCandidates = 16;
inline constexpr int kRandomCandidates = 2;
inline constexpr int kMaxCandidates = kRankedCandidates + kRandomCandidates;

struct SearchOptions {
    int simulations = 64;
    bool exploration = true;
    bool record_actions = false;
};

struct TurnCandidate {
    GameState next;
    // State at the end of the acting player's moves, before the next player's draw and PP refresh.
    GameState decision;
    Prediction prediction;
    std::vector<Action> actions;
};

[[nodiscard]] std::vector<TurnCandidate> beam_search(const GameState& initial, const Mlp& model,
                                                     std::size_t width = kBeamWidth,
                                                     bool record_actions = false);

struct PolicySample {
    Features features;
    float probability;
};

struct CandidateTrace {
    std::vector<Action> actions;
    int visits;
    // Mean backed-up value for the acting player. No estimate for unvisited candidates.
    std::optional<float> value;
    bool selected;
};

struct TurnTrace {
    int total_visits = 0;
    std::vector<CandidateTrace> ranked;
};

struct SearchResult {
    GameState next;
    std::vector<PolicySample> policy;
    TurnTrace trace;
};

class TurnSearch {
public:
    TurnSearch(const Mlp& first, const Mlp& second, std::mt19937& rng, SearchOptions options = {});
    ~TurnSearch();

    [[nodiscard]] SearchResult choose_turn(const GameState& state, bool collect_policy);

private:
    struct Node;
    void expand(Node& node, std::size_t width);
    float simulate(Node& node, const float* root_priors = nullptr);

    std::array<const Mlp*, kPlayers> models_;
    std::mt19937& rng_;
    SearchOptions options_;
    std::unique_ptr<Node> root_;
};

} // namespace svwb
