#include "wb_search.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <unordered_set>

namespace svwb::wb {
namespace {

constexpr int kPlayerFeatures = 120;
constexpr int kPlayerScalars = 14;
constexpr int kBoardFeatures = 12;
constexpr int kHandZone = kPlayerScalars + kBoardLimit * kBoardFeatures;
constexpr int kDeckZone = kHandZone + kCardSlots;
constexpr int kCrestZone = kDeckZone + kDeckCardKinds;
constexpr int kNextDraws = kCrestZone + 3;
static_assert(kNextDraws + 2 <= kPlayerFeatures);

bool ready(const Permanent& object) {
    return !object.attacked_this_turn && !(object.abilities & kAttackBlocked) &&
        (object.readied || (object.abilities & (FLAG_RUSH | FLAG_STORM)));
}

void encode_player(float* out, const GameState& state, int seat) {
    const auto& p = state.players[seat];
    out[0] = std::max(0, static_cast<int>(p.leader_hp)) / 20.0F;
    out[1] = p.available_pp() / 11.0F;
    out[2] = p.max_pp / 10.0F;
    out[3] = p.ep / 2.0F;
    out[4] = std::min(64, static_cast<int>(p.shadows)) / 64.0F;
    out[5] = p.hand_count / 9.0F;
    out[6] = p.board_count / 5.0F;
    out[7] = (kDeckSize - p.deck_idx) / 40.0F;
    out[8] = static_cast<float>(p.manual_evolved);
    out[9] = p.sep / 2.0F;
    out[10] = static_cast<float>(p.bonus_pp);
    out[11] = static_cast<float>(p.extra_pp_available);
    out[12] = p.extra_pp_uses / 2.0F;
    out[13] = static_cast<float>(seat == 0);
    for (int i = 0; i < p.board_count; ++i) {
        const auto& object = p.board[i];
        auto* row = out + kPlayerScalars + i * kBoardFeatures;
        row[0] = (object.card_id - kFirstCard + 1) / static_cast<float>(kCardSlots);
        row[1] = object.atk / 10.0F;
        row[2] = object.hp / 10.0F;
        row[3] = object.max_hp / 10.0F;
        row[4] = object.countdown / 5.0F;
        row[5] = std::min(16, static_cast<int>(object.clue_count)) / 16.0F;
        row[6] = static_cast<int>(object.evolution) / 2.0F;
        const bool can_attack = find_card(object.card_id)->kind == Kind::Follower && ready(object);
        row[7] = static_cast<float>(can_attack);
        row[8] = static_cast<float>(can_attack && (object.readied || (object.abilities & FLAG_STORM)));
        row[9] = static_cast<float>((object.abilities & FLAG_WARD) != 0);
        row[10] = static_cast<float>(object.acted_this_turn);
        row[11] = static_cast<float>(object.silenced);
    }
    for (int i = 0; i < p.hand_count; ++i) {
        out[kHandZone + p.hand[i] - kFirstCard] += 1.0F / kHandLimit;
    }
    for (int i = p.deck_idx; i < kDeckSize; ++i) {
        const int index = p.deck[i] == kCorrector ? kDeckCardKinds - 1 : p.deck[i] - kFirstCard;
        out[kDeckZone + index] += 1.0F / kDeckSize;
    }
    for (int i = 0; i < p.crest_count; ++i) {
        const auto& crest = p.crests[i];
        out[kCrestZone + static_cast<int>(crest.id) - 1] = crest.countdown / 4.0F;
    }
    for (int i = 0; i < 2 && p.deck_idx + i < kDeckSize; ++i) {
        out[kNextDraws + i] = (p.deck[p.deck_idx + i] - kFirstCard + 1) / static_cast<float>(kCardSlots);
    }
}

// All positions in one beam search share their deck arrays. Preserve hand/board order and RNG:
// tied random targets are taken at a position in the ordered candidate list.
struct Position {
    static constexpr std::size_t word_count = 3 + kPlayers * (4 + 4 + 2 * kBoardLimit + kCrestLimit + 6 + 6);
    std::array<std::uint64_t, word_count> words{};

