#include "wb_search.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace svwb::wb {
namespace {

constexpr int kPlayerFeatures = 120;
constexpr int kPlayerScalars = 14;
constexpr int kBoardFeatures = 12;
constexpr int kHandZone = kPlayerScalars + kBoardLimit * kBoardFeatures;
constexpr int kCrestZone = kHandZone + kCardSlots + kDeckCardKinds;

GameState sample_hidden(const GameState& state, std::mt19937& rng) {
    auto sampled = state;
    const int actor = state.turn_player;
    auto& own = sampled.players[actor];
    auto& opponent = sampled.players[1 - actor];

    // The actor knows the remaining contents of their own deck. Sort before shuffling so
    // neither the actual order nor the order of modified copies affects the sample.
    using Card = std::pair<CardId, CardModifiers>;
    std::array<Card, kDeckCapacity> known{};
    const int own_remaining = own.deck_remaining();
    for (int i = 0; i < own_remaining; ++i) {
        known[i] = {own.deck[own.deck_idx + i], own.deck_modifiers[own.deck_idx + i]};
    }
    std::sort(known.begin(), known.begin() + own_remaining);
    std::shuffle(known.begin(), known.begin() + own_remaining, rng);
    own.deck = {};
    own.deck_modifiers = {};
    own.deck_idx = 0;
    own.deck_end = static_cast<std::uint8_t>(own_remaining);
    for (int i = 0; i < own_remaining; ++i) {
        std::tie(own.deck[i], own.deck_modifiers[i]) = known[i];
    }

    // Use the declared deck, public plays and known generation. Hidden discards, copies and
    // transforms can leave the precise distribution unknown, but public zone sizes are exact.
    std::array<CardId, std::max(2 * kDeckSize, kDeckCapacity + kHandLimit)> unseen{};
    int count = 0;
    for (std::size_t i = 0; i < opponent.public_deck.size(); ++i) {
        if (opponent.public_deck[i] && !(opponent.public_spent & (std::uint64_t{1} << i))) {
            unseen[count++] = opponent.public_deck[i];
        }
    }
    std::copy_n(opponent.public_generated.begin(), opponent.public_generated_count, unseen.begin() + count);
    count += opponent.public_generated_count;
    const auto remaining = opponent.deck_remaining();
    const auto needed = remaining + opponent.hand_count;
    const auto& base = opponent.public_deck[0] ? opponent.public_deck : kMirrorDeck;
    while (count < needed) {
        unseen[count++] = base[std::uniform_int_distribution<int>(0, kDeckSize - 1)(rng)];
    }
    std::shuffle(unseen.begin(), unseen.begin() + count, rng);
    opponent.hand = {};
    opponent.hand_modifiers = {};
    opponent.deck = {};
    opponent.deck_modifiers = {};
    opponent.deck_idx = 0;
    opponent.deck_end = static_cast<std::uint8_t>(remaining);
    std::copy_n(unseen.begin(), opponent.hand_count, opponent.hand.begin());
    std::copy_n(unseen.begin() + opponent.hand_count, remaining, opponent.deck.begin());
    const auto high = static_cast<std::uint64_t>(rng()) << 32;
    sampled.rng_state = high | rng();
    return sampled;
}

bool same_observation(GameState left, GameState right, int actor) {
    for (auto* state : {&left, &right}) {
        state->rng_state = 0;
        for (auto& player : state->players) {
            player.deck_end = static_cast<std::uint8_t>(player.deck_remaining());
            player.deck_idx = 0;
            player.deck = {};
            player.deck_modifiers = {};
        }
        state->players[1 - actor].hand = {};
        state->players[1 - actor].hand_modifiers = {};
    }
    return left == right;
}

float local_feature(CardId id, const MatchupFeatures& matchup) {
    return static_cast<float>(matchup.local_id(id) + 1) /
        static_cast<float>(std::max<std::size_t>(1, matchup.cards().size()));
}

void encode_player(float* out, const GameState& state, int seat, const MatchupFeatures* matchup) {
    const auto& p = state.players[seat];
    const bool legacy = !matchup || matchup->legacy_mirror();
    out[0] = std::max(0, static_cast<int>(p.leader_hp)) / 20.0F;
    out[1] = p.available_pp() / 11.0F;
    out[2] = p.max_pp / 10.0F;
    out[3] = p.ep / 2.0F;
    out[4] = std::min(64, static_cast<int>(p.shadows)) / 64.0F;
    out[5] = p.hand_count / 9.0F;
    out[6] = p.board_count / 5.0F;
    out[7] = p.deck_remaining() / 40.0F;
    out[8] = static_cast<float>(p.manual_evolved);
    out[9] = p.sep / 2.0F;
    out[10] = static_cast<float>(p.bonus_pp);
    out[11] = static_cast<float>(p.extra_pp_available);
    out[12] = p.extra_pp_uses / 2.0F;
    out[13] = static_cast<float>(seat == 0);
    for (int i = 0; i < p.board_count; ++i) {
        const auto& object = p.board[i];
        auto* row = out + kPlayerScalars + i * kBoardFeatures;
        row[0] = legacy ? (object.card_id - kFirstCard + 1) / static_cast<float>(kCardSlots)
                        : local_feature(object.card_id, *matchup);
        row[1] = object.atk / 10.0F;
        row[2] = object.hp / 10.0F;
        row[3] = object.max_hp / 10.0F;
        row[4] = object.countdown / 5.0F;
        row[5] = std::min(16, static_cast<int>(std::max(object.clue_count, object.earth_count))) / 16.0F;
        row[6] = static_cast<int>(object.evolution) / 2.0F;
        const bool can_attack = find_card(object.card_id)->kind == Kind::Follower && attack_ready(object);
        row[7] = static_cast<float>(can_attack);
        row[8] = static_cast<float>(can_attack && (object.readied || (object.abilities & FLAG_STORM)));
        row[9] = static_cast<float>((object.abilities & FLAG_WARD) != 0);
        row[10] = static_cast<float>(object.acted_this_turn);
        row[11] = static_cast<float>(object.silenced);
    }
    if (!legacy) {
        if (seat == state.turn_player) {
            for (int i = 0; i < p.hand_count; ++i) {
                const auto card = p.hand[i];
                const auto modifiers = p.hand_modifiers[i];
                out[kHandZone + 3 * i] = local_feature(card, *matchup);
                out[kHandZone + 3 * i + 1] = card_cost(*find_card(card), modifiers) / 10.0F;
                out[kHandZone + 3 * i + 2] = modifiers.spellboost / 20.0F;
            }
        }
        out[101] = p.plays_this_turn / 20.0F;
        out[102] = p.followers_entered / 64.0F;
        out[103] = p.crest_count / 5.0F;
        for (int i = 0; i < p.crest_count; ++i) {
            out[104 + 2 * i] = static_cast<int>(p.crests[i].id) / 255.0F;
            out[105 + 2 * i] = p.crests[i].countdown / 4.0F;
        }
        out[114] = p.faith_count / 5.0F;
        // Slots 115–118 stay zero. Deck size is public, the remaining cards are not.
        out[119] = static_cast<float>(p.attacked_leader_last_turn);
        return;
    }
    if (seat == state.turn_player) {
        for (int i = 0; i < p.hand_count; ++i) {
            if (p.hand[i] >= kFirstCard && p.hand[i] <= kLastForm) {
                out[kHandZone + p.hand[i] - kFirstCard] += 1.0F / kHandLimit;
            }
        }
    }
    for (int i = 0; i < p.crest_count; ++i) {
        const auto& crest = p.crests[i];
        const int index = static_cast<int>(crest.id) - 1;
        if (index >= 0 && index < 3) out[kCrestZone + index] = crest.countdown / 4.0F;
    }
}

// The Phase 4B mirror has immutable turn decks and cannot produce the Phase 5 modifiers.
struct LegacyPosition {
    static constexpr std::size_t word_count = 3 + kPlayers * (4 + 4 + 2 * kBoardLimit + kCrestLimit + 6 + 6);
    std::array<std::uint64_t, word_count> words{};

