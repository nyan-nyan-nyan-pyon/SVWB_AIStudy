#include "search.hpp"

#include "engine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace svwb {
namespace {

// For one turn all states share the same deck orders. Only the draw cursors can change.
// In Phase 1 hand/board permutations preserve play, targeting, and death effects.
// Revisit the board canonicalization when effects start depending on position or death order.
using PositionKey = std::array<std::uint64_t, 1 + kPlayers * (1 + 3 + kBoardLimit)>;

PositionKey position_key(const GameState& state) {
    PositionKey key{};
    std::size_t offset = 0;
    key[offset++] = state.turn_player | (static_cast<std::uint64_t>(state.turn_number) << 8) |
        (static_cast<std::uint64_t>(state.winner + 1) << 16);
    for (const auto& player : state.players) {
        key[offset++] = static_cast<std::uint8_t>(player.leader_hp) |
            (static_cast<std::uint64_t>(player.pp) << 8) |
            (static_cast<std::uint64_t>(player.max_pp) << 12) |
            (static_cast<std::uint64_t>(player.ep) << 16) |
            (static_cast<std::uint64_t>(player.shadows) << 24) |
            (static_cast<std::uint64_t>(player.deck_idx) << 32) |
            (static_cast<std::uint64_t>(player.evolved_this_turn) << 40);
        auto hand = player.hand;
        std::sort(hand.begin(), hand.begin() + player.hand_count);
        for (int i = 0; i < player.hand_count; ++i) {
            key[offset + i / 4] |= static_cast<std::uint64_t>(hand[i]) << (16 * (i % 4));
        }
        offset += 3;
        for (int i = 0; i < player.board_count; ++i) {
            const auto& f = player.board[i];
            key[offset + i] = f.card_id |
                (static_cast<std::uint64_t>(static_cast<std::uint8_t>(f.atk)) << 16) |
                (static_cast<std::uint64_t>(static_cast<std::uint8_t>(f.hp)) << 24) |
                (static_cast<std::uint64_t>(static_cast<std::uint8_t>(f.max_hp)) << 32) |
                (static_cast<std::uint64_t>(f.flags) << 40);
        }
        std::sort(key.begin() + offset, key.begin() + offset + player.board_count);
        offset += kBoardLimit;
    }
    return key;
}

struct PositionHash {
    std::size_t operator()(const PositionKey& key) const noexcept {
        std::uint64_t hash = 0x93E2'B76C'70A3'2D85;
        for (const auto word : key) {
            hash ^= word + 0x9E37'79B9'7F4A'7C15 + (hash << 6) + (hash >> 2);
        }
        return static_cast<std::size_t>(hash);
    }
};

using Positions = std::unordered_set<PositionKey, PositionHash>;

struct BeamNode {
    GameState state;
    Prediction prediction;
    std::size_t serial;
};

struct BeamStep {
    std::size_t parent;
    Action action;
};

constexpr Action kEndTurn{ActionType::END_TURN, kNoTarget, kNoTarget};
constexpr std::size_t kWideBeam = 128;

float outcome(const GameState& state, int seat) {
    return state.winner == kDraw ? 0.5F : static_cast<float>(state.winner == seat);
}

float candidate_score(const TurnCandidate& candidate) {
    if (candidate.next.winner != kOngoing) {
        return 3.0F * outcome(candidate.next, candidate.decision.turn_player) - 0.5F;
    }
    return candidate.prediction.value + 1.0F / (1.0F + std::exp(-candidate.prediction.policy_logit));
}

} // namespace