    bool operator==(const Position&) const = default;
};

std::uint64_t pack(std::initializer_list<unsigned> bytes) {
    std::uint64_t result = 0;
    unsigned shift = 0;
    for (const auto byte : bytes) {
        result |= static_cast<std::uint64_t>(byte & 255U) << shift;
        shift += 8;
    }
    return result;
}

Position key(const GameState& state) {
    Position position;
    std::size_t n = 0;
    auto& w = position.words;
    w[n++] = state.rng_state;
    w[n++] = state.next_creation_order;
    w[n++] = pack({state.turn_player, state.turn_number, static_cast<unsigned>(state.outcome),
                   static_cast<unsigned>(state.end_reason)});
    for (const auto& p : state.players) {
        w[n++] = pack({p.deck_idx, p.hand_count, p.board_count, p.crest_count, p.pp, p.max_pp, p.bonus_pp, p.ep});
        w[n++] = pack({p.sep, p.extra_pp_uses, p.extra_pp_available, p.manual_evolved, p.leader_barrier});
        w[n++] = static_cast<std::uint16_t>(p.leader_hp);
        w[n++] = p.shadows;
        for (int i = 0; i < kHandLimit; ++i) {
            w[n + i / 4] |= static_cast<std::uint64_t>(p.hand[i]) << (16 * (i % 4));
        }
        n += 4;
        for (const auto& object : p.board) {
            w[n++] = object.creation_order | (static_cast<std::uint64_t>(object.abilities) << 32) |
                     (static_cast<std::uint64_t>(object.clue_count) << 48);
            w[n++] = pack({object.card_id, static_cast<std::uint16_t>(object.atk),
                static_cast<std::uint16_t>(object.hp), static_cast<std::uint16_t>(object.max_hp),
                object.countdown, object.entry_ply, static_cast<unsigned>(object.evolution),
                static_cast<unsigned>(object.silenced) | (static_cast<unsigned>(object.readied) << 1) |
                    (static_cast<unsigned>(object.attacked_this_turn) << 2) |
                    (static_cast<unsigned>(object.acted_this_turn) << 3)});
        }
        for (const auto& crest : p.crests) {
            w[n++] = crest.creation_order | (static_cast<std::uint64_t>(crest.id) << 32) |
                     (static_cast<std::uint64_t>(crest.countdown) << 40);
        }
        for (int i = 0; i < kCardSlots; ++i) {
            const auto& actions = p.actions[i];
            const auto named = actions.play | (static_cast<unsigned>(actions.attack) << 5) |
                               (static_cast<unsigned>(actions.act) << 10);
            w[n + i / 4] |= static_cast<std::uint64_t>(named) << (16 * (i % 4));
            w[n + 6 + i / 4] |= static_cast<std::uint64_t>(p.destroyed_followers[i]) << (16 * (i % 4));
        }
        n += 12;
    }
    return position;
}

struct PositionHash {
    std::size_t operator()(const Position& position) const noexcept {
        std::uint64_t hash = 0x0724DD61D03886F1;
        for (const auto word : position.words) {
            hash ^= word;
            hash *= 0x100000001b3;
        }
        return static_cast<std::size_t>(hash ^ (hash >> 32));
    }
};

struct Candidate {
    GameState next;
    GameState decision;
    Prediction prediction;
    std::vector<Action> actions;
};

struct BeamNode {
    GameState state;
    Prediction prediction;
    std::size_t id;
};

struct Step {
    std::size_t parent;
    Action action;
};

std::vector<Candidate> beam(const GameState& initial, const Mlp& model, std::size_t width, bool trace) {
    std::unordered_set<Position, PositionHash> seen, finished;
    seen.reserve(width * 24);
    finished.reserve(width * 5);
    seen.insert(key(initial));
    std::vector<BeamNode> level{{initial, model.predict(extract_features(initial)), 0}}, next_level;
    std::vector<Candidate> results;
    std::vector<Step> steps;
    if (trace) {
        steps.push_back({});
    }
    std::size_t serial = 0;
    const auto finish = [&](const auto& next, const auto& decision, Prediction prediction,
                            std::size_t parent, Action action) {
        if (!finished.insert(key(next)).second) {
            return;
        }
        auto& result = results.emplace_back(Candidate{next, decision, prediction, {}});
        if (trace) {
            result.actions.push_back(action);
            for (auto i = parent; i != 0; i = steps[i].parent) {
                result.actions.push_back(steps[i].action);
            }
            std::reverse(result.actions.begin(), result.actions.end());
        }
    };
    const auto better = [](const auto& a, const auto& b) {
        return a.prediction.value != b.prediction.value ? a.prediction.value > b.prediction.value : a.id < b.id;
    };
    for (int depth = 0; depth <= kBeamSteps && !level.empty(); ++depth) {
        next_level.clear();
        for (const auto& node : level) {
            auto ended = node.state;
            apply_legal_action(ended, {ActionType::EndTurn});
            finish(ended, node.state, node.prediction, node.id, {ActionType::EndTurn});
            if (depth == kBeamSteps) {
                continue;
            }
            for (const auto action : legal_actions(node.state)) {
                if (action.type == ActionType::EndTurn) {
                    continue;
                }
                auto next = node.state;
                apply_legal_action(next, action);
                if (!seen.insert(key(next)).second) {
                    continue;
                }
                const auto prediction = model.predict(extract_features(next));
                if (next.outcome != Outcome::Ongoing) {
                    finish(next, next, prediction, node.id, action);
                } else {
                    next_level.push_back({next, prediction, ++serial});
                    if (trace) {
                        steps.push_back({node.id, action});
                    }
                }
            }
        }
        if (next_level.size() > width) {
            std::nth_element(next_level.begin(), next_level.begin() + width, next_level.end(), better);
            next_level.resize(width);
        }
        std::sort(next_level.begin(), next_level.end(), better);
        level.swap(next_level);
    }
    return results;
}

float score(const Candidate& candidate) {
    if (candidate.next.outcome != Outcome::Ongoing) {
        return 3.0F * outcome_value(candidate.next.outcome, candidate.decision.turn_player) - 0.5F;
    }
    return candidate.prediction.value + 1.0F / (1.0F + std::exp(-candidate.prediction.policy_logit));
}

} // namespace

Features extract_features(const GameState& state) {
    Features features{};
    encode_player(features.data(), state, state.turn_player);
    encode_player(features.data() + kPlayerFeatures, state, 1 - state.turn_player);
    features[240] = state.turn_number / 40.0F;
    return features;
}

float outcome_value(Outcome outcome, int seat) {
    return static_cast<float>(seat == 0 ? outcome == Outcome::FirstWins : outcome == Outcome::SecondWins);
}

struct Search::Node {
    struct Edge {
        Candidate candidate;
        float prior;
        float value_sum = 0.0F;
        int visits = 0;
        std::unique_ptr<Node> child;
    };

