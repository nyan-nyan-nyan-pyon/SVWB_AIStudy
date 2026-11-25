#pragma once

#include "attention.hpp"
#include "search.hpp"
#include "wb_cards.hpp"

#include <cmath>
#include <optional>
#include <span>

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

struct GlobalPlayerContext {
    int seat = 0;
    int personal_turn = 1;        // Active: current personal turn; Non-active: previous personal turn
    int next_action_turn = 1;     // Active: current personal turn; Non-active: next personal turn
    int next_pp_refresh_turn = 1; // Both: next personal turn where PP refresh runs
    bool is_active_turn = false;
};

[[nodiscard]] GlobalPlayerContext make_global_player_context(const GameState& state, int seat) noexcept;
[[nodiscard]] int next_pp_budget(const PlayerState& p, int seat, int next_pp_refresh_turn) noexcept;
[[nodiscard]] std::uint32_t compute_card_vocab_hash();

[[nodiscard]] Features extract_features(const GameState& state, const MatchupFeatures* matchup = nullptr);
[[nodiscard]] attn::Tokens extract_attention_tokens(const GameState& state);
[[nodiscard]] attn::Tokens extract_attention_tokens_v2(const GameState& state);
[[nodiscard]] attn::Tokens extract_attention_tokens_for(const GameState& state, const attn::AttentionModel& model);
[[nodiscard]] attn::CandidateFeatures extract_candidate_features(const GameState& initial, const GameState& decision,
                                                                 const GameState& next);
[[nodiscard]] attn::CandidateSparseDiff extract_candidate_sparse_diff(const GameState& initial,
                                                                      const GameState& decision);

[[nodiscard]] inline const char* feature_schema(const MatchupFeatures& matchup) {
    return matchup.legacy_mirror() ? "wb-mirror-v2" : "wb-matchup-v2";
}
[[nodiscard]] inline const char* attention_feature_schema(attn::ArchVariant variant = attn::ArchVariant::V3GlobalEmbDiff) {
    switch (variant) {
    case attn::ArchVariant::LegacyV2: return "wb-attention-v1";
    case attn::ArchVariant::V3Global: return "wb-attention-v3-g";
    case attn::ArchVariant::V3GlobalEmbDiff: return "wb-attention-v3";
    }
    return "wb-attention-v3";
}

struct ActionPlan {
    std::vector<Action> prefix;
    bool requires_replan_after_observation = false;

    bool operator==(const ActionPlan&) const = default;
};

struct AttentionPolicySample {
    attn::CandidateFeatures features{};
    attn::CandidateSparseDiff emb_diff{};
    float probability = 0.0F;

    bool operator==(const AttentionPolicySample&) const = default;
};

struct RankedTurn {
    std::vector<Action> actions;
    int visits = 0;
    std::optional<float> value;
    bool selected = false;
    ActionPlan plan{};
    LethalStatus lethal_status = LethalStatus::NotFound;
};

struct Decision {
    GameState next{};
    std::vector<PolicySample> policy;
    std::vector<AttentionPolicySample> attention_policy;
    int visits = 0;
    bool forced = false;
    std::vector<RankedTurn> ranked;
    std::vector<Action> actions;
    std::unique_ptr<GameState> sampled_root;
    bool replanned = false;
    ActionPlan plan{};
    LethalStatus lethal_status = LethalStatus::NotFound;
    LethalEvidence lethal_evidence{};
};

struct ModelRef {
    const Mlp* mlp = nullptr;
    const attn::AttentionModel* attention = nullptr;

    ModelRef() = default;
    ModelRef(const Mlp& model) : mlp(&model) {}
    ModelRef(const attn::AttentionModel& model) : attention(&model) {}
    [[nodiscard]] bool is_attention() const { return attention != nullptr; }
};

// Shared MCTS root-perspective conversion and one-sided PUCT helpers (P0-1).
[[nodiscard]] inline constexpr float leaf_to_root_perspective(float actor_value, int actor, int root_player) noexcept {
    return actor == root_player ? actor_value : (1.0F - actor_value);
}

[[nodiscard]] inline float puct_score(float q_root, float prior, int parent_visits, int edge_visits,
                                      int node_actor, int root_player, float c_puct = 1.5F) noexcept {
    const float q_actor = node_actor == root_player ? q_root : (1.0F - q_root);
    const float u = c_puct * std::sqrt(static_cast<float>(parent_visits + 1)) * prior /
                    static_cast<float>(edge_visits + 1);
    return q_actor + u;
}