std::vector<TurnCandidate> beam_search(const GameState& initial, const Mlp& model, std::size_t width,
                                       bool record_actions) {
    if (width == 0 || initial.winner != kOngoing) {
        throw std::invalid_argument("Beam search needs a positive width and an active game");
    }
    Positions expanded;
    Positions finished;
    expanded.reserve(width * 16);
    finished.reserve(width * 4);
    expanded.insert(position_key(initial));
    std::vector<BeamNode> beam{{initial, model.predict(extract_features(initial)), 0}};
    std::vector<BeamNode> next_beam;
    std::vector<TurnCandidate> candidates;
    std::vector<BeamStep> steps;
    if (record_actions) {
        steps.push_back({}); // Index zero is the start of the turn.
    }
    std::size_t serial = 0;

    const auto finish = [&](const GameState& next, const GameState& decision, Prediction prediction,
                            std::size_t parent, Action last) {
        if (finished.insert(position_key(next)).second) {
            auto& candidate = candidates.emplace_back(TurnCandidate{next, decision, prediction, {}});
            if (record_actions) {
                candidate.actions.push_back(last);
                for (auto index = parent; index != 0; index = steps[index].parent) {
                    candidate.actions.push_back(steps[index].action);
                }
                std::reverse(candidate.actions.begin(), candidate.actions.end());
            }
        }
    };
    const auto better = [](const BeamNode& a, const BeamNode& b) {
        if (a.prediction.value != b.prediction.value) {
            return a.prediction.value > b.prediction.value;
        }
        return a.serial < b.serial;
    };
    for (int step = 0; step <= kBeamSteps && !beam.empty(); ++step) {
        next_beam.clear();
        for (const auto& node : beam) {
            auto ended = node.state;
            apply_legal_action(ended, kEndTurn);
            finish(ended, node.state, node.prediction, node.serial, kEndTurn);
            if (step == kBeamSteps) {
                continue;
            }
            for (Action action : legal_actions(node.state)) {
                if (action.type == ActionType::END_TURN) {
                    continue;
                }
                auto next = node.state;
                apply_legal_action(next, action);
                if (!expanded.insert(position_key(next)).second) {
                    continue;
                }
                const auto prediction = model.predict(extract_features(next));
                if (next.winner != kOngoing) {
                    finish(next, next, prediction, node.serial, action);
                } else {
                    next_beam.push_back({next, prediction, ++serial});
                    if (record_actions) {
                        steps.push_back({node.serial, action});
                    }
                }
            }
        }
        if (next_beam.size() > width) {
            std::nth_element(next_beam.begin(), next_beam.begin() + width, next_beam.end(), better);
            next_beam.resize(width);
        }
        // Keep deterministic traversal and tie-breaking inside this game.
        std::sort(next_beam.begin(), next_beam.end(), better);
        beam.swap(next_beam);
    }
    return candidates;
}

struct TurnSearch::Node {
    struct Edge {
        TurnCandidate candidate;
        float prior;
        float value_sum = 0.0F;
        int visits = 0;
        std::unique_ptr<Node> child{};
    };

    explicit Node(const GameState& position) : state(position) {}

    GameState state;
    int visits = 0;
    bool evaluated = false;
    std::vector<Edge> edges;
};

TurnSearch::TurnSearch(const Mlp& first, const Mlp& second, std::mt19937& rng, SearchOptions options)
    : models_{&first, &second}, rng_(rng), options_(options) {
    if (options.simulations < 1) {
        throw std::invalid_argument("PUCT simulation count must be positive");
    }
}

TurnSearch::~TurnSearch() = default;

void TurnSearch::expand(Node& node, std::size_t width) {
    auto candidates = beam_search(node.state, *models_[node.state.turn_player], width, options_.record_actions);
    const auto ranked = std::min(candidates.size(), static_cast<std::size_t>(kRankedCandidates));
    std::vector<float> scores;
    scores.reserve(candidates.size());
    std::transform(candidates.begin(), candidates.end(), std::back_inserter(scores), candidate_score);
    std::vector<std::size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), 0);
    std::partial_sort(order.begin(), order.begin() + ranked, order.end(), [&](std::size_t a, std::size_t b) {
        return scores[a] != scores[b] ? scores[a] > scores[b] : a < b;
    });
    const auto selected = options_.exploration ? std::min(candidates.size(), ranked + kRandomCandidates) : ranked;
    for (auto i = ranked; i < selected; ++i) {
        const auto pick = std::uniform_int_distribution<std::size_t>(i, order.size() - 1)(rng_);
        std::swap(order[i], order[pick]);
    }
    node.edges.reserve(selected);
    float max_logit = candidates[order[0]].prediction.policy_logit;
    for (std::size_t i = 1; i < selected; ++i) {
        max_logit = std::max(max_logit, candidates[order[i]].prediction.policy_logit);
    }
    float total = 0.0F;
    for (std::size_t i = 0; i < selected; ++i) {
        auto& candidate = candidates[order[i]];
        const float prior = std::exp(candidate.prediction.policy_logit - max_logit);
        total += prior;
        node.edges.push_back({std::move(candidate), prior});
    }
    for (auto& edge : node.edges) {
        edge.prior /= total;
    }
}