    bool operator==(const LegacyPosition&) const = default;
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

LegacyPosition legacy_key(const GameState& state) {
    LegacyPosition position;
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
        w[n++] = p.shadows | (static_cast<std::uint64_t>(p.leader_max_hp) << 16);
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

struct LegacyPositionHash {
    std::size_t operator()(const LegacyPosition& position) const noexcept {
        std::uint64_t hash = 0x0724DD61D03886F1;
        for (const auto word : position.words) {
            hash ^= word;
            hash *= 0x100000001b3;
        }
        return static_cast<std::size_t>(hash ^ (hash >> 32));
    }
};

// Hash frequently changing fields. Full state equality resolves collisions, including deck mutations.
struct PositionHash {
    std::size_t operator()(const GameState& state) const noexcept {
        std::uint64_t hash = state.rng_state;
        const auto mix = [&](std::uint64_t value) {
            hash = (hash ^ value) * 0x100000001b3ULL;
        };
        mix(state.next_creation_order | (static_cast<std::uint64_t>(state.turn_player) << 32) |
            (static_cast<std::uint64_t>(state.turn_number) << 40));
        mix(static_cast<unsigned>(state.outcome));
        for (const auto& player : state.players) {
            mix(player.deck_idx | (player.deck_end << 8) | (player.pp << 16) | (player.bonus_pp << 24));
            mix(static_cast<std::uint16_t>(player.leader_hp) | (static_cast<std::uint64_t>(player.shadows) << 16));
            mix(player.plays_this_turn | (static_cast<std::uint64_t>(player.followers_entered) << 16));
            for (int i = 0; i < player.hand_count; ++i) {
                const auto& card = player.hand_modifiers[i];
                mix(player.hand[i] | (static_cast<std::uint64_t>(card.spellboost) << 16) |
                    (static_cast<std::uint64_t>(card.fused_ply) << 24));
            }
            for (int i = 0; i < player.board_count; ++i) {
                const auto& card = player.board[i];
                mix(card.creation_order | (static_cast<std::uint64_t>(card.card_id) << 32) |
                    (static_cast<std::uint64_t>(card.abilities) << 48));
                mix(static_cast<std::uint16_t>(card.hp) | (static_cast<std::uint64_t>(card.atk) << 16) |
                    (static_cast<std::uint64_t>(card.countdown) << 32) |
                    (static_cast<std::uint64_t>(card.attacked_this_turn) << 40) |
                    (static_cast<std::uint64_t>(card.attacked_twice) << 41) |
                    (static_cast<std::uint64_t>(card.acted_this_turn) << 42) |
                    (static_cast<std::uint64_t>(card.attacked_thrice) << 43) |
                    (static_cast<std::uint64_t>(card.attack_limit) << 44));
            }
        }
        return static_cast<std::size_t>(hash ^ (hash >> 32));
    }
};

struct Candidate {
    GameState next;
    GameState decision;
    Prediction prediction;
    std::vector<Action> actions;
    float score;
    std::size_t order;
    attn::CandidateFeatures attention_features{};
};

constexpr std::array<CardId, 38> kCrestCards{
    0, 109, 111, 113, 165, 211, 214, 216, 228, 229, 263, 276, 247, 327, 286, 322, 316,
    384, 378, 398, 431, 471, 476, 442, 521, 491, 512, 514, 548, 508, 530, 545, 572, 556,
    581, 652, 654, 666,
};

int card_vocab(CardId id) {
    if (id < kFirstCard) return 0;
    const int index = static_cast<int>(id - kFirstCard + 1);
    return index < attn::kVocab ? index : attn::kVocab - 1;
}

int crest_vocab(CrestId id) {
    const auto index = static_cast<std::size_t>(id);
    if (index == 0 || index >= kCrestCards.size()) return 0;
    return card_vocab(kCrestCards[index]);
}

void write_keyword_attrs(float* out, std::uint16_t abilities) {
    out[21] = static_cast<float>((abilities & FLAG_STORM) != 0);
    out[22] = static_cast<float>((abilities & FLAG_RUSH) != 0);
    out[23] = static_cast<float>((abilities & FLAG_WARD) != 0);
    out[24] = static_cast<float>((abilities & kBane) != 0);
    out[25] = static_cast<float>((abilities & kAmbush) != 0);
    out[26] = static_cast<float>((abilities & kBarrier) != 0);
    out[27] = static_cast<float>((abilities & kAura) != 0);
    out[28] = static_cast<float>((abilities & kDrain) != 0);
    out[29] = static_cast<float>((abilities & (kIntimidate | kDoubleStrike)) != 0);
}

struct BoardSummary {
    int follower_count = 0;
    int amulet_count = 0;
    int total_atk = 0;
    int total_hp = 0;
    int max_atk = 0;
    int ward_count = 0;
    int ward_hp = 0;
    int evolved_count = 0;
    int super_evolved_count = 0;
    int special_keywords = 0;
    int attacked_count = 0;
    int earth_stacks = 0;
};

BoardSummary summarize_board(const PlayerState& player) {
    BoardSummary summary{};
    for (int i = 0; i < player.board_count; ++i) {
        const auto& obj = player.board[i];
        const auto* def = find_card(obj.card_id);
        const bool is_follower = def && def->kind == Kind::Follower;
        if (is_follower) {
            ++summary.follower_count;
            const int atk = std::max(0, static_cast<int>(obj.atk));
            const int hp = std::max(0, static_cast<int>(obj.hp));
            summary.total_atk += atk;
            summary.total_hp += hp;
            summary.max_atk = std::max(summary.max_atk, atk);
            if (obj.abilities & FLAG_WARD) {
                ++summary.ward_count;
                summary.ward_hp += hp;
            }
            if (obj.evolution != Evolution::None) ++summary.evolved_count;
            if (obj.evolution == Evolution::Super) ++summary.super_evolved_count;
            summary.special_keywords += ((obj.abilities & kBane) != 0) + ((obj.abilities & kBarrier) != 0) +
                                        ((obj.abilities & kAura) != 0) + ((obj.abilities & kAmbush) != 0);
            if (obj.attacked_this_turn) ++summary.attacked_count;
        } else {
            ++summary.amulet_count;
        }
        summary.earth_stacks += obj.earth_count;
    }
    return summary;
}

attn::CandidateFeatures extract_candidate_features_with_base(const GameState& initial, const BoardSummary& i_own_b,
                                                             const BoardSummary& i_opp_b, const GameState& decision,
                                                             const GameState& next) {
    attn::CandidateFeatures f{};
    const int actor = initial.turn_player;
    const int enemy = 1 - actor;
    const auto& i_own = initial.players[actor];
    const auto& i_opp = initial.players[enemy];
    const auto& d_own = decision.players[actor];
    const auto& d_opp = decision.players[enemy];
    const auto& n_own = next.players[actor];
    const auto& n_opp = next.players[enemy];

    const auto d_own_b = summarize_board(d_own);
    const auto n_own_b = (&d_own == &n_own) ? d_own_b : summarize_board(n_own);
    const auto n_opp_b = summarize_board(n_opp);

    const bool win = next.outcome != Outcome::Ongoing && outcome_value(next.outcome, actor) == 1.0F;
    const bool lose = next.outcome != Outcome::Ongoing && outcome_value(next.outcome, actor) == 0.0F;
    const bool direct_win = decision.outcome != Outcome::Ongoing && outcome_value(decision.outcome, actor) == 1.0F;

    f[0] = static_cast<float>(win);
    f[1] = static_cast<float>(lose);
    f[2] = static_cast<float>(direct_win);
    f[3] = static_cast<float>(n_opp.leader_hp <= 5);

    f[4] = std::max(0, static_cast<int>(n_own.leader_hp)) / 20.0F;
    f[5] = std::max(0, static_cast<int>(n_opp.leader_hp)) / 20.0F;
    f[6] = (static_cast<int>(n_own.leader_hp) - static_cast<int>(i_own.leader_hp)) / 20.0F;
    f[7] = (static_cast<int>(i_opp.leader_hp) - static_cast<int>(n_opp.leader_hp)) / 20.0F;
    f[8] = (static_cast<int>(d_own.leader_hp) - static_cast<int>(i_own.leader_hp)) / 20.0F;
    f[9] = (static_cast<int>(i_opp.leader_hp) - static_cast<int>(d_opp.leader_hp)) / 20.0F;
    f[10] = static_cast<float>(n_own.leader_barrier) - static_cast<float>(i_own.leader_barrier);
    f[11] = static_cast<float>(n_opp.leader_barrier) - static_cast<float>(i_opp.leader_barrier);

    const int rem_pp = d_own.available_pp();
    const int spent_pp = std::max(0, i_own.available_pp() - rem_pp);
    const int plays_delta = std::max(0, static_cast<int>(d_own.plays_this_turn) - static_cast<int>(i_own.plays_this_turn));
    const int entered_delta = std::max(0, static_cast<int>(d_own.followers_entered) - static_cast<int>(i_own.followers_entered));

    f[12] = rem_pp / 11.0F;
    f[13] = spent_pp / 11.0F;
    f[14] = static_cast<float>(rem_pp == 0);
    f[15] = static_cast<float>(std::max(0, static_cast<int>(i_own.ep) - static_cast<int>(d_own.ep)));
    f[16] = static_cast<float>(std::max(0, static_cast<int>(i_own.sep) - static_cast<int>(d_own.sep)));
    f[17] = static_cast<float>(d_own.manual_evolved && !i_own.manual_evolved);
    f[18] = static_cast<float>(std::max(0, static_cast<int>(i_own.extra_pp_uses) - static_cast<int>(d_own.extra_pp_uses)));
    f[19] = std::min(10, plays_delta) / 10.0F;
    f[20] = std::min(10, entered_delta) / 10.0F;
    f[21] = static_cast<float>(d_own.attacked_leader_this_turn);
    f[22] = static_cast<float>(d_own.acted_evidence || d_own.acted_amulet);
    f[23] = static_cast<float>(plays_delta == 0 && d_own_b.attacked_count == 0 && !d_own.acted_amulet && !d_own.acted_evidence);

    f[24] = std::min(9, std::max(0, i_own.deck_remaining() - d_own.deck_remaining())) / 9.0F;
    f[25] = d_own.hand_count / 9.0F;
    f[26] = (static_cast<int>(d_own.hand_count) - static_cast<int>(i_own.hand_count)) / 9.0F;
    f[27] = n_own.hand_count / 9.0F;
    f[28] = std::min(16, std::max(0, static_cast<int>(n_own.shadows) - static_cast<int>(i_own.shadows))) / 16.0F;
    f[29] = std::min(16, std::max(0, static_cast<int>(n_opp.shadows) - static_cast<int>(i_opp.shadows))) / 16.0F;
    f[30] = (static_cast<int>(n_own.max_pp) - static_cast<int>(i_own.max_pp)) / 10.0F;
    f[31] = (static_cast<int>(n_own.crest_count) - static_cast<int>(i_own.crest_count)) / 5.0F;

    f[32] = n_own_b.follower_count / 5.0F;
    f[33] = n_own_b.amulet_count / 5.0F;
    f[34] = (n_own_b.follower_count - i_own_b.follower_count) / 5.0F;
    f[35] = (n_own_b.amulet_count - i_own_b.amulet_count) / 5.0F;
    f[36] = n_own_b.total_atk / 30.0F;
    f[37] = n_own_b.total_hp / 30.0F;
    f[38] = (n_own_b.total_atk - i_own_b.total_atk) / 30.0F;
    f[39] = (n_own_b.total_hp - i_own_b.total_hp) / 30.0F;
    f[40] = n_own_b.max_atk / 15.0F;
    f[41] = n_own_b.ward_count / 5.0F;
    f[42] = n_own_b.ward_hp / 20.0F;
    f[43] = n_own_b.evolved_count / 5.0F;
    f[44] = n_own_b.super_evolved_count / 5.0F;
    f[45] = n_own_b.special_keywords / 10.0F;
    f[46] = d_own_b.attacked_count / 5.0F;
    f[47] = (n_own_b.earth_stacks - i_own_b.earth_stacks) / 10.0F;

    f[48] = n_opp_b.follower_count / 5.0F;
    f[49] = n_opp_b.amulet_count / 5.0F;
    f[50] = (i_opp_b.follower_count - n_opp_b.follower_count) / 5.0F;
    f[51] = (i_opp_b.amulet_count - n_opp_b.amulet_count) / 5.0F;
    f[52] = n_opp_b.total_atk / 30.0F;
    f[53] = n_opp_b.total_hp / 30.0F;
    f[54] = (i_opp_b.total_atk - n_opp_b.total_atk) / 30.0F;
    f[55] = (i_opp_b.total_hp - n_opp_b.total_hp) / 30.0F;
    f[56] = n_opp_b.max_atk / 15.0F;
    f[57] = n_opp_b.ward_count / 5.0F;
    f[58] = n_opp_b.ward_hp / 20.0F;
    f[59] = static_cast<float>(n_opp_b.follower_count == 0);
    f[60] = (n_own_b.total_atk - n_opp_b.total_atk) / 30.0F;
    f[61] = (n_own_b.total_hp - n_opp_b.total_hp) / 30.0F;
    f[62] = static_cast<float>(n_opp_b.total_atk >= n_own.leader_hp && n_own_b.ward_count == 0);
    f[63] = (static_cast<int>(n_own.leader_hp) - static_cast<int>(n_opp.leader_hp)) / 20.0F;
    return f;
}

struct BeamNode {
    GameState state;
    Prediction prediction;
    std::size_t id;
    bool certain;
};

struct Step {
    std::size_t parent;
    Action action;
};

struct BeamResult {
    std::vector<Candidate> candidates;
    std::optional<Candidate> forced;
};

template<bool Legacy>
BeamResult beam(const GameState& initial, const Mlp& model, std::size_t width, bool trace,
                const MatchupFeatures* matchup, bool ranked_only) {
    using Key = std::conditional_t<Legacy, LegacyPosition, GameState>;
    using Hash = std::conditional_t<Legacy, LegacyPositionHash, PositionHash>;
    const auto key_for = [](const GameState& state) -> decltype(auto) {
        if constexpr (Legacy) return legacy_key(state);
        else return state;
    };
    std::unordered_map<Key, bool, Hash> seen;
    std::unordered_set<Key, Hash> finished;
    seen.reserve(width * 24);
    finished.reserve(width * 5);
    seen.try_emplace(key_for(initial), true);
    std::vector<BeamNode> level{{initial, model.predict(extract_features(initial, matchup)), 0, true}}, next_level;
    level.reserve(width);
    next_level.reserve(width);
    std::vector<std::size_t> current{0};
    std::vector<std::size_t> order;
    std::vector<Candidate> results;
    std::vector<Step> steps;
    if (trace) {
        steps.push_back({});
    }
    std::size_t serial = 0;
    std::size_t completed = 0;
    const auto lower_rank = [](const Candidate& a, const Candidate& b) {
        return a.score != b.score ? a.score < b.score : a.order > b.order;
    };
    const auto record = [&](Candidate& candidate, std::size_t parent, Action action) {
        if (!trace) return;
        candidate.actions.push_back(action);
        for (auto i = parent; i != 0; i = steps[i].parent) {
            candidate.actions.push_back(steps[i].action);
        }
        std::reverse(candidate.actions.begin(), candidate.actions.end());
    };
    const auto force = [&](const GameState& next, const GameState& decision, std::size_t parent, Action action) {
        Candidate candidate{next, decision, {}, {}, 0.0F, 0};
        record(candidate, parent, action);
        return BeamResult{{}, std::move(candidate)};
    };
    const auto lethal = [&](const GameState& next, bool certain) {
        const int actor = initial.turn_player;
        return certain && next.turn_player == actor && next.outcome != Outcome::Ongoing &&
            next.players[1 - actor].leader_hp <= 0 && next.players[actor].leader_hp > 0 &&
            outcome_value(next.outcome, actor) == 1.0F;
    };
    const auto apply = [](GameState& next, Action action, bool certain) {
        if (certain) return apply_observed_action(next, action);
        apply_legal_action(next, action);
        return false;
    };
    const auto finish = [&](const auto& next, const auto& decision, Prediction prediction,
                            std::size_t parent, Action action) {
        if (!finished.insert(key_for(next)).second) {
            return;
        }
        const auto order = completed++;
        const float score = next.outcome != Outcome::Ongoing
            ? 3.0F * outcome_value(next.outcome, decision.turn_player) - 0.5F
            : prediction.value + 1.0F / (1.0F + std::exp(-prediction.policy_logit));
        Candidate* result = nullptr;
        if (ranked_only && results.size() == kRankedCandidates) {
            const auto worst = std::min_element(results.begin(), results.end(), lower_rank);
            if (score <= worst->score) return;
            *worst = Candidate{next, decision, prediction, {}, score, order};
            result = &*worst;
        } else {
            result = &results.emplace_back(Candidate{next, decision, prediction, {}, score, order});
        }
        record(*result, parent, action);
    };
    const auto better = [](const auto& a, const auto& b) {
        return a.prediction.value != b.prediction.value ? a.prediction.value > b.prediction.value : a.id < b.id;
    };
    for (int depth = 0; depth <= kBeamSteps && !current.empty(); ++depth) {
        next_level.clear();
        order.clear();
        const auto better_index = [&](std::size_t a, std::size_t b) { return better(next_level[a], next_level[b]); };
        for (const auto index : current) {
            const auto& node = level[index];
            auto ended = node.state;
            const Action end{ActionType::EndTurn};
            const bool certain_end = apply(ended, end, node.certain);
            if (lethal(ended, certain_end)) {
                return force(ended, node.state, node.id, end);
            }
            finish(ended, node.state, node.prediction, node.id, end);
            if (depth == kBeamSteps) {
                continue;
            }
            for (const auto action : legal_actions(node.state, {}, PlayCopies::Canonical)) {
                if (action.type == ActionType::EndTurn) {
                    continue;
                }
                auto next = node.state;
                const bool certain = apply(next, action, node.certain);
                if (lethal(next, certain)) {
                    return force(next, next, node.id, action);
                }
                const auto [existing, inserted] = seen.try_emplace(key_for(next), certain);
                if (!inserted && (!certain || existing->second)) {
                    continue;
                }
                existing->second = certain;
                const auto prediction = model.predict(extract_features(next, matchup));
                if (next.outcome != Outcome::Ongoing) {
                    finish(next, next, prediction, node.id, action);
                } else {
                    const auto id = ++serial;
                    if (trace) {
                        steps.push_back({node.id, action});
                    }
                    if (order.size() < width) {
                        order.push_back(next_level.size());
                        next_level.push_back({next, prediction, id, certain});
                        std::push_heap(order.begin(), order.end(), better_index);
                    } else if (prediction.value > next_level[order.front()].prediction.value) {
                        // The newest position loses a value tie. Move indices, not game states, in the heap.
                        std::pop_heap(order.begin(), order.end(), better_index);
                        next_level[order.back()] = {next, prediction, id, certain};
                        std::push_heap(order.begin(), order.end(), better_index);
                    }
                }
            }
        }
        std::sort_heap(order.begin(), order.end(), better_index);
        current.swap(order);
        level.swap(next_level);
    }
    if (ranked_only) {
        std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
    }
    return {std::move(results), std::nullopt};
}

template<bool Legacy>
BeamResult beam_attention(const GameState& initial, const attn::AttentionModel& model,
                          const attn::RootContext& root_context, float root_value, std::size_t width, bool trace,
                          bool ranked_only) {
    using Key = std::conditional_t<Legacy, LegacyPosition, GameState>;
    using Hash = std::conditional_t<Legacy, LegacyPositionHash, PositionHash>;
    const auto key_for = [](const GameState& state) -> decltype(auto) {
        if constexpr (Legacy) return legacy_key(state);
        else return state;
    };
    const int initial_actor = initial.turn_player;
    const auto i_own_b = summarize_board(initial.players[initial_actor]);
    const auto i_opp_b = summarize_board(initial.players[1 - initial_actor]);
    std::unordered_map<Key, bool, Hash> seen;
    std::unordered_set<Key, Hash> finished;
    seen.reserve(width * 24);
    finished.reserve(width * 5);
    seen.try_emplace(key_for(initial), true);
    std::vector<BeamNode> level{{initial, {root_value, 0.0F}, 0, true}}, next_level;
    level.reserve(width);
    next_level.reserve(width);
    std::vector<std::size_t> current{0};
    std::vector<std::size_t> order;
    std::vector<Candidate> results;
    std::vector<Step> steps;
    if (trace) {
        steps.push_back({});
    }
    std::size_t serial = 0;
    std::size_t completed = 0;
    const auto lower_rank = [](const Candidate& a, const Candidate& b) {
        return a.score != b.score ? a.score < b.score : a.order > b.order;
    };
    const auto record = [&](Candidate& candidate, std::size_t parent, Action action) {
        if (!trace) return;
        candidate.actions.push_back(action);
        for (auto i = parent; i != 0; i = steps[i].parent) {
            candidate.actions.push_back(steps[i].action);
        }
        std::reverse(candidate.actions.begin(), candidate.actions.end());
    };
    const auto force = [&](const GameState& next, const GameState& decision, std::size_t parent, Action action) {
        Candidate candidate{
            next, decision, {}, {}, 0.0F, 0,
            extract_candidate_features_with_base(initial, i_own_b, i_opp_b, decision, next),
        };
        record(candidate, parent, action);
        return BeamResult{{}, std::move(candidate)};
    };
    const auto lethal = [&](const GameState& next, bool certain) {
        const int actor = initial.turn_player;
        return certain && next.turn_player == actor && next.outcome != Outcome::Ongoing &&
            next.players[1 - actor].leader_hp <= 0 && next.players[actor].leader_hp > 0 &&
            outcome_value(next.outcome, actor) == 1.0F;
    };
    const auto apply = [](GameState& next, Action action, bool certain) {
        if (certain) return apply_observed_action(next, action);
        apply_legal_action(next, action);
        return false;
    };
    const auto finish = [&](const auto& next, const auto& decision, std::size_t parent, Action action) {
        if (!finished.insert(key_for(next)).second) {
            return;
        }
        const auto cand_features = extract_candidate_features_with_base(initial, i_own_b, i_opp_b, decision, next);
        const float policy_logit = model.policy_logit(root_context, cand_features.data());
        const Prediction prediction{root_value, policy_logit};
        const auto order_id = completed++;
        const float score = next.outcome != Outcome::Ongoing
            ? 3.0F * outcome_value(next.outcome, decision.turn_player) - 0.5F
            : prediction.value + 1.0F / (1.0F + std::exp(-prediction.policy_logit));
        Candidate* result = nullptr;
        if (ranked_only && results.size() == kRankedCandidates) {
            const auto worst = std::min_element(results.begin(), results.end(), lower_rank);
            if (score <= worst->score) return;
            *worst = Candidate{next, decision, prediction, {}, score, order_id, cand_features};
            result = &*worst;
        } else {
            result = &results.emplace_back(Candidate{next, decision, prediction, {}, score, order_id, cand_features});
        }
        record(*result, parent, action);
    };
    const auto better = [](const auto& a, const auto& b) {
        return a.prediction.value != b.prediction.value ? a.prediction.value > b.prediction.value : a.id < b.id;
    };
    for (int depth = 0; depth <= kBeamSteps && !current.empty(); ++depth) {
        next_level.clear();
        order.clear();
        const auto better_index = [&](std::size_t a, std::size_t b) { return better(next_level[a], next_level[b]); };
        for (const auto index : current) {
            const auto& node = level[index];
            auto ended = node.state;
            const Action end{ActionType::EndTurn};
            const bool certain_end = apply(ended, end, node.certain);
            if (lethal(ended, certain_end)) {
                return force(ended, node.state, node.id, end);
            }
            finish(ended, node.state, node.id, end);
            if (depth == kBeamSteps) {
                continue;
            }
            for (const auto action : legal_actions(node.state, {}, PlayCopies::Canonical)) {
                if (action.type == ActionType::EndTurn) {
                    continue;
                }
                auto next = node.state;
                const bool certain = apply(next, action, node.certain);
                if (lethal(next, certain)) {
                    return force(next, next, node.id, action);
                }
                const auto [existing, inserted] = seen.try_emplace(key_for(next), certain);
                if (!inserted && (!certain || existing->second)) {
                    continue;
                }
                existing->second = certain;
                if (next.outcome != Outcome::Ongoing) {
                    finish(next, next, node.id, action);
                } else {
                    const auto step_features = extract_candidate_features_with_base(initial, i_own_b, i_opp_b, next, next);
                    const float step_logit = model.policy_logit(root_context, step_features.data());
                    const Prediction prediction{1.0F / (1.0F + std::exp(-step_logit)), step_logit};
                    const auto id = ++serial;
                    if (trace) {
                        steps.push_back({node.id, action});
                    }
                    if (order.size() < width) {
                        order.push_back(next_level.size());
                        next_level.push_back({next, prediction, id, certain});
                        std::push_heap(order.begin(), order.end(), better_index);
                    } else if (prediction.value > next_level[order.front()].prediction.value) {
                        std::pop_heap(order.begin(), order.end(), better_index);
                        next_level[order.back()] = {next, prediction, id, certain};
                        std::push_heap(order.begin(), order.end(), better_index);
                    }
                }
            }
        }
        std::sort_heap(order.begin(), order.end(), better_index);
        current.swap(order);
        level.swap(next_level);
    }
    if (ranked_only) {
        std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
    }
    return {std::move(results), std::nullopt};
}

} // namespace

MatchupFeatures::MatchupFeatures(const Deck& first, const Deck& second) {
    legacy_ = std::is_permutation(first.begin(), first.end(), kMirrorDeck.begin()) &&
              std::is_permutation(second.begin(), second.end(), kMirrorDeck.begin());
    const auto add = [&](CardId id) {
        if (id && std::find(cards_.begin(), cards_.end(), id) == cards_.end()) cards_.push_back(id);
    };
    for (const auto& deck : {&first, &second}) {
        for (auto id : *deck) add(id);
    }
    const auto collect = [&](std::span<const Effect> effects) {
        for (const auto& effect : effects) {
            add(effect.card);
            add(effect.alternate_card);
            if (effect.operation == Operation::AddEarth) add(kEarthSigil);
            if (effect.operation == Operation::GrantCrest) {
                if (const auto* crest = find_crest(effect.crest)) {
                    for (const auto& step : crest->effects) {
                        add(step.card);
                        add(step.alternate_card);
                    }
                    if (effect.crest == CrestId::Kukishiro) add(kHolyFalcon);
                }
            }
        }
    };
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        const auto* card = find_card(cards_[i]);
        if (!card || !card->implemented) throw std::invalid_argument("Invalid card in matchup");
        add(card->crystal_form);
        add(card->accelerate_form);
        add(card->faith);
        for (const auto result : card->fusion_results) add(result);
        for (const auto material : card->fusion_materials) add(material);
        for (const auto related : card->related) {
            if (const auto* def = find_card(related); def && def->implemented) add(related);
        }
        collect(card->effects);
    }
    std::sort(cards_.begin(), cards_.end(), [](CardId a, CardId b) {
        const auto* left = find_card(a);
        const auto* right = find_card(b);
        return std::pair{left->official_id, a} < std::pair{right->official_id, b};
    });
    if (!cards_.empty()) {
        by_card_.assign(static_cast<std::size_t>(*std::max_element(cards_.begin(), cards_.end())) + 1, -1);
        for (std::size_t index = 0; index < cards_.size(); ++index) by_card_[cards_[index]] = static_cast<int>(index);
    }
}

int MatchupFeatures::local_id(CardId id) const {
    return id < by_card_.size() ? by_card_[id] : -1;
}

Features extract_features(const GameState& state, const MatchupFeatures* matchup) {
    Features features{};
    encode_player(features.data(), state, state.turn_player, matchup);
    encode_player(features.data() + kPlayerFeatures, state, 1 - state.turn_player, matchup);
    features[240] = state.turn_number / 40.0F;
    if (matchup && !matchup->legacy_mirror()) {
        features[241] = 1.0F;
        for (int offset = 0; offset < kPlayers; ++offset) {
            const auto& player = state.players[(state.turn_player + offset) % kPlayers];
            auto* row = features.data() + 242 + offset * 7;
            for (int i = 0; i < std::min(2, static_cast<int>(player.faith_count)); ++i) {
                row[3 * i] = local_feature(player.faiths[i], *matchup);
                row[3 * i + 1] = std::min(20, static_cast<int>(player.faith_values[i])) / 20.0F;
                row[3 * i + 2] = std::min(5, static_cast<int>(player.faith_upgrades[i])) / 5.0F;
            }
            row[6] = static_cast<float>(player.acted_evidence);
        }
    }
    return features;
}

attn::Tokens extract_attention_tokens(const GameState& state) {
    attn::Tokens tokens{};
    const int actor = state.turn_player;
    const auto& own = state.players[actor];
    const auto& opp = state.players[1 - actor];

    int own_earth = 0;
    for (int i = 0; i < own.board_count; ++i) {
        own_earth += own.board[i].earth_count;
    }

    auto& g = tokens.global;
    g[0] = state.turn_number / 40.0F;
    g[1] = static_cast<float>(actor == 0);
    g[2] = own.available_pp() / 11.0F;
    g[3] = own.max_pp / 10.0F;
    g[4] = static_cast<float>(own.bonus_pp);
    g[5] = static_cast<float>(own.extra_pp_available);
    g[6] = own.extra_pp_uses / 2.0F;
    g[7] = own.ep / 2.0F;
    g[8] = own.sep / 2.0F;
    g[9] = static_cast<float>(own.manual_evolved);
    g[10] = std::max(0, static_cast<int>(own.leader_hp)) / 20.0F;
    g[11] = std::max(0, static_cast<int>(own.leader_max_hp)) / 20.0F;
    g[12] = static_cast<float>(own.leader_barrier);
    g[13] = std::min(64, static_cast<int>(own.shadows)) / 64.0F;
    g[14] = std::min(20, static_cast<int>(own.plays_this_turn)) / 20.0F;
    g[15] = std::min(64, static_cast<int>(own.followers_entered)) / 64.0F;
    g[16] = std::min(16, own_earth) / 16.0F;
    g[17] = static_cast<float>(own.overflow());
    g[18] = own.hand_count / 9.0F;
    g[19] = own.board_count / 5.0F;
    g[20] = own.deck_remaining() / 40.0F;
    g[21] = (own.crest_count + own.faith_count) / 5.0F;
    g[22] = static_cast<float>(own.acted_evidence || own.acted_amulet);
    g[23] = std::max(0, static_cast<int>(opp.leader_hp)) / 20.0F;
    g[24] = std::max(0, static_cast<int>(opp.leader_max_hp)) / 20.0F;
    g[25] = static_cast<float>(opp.leader_barrier);
    g[26] = opp.max_pp / 10.0F;
    g[27] = (opp.ep + opp.sep) / 4.0F;
    g[28] = std::min(64, static_cast<int>(opp.shadows)) / 64.0F;
    g[29] = std::min(64, static_cast<int>(opp.followers_entered)) / 64.0F;
    g[30] = opp.hand_count / 9.0F;
    g[31] = opp.deck_remaining() / 40.0F;

    const int avail_pp = own.available_pp();
    for (int i = 0; i < std::min(attn::kHandTokens, static_cast<int>(own.hand_count)); ++i) {
        const CardId card = own.hand[i];
        const auto* def = find_card(card);
        if (!def) continue;
        const auto& mod = own.hand_modifiers[i];
        const int slot = attn::kHandOffset + i;
        tokens.vocab[slot] = card_vocab(card);
        float* a = tokens.attrs.data() + slot * attn::kAttrDim;
        a[0] = 1.0F; // Own hand zone
        a[5 + static_cast<int>(def->kind)] = 1.0F;
        const int eff_cost = card_cost(*def, mod);
        int min_play_cost = eff_cost;
        if (def->crystal_form) {
            if (const auto* crystal = find_card(def->crystal_form)) min_play_cost = std::min(min_play_cost, crystal->cost);
        }
        if (def->accelerate_form) {
            if (const auto* accel = find_card(def->accelerate_form)) min_play_cost = std::min(min_play_cost, accel->cost);
        }
        a[9] = eff_cost / 10.0F;
        a[10] = def->cost / 10.0F;
        a[11] = static_cast<float>(def->may_play_from_hand && min_play_cost <= avail_pp);
        if (def->kind == Kind::Follower) {
            const float atk = std::max(0, def->atk + mod.attack) / 10.0F;
            const float hp = std::max(0, def->hp + mod.defense) / 10.0F;
            a[12] = atk;
            a[13] = hp;
            a[14] = hp;
        }
        write_keyword_attrs(a, static_cast<std::uint16_t>(def->abilities | mod.abilities));
        a[30] = def->countdown / 5.0F;
        a[31] = std::min(20, static_cast<int>(mod.spellboost) + std::popcount(mod.fused_materials)) / 20.0F;
    }

    const auto encode_board = [&](const PlayerState& player, int zone_bit, int base_slot, bool is_own) {
        for (int i = 0; i < std::min(attn::kBoardTokens, static_cast<int>(player.board_count)); ++i) {
            const auto& obj = player.board[i];
            const auto* def = find_card(obj.card_id);
            if (!def) continue;
            const int slot = base_slot + i;
            tokens.vocab[slot] = card_vocab(obj.card_id);
            float* a = tokens.attrs.data() + slot * attn::kAttrDim;
            a[zone_bit] = 1.0F;
            a[5 + static_cast<int>(def->kind)] = 1.0F;
            a[9] = def->cost / 10.0F;
            a[10] = def->cost / 10.0F;
            a[11] = static_cast<float>(is_own && def->act_cost >= 0 && !obj.acted_this_turn && def->act_cost <= avail_pp);
            a[12] = std::max(0, static_cast<int>(obj.atk)) / 10.0F;
            a[13] = std::max(0, static_cast<int>(obj.hp)) / 10.0F;
            a[14] = std::max(0, static_cast<int>(obj.max_hp)) / 10.0F;
            a[15] = static_cast<float>(obj.evolution == Evolution::Normal);
            a[16] = static_cast<float>(obj.evolution == Evolution::Super);
            a[17] = static_cast<float>(obj.silenced);
            const bool can_atk = def->kind == Kind::Follower && attack_ready(obj);
            a[18] = static_cast<float>(can_atk);
            a[19] = static_cast<float>(can_atk && (obj.readied || (obj.abilities & FLAG_STORM)));
            a[20] = static_cast<float>(obj.acted_this_turn || obj.attacked_this_turn);
            write_keyword_attrs(a, obj.abilities);
            a[30] = obj.countdown / 5.0F;
            a[31] = std::min(16, static_cast<int>(std::max(obj.clue_count, obj.earth_count))) / 16.0F;
        }
    };
    encode_board(own, 1, attn::kOwnBoardOffset, true);
    encode_board(opp, 2, attn::kEnemyBoardOffset, false);

    const auto encode_crests_and_faiths = [&](const PlayerState& player, int zone_bit, int base_slot) {
        int used = 0;
        for (int i = 0; i < player.crest_count && used < 5; ++i) {
            const auto& crest = player.crests[i];
            const int vocab = crest_vocab(crest.id);
            if (vocab <= 0) continue;
            const int slot = base_slot + used++;
            tokens.vocab[slot] = vocab;
            float* a = tokens.attrs.data() + slot * attn::kAttrDim;
            a[zone_bit] = 1.0F;
            a[8] = 1.0F;
            a[30] = crest.countdown / 5.0F;
        }
        for (int i = 0; i < player.faith_count && used < 5; ++i) {
            const int vocab = card_vocab(player.faiths[i]);
            if (vocab <= 0) continue;
            const int slot = base_slot + used++;
            tokens.vocab[slot] = vocab;
            float* a = tokens.attrs.data() + slot * attn::kAttrDim;
            a[zone_bit] = 1.0F;
            a[8] = 1.0F;
            a[13] = std::min(20, static_cast<int>(player.faith_values[i])) / 20.0F;
            a[14] = std::min(5, static_cast<int>(player.faith_upgrades[i])) / 5.0F;
            a[31] = 1.0F;
        }
    };
    encode_crests_and_faiths(own, 3, attn::kCrestOffset);
    encode_crests_and_faiths(opp, 4, attn::kCrestOffset + 5);
    return tokens;
}

attn::CandidateFeatures extract_candidate_features(const GameState& initial, const GameState& decision,
                                                   const GameState& next) {
    const int actor = initial.turn_player;
    return extract_candidate_features_with_base(
        initial, summarize_board(initial.players[actor]), summarize_board(initial.players[1 - actor]), decision, next);
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
    bool context_ready = false;
    float root_value = 0.5F;
    attn::RootContext attention_context{};
    std::vector<Edge> edges;
    std::unique_ptr<Candidate> forced;
};

Search::Search(const Mlp& first, const Mlp& second, std::mt19937& rng, SearchOptions options,
               const MatchupFeatures* matchup)
    : models_{&first, &second}, rng_(rng), options_(options), matchup_(matchup) {
    if (options.simulations < 1) {
        throw std::invalid_argument("Search needs a positive simulation count");
    }
}

Search::Search(const attn::AttentionModel& first, const attn::AttentionModel& second, std::mt19937& rng,
               SearchOptions options, const MatchupFeatures* matchup)
    : attention_models_{&first, &second}, rng_(rng), options_(options), matchup_(matchup) {
    if (options.simulations < 1) {
        throw std::invalid_argument("Search needs a positive simulation count");
    }
}

Search::Search(ModelRef first, ModelRef second, std::mt19937& rng, SearchOptions options,
               const MatchupFeatures* matchup)
    : models_{first.mlp, second.mlp}, attention_models_{first.attention, second.attention},
      rng_(rng), options_(options), matchup_(matchup) {
    if (options.simulations < 1 || (!first.mlp && !first.attention) || (!second.mlp && !second.attention)) {
        throw std::invalid_argument("Search needs valid models and a positive simulation count");
    }
}

Search::~Search() = default;

void Search::set_determinization(bool enabled) {
    if (options_.determinization == enabled) return;
    options_.determinization = enabled;
    root_.reset();
}

void Search::expand(Node& node, std::size_t width, bool trace) {
    const int player = node.state.turn_player;
    const bool legacy = matchup_ && matchup_->legacy_mirror();
    BeamResult result;
    if (attention_models_[player]) {
        const auto& model = *attention_models_[player];
        if (!node.context_ready) {
            node.root_value = model.evaluate_root(extract_attention_tokens(node.state), node.attention_context);
            node.context_ready = true;
        }
        result = legacy
            ? beam_attention<true>(node.state, model, node.attention_context, node.root_value, width, trace, !options_.exploration)
            : beam_attention<false>(node.state, model, node.attention_context, node.root_value, width, trace, !options_.exploration);
    } else {
        const auto& model = *models_[player];
        result = legacy
            ? beam<true>(node.state, model, width, trace, matchup_, !options_.exploration)
            : beam<false>(node.state, model, width, trace, matchup_, !options_.exploration);
    }
    if (result.forced) {
        node.forced = std::make_unique<Candidate>(std::move(*result.forced));
        return;
    }
    auto& candidates = result.candidates;
    std::vector<std::size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), 0);
    const auto limit = std::min(candidates.size(), static_cast<std::size_t>(kRankedCandidates));
    std::partial_sort(order.begin(), order.begin() + limit, order.end(), [&](const auto a, const auto b) {
        return candidates[a].score != candidates[b].score ? candidates[a].score > candidates[b].score : a < b;
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
    if (node.edges.empty() && !node.forced) {
        if (!node.evaluated) {
            node.evaluated = true;
            float estimate = 0.5F;
            if (attention_models_[player]) {
                estimate = attention_models_[player]->evaluate_root(extract_attention_tokens(node.state), node.attention_context);
                node.root_value = estimate;
                node.context_ready = true;
            } else {
                estimate = models_[player]->predict(extract_features(node.state, matchup_)).value;
            }
            return player == 0 ? std::array{estimate, 1.0F - estimate} : std::array{1.0F - estimate, estimate};
        }
        expand(node, kBeamWidth, options_.record_actions && !options_.determinization);
    }
    if (node.forced) {
        return {outcome_value(node.forced->next.outcome, 0), outcome_value(node.forced->next.outcome, 1)};
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

Decision Search::choose_sample(const GameState& state, bool policy) {
    if (state.phase != Phase::Main || state.outcome != Outcome::Ongoing) {
        throw std::invalid_argument("Search requires an active main-phase turn");
    }
    const bool widen = options_.exploration && std::bernoulli_distribution(0.01)(rng_);
    if (!root_ || root_->state != state || widen) {
        root_ = std::make_unique<Node>(state);
    }
    if (root_->edges.empty() && !root_->forced) {
        expand(*root_, widen ? 2 * kBeamWidth : kBeamWidth, options_.record_actions || options_.determinization);
    }
    if (root_->forced) {
        auto& forced = *root_->forced;
        Decision result{};
        result.next = forced.next;
        result.forced = true;
        if (policy) {
            if (attention_models_[state.turn_player]) {
                result.attention_policy.push_back({forced.attention_features, 1.0F});
            } else {
                result.policy.push_back({extract_features(forced.decision, matchup_), 1.0F});
            }
        }
        result.actions = forced.actions;
        if (options_.record_actions) result.ranked.push_back({std::move(forced.actions), 0, 1.0F, true});
        root_.reset();
        return result;
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
    for (std::size_t i = 0; i < count; ++i) {
        const auto& edge = root_->edges[i];
        visits[i] = edge.visits;
        if (policy) {
            const float prob = static_cast<float>(edge.visits) / static_cast<float>(root_->visits);
            if (attention_models_[state.turn_player]) {
                result.attention_policy.push_back({edge.candidate.attention_features, prob});
            } else {
                result.policy.push_back({extract_features(edge.candidate.decision, matchup_), prob});
            }
        }
    }
    std::size_t chosen;
    if (options_.exploration && state.turn_number <= 5) {
        chosen = std::discrete_distribution<std::size_t>(visits.begin(), visits.begin() + count)(rng_);
    } else {
        chosen = std::max_element(visits.begin(), visits.begin() + count) - visits.begin();
    }
    result.next = root_->edges[chosen].candidate.next;
    result.actions = root_->edges[chosen].candidate.actions;
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
    if (options_.determinization) {
        root_.reset();
    } else {
        auto next_root = std::move(root_->edges[chosen].child);
        root_ = std::move(next_root);
    }
    return result;
}

Decision Search::choose(const GameState& state, bool policy) {
    if (!options_.determinization) return choose_sample(state, policy);
    auto sampled = sample_hidden(state, rng_);
    auto result = choose_sample(sampled, policy);
    if (options_.record_actions) result.sampled_root = std::make_unique<GameState>(sampled);
    return result;
}

Decision Search::play(const GameState& state, bool policy) {
    if (!options_.determinization) return choose_sample(state, policy);
    auto actual = state;
    const int actor = state.turn_player;
    const auto active = [&] { return actual.outcome == Outcome::Ongoing && actual.turn_player == actor; };
    Decision first;
    std::vector<Action> played;
    while (active() && played.size() < kBeamSteps) {
        const bool opening = played.empty();
        auto sampled = sample_hidden(actual, rng_);
        auto plan = choose_sample(sampled, opening && policy);
        if (opening) {
            first = std::move(plan);
            if (options_.record_actions) first.sampled_root = std::make_unique<GameState>(sampled);
        }
        const auto& actions = opening ? first.actions : plan.actions;
        const auto before = played.size();
        for (const auto action : actions) {
            if (played.size() == kBeamSteps || !apply_action(actual, action)) break;
            played.push_back(action);
            if (!active()) break;
            apply_legal_action(sampled, action);
            if (!same_observation(actual, sampled, actor)) break;
        }
        if (played.size() == before) break;
        if (active()) first.replanned = true;
    }
    if (active()) {
        const Action end{ActionType::EndTurn};
        apply_legal_action(actual, end);
        played.push_back(end);
        first.replanned = true;
    }
    first.actions = std::move(played);
    first.next = actual;
    return first;
}

Action choose_mulligan(const GameState& state, const Mlp& model, std::mt19937& rng, const MatchupFeatures* matchup) {
    constexpr int samples = 8;
    const int actor = state.turn_player;
    std::array<float, 1 << kOpeningHand> values{};
    for (int i = 0; i < samples; ++i) {
        auto sample = state;
        for (auto& player : sample.players) {
            std::shuffle(player.deck.begin() + player.deck_idx, player.deck.begin() + player.deck_end, rng);
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
            values[mask.choice] += model.predict(extract_features(next, matchup)).value;
        }
    }
    const auto best = std::max_element(values.begin(), values.end()) - values.begin();
    return {ActionType::Mulligan, kNoTarget, kNoTarget, static_cast<std::uint8_t>(best)};
}

Action choose_mulligan(const GameState& state, const attn::AttentionModel& model, std::mt19937& rng,
                       const MatchupFeatures*) {
    constexpr int samples = 8;
    const int actor = state.turn_player;
    std::array<float, 1 << kOpeningHand> values{};
    for (int i = 0; i < samples; ++i) {
        auto sample = state;
        for (auto& player : sample.players) {
            std::shuffle(player.deck.begin() + player.deck_idx, player.deck.begin() + player.deck_end, rng);
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
            values[mask.choice] += model.value(extract_attention_tokens(next));
        }
    }
    const auto best = std::max_element(values.begin(), values.end()) - values.begin();
    return {ActionType::Mulligan, kNoTarget, kNoTarget, static_cast<std::uint8_t>(best)};
}

Action choose_mulligan(const GameState& state, ModelRef model, std::mt19937& rng, const MatchupFeatures* matchup) {
    return model.attention ? choose_mulligan(state, *model.attention, rng, matchup)
                           : choose_mulligan(state, *model.mlp, rng, matchup);
}

} // namespace svwb::wb