    explicit Node(const GameState& position) : state(position) {}
    GameState state;
    int visits = 0;
    bool evaluated = false;
    std::vector<Edge> edges;
};

Search::Search(const Mlp& first, const Mlp& second, std::mt19937& rng, SearchOptions options)
    : models_{&first, &second}, rng_(rng), options_(options) {
    if (options.simulations < 1) {
        throw std::invalid_argument("Search needs a positive simulation count");
    }
}

Search::~Search() = default;

void Search::expand(Node& node, std::size_t width) {
    auto candidates = beam(node.state, *models_[node.state.turn_player], width, options_.record_actions);
    std::vector<float> scores;
    scores.reserve(candidates.size());
    std::transform(candidates.begin(), candidates.end(), std::back_inserter(scores), score);
    std::vector<std::size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), 0);
    const auto limit = std::min(candidates.size(), static_cast<std::size_t>(kRankedCandidates));
    std::partial_sort(order.begin(), order.begin() + limit, order.end(), [&](const auto a, const auto b) {
        return scores[a] != scores[b] ? scores[a] > scores[b] : a < b;
    });
    const auto selected = options_.exploration ? std::min(candidates.size(), limit + kRandomCandidates) : limit;
    for (auto i = limit; i < selected; ++i) {
        const auto pick = std::uniform_int_distribution<std::size_t>(i, order.size() - 1)(rng_);
        std::swap(order[i], order[pick]);
    }
    node.edges.reserve(selected);
    float max_logit = candidates[order[0]].prediction.policy_logit;
    for (std::size_t i = 1; i < selected; ++i) {
        max_logit = std::max(max_logit, candidates[order[i]].prediction.policy_logit);
    }
    float sum = 0.0F;
    for (std::size_t i = 0; i < selected; ++i) {
        auto& candidate = candidates[order[i]];
        const float prior = std::exp(candidate.prediction.policy_logit - max_logit);
        sum += prior;
        node.edges.push_back({std::move(candidate), prior, 0.0F, 0, {}});
    }
    for (auto& edge : node.edges) {
        edge.prior /= sum;
    }
}

std::array<float, kPlayers> Search::simulate(Node& node, const float* root_priors) {
    const int player = node.state.turn_player;
    if (node.edges.empty()) {
        if (!node.evaluated) {
            node.evaluated = true;
            const auto estimate = models_[player]->predict(extract_features(node.state)).value;
            return player == 0 ? std::array{estimate, 1.0F - estimate} : std::array{1.0F - estimate, estimate};
        }
        expand(node, kBeamWidth);
    }
    const float factor = 1.5F * std::sqrt(static_cast<float>(node.visits + 1));
    std::size_t best = 0;
    float best_score = -1.0F;
    for (std::size_t i = 0; i < node.edges.size(); ++i) {
        const auto& edge = node.edges[i];
        const auto result = edge.candidate.next.outcome;
        const float value = result != Outcome::Ongoing ? outcome_value(result, player) :
            edge.visits == 0 ? 0.5F : edge.value_sum / static_cast<float>(edge.visits);
        const float prior = root_priors ? root_priors[i] : edge.prior;
        const float puct = value + factor * prior / static_cast<float>(edge.visits + 1);
        if (puct > best_score) {
            best_score = puct;
            best = i;
        }
    }
    auto& edge = node.edges[best];
    const auto& next = edge.candidate.next;
    std::array<float, kPlayers> value;
    if (next.outcome != Outcome::Ongoing) {
        value = {outcome_value(next.outcome, 0), outcome_value(next.outcome, 1)};
    } else {
        if (!edge.child) {
            edge.child = std::make_unique<Node>(next);
        }
        value = simulate(*edge.child);
    }
    edge.value_sum += value[player];
    ++edge.visits;
    ++node.visits;
    return value;
}