float TurnSearch::simulate(Node& node, const float* root_priors) {
    const int player = node.state.turn_player;
    if (node.state.winner != kOngoing) {
        return outcome(node.state, player);
    }
    if (node.edges.empty()) {
        if (!node.evaluated) {
            node.evaluated = true;
            return models_[player]->predict(extract_features(node.state)).value;
        }
        expand(node, kBeamWidth);
    }

    constexpr float kPuct = 1.5F;
    const float exploration = kPuct * std::sqrt(static_cast<float>(node.visits + 1));
    float best_score = -1.0F;
    std::size_t best = 0;
    for (std::size_t i = 0; i < node.edges.size(); ++i) {
        const auto& edge = node.edges[i];
        const float value = edge.candidate.next.winner != kOngoing
            ? outcome(edge.candidate.next, player)
            : edge.visits > 0 ? edge.value_sum / static_cast<float>(edge.visits) : 0.5F;
        const float prior = root_priors ? root_priors[i] : edge.prior;
        const float score = value + exploration * prior / static_cast<float>(1 + edge.visits);
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    auto& edge = node.edges[best];
    const auto& next = edge.candidate.next;
    float value;
    if (next.winner != kOngoing) {
        value = outcome(next, player);
    } else {
        if (!edge.child) {
            edge.child = std::make_unique<Node>(next);
        }
        const float child_value = simulate(*edge.child);
        value = next.turn_player == player ? child_value : 1.0F - child_value;
    }
    edge.value_sum += value;
    ++edge.visits;
    ++node.visits;
    return value;
}

SearchResult TurnSearch::choose_turn(const GameState& state, bool collect_policy) {
    if (state.winner != kOngoing) {
        throw std::invalid_argument("Cannot search a finished game");
    }
    const bool widen = options_.exploration && std::bernoulli_distribution(0.01)(rng_);
    if (!root_ || root_->state != state || widen) {
        root_ = std::make_unique<Node>(state);
    }
    if (root_->edges.empty()) {
        expand(*root_, widen ? kWideBeam : kBeamWidth);
    }

    std::array<float, kMaxCandidates> priors{};
    if (options_.exploration) {
        std::gamma_distribution<float> gamma(0.3F, 1.0F);
        float total = 0.0F;
        for (std::size_t i = 0; i < root_->edges.size(); ++i) {
            priors[i] = gamma(rng_);
            total += priors[i];
        }
        for (std::size_t i = 0; i < root_->edges.size(); ++i) {
            const auto base = root_->edges[i].prior;
            priors[i] = total > 0.0F ? 0.75F * base + 0.25F * priors[i] / total : base;
        }
    }
    for (int simulation = 0; simulation < options_.simulations; ++simulation) {
        simulate(*root_, options_.exploration ? priors.data() : nullptr);
    }

    SearchResult result{};
    std::array<int, kMaxCandidates> visits{};
    for (std::size_t i = 0; i < root_->edges.size(); ++i) {
        const auto& edge = root_->edges[i];
        visits[i] = edge.visits;
        if (collect_policy) {
            result.policy.push_back({extract_features(edge.candidate.decision),
                                     static_cast<float>(edge.visits) / static_cast<float>(root_->visits)});
        }
    }

    // A known forced win in this turn should survive any temperature sampling.
    const auto winning = std::find_if(root_->edges.begin(), root_->edges.end(), [&](const Node::Edge& edge) {
        return edge.candidate.next.winner == state.turn_player;
    });
    std::size_t chosen;
    if (winning != root_->edges.end()) {
        chosen = static_cast<std::size_t>(winning - root_->edges.begin());
    } else if (options_.exploration && state.turn_number <= 5) {
        chosen = std::discrete_distribution<std::size_t>(visits.begin(), visits.begin() + root_->edges.size())(rng_);
    } else {
        chosen = static_cast<std::size_t>(std::max_element(visits.begin(), visits.begin() + root_->edges.size()) -
                                         visits.begin());
    }
    result.next = root_->edges[chosen].candidate.next;
    if (options_.record_actions) {
        result.trace.total_visits = root_->visits;
        result.trace.ranked.reserve(root_->edges.size());
        for (std::size_t i = 0; i < root_->edges.size(); ++i) {
            auto& edge = root_->edges[i];
            std::optional<float> value;
            if (edge.visits > 0) {
                value = edge.value_sum / static_cast<float>(edge.visits);
            }
            result.trace.ranked.push_back({std::move(edge.candidate.actions), edge.visits, value, i == chosen});
        }
        std::stable_sort(result.trace.ranked.begin(), result.trace.ranked.end(),
                         [](const CandidateTrace& a, const CandidateTrace& b) { return a.visits > b.visits; });
    }
    auto next_root = std::move(root_->edges[chosen].child);
    root_ = std::move(next_root);
    return result;
}

} // namespace svwb