class Search {
public:
    Search(const Mlp& first, const Mlp& second, std::mt19937& rng, SearchOptions options,
           const MatchupFeatures* matchup = nullptr);
    Search(const attn::AttentionModel& first, const attn::AttentionModel& second, std::mt19937& rng,
           SearchOptions options, const MatchupFeatures* matchup = nullptr);
    Search(ModelRef first, ModelRef second, std::mt19937& rng, SearchOptions options,
           const MatchupFeatures* matchup = nullptr);
    ~Search();
    // Research plans and candidate values describe the sampled position.
    [[nodiscard]] Decision choose(const GameState& state, bool policy);
    // Advance the live game, replanning if a played action crosses an observation boundary.
    [[nodiscard]] Decision play(const GameState& state, bool policy);
    void set_determinization(bool enabled);
    void set_search_version(SearchVersion version);

private:
    struct Node;
    [[nodiscard]] Decision choose_legacy_sample(const GameState& state, bool policy);
    [[nodiscard]] Decision choose_p0(const GameState& state, bool policy);
    void expand_legacy(Node& node, std::size_t width, bool trace);
    [[nodiscard]] std::array<float, kPlayers> simulate_legacy(Node& node, const float* root_priors = nullptr);
    void expand_p0(Node& node, int root_player, std::size_t width, bool trace);
    [[nodiscard]] float simulate_p0(Node& node, int root_player, const float* root_priors = nullptr);

    std::array<const Mlp*, kPlayers> models_{};
    std::array<const attn::AttentionModel*, kPlayers> attention_models_{};
    std::mt19937& rng_;
    SearchOptions options_;
    const MatchupFeatures* matchup_;
    std::unique_ptr<Node> root_;
};

// Imperfect-information sampling, opponent-policy isolation, mulligan CRN, and explicit MCTS verification APIs.
[[nodiscard]] GameState sample_hidden_state(const GameState& state, std::mt19937& rng, int observer = -1);

struct OpponentPolicySnapshot {
    std::vector<ActionPlan> plans;
    std::vector<float> priors;
    std::size_t greedy_choice = 0;
};
[[nodiscard]] OpponentPolicySnapshot evaluate_opponent_turn_policy(
    const GameState& state, ModelRef opponent_model, int root_player, std::uint64_t rng_seed,
    const MatchupFeatures* matchup = nullptr);

struct MulliganSampleResult {
    std::array<CardId, kOpeningHand> offered_cards{};
    std::array<int, kOpeningHand> offered_physical_ids{};
    std::array<CardId, kOpeningHand> redrawn_cards{};
    std::array<int, kOpeningHand> redrawn_physical_ids{};
    GameState turn1_state{};
};
[[nodiscard]] MulliganSampleResult simulate_mulligan_crn(const GameState& state, std::uint8_t mask,
                                                         std::uint64_t sample_seed);

struct ExplicitMctsNode {
    struct Edge {
        float prior = 0.0F;
        float value_sum = 0.0F; // Always stored in root_player perspective
        int visits = 0;
        std::vector<std::unique_ptr<ExplicitMctsNode>> world_children; // size 1 for single-world, K for root
    };

    int actor = 0;
    Outcome terminal = Outcome::Ongoing;
    float leaf_actor_value = 0.5F;
    bool evaluated = false;
    int visits = 0;
    std::vector<Edge> edges;
};
void run_explicit_mcts_simulation(ExplicitMctsNode& root, int root_player, int world_index = -1, int* nn_evals = nullptr);

// Evaluate each mask over common sampled replacement decks, without inspecting the actual next cards.
[[nodiscard]] Action choose_mulligan(const GameState& state, const Mlp& model, std::mt19937& rng,
                                     const MatchupFeatures* matchup = nullptr,
                                     SearchVersion version = SearchVersion::P0);
[[nodiscard]] Action choose_mulligan(const GameState& state, const attn::AttentionModel& model, std::mt19937& rng,
                                     const MatchupFeatures* matchup = nullptr,
                                     SearchVersion version = SearchVersion::P0);
[[nodiscard]] Action choose_mulligan(const GameState& state, ModelRef model, std::mt19937& rng,
                                     const MatchupFeatures* matchup = nullptr,
                                     SearchVersion version = SearchVersion::P0);
[[nodiscard]] float outcome_value(Outcome outcome, int seat);

} // namespace svwb::wb