Decision Search::choose(const GameState& state, bool policy) {
    if (state.phase != Phase::Main || state.outcome != Outcome::Ongoing) {
        throw std::invalid_argument("Search requires an active main-phase turn");
    }
    const bool widen = options_.exploration && std::bernoulli_distribution(0.01)(rng_);
    if (!root_ || root_->state != state || widen) {
        root_ = std::make_unique<Node>(state);
    }
    if (root_->edges.empty()) {
        expand(*root_, widen ? 2 * kBeamWidth : kBeamWidth);
    }
    std::array<float, kMaxCandidates> priors{};
    if (options_.exploration) {
        std::gamma_distribution<float> gamma(0.3F, 1.0F);
        float sum = 0.0F;
        for (std::size_t i = 0; i < root_->edges.size(); ++i) {
            priors[i] = gamma(rng_);
            sum += priors[i];
        }
        for (std::size_t i = 0; i < root_->edges.size(); ++i) {
            const float base = root_->edges[i].prior;
            priors[i] = sum > 0.0F ? 0.75F * base + 0.25F * priors[i] / sum : base;
        }
    }
    for (int i = 0; i < options_.simulations; ++i) {
        static_cast<void>(simulate(*root_, options_.exploration ? priors.data() : nullptr));
    }
    Decision result{};
    result.visits = root_->visits;
    const auto count = root_->edges.size();
    std::array<int, kMaxCandidates> visits{};
    std::optional<std::size_t> winning;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& edge = root_->edges[i];
        visits[i] = edge.visits;
        if (edge.candidate.next.outcome != Outcome::Ongoing &&
            outcome_value(edge.candidate.next.outcome, state.turn_player) == 1.0F &&
            (!winning || edge.visits > visits[*winning])) {
            winning = i;
        }
        if (policy) {
            result.policy.push_back({extract_features(edge.candidate.decision),
                                     static_cast<float>(edge.visits) / static_cast<float>(root_->visits)});
        }
    }
    std::size_t chosen;
    if (winning) {
        chosen = *winning;
    } else if (options_.exploration && state.turn_number <= 5) {
        chosen = std::discrete_distribution<std::size_t>(visits.begin(), visits.begin() + count)(rng_);
    } else {
        chosen = std::max_element(visits.begin(), visits.begin() + count) - visits.begin();
    }
    result.next = root_->edges[chosen].candidate.next;
    if (options_.record_actions) {
        for (std::size_t i = 0; i < count; ++i) {
            auto& edge = root_->edges[i];
            std::optional<float> value;
            if (edge.visits > 0) {
                value = edge.value_sum / static_cast<float>(edge.visits);
            }
            result.ranked.push_back({std::move(edge.candidate.actions), edge.visits, value, i == chosen});
        }
        std::stable_sort(result.ranked.begin(), result.ranked.end(),
                         [](const auto& a, const auto& b) { return a.visits > b.visits; });
    }
    auto next_root = std::move(root_->edges[chosen].child);
    root_ = std::move(next_root);
    return result;
}

Action choose_mulligan(const GameState& state, const Mlp& model, std::mt19937& rng) {
    constexpr int samples = 8;
    const int actor = state.turn_player;
    std::array<float, 1 << kOpeningHand> values{};
    for (int i = 0; i < samples; ++i) {
        auto sample = state;
        for (auto& player : sample.players) {
            std::shuffle(player.deck.begin() + player.deck_idx, player.deck.end(), rng);
        }
        const auto high = static_cast<std::uint64_t>(rng()) << 32;
        sample.rng_state = high | rng();
        for (const auto mask : legal_actions(sample)) {
            auto next = sample;
            apply_legal_action(next, mask);
            if (next.phase == Phase::Mulligan) {
                apply_legal_action(next, {ActionType::Mulligan});
            }
            if (actor == 1) {
                apply_legal_action(next, {ActionType::EndTurn});
            }
            values[mask.choice] += model.predict(extract_features(next)).value;
        }
    }
    const auto best = std::max_element(values.begin(), values.end()) - values.begin();
    return {ActionType::Mulligan, kNoTarget, kNoTarget, static_cast<std::uint8_t>(best)};
}

} // namespace svwb::wb
