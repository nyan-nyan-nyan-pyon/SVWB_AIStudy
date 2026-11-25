#include "wb_search.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
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
constexpr std::size_t kMaxChancePlans = 24;

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

bool is_non_faith_card(CardId id) {
    const auto* def = find_card(id);
    return def != nullptr && def->kind != Kind::Faith;
}

float legacy_outcome_value(Outcome outcome, int seat) {
    return static_cast<float>(seat == 0 ? outcome == Outcome::FirstWins : outcome == Outcome::SecondWins);
}

// Sanitize all hidden information not observable by `observer` into deterministic dummy placeholders.
// Never reads `opponent.hand`, `opponent.deck`, or `own.deck` order/contents.
GameState make_clean_observation_state(const GameState& state, int observer) {
    GameState clean = state;
    clean.rng_state = 0x53565742'4F425330ULL;
    auto& own = clean.players[observer];
    const int own_rem = own.deck_remaining();
    own.deck = {};
    own.deck_modifiers = {};
    own.deck_idx = 0;
    own.deck_end = static_cast<std::uint8_t>(own_rem);
    std::fill_n(own.deck.begin(), own_rem, kWingedStatue);

    auto& opp = clean.players[1 - observer];
    const int opp_rem = opp.deck_remaining();
    opp.hand = {};
    opp.hand_modifiers = {};
    std::fill_n(opp.hand.begin(), opp.hand_count, kWingedStatue);
    opp.deck = {};
    opp.deck_modifiers = {};
    opp.deck_idx = 0;
    opp.deck_end = static_cast<std::uint8_t>(opp_rem);
    std::fill_n(opp.deck.begin(), opp_rem, kWingedStatue);
    return clean;
}

// Build a deterministic seed from only the part of `state` observable by `observer`.
std::uint64_t observation_hash_for(const GameState& state, int observer) {
    const GameState clean = make_clean_observation_state(state, observer);
    std::uint64_t h = 0xCBF29CE484222325ULL;
    const auto mix = [&](std::uint64_t v) {
        h ^= v;
        h *= 0x100000001B3ULL;
    };
    mix(clean.next_creation_order);
    mix(clean.turn_player | (static_cast<std::uint64_t>(clean.turn_number) << 8));
    for (int s = 0; s < kPlayers; ++s) {
        const auto& p = clean.players[s];
        mix(static_cast<std::uint16_t>(p.leader_hp) | (static_cast<std::uint64_t>(p.pp) << 16) |
            (static_cast<std::uint64_t>(p.max_pp) << 24) | (static_cast<std::uint64_t>(p.hand_count) << 32) |
            (static_cast<std::uint64_t>(p.deck_remaining()) << 40) | (static_cast<std::uint64_t>(p.ep) << 48) |
            (static_cast<std::uint64_t>(p.sep) << 56));
        mix(p.public_spent);
        if (s == observer) {
            for (int i = 0; i < p.hand_count; ++i) {
                mix(p.hand[i] | (static_cast<std::uint64_t>(p.hand_modifiers[i].spellboost) << 16) |
                    (static_cast<std::uint64_t>(static_cast<std::uint8_t>(p.hand_modifiers[i].cost)) << 24));
            }
        }
        for (int i = 0; i < p.board_count; ++i) {
            const auto& b = p.board[i];
            mix(b.creation_order | (static_cast<std::uint64_t>(b.card_id) << 32));
            mix(static_cast<std::uint16_t>(b.atk) | (static_cast<std::uint64_t>(static_cast<std::uint16_t>(b.hp)) << 16) |
                (static_cast<std::uint64_t>(b.abilities) << 32) | (static_cast<std::uint64_t>(b.countdown) << 48));
        }
    }
    return h;
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
        std::uint64_t hash = 0x0724DD61D03886F1ULL;
        for (const auto word : position.words) {
            hash ^= word;
            hash *= 0x100000001b3ULL;
        }
        return static_cast<std::size_t>(hash ^ (hash >> 32));
    }
};

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
    GameState next{};
    GameState decision{};
    Prediction prediction{};
    std::vector<Action> actions;
    float score = 0.0F;
    std::size_t order = 0;
    attn::CandidateFeatures attention_features{};
    attn::CandidateSparseDiff attention_diff{};
    ActionPlan plan{};
    LethalStatus lethal_status = LethalStatus::NotFound;
    LethalEvidence lethal_evidence{};
};

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

int sum_earth_stacks(const PlayerState& player) noexcept {
    int sum = 0;
    for (int i = 0; i < player.board_count; ++i) sum += player.board[i].earth_count;
    return sum;
}

int sum_evidence_stacks(const PlayerState& player) noexcept {
    int sum = 0;
    for (int i = 0; i < player.board_count; ++i) sum += player.board[i].clue_count;
    return sum;
}

void encode_card_tokens(const GameState& state, attn::Tokens& tokens) {
    const int actor = state.turn_player;
    const auto& own = state.players[actor];
    const auto& opp = state.players[1 - actor];
    const int avail_pp = own.available_pp();

    for (int i = 0; i < std::min(attn::kHandTokens, static_cast<int>(own.hand_count)); ++i) {
        const CardId card = own.hand[i];
        const auto* def = find_card(card);
        if (!def) continue;
        const auto& mod = own.hand_modifiers[i];
        const int slot = attn::kHandOffset + i;
        tokens.vocab[slot] = card_vocab(card);
        float* a = tokens.attrs.data() + slot * attn::kAttrDim;
        a[0] = 1.0F;
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
}

void encode_global_player_v3(float* out, const PlayerState& p, const GlobalPlayerContext& ctx) {
    out[0] = std::max(0, static_cast<int>(p.leader_hp)) / 20.0F;
    out[1] = std::max(0, static_cast<int>(p.leader_max_hp)) / 20.0F;
    out[2] = p.leader_barrier ? 1.0F : 0.0F;
    out[3] = ctx.is_active_turn ? (p.available_pp() / 11.0F) : 0.0F;
    out[4] = p.max_pp / 10.0F;
    out[5] = static_cast<float>(p.bonus_pp);
    out[6] = p.extra_pp_available ? 1.0F : 0.0F;
    out[7] = p.extra_pp_uses / 2.0F;
    out[8] = next_pp_budget(p, ctx.seat, ctx.next_pp_refresh_turn) / 11.0F;
    out[9] = p.ep / 2.0F;
    out[10] = p.sep / 2.0F;
    out[11] = ctx.is_active_turn ? (p.manual_evolved ? 1.0F : 0.0F) : 0.0F;
    out[12] = (ctx.next_action_turn >= (ctx.seat == 0 ? 5 : 4)) ? 1.0F : 0.0F;
    out[13] = (ctx.next_action_turn >= (ctx.seat == 0 ? 7 : 6)) ? 1.0F : 0.0F;
    out[14] = p.shadows / 64.0F;
    out[15] = ctx.is_active_turn ? (p.plays_this_turn / 20.0F) : 0.0F;
    out[16] = p.followers_entered / 64.0F;
    out[17] = sum_earth_stacks(p) / 16.0F;
    out[18] = p.overflow() ? 1.0F : 0.0F;
    out[19] = p.hand_count / 9.0F;
    out[20] = p.board_count / 5.0F;
    out[21] = p.deck_remaining() / 40.0F;
    out[22] = (p.crest_count + p.faith_count) / 5.0F;
    out[23] = ctx.is_active_turn ? ((p.acted_evidence || p.acted_amulet) ? 1.0F : 0.0F) : 0.0F;
    out[24] = p.attacked_leader_last_turn ? 1.0F : 0.0F;
    out[25] = sum_evidence_stacks(p) / 16.0F;
    out[26] = p.crest_count / 5.0F;
    out[27] = p.faith_count / 5.0F;
}

struct ZoneVocabSnapshot {
    std::array<std::array<std::uint16_t, 9>, attn::kDiffZones> ids{};
    std::array<int, attn::kDiffZones> counts{};
};

ZoneVocabSnapshot snapshot_zones(const GameState& state, int actor) {
    ZoneVocabSnapshot snap{};
    const auto& own = state.players[actor];
    const auto& opp = state.players[1 - actor];

    const int hand_n = std::min(attn::kHandTokens, static_cast<int>(own.hand_count));
    for (int i = 0; i < hand_n; ++i) {
        const int v = card_vocab(own.hand[i]);
        if (v > 0) snap.ids[0][snap.counts[0]++] = static_cast<std::uint16_t>(v);
    }

    const int own_b_n = std::min(attn::kBoardTokens, static_cast<int>(own.board_count));
    for (int i = 0; i < own_b_n; ++i) {
        const int v = card_vocab(own.board[i].card_id);
        if (v > 0) snap.ids[1][snap.counts[1]++] = static_cast<std::uint16_t>(v);
    }

    const int opp_b_n = std::min(attn::kBoardTokens, static_cast<int>(opp.board_count));
    for (int i = 0; i < opp_b_n; ++i) {
        const int v = card_vocab(opp.board[i].card_id);
        if (v > 0) snap.ids[2][snap.counts[2]++] = static_cast<std::uint16_t>(v);
    }

    const auto gather_cf = [&](const PlayerState& p, int z) {
        int used = 0;
        for (int i = 0; i < p.crest_count && used < 5; ++i) {
            const int v = crest_vocab(p.crests[i].id);
            if (v > 0) {
                snap.ids[z][snap.counts[z]++] = static_cast<std::uint16_t>(v);
                ++used;
            }
        }
        for (int i = 0; i < p.faith_count && used < 5; ++i) {
            const int v = card_vocab(p.faiths[i]);
            if (v > 0) {
                snap.ids[z][snap.counts[z]++] = static_cast<std::uint16_t>(v);
                ++used;
            }
        }
    };
    gather_cf(own, 3);
    gather_cf(opp, 4);
    return snap;
}

// Compute 5-zone sparse signed diff strictly from initial -> decision (never touches `next`).
attn::CandidateSparseDiff extract_sparse_diff_with_base(const ZoneVocabSnapshot& initial_snap, int actor,
                                                        const GameState& decision) {
    const auto dec_snap = snapshot_zones(decision, actor);
    attn::CandidateSparseDiff out{};
    int cursor = 0;
    out.zone_offsets[0] = 0;

    for (int z = 0; z < attn::kDiffZones; ++z) {
        std::array<attn::SparseDiffPair, 18> accum{};
        int accum_count = 0;
        const auto add = [&](std::uint16_t v, int delta) {
            if (v == 0) return; // Explicit vocab_id == 0 skip
            for (int i = 0; i < accum_count; ++i) {
                if (accum[i].vocab_id == v) {
                    accum[i].signed_count = static_cast<std::int8_t>(accum[i].signed_count + delta);
                    return;
                }
            }
            assert(accum_count < static_cast<int>(accum.size()));
            accum[accum_count++] = {v, static_cast<std::int8_t>(delta), 0};
        };

        for (int i = 0; i < dec_snap.counts[z]; ++i) add(dec_snap.ids[z][i], +1);
        for (int i = 0; i < initial_snap.counts[z]; ++i) add(initial_snap.ids[z][i], -1);

        int kept = 0;
        for (int i = 0; i < accum_count; ++i) {
            if (accum[i].signed_count != 0 && accum[i].vocab_id != 0) {
                accum[kept++] = accum[i];
            }
        }
        std::sort(accum.begin(), accum.begin() + kept,
                  [](const auto& a, const auto& b) { return a.vocab_id < b.vocab_id; });
        assert(kept <= attn::kZoneMaxDiff[z]);
        assert(cursor + kept <= attn::kMaxSparseDiffEntries);
        if (kept > attn::kZoneMaxDiff[z] || cursor + kept > attn::kMaxSparseDiffEntries) {
            throw std::logic_error("CandidateSparseDiff capacity exceeded");
        }
        for (int i = 0; i < kept; ++i) {
            out.entries[cursor++] = accum[i];
        }
        out.zone_offsets[z + 1] = static_cast<std::uint8_t>(cursor);
    }
    return out;
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

attn::CandidateFeatures extract_candidate_features_with_base(
    const GameState& initial, const BoardSummary& i_own_b, const BoardSummary& i_opp_b,
    const GameState& decision, const GameState& next, bool legacy_reward = false) {
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

    const auto eval_out = [&](Outcome o, int s) {
        return legacy_reward ? legacy_outcome_value(o, s) : outcome_value(o, s);
    };
    const bool win = next.outcome != Outcome::Ongoing && eval_out(next.outcome, actor) == 1.0F;
    const bool lose = next.outcome != Outcome::Ongoing && eval_out(next.outcome, actor) == 0.0F;
    const bool direct_win = decision.outcome != Outcome::Ongoing && eval_out(decision.outcome, actor) == 1.0F;

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

// ============================================================================
// Legacy v2 Beam (preserved strictly for A0 / SearchVersion::LegacyV2)
// ============================================================================

template<bool Legacy>
BeamResult beam_legacy(const GameState& initial, const Mlp& model, std::size_t width, bool trace,
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
    if (trace) steps.push_back({});
    std::size_t serial = 0;
    std::size_t completed = 0;
    const auto lower_rank = [](const Candidate& a, const Candidate& b) {
        return a.score != b.score ? a.score < b.score : a.order > b.order;
    };
    const auto record = [&](Candidate& candidate, std::size_t parent, Action action) {
        if (!trace) return;
        candidate.actions.push_back(action);
        for (auto i = parent; i != 0; i = steps[i].parent) candidate.actions.push_back(steps[i].action);
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
            legacy_outcome_value(next.outcome, actor) == 1.0F;
    };
    const auto apply = [](GameState& next, Action action, bool certain) {
        if (certain) return apply_observed_action(next, action);
        apply_legal_action(next, action);
        return false;
    };
    const auto finish = [&](const auto& next, const auto& decision, Prediction prediction,
                            std::size_t parent, Action action) {
        if (!finished.insert(key_for(next)).second) return;
        const auto ord = completed++;
        const float score = next.outcome != Outcome::Ongoing
            ? 3.0F * legacy_outcome_value(next.outcome, decision.turn_player) - 0.5F
            : prediction.value + 1.0F / (1.0F + std::exp(-prediction.policy_logit));
        Candidate* result = nullptr;
        if (ranked_only && results.size() == kRankedCandidates) {
            const auto worst = std::min_element(results.begin(), results.end(), lower_rank);
            if (score <= worst->score) return;
            *worst = Candidate{next, decision, prediction, {}, score, ord};
            result = &*worst;
        } else {
            result = &results.emplace_back(Candidate{next, decision, prediction, {}, score, ord});
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
            if (lethal(ended, certain_end)) return force(ended, node.state, node.id, end);
            finish(ended, node.state, node.prediction, node.id, end);
            if (depth == kBeamSteps) continue;
            for (const auto action : legal_actions(node.state, {}, PlayCopies::Canonical)) {
                if (action.type == ActionType::EndTurn) continue;
                auto next = node.state;
                const bool certain = apply(next, action, node.certain);
                if (lethal(next, certain)) return force(next, next, node.id, action);
                const auto [existing, inserted] = seen.try_emplace(key_for(next), certain);
                if (!inserted && (!certain || existing->second)) continue;
                existing->second = certain;
                const auto prediction = model.predict(extract_features(next, matchup));
                if (next.outcome != Outcome::Ongoing) {
                    finish(next, next, prediction, node.id, action);
                } else {
                    const auto id = ++serial;
                    if (trace) steps.push_back({node.id, action});
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

template<bool Legacy>
BeamResult beam_attention_legacy(const GameState& initial, const attn::AttentionModel& model,
                                 const attn::RootContext& root_context, float root_value, std::size_t width,
                                 bool trace, bool ranked_only) {
    using Key = std::conditional_t<Legacy, LegacyPosition, GameState>;
    using Hash = std::conditional_t<Legacy, LegacyPositionHash, PositionHash>;
    const auto key_for = [](const GameState& state) -> decltype(auto) {
        if constexpr (Legacy) return legacy_key(state);
        else return state;
    };
    const int initial_actor = initial.turn_player;
    const auto i_own_b = summarize_board(initial.players[initial_actor]);
    const auto i_opp_b = summarize_board(initial.players[1 - initial_actor]);
    const auto init_snap = snapshot_zones(initial, initial_actor);
    const bool need_diff = model.candidate_embdiff_dim() > 0;

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
    if (trace) steps.push_back({});
    std::size_t serial = 0;
    std::size_t completed = 0;
    const auto lower_rank = [](const Candidate& a, const Candidate& b) {
        return a.score != b.score ? a.score < b.score : a.order > b.order;
    };
    const auto record = [&](Candidate& candidate, std::size_t parent, Action action) {
        if (!trace) return;
        candidate.actions.push_back(action);
        for (auto i = parent; i != 0; i = steps[i].parent) candidate.actions.push_back(steps[i].action);
        std::reverse(candidate.actions.begin(), candidate.actions.end());
    };
    const auto force = [&](const GameState& next, const GameState& decision, std::size_t parent, Action action) {
        Candidate candidate{
            next, decision, {}, {}, 0.0F, 0,
            extract_candidate_features_with_base(initial, i_own_b, i_opp_b, decision, next, true),
            need_diff ? extract_sparse_diff_with_base(init_snap, initial_actor, decision) : attn::CandidateSparseDiff{},
        };
        record(candidate, parent, action);
        return BeamResult{{}, std::move(candidate)};
    };
    const auto lethal = [&](const GameState& next, bool certain) {
        const int actor = initial.turn_player;
        return certain && next.turn_player == actor && next.outcome != Outcome::Ongoing &&
            next.players[1 - actor].leader_hp <= 0 && next.players[actor].leader_hp > 0 &&
            legacy_outcome_value(next.outcome, actor) == 1.0F;
    };
    const auto apply = [](GameState& next, Action action, bool certain) {
        if (certain) return apply_observed_action(next, action);
        apply_legal_action(next, action);
        return false;
    };
    const auto finish = [&](const auto& next, const auto& decision, std::size_t parent, Action action) {
        if (!finished.insert(key_for(next)).second) return;
        const auto cand_features = extract_candidate_features_with_base(initial, i_own_b, i_opp_b, decision, next, true);
        const auto cand_diff = need_diff ? extract_sparse_diff_with_base(init_snap, initial_actor, decision)
                                         : attn::CandidateSparseDiff{};
        const float policy_logit = model.policy_logit(root_context, cand_features.data(), need_diff ? &cand_diff : nullptr);
        const Prediction prediction{root_value, policy_logit};
        const auto order_id = completed++;
        const float score = next.outcome != Outcome::Ongoing
            ? 3.0F * legacy_outcome_value(next.outcome, decision.turn_player) - 0.5F
            : prediction.value + 1.0F / (1.0F + std::exp(-prediction.policy_logit));
        Candidate* result = nullptr;
        if (ranked_only && results.size() == kRankedCandidates) {
            const auto worst = std::min_element(results.begin(), results.end(), lower_rank);
            if (score <= worst->score) return;
            *worst = Candidate{next, decision, prediction, {}, score, order_id, cand_features, cand_diff};
            result = &*worst;
        } else {
            result = &results.emplace_back(Candidate{next, decision, prediction, {}, score, order_id, cand_features, cand_diff});
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
            if (lethal(ended, certain_end)) return force(ended, node.state, node.id, end);
            finish(ended, node.state, node.id, end);
            if (depth == kBeamSteps) continue;
            for (const auto action : legal_actions(node.state, {}, PlayCopies::Canonical)) {
                if (action.type == ActionType::EndTurn) continue;
                auto next = node.state;
                const bool certain = apply(next, action, node.certain);
                if (lethal(next, certain)) return force(next, next, node.id, action);
                const auto [existing, inserted] = seen.try_emplace(key_for(next), certain);
                if (!inserted && (!certain || existing->second)) continue;
                existing->second = certain;
                if (next.outcome != Outcome::Ongoing) {
                    finish(next, next, node.id, action);
                } else {
                    const auto step_features = extract_candidate_features_with_base(initial, i_own_b, i_opp_b, next, next, true);
                    const auto step_diff = need_diff ? extract_sparse_diff_with_base(init_snap, initial_actor, next)
                                                     : attn::CandidateSparseDiff{};
                    const float step_logit = model.policy_logit(root_context, step_features.data(),
                                                                need_diff ? &step_diff : nullptr);
                    const Prediction prediction{1.0F / (1.0F + std::exp(-step_logit)), step_logit};
                    const auto id = ++serial;
                    if (trace) steps.push_back({node.id, action});
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

// ============================================================================
// P0 Observation-Bounded ActionPlan Generator & Multi-World Evaluator
// ============================================================================

struct GeneratedPlans {
    std::vector<ActionPlan> deterministic_plans;
    std::vector<float> deterministic_prescores;
    std::vector<ActionPlan> chance_plans;
    std::vector<float> chance_prescores;
    std::optional<ActionPlan> proven_lethal;
    LethalEvidence proven_evidence{};
};

template<bool Legacy>
GeneratedPlans generate_observation_plans(const GameState& obs_initial, const Mlp* mlp_model,
                                          const attn::AttentionModel* attn_model,
                                          const attn::RootContext* root_context, float root_value,
                                          std::size_t width, const MatchupFeatures* matchup) {
    using Key = std::conditional_t<Legacy, LegacyPosition, GameState>;
    using Hash = std::conditional_t<Legacy, LegacyPositionHash, PositionHash>;
    const auto key_for = [](const GameState& state) -> decltype(auto) {
        if constexpr (Legacy) return legacy_key(state);
        else return state;
    };

    const int actor = obs_initial.turn_player;
    const auto i_own_b = summarize_board(obs_initial.players[actor]);
    const auto i_opp_b = summarize_board(obs_initial.players[1 - actor]);
    const auto init_snap = snapshot_zones(obs_initial, actor);
    const bool need_diff = attn_model && attn_model->candidate_embdiff_dim() > 0;

    const auto eval_step = [&](const GameState& decision_s, const GameState& next_s) -> Prediction {
        if (attn_model) {
            const auto feat = extract_candidate_features_with_base(obs_initial, i_own_b, i_opp_b, decision_s, next_s);
            const auto diff = need_diff ? extract_sparse_diff_with_base(init_snap, actor, decision_s)
                                        : attn::CandidateSparseDiff{};
            const float logit = attn_model->policy_logit(*root_context, feat.data(), need_diff ? &diff : nullptr);
            return {1.0F / (1.0F + std::exp(-logit)), logit};
        }
        return mlp_model->predict(extract_features(decision_s, matchup));
    };

    struct DetBeamNode {
        GameState state;
        Prediction prediction;
        std::size_t step_id;
    };

    std::unordered_set<Key, Hash> seen;
    std::unordered_set<Key, Hash> finished_det;
    seen.reserve(width * 24);
    finished_det.reserve(width * 5);
    seen.insert(key_for(obs_initial));

    const Prediction init_pred = attn_model ? Prediction{root_value, 0.0F}
                                            : mlp_model->predict(extract_features(obs_initial, matchup));
    std::vector<DetBeamNode> level{{obs_initial, init_pred, 0}}, next_level;
    level.reserve(width);
    next_level.reserve(width);
    std::vector<std::size_t> current{0};
    std::vector<std::size_t> order;
    std::vector<Step> steps{{0, {}}};

    const auto build_prefix = [&](std::size_t parent, Action last) {
        std::vector<Action> prefix;
        prefix.push_back(last);
        for (auto i = parent; i != 0; i = steps[i].parent) {
            prefix.push_back(steps[i].action);
        }
        std::reverse(prefix.begin(), prefix.end());
        return prefix;
    };

    GeneratedPlans out;
    const auto better = [](const DetBeamNode& a, const DetBeamNode& b) {
        return a.prediction.value != b.prediction.value ? a.prediction.value > b.prediction.value : a.step_id < b.step_id;
    };

    for (int depth = 0; depth <= kBeamSteps && !current.empty(); ++depth) {
        next_level.clear();
        order.clear();
        const auto better_index = [&](std::size_t a, std::size_t b) { return better(next_level[a], next_level[b]); };

        for (const auto idx : current) {
            const auto& node = level[idx];

            // 1. Evaluate EndTurn from this deterministic pre-EndTurn state.
            {
                auto ended = node.state;
                std::uint16_t unobs = 0;
                const auto ev_end = apply_action_with_evidence(ended, {ActionType::EndTurn}, actor, &unobs);
                if (ev_end.is_proven()) {
                    out.proven_lethal = ActionPlan{build_prefix(node.step_id, {ActionType::EndTurn}), false};
                    out.proven_evidence = ev_end;
                    return out;
                }
                if (finished_det.insert(key_for(node.state)).second) {
                    const auto pred = attn_model ? eval_step(node.state, ended) : node.prediction;
                    const float score = ended.outcome != Outcome::Ongoing
                        ? 3.0F * outcome_value(ended.outcome, actor) - 0.5F
                        : (attn_model ? root_value : pred.value) + 1.0F / (1.0F + std::exp(-pred.policy_logit));
                    out.deterministic_plans.push_back({build_prefix(node.step_id, {ActionType::EndTurn}), false});
                    out.deterministic_prescores.push_back(score);
                }
            }

            if (depth == kBeamSteps) continue;

            // 2. Evaluate non-EndTurn canonical legal actions.
            for (const auto action : legal_actions(node.state, {}, PlayCopies::Canonical)) {
                if (action.type == ActionType::EndTurn) continue;
                auto next = node.state;
                std::uint16_t unobs = 0;
                const auto ev = apply_action_with_evidence(next, action, actor, &unobs);
                if (ev.is_proven()) {
                    out.proven_lethal = ActionPlan{build_prefix(node.step_id, action), false};
                    out.proven_evidence = ev;
                    return out;
                }

                const bool crossed_boundary =
                    ev.crossed_unresolved_chance || ev.depends_on_hidden_information || ev.used_unobserved_card;

                if (crossed_boundary) {
                    // Split right at the chance/hidden-information boundary: require replanning after observation!
                    auto ended_obs = next;
                    if (ended_obs.outcome == Outcome::Ongoing && ended_obs.turn_player == actor) {
                        apply_legal_action(ended_obs, {ActionType::EndTurn});
                    }
                    const auto cpred = attn_model ? eval_step(next, ended_obs) : eval_step(next, next);
                    const float cscore = ended_obs.outcome != Outcome::Ongoing
                        ? 3.0F * outcome_value(ended_obs.outcome, actor) - 0.5F
                        : (attn_model ? root_value : cpred.value) + 1.0F / (1.0F + std::exp(-cpred.policy_logit));
                    out.chance_plans.push_back({build_prefix(node.step_id, action), true});
                    out.chance_prescores.push_back(cscore);
                    continue;
                }

                if (!seen.insert(key_for(next)).second) continue;

                if (next.outcome != Outcome::Ongoing) {
                    const auto pred = eval_step(next, next);
                    const float score = 3.0F * outcome_value(next.outcome, actor) - 0.5F;
                    out.deterministic_plans.push_back({build_prefix(node.step_id, action), false});
                    out.deterministic_prescores.push_back(score + 0.01F * pred.value);
                } else {
                    const auto pred = eval_step(next, next);
                    const std::size_t sid = steps.size();
                    steps.push_back({node.step_id, action});
                    if (order.size() < width) {
                        order.push_back(next_level.size());
                        next_level.push_back({next, pred, sid});
                        std::push_heap(order.begin(), order.end(), better_index);
                    } else if (pred.value > next_level[order.front()].prediction.value) {
                        std::pop_heap(order.begin(), order.end(), better_index);
                        next_level[order.back()] = {next, pred, sid};
                        std::push_heap(order.begin(), order.end(), better_index);
                    }
                }
            }
        }
        std::sort_heap(order.begin(), order.end(), better_index);
        current.swap(order);
        level.swap(next_level);
    }
    return out;
}

// Complete a chance-bounded prefix on a concrete determinized world `world_state` via bounded beam search up to EndTurn.
struct WorldCompletion {
    GameState decision{};
    GameState next{};
    std::vector<Action> full_actions;
    LethalEvidence evidence{};
    float score = 0.0F;
    float policy_logit = 0.0F;
    attn::CandidateFeatures cand_features{};
    attn::CandidateSparseDiff cand_diff{};
};

constexpr std::size_t kCompletionBeamWidth = 6;
constexpr std::size_t kMaxDetPlans = 24;

void trim_top_plans(std::vector<ActionPlan>& plans, std::span<const float> scores, std::size_t max_keep) {
    if (plans.size() <= max_keep) return;
    std::vector<std::size_t> idx(plans.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::partial_sort(idx.begin(), idx.begin() + max_keep, idx.end(), [&](std::size_t a, std::size_t b) {
        return scores[a] != scores[b] ? scores[a] > scores[b] : a < b;
    });
    std::vector<ActionPlan> kept;
    kept.reserve(max_keep);
    for (std::size_t i = 0; i < max_keep; ++i) kept.push_back(std::move(plans[idx[i]]));
    plans = std::move(kept);
}

WorldCompletion evaluate_plan_in_world(
    const GameState& world_initial, const BoardSummary& i_own_b, const BoardSummary& i_opp_b,
    const ZoneVocabSnapshot& init_snap, const ActionPlan& plan,
    const Mlp* mlp_model, const attn::AttentionModel* attn_model,
    const attn::RootContext* root_context, float root_value,
    const MatchupFeatures* matchup, bool record_full_actions) {
    WorldCompletion res{};
    const int actor = world_initial.turn_player;
    const bool need_diff = attn_model && attn_model->candidate_embdiff_dim() > 0;

    GameState cur = world_initial;
    GameState last_before_end = world_initial;
    std::uint16_t unobs_mask = 0;
    LethalEvidence total_ev{};
    if (record_full_actions) res.full_actions.reserve(plan.prefix.size() + 6);

    std::size_t step_count = 0;
    for (const auto action : plan.prefix) {
        if (action.type == ActionType::EndTurn) {
            last_before_end = cur;
        }
        const auto step_ev = apply_action_with_evidence(cur, action, actor, &unobs_mask);
        total_ev.merge(step_ev);
        if (record_full_actions) res.full_actions.push_back(action);
        ++step_count;
        if (action.type != ActionType::EndTurn) {
            last_before_end = cur;
        }
        if (cur.outcome != Outcome::Ongoing || cur.turn_player != actor) break;
    }

    const auto score_completed = [&](const GameState& dec_s, const GameState& next_s,
                                     attn::CandidateFeatures& out_feat, attn::CandidateSparseDiff& out_diff,
                                     float& out_logit) -> float {
        if (attn_model) {
            out_feat = extract_candidate_features_with_base(world_initial, i_own_b, i_opp_b, dec_s, next_s);
            if (need_diff) out_diff = extract_sparse_diff_with_base(init_snap, actor, dec_s);
            out_logit = attn_model->policy_logit(*root_context, out_feat.data(), need_diff ? &out_diff : nullptr);
            return next_s.outcome != Outcome::Ongoing
                ? 3.0F * outcome_value(next_s.outcome, actor) - 0.5F
                : root_value + 1.0F / (1.0F + std::exp(-out_logit));
        }
        const auto pred = mlp_model->predict(extract_features(dec_s, matchup));
        out_logit = pred.policy_logit;
        return next_s.outcome != Outcome::Ongoing
            ? 3.0F * outcome_value(next_s.outcome, actor) - 0.5F
            : pred.value + 1.0F / (1.0F + std::exp(-pred.policy_logit));
    };

    // Fast path for deterministic plans that already completed the turn (EndTurn or terminal).
    if (cur.outcome != Outcome::Ongoing || cur.turn_player != actor) {
        res.decision = last_before_end;
        res.next = cur;
        res.evidence = total_ev;
        res.score = score_completed(res.decision, res.next, res.cand_features, res.cand_diff, res.policy_logit);
        return res;
    }

    // Beam search continuation after crossing the chance/observation boundary in `world_initial`.
    // Intermediate states only compete against intermediate states at the same depth; every beam node also
    // evaluates its completed `EndTurn` afterstate so multi-step Play -> Evolve -> Attack combos are found!
    struct CompNode {
        GameState state;
        float step_priority;
        std::size_t step_id;
        std::uint16_t unobs;
        LethalEvidence ev;
    };
    struct CompStep {
        std::size_t parent;
        Action action;
    };

    std::unordered_set<GameState, PositionHash> seen;
    seen.reserve(kCompletionBeamWidth * 16);
    seen.insert(cur);

    std::vector<CompNode> level{{cur, root_value, 0, unobs_mask, total_ev}};
    std::vector<CompNode> next_level;
    level.reserve(kCompletionBeamWidth);
    next_level.reserve(kCompletionBeamWidth);
    std::vector<std::size_t> current{0};
    std::vector<std::size_t> order;
    std::vector<CompStep> comp_steps;
    if (record_full_actions) comp_steps.push_back({0, {}});

    bool has_best = false;
    std::size_t best_parent = 0;
    Action best_last{ActionType::EndTurn};

    const auto consider_finish = [&](const GameState& dec_s, const GameState& next_s, const LethalEvidence& ev_fin,
                                     std::size_t parent_id, Action last_act) {
        attn::CandidateFeatures cf{};
        attn::CandidateSparseDiff cd{};
        float logit = 0.0F;
        const float sc = score_completed(dec_s, next_s, cf, cd, logit);
        if (!has_best || sc > res.score) {
            has_best = true;
            res.decision = dec_s;
            res.next = next_s;
            res.evidence = ev_fin;
            res.score = sc;
            res.policy_logit = logit;
            res.cand_features = cf;
            res.cand_diff = cd;
            best_parent = parent_id;
            best_last = last_act;
        }
    };

    const int max_rem_steps = static_cast<int>(kBeamSteps - std::min<std::size_t>(step_count, kBeamSteps));
    for (int depth = 0; depth <= max_rem_steps && !current.empty(); ++depth) {
        next_level.clear();
        order.clear();
        const auto better_idx = [&](std::size_t a, std::size_t b) {
            return next_level[a].step_priority != next_level[b].step_priority
                       ? next_level[a].step_priority > next_level[b].step_priority
                       : next_level[a].step_id < next_level[b].step_id;
        };

        for (const std::size_t idx : current) {
            const auto& bnode = level[idx];
            {
                GameState ended = bnode.state;
                std::uint16_t u_end = bnode.unobs;
                LethalEvidence ev_end = bnode.ev;
                ev_end.merge(apply_action_with_evidence(ended, {ActionType::EndTurn}, actor, &u_end));
                consider_finish(bnode.state, ended, ev_end, bnode.step_id, {ActionType::EndTurn});
            }
            if (depth == max_rem_steps) continue;

            for (const auto act : legal_actions(bnode.state, {}, PlayCopies::Canonical)) {
                if (act.type == ActionType::EndTurn) continue;
                GameState next_s = bnode.state;
                std::uint16_t u_next = bnode.unobs;
                LethalEvidence ev_next = bnode.ev;
                ev_next.merge(apply_action_with_evidence(next_s, act, actor, &u_next));
                if (!seen.insert(next_s).second) continue;

                if (next_s.outcome != Outcome::Ongoing) {
                    consider_finish(next_s, next_s, ev_next, bnode.step_id, act);
                    continue;
                }

                float prio = 0.0F;
                if (attn_model) {
                    const auto sf = extract_candidate_features_with_base(world_initial, i_own_b, i_opp_b, next_s, next_s);
                    const auto sd = need_diff ? extract_sparse_diff_with_base(init_snap, actor, next_s)
                                              : attn::CandidateSparseDiff{};
                    const float slogit = attn_model->policy_logit(*root_context, sf.data(), need_diff ? &sd : nullptr);
                    prio = 1.0F / (1.0F + std::exp(-slogit));
                } else {
                    prio = mlp_model->predict(extract_features(next_s, matchup)).value;
                }

                std::size_t sid = bnode.step_id;
                if (record_full_actions) {
                    sid = comp_steps.size();
                    comp_steps.push_back({bnode.step_id, act});
                }
                if (order.size() < kCompletionBeamWidth) {
                    order.push_back(next_level.size());
                    next_level.push_back({std::move(next_s), prio, sid, u_next, ev_next});
                    std::push_heap(order.begin(), order.end(), better_idx);
                } else if (prio > next_level[order.front()].step_priority) {
                    std::pop_heap(order.begin(), order.end(), better_idx);
                    next_level[order.back()] = {std::move(next_s), prio, sid, u_next, ev_next};
                    std::push_heap(order.begin(), order.end(), better_idx);
                }
            }
        }
        std::sort_heap(order.begin(), order.end(), better_idx);
        current.swap(order);
        level.swap(next_level);
    }

    if (record_full_actions && has_best) {
        std::vector<Action> tail;
        tail.push_back(best_last);
        for (std::size_t i = best_parent; i != 0; i = comp_steps[i].parent) {
            tail.push_back(comp_steps[i].action);
        }
        std::reverse(tail.begin(), tail.end());
        res.full_actions.insert(res.full_actions.end(), tail.begin(), tail.end());
    }
    return res;
}

} // namespace

// ============================================================================
// Public Feature & Vocabulary Functions
// ============================================================================

GlobalPlayerContext make_global_player_context(const GameState& state, int seat) noexcept {
    const int t = static_cast<int>(state.turn_number);
    if (seat == state.turn_player) {
        return GlobalPlayerContext{seat, t, t, t + 1, true};
    }
    const int next_t = (seat == 1) ? t : (t + 1);
    return GlobalPlayerContext{seat, next_t - 1, next_t, next_t, false};
}

int next_pp_budget(const PlayerState& p, int seat, int next_pp_refresh_turn) noexcept {
    const int base = std::min(10, static_cast<int>(p.max_pp) + 1);
    const bool extra = (seat == 1) && (p.extra_pp_available || next_pp_refresh_turn == 6);
    return base + static_cast<int>(extra);
}

std::uint32_t compute_card_vocab_hash() {
    std::vector<unsigned char> buf;
    buf.reserve(640 * 8);
    const auto push_u16 = [&](std::uint16_t v) {
        buf.push_back(static_cast<unsigned char>(v & 0xFFU));
        buf.push_back(static_cast<unsigned char>((v >> 8) & 0xFFU));
    };
    const auto push_u32 = [&](std::uint32_t v) {
        for (int b = 0; b < 4; ++b) buf.push_back(static_cast<unsigned char>((v >> (8 * b)) & 0xFFU));
    };
    for (CardId cid = kFirstCard;; ++cid) {
        const auto* def = find_card(cid);
        if (!def) break;
        push_u16(cid);
        push_u16(static_cast<std::uint16_t>(card_vocab(cid)));
        push_u32(def->official_id);
    }
    for (std::size_t idx = 1; idx < kCrestCards.size(); ++idx) {
        const auto* crest = find_crest(static_cast<CrestId>(idx));
        if (!crest) break;
        push_u16(static_cast<std::uint16_t>(idx));
        push_u16(static_cast<std::uint16_t>(crest_vocab(static_cast<CrestId>(idx))));
        push_u32(crest->official_id);
    }
    return attn::crc32(buf.data(), buf.size());
}

GameState sample_hidden_state(const GameState& state, std::mt19937& rng, int observer) {
    auto sampled = state;
    const int actor = observer >= 0 ? observer : static_cast<int>(state.turn_player);
    auto& own = sampled.players[actor];
    auto& opponent = sampled.players[1 - actor];

    // The observer knows the remaining contents of their own deck (unordered multiset).
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

    // Reconstruct opponent unseen pool strictly from declared deck, public spent mask, and public generated cards.
    std::array<CardId, std::max(2 * kDeckSize, kDeckCapacity + kHandLimit)> unseen{};
    int count = 0;
    for (std::size_t i = 0; i < opponent.public_deck.size(); ++i) {
        const CardId cid = opponent.public_deck[i];
        if (cid && is_non_faith_card(cid) && !(opponent.public_spent & (std::uint64_t{1} << i))) {
            unseen[count++] = cid;
        }
    }
    for (int i = 0; i < opponent.public_generated_count; ++i) {
        const CardId cid = opponent.public_generated[i];
        if (cid && is_non_faith_card(cid)) unseen[count++] = cid;
    }
    std::sort(unseen.begin(), unseen.begin() + count);
    const auto remaining = opponent.deck_remaining();
    const auto needed = remaining + opponent.hand_count;
    const auto& base = opponent.public_deck[0] ? opponent.public_deck : kMirrorDeck;
    std::array<CardId, kDeckSize> eligible_base{};
    int eligible_count = 0;
    for (const CardId cid : base) {
        if (cid && is_non_faith_card(cid)) eligible_base[eligible_count++] = cid;
    }
    if (eligible_count == 0) eligible_base[eligible_count++] = kWingedStatue;
    while (count < needed) {
        unseen[count++] = eligible_base[std::uniform_int_distribution<int>(0, eligible_count - 1)(rng)];
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

attn::Tokens extract_attention_tokens_v2(const GameState& state) {
    attn::Tokens tokens{};
    const int actor = state.turn_player;
    const auto& own = state.players[actor];
    const auto& opp = state.players[1 - actor];
    const int own_earth = sum_earth_stacks(own);

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

    encode_card_tokens(state, tokens);
    return tokens;
}

attn::Tokens extract_attention_tokens(const GameState& state) {
    attn::Tokens tokens{};
    const int actor = state.turn_player;
    const int enemy = 1 - actor;
    const auto& own = state.players[actor];
    const auto& opp = state.players[enemy];

    const auto own_ctx = make_global_player_context(state, actor);
    const auto opp_ctx = make_global_player_context(state, enemy);

    auto& g = tokens.global;
    encode_global_player_v3(g.data(), own, own_ctx);
    encode_global_player_v3(g.data() + 28, opp, opp_ctx);

    g[56] = state.turn_number / 40.0F;
    g[57] = (actor == 0) ? 1.0F : 0.0F;
    g[58] = (static_cast<int>(own.leader_hp) - static_cast<int>(opp.leader_hp)) / 20.0F;
    g[59] = (static_cast<int>(own.max_pp) - static_cast<int>(opp.max_pp)) / 10.0F;
    g[60] = (static_cast<int>(own.ep) - static_cast<int>(opp.ep)) / 2.0F;
    g[61] = (static_cast<int>(own.sep) - static_cast<int>(opp.sep)) / 2.0F;
    g[62] = (static_cast<int>(own.hand_count) - static_cast<int>(opp.hand_count)) / 9.0F;
    g[63] = 0.0F; // Root phase identifier: fixed 0.0f for normal root positions to preserve RootContext cache

    encode_card_tokens(state, tokens);
    return tokens;
}

attn::Tokens extract_attention_tokens_for(const GameState& state, const attn::AttentionModel& model) {
    return model.global_dim() == attn::kGlobalDimV2 ? extract_attention_tokens_v2(state)
                                                    : extract_attention_tokens(state);
}

attn::CandidateFeatures extract_candidate_features(const GameState& initial, const GameState& decision,
                                                   const GameState& next) {
    const int actor = initial.turn_player;
    return extract_candidate_features_with_base(
        initial, summarize_board(initial.players[actor]), summarize_board(initial.players[1 - actor]), decision, next);
}

attn::CandidateSparseDiff extract_candidate_sparse_diff(const GameState& initial, const GameState& decision) {
    const int actor = initial.turn_player;
    return extract_sparse_diff_with_base(snapshot_zones(initial, actor), actor, decision);
}

float outcome_value(Outcome outcome, int seat) {
    if (outcome == Outcome::BothLose) return 0.5F;
    return static_cast<float>(seat == 0 ? outcome == Outcome::FirstWins : outcome == Outcome::SecondWins);
}

// ============================================================================
// Explicit MCTS Verification Engine (uses exact shared P0-1 formulas)
// ============================================================================

namespace {

float simulate_explicit_node(ExplicitMctsNode& node, int root_player, int world_index, int* nn_evals) {
    if (node.terminal != Outcome::Ongoing) {
        return outcome_value(node.terminal, root_player);
    }
    if (node.edges.empty() || !node.evaluated) {
        if (!node.evaluated) {
            node.evaluated = true;
            if (nn_evals) ++(*nn_evals);
            return leaf_to_root_perspective(node.leaf_actor_value, node.actor, root_player);
        }
        if (node.edges.empty()) {
            return leaf_to_root_perspective(node.leaf_actor_value, node.actor, root_player);
        }
    }

    std::size_t best = 0;
    float best_score = -1e9F;
    for (std::size_t i = 0; i < node.edges.size(); ++i) {
        const auto& edge = node.edges[i];
        const float q_root = edge.visits > 0 ? (edge.value_sum / static_cast<float>(edge.visits)) : 0.5F;
        const float score = puct_score(q_root, edge.prior, node.visits, edge.visits, node.actor, root_player);
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }

    auto& chosen = node.edges[best];
    const std::size_t w_idx = chosen.world_children.size() > 1
        ? (world_index < 0
               ? static_cast<std::size_t>(chosen.visits) % chosen.world_children.size()
               : static_cast<std::size_t>(world_index) % chosen.world_children.size())
        : 0;
    const float v_root = simulate_explicit_node(*chosen.world_children[w_idx], root_player, world_index, nn_evals);
    chosen.value_sum += v_root;
    ++chosen.visits;
    ++node.visits;
    return v_root;
}

} // namespace

void run_explicit_mcts_simulation(ExplicitMctsNode& root, int root_player, int world_index, int* nn_evals) {
    root.evaluated = true;
    static_cast<void>(simulate_explicit_node(root, root_player, world_index, nn_evals));
}

// ============================================================================
// Opponent Private-Information Isolation Policy Helper (F03)
// ============================================================================

OpponentPolicySnapshot evaluate_opponent_turn_policy(
    const GameState& state, ModelRef opponent_model, int root_player, std::uint64_t rng_seed,
    const MatchupFeatures* matchup) {
    const int opp_seat = state.turn_player;
    if (opp_seat == root_player) {
        throw std::invalid_argument("evaluate_opponent_turn_policy expects opponent as active turn_player");
    }

    // Sanitize root_player's private hand and both decks strictly from opponent's observation!
    GameState opp_clean_base = state;
    opp_clean_base.players[root_player].hand = {};
    opp_clean_base.players[root_player].hand_modifiers = {};
    opp_clean_base.players[root_player].deck = {};
    opp_clean_base.players[root_player].deck_modifiers = {};
    const int root_rem = state.players[root_player].deck_remaining();
    opp_clean_base.players[root_player].deck_idx = 0;
    opp_clean_base.players[root_player].deck_end = static_cast<std::uint8_t>(root_rem);

    std::mt19937 opp_rng(static_cast<std::uint32_t>((rng_seed ^ (rng_seed >> 32)) & 0xFFFFFFFFU));
    const GameState opp_sampled_world = sample_hidden_state(opp_clean_base, opp_rng, opp_seat);
    const GameState opp_obs = make_clean_observation_state(opp_clean_base, opp_seat);

    const bool legacy = matchup && matchup->legacy_mirror();
    attn::RootContext ctx{};
    float root_val = 0.5F;
    if (opponent_model.attention) {
        root_val = opponent_model.attention->evaluate_root(
            extract_attention_tokens_for(opp_obs, *opponent_model.attention), ctx);
    }

    const auto generated = legacy
        ? generate_observation_plans<true>(opp_obs, opponent_model.mlp, opponent_model.attention, &ctx, root_val,
                                           kBeamWidth, matchup)
        : generate_observation_plans<false>(opp_obs, opponent_model.mlp, opponent_model.attention, &ctx, root_val,
                                            kBeamWidth, matchup);

    OpponentPolicySnapshot snap{};
    if (generated.proven_lethal) {
        snap.plans.push_back(*generated.proven_lethal);
        snap.priors.push_back(1.0F);
        snap.greedy_choice = 0;
        return snap;
    }

    std::vector<ActionPlan> all_plans = generated.deterministic_plans;
    trim_top_plans(all_plans, generated.deterministic_prescores, kMaxDetPlans);
    if (!generated.chance_plans.empty()) {
        auto cplans = generated.chance_plans;
        trim_top_plans(cplans, generated.chance_prescores, 16);
        for (auto& cp : cplans) all_plans.push_back(std::move(cp));
    }

    const auto i_own_b = summarize_board(opp_sampled_world.players[opp_seat]);
    const auto i_opp_b = summarize_board(opp_sampled_world.players[1 - opp_seat]);
    const auto init_snap = snapshot_zones(opp_sampled_world, opp_seat);

    struct ScoredPlan {
        ActionPlan plan;
        float score;
        float logit;
    };
    std::vector<ScoredPlan> scored;
    scored.reserve(all_plans.size());
    for (const auto& plan : all_plans) {
        const auto comp = evaluate_plan_in_world(opp_sampled_world, i_own_b, i_opp_b, init_snap, plan,
                                                 opponent_model.mlp, opponent_model.attention, &ctx, root_val,
                                                 matchup, false);
        scored.push_back({plan, comp.score, comp.policy_logit});
    }

    std::vector<std::size_t> ord(scored.size());
    std::iota(ord.begin(), ord.end(), 0);
    const auto limit = std::min(scored.size(), static_cast<std::size_t>(kRankedCandidates));
    std::partial_sort(ord.begin(), ord.begin() + limit, ord.end(), [&](std::size_t a, std::size_t b) {
        return scored[a].score != scored[b].score ? scored[a].score > scored[b].score : a < b;
    });

    float max_logit = scored[ord[0]].logit;
    for (std::size_t i = 1; i < limit; ++i) max_logit = std::max(max_logit, scored[ord[i]].logit);
    float sum = 0.0F;
    for (std::size_t i = 0; i < limit; ++i) {
        snap.plans.push_back(scored[ord[i]].plan);
        const float p = std::exp(scored[ord[i]].logit - max_logit);
        snap.priors.push_back(p);
        sum += p;
    }
    for (float& p : snap.priors) p /= sum;
    snap.greedy_choice = static_cast<std::size_t>(
        std::max_element(snap.priors.begin(), snap.priors.end()) - snap.priors.begin());
    return snap;
}

// ============================================================================
// Search Implementation (Supports P0 and LegacyV2)
// ============================================================================

struct Search::Node {
    struct Edge {
        Candidate candidate;
        float prior = 0.0F;
        float value_sum = 0.0F;
        int visits = 0;
        std::unique_ptr<Node> child;
        std::vector<GameState> world_next;
        std::vector<std::unique_ptr<Node>> world_children;
    };

    explicit Node(const GameState& position, int perspective = -1)
        : state(position), perspective_seat(perspective) {}
    GameState state;
    int perspective_seat = -1;
    int visits = 0;
    bool evaluated = false;
    bool context_ready = false;
    float root_value = 0.5F;
    attn::RootContext attention_context{};
    std::vector<Edge> edges;
    std::unique_ptr<Candidate> forced;
};

namespace {

void flip_node_perspective(auto& node, int new_perspective) {
    if (node.perspective_seat == new_perspective) return;
    node.perspective_seat = new_perspective;
    for (auto& edge : node.edges) {
        edge.value_sum = static_cast<float>(edge.visits) - edge.value_sum;
        if (edge.child) flip_node_perspective(*edge.child, new_perspective);
        for (auto& w_child : edge.world_children) {
            if (w_child) flip_node_perspective(*w_child, new_perspective);
        }
    }
}

} // namespace

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

void Search::set_search_version(SearchVersion version) {
    if (options_.search_version == version) return;
    options_.search_version = version;
    root_.reset();
}

void Search::expand_legacy(Node& node, std::size_t width, bool trace) {
    const int player = node.state.turn_player;
    const bool legacy = matchup_ && matchup_->legacy_mirror();
    BeamResult result;
    if (attention_models_[player]) {
        const auto& model = *attention_models_[player];
        if (!node.context_ready) {
            node.root_value = model.evaluate_root(extract_attention_tokens_for(node.state, model), node.attention_context);
            node.context_ready = true;
        }
        result = legacy
            ? beam_attention_legacy<true>(node.state, model, node.attention_context, node.root_value, width, trace, !options_.exploration)
            : beam_attention_legacy<false>(node.state, model, node.attention_context, node.root_value, width, trace, !options_.exploration);
    } else {
        const auto& model = *models_[player];
        result = legacy
            ? beam_legacy<true>(node.state, model, width, trace, matchup_, !options_.exploration)
            : beam_legacy<false>(node.state, model, width, trace, matchup_, !options_.exploration);
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
        node.edges.push_back({std::move(candidate), prior, 0.0F, 0, {}, {}, {}});
    }
    for (auto& edge : node.edges) edge.prior /= sum;
}

std::array<float, kPlayers> Search::simulate_legacy(Node& node, const float* root_priors) {
    const int player = node.state.turn_player;
    if (node.edges.empty() && !node.forced) {
        if (!node.evaluated) {
            node.evaluated = true;
            float estimate = 0.5F;
            if (attention_models_[player]) {
                const auto& m = *attention_models_[player];
                estimate = m.evaluate_root(extract_attention_tokens_for(node.state, m), node.attention_context);
                node.root_value = estimate;
                node.context_ready = true;
            } else {
                estimate = models_[player]->predict(extract_features(node.state, matchup_)).value;
            }
            return player == 0 ? std::array{estimate, 1.0F - estimate} : std::array{1.0F - estimate, estimate};
        }
        expand_legacy(node, kBeamWidth, options_.record_actions && !options_.determinization);
    }
    if (node.forced) {
        return {legacy_outcome_value(node.forced->next.outcome, 0), legacy_outcome_value(node.forced->next.outcome, 1)};
    }
    const float factor = 1.5F * std::sqrt(static_cast<float>(node.visits + 1));
    std::size_t best = 0;
    float best_score = -1.0F;
    for (std::size_t i = 0; i < node.edges.size(); ++i) {
        const auto& edge = node.edges[i];
        const auto result = edge.candidate.next.outcome;
        const float value = result != Outcome::Ongoing ? legacy_outcome_value(result, player) :
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
        value = {legacy_outcome_value(next.outcome, 0), legacy_outcome_value(next.outcome, 1)};
    } else {
        if (!edge.child) edge.child = std::make_unique<Node>(next);
        value = simulate_legacy(*edge.child);
    }
    edge.value_sum += value[player];
    ++edge.visits;
    ++node.visits;
    return value;
}

Decision Search::choose_legacy_sample(const GameState& state, bool policy) {
    if (state.phase != Phase::Main || state.outcome != Outcome::Ongoing) {
        throw std::invalid_argument("Search requires an active main-phase turn");
    }
    const bool widen = options_.exploration && std::bernoulli_distribution(0.01)(rng_);
    if (!root_ || root_->state != state || widen) {
        root_ = std::make_unique<Node>(state);
    }
    if (root_->edges.empty() && !root_->forced) {
        expand_legacy(*root_, widen ? 2 * kBeamWidth : kBeamWidth, options_.record_actions || options_.determinization);
    }
    if (root_->forced) {
        auto& forced = *root_->forced;
        Decision result{};
        result.next = forced.next;
        result.forced = true;
        if (policy) {
            if (attention_models_[state.turn_player]) {
                result.attention_policy.push_back({forced.attention_features, forced.attention_diff, 1.0F});
            } else {
                result.policy.push_back({extract_features(forced.decision, matchup_), 1.0F});
            }
        }
        result.actions = forced.actions;
        result.plan = {forced.actions, false};
        if (options_.record_actions) result.ranked.push_back({std::move(forced.actions), 0, 1.0F, true, result.plan, LethalStatus::Proven});
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
        static_cast<void>(simulate_legacy(*root_, options_.exploration ? priors.data() : nullptr));
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
                result.attention_policy.push_back({edge.candidate.attention_features, edge.candidate.attention_diff, prob});
            } else {
                result.policy.push_back({extract_features(edge.candidate.decision, matchup_), prob});
            }
        }
    }
    std::size_t chosen;
    if (options_.exploration && state.turn_number <= 5) {
        chosen = std::discrete_distribution<std::size_t>(visits.begin(), visits.begin() + count)(rng_);
    } else {
        chosen = static_cast<std::size_t>(std::max_element(visits.begin(), visits.begin() + count) - visits.begin());
    }
    result.next = root_->edges[chosen].candidate.next;
    result.actions = root_->edges[chosen].candidate.actions;
    result.plan = {result.actions, false};
    if (options_.record_actions) {
        for (std::size_t i = 0; i < count; ++i) {
            auto& edge = root_->edges[i];
            std::optional<float> value;
            if (edge.visits > 0) value = edge.value_sum / static_cast<float>(edge.visits);
            ActionPlan rplan{edge.candidate.actions, false};
            result.ranked.push_back({std::move(edge.candidate.actions), edge.visits, value, i == chosen, std::move(rplan)});
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

// ============================================================================
// P0 Search Node Expansion & Root-Perspective Simulation
// ============================================================================

void Search::expand_p0(Node& node, int root_player, std::size_t width, bool trace) {
    const int player = node.state.turn_player;
    const bool legacy = matchup_ && matchup_->legacy_mirror();
    const Mlp* mlp = models_[player];
    const attn::AttentionModel* attn_m = attention_models_[player];

    // Opponent turn policy isolation (F03): never reference root_player's private hand during opponent candidate/prior eval!
    if (options_.determinization && player != root_player) {
        const ModelRef opp_ref = attn_m ? ModelRef(*attn_m) : ModelRef(*mlp);
        const std::uint64_t obs_seed = observation_hash_for(node.state, player);
        const auto opp_snap = evaluate_opponent_turn_policy(node.state, opp_ref, root_player, obs_seed, matchup_);
        const auto i_own_b = summarize_board(node.state.players[player]);
        const auto i_opp_b = summarize_board(node.state.players[1 - player]);
        const auto init_snap = snapshot_zones(node.state, player);
        if (attn_m && !node.context_ready) {
            node.root_value = attn_m->evaluate_root(extract_attention_tokens_for(node.state, *attn_m), node.attention_context);
            node.context_ready = true;
        }
        node.edges.reserve(opp_snap.plans.size());
        for (std::size_t i = 0; i < opp_snap.plans.size(); ++i) {
            const auto& plan = opp_snap.plans[i];
            const auto comp = evaluate_plan_in_world(
                node.state, i_own_b, i_opp_b, init_snap, plan, mlp, attn_m,
                attn_m ? &node.attention_context : nullptr, node.root_value, matchup_, trace);
            Candidate cand{};
            cand.next = comp.next;
            cand.decision = comp.decision;
            cand.prediction = {node.root_value, comp.policy_logit};
            cand.actions = comp.full_actions;
            cand.score = comp.score;
            cand.order = i;
            cand.attention_features = comp.cand_features;
            cand.attention_diff = comp.cand_diff;
            cand.plan = plan;
            cand.lethal_status = classify_lethal(comp.evidence);
            cand.lethal_evidence = comp.evidence;
            if (cand.lethal_status == LethalStatus::Proven) {
                node.forced = std::make_unique<Candidate>(std::move(cand));
                node.edges.clear();
                return;
            }
            node.edges.push_back({std::move(cand), opp_snap.priors[i], 0.0F, 0, {}, {}, {}});
        }
        return;
    }

    const GameState obs_base = options_.determinization ? make_clean_observation_state(node.state, player) : node.state;
    if (attn_m && !node.context_ready) {
        node.root_value = attn_m->evaluate_root(extract_attention_tokens_for(obs_base, *attn_m), node.attention_context);
        node.context_ready = true;
    }
    const auto generated = legacy
        ? generate_observation_plans<true>(obs_base, mlp, attn_m, attn_m ? &node.attention_context : nullptr,
                                           node.root_value, width, matchup_)
        : generate_observation_plans<false>(obs_base, mlp, attn_m, attn_m ? &node.attention_context : nullptr,
                                            node.root_value, width, matchup_);

    const auto i_own_b = summarize_board(node.state.players[player]);
    const auto i_opp_b = summarize_board(node.state.players[1 - player]);
    const auto init_snap = snapshot_zones(node.state, player);

    if (generated.proven_lethal) {
        const auto comp = evaluate_plan_in_world(
            node.state, i_own_b, i_opp_b, init_snap, *generated.proven_lethal, mlp, attn_m,
            attn_m ? &node.attention_context : nullptr, node.root_value, matchup_, trace);
        Candidate cand{};
        cand.next = comp.next;
        cand.decision = comp.decision;
        cand.prediction = {1.0F, comp.policy_logit};
        cand.actions = comp.full_actions;
        cand.attention_features = comp.cand_features;
        cand.attention_diff = comp.cand_diff;
        cand.plan = *generated.proven_lethal;
        cand.lethal_status = LethalStatus::Proven;
        cand.lethal_evidence = generated.proven_evidence;
        node.forced = std::make_unique<Candidate>(std::move(cand));
        return;
    }

    std::vector<ActionPlan> all_plans = generated.deterministic_plans;
    trim_top_plans(all_plans, generated.deterministic_prescores, kMaxDetPlans);
    if (!generated.chance_plans.empty()) {
        auto cplans = generated.chance_plans;
        trim_top_plans(cplans, generated.chance_prescores, 16);
        for (auto& cp : cplans) all_plans.push_back(std::move(cp));
    }

    std::vector<Candidate> candidates;
    candidates.reserve(all_plans.size());
    for (std::size_t i = 0; i < all_plans.size(); ++i) {
        const auto comp = evaluate_plan_in_world(
            node.state, i_own_b, i_opp_b, init_snap, all_plans[i], mlp, attn_m,
            attn_m ? &node.attention_context : nullptr, node.root_value, matchup_, trace);
        Candidate cand{};
        cand.next = comp.next;
        cand.decision = comp.decision;
        cand.prediction = {node.root_value, comp.policy_logit};
        cand.actions = comp.full_actions;
        cand.score = comp.score;
        cand.order = i;
        cand.attention_features = comp.cand_features;
        cand.attention_diff = comp.cand_diff;
        cand.plan = all_plans[i];
        cand.lethal_status = classify_lethal(comp.evidence);
        cand.lethal_evidence = comp.evidence;
        candidates.push_back(std::move(cand));
    }

    if (candidates.empty()) return;

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
        node.edges.push_back({std::move(candidate), prior, 0.0F, 0, {}, {}, {}});
    }
    for (auto& edge : node.edges) edge.prior /= sum;
}

float Search::simulate_p0(Node& node, int root_player, const float* root_priors) {
    // 1. Terminal node bypass: never invoke NN on terminal states.
    if (node.state.outcome != Outcome::Ongoing) {
        return outcome_value(node.state.outcome, root_player);
    }

    const int actor = node.state.turn_player;
    if (node.edges.empty() && !node.forced) {
        if (!node.evaluated) {
            node.evaluated = true;
            float actor_estimate = 0.5F;
            if (attention_models_[actor]) {
                const auto& m = *attention_models_[actor];
                actor_estimate = m.evaluate_root(extract_attention_tokens_for(node.state, m), node.attention_context);
                node.root_value = actor_estimate;
                node.context_ready = true;
            } else {
                actor_estimate = models_[actor]->predict(extract_features(node.state, matchup_)).value;
                node.root_value = actor_estimate;
            }
            // Convert once at leaf to root_player perspective.
            return leaf_to_root_perspective(actor_estimate, actor, root_player);
        }
        expand_p0(node, root_player, kBeamWidth, options_.record_actions && !options_.determinization);
    }
    if (node.forced) {
        return outcome_value(node.forced->next.outcome, root_player);
    }
    if (node.edges.empty()) {
        return leaf_to_root_perspective(node.root_value, actor, root_player);
    }

    // 2. PUCT selection referencing `actor` (not depth parity): flip Q only when actor != root_player; U never flipped.
    std::size_t best = 0;
    float best_score = -1e9F;
    for (std::size_t i = 0; i < node.edges.size(); ++i) {
        const auto& edge = node.edges[i];
        const auto result = edge.candidate.next.outcome;
        const float q_root = result != Outcome::Ongoing
            ? outcome_value(result, root_player)
            : (edge.visits == 0 ? 0.5F : edge.value_sum / static_cast<float>(edge.visits));
        const float prior = root_priors ? root_priors[i] : edge.prior;
        const float score = puct_score(q_root, prior, node.visits, edge.visits, actor, root_player);
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }

    auto& edge = node.edges[best];
    const auto& next = edge.candidate.next;
    float v_root = 0.5F;
    if (next.outcome != Outcome::Ongoing) {
        v_root = outcome_value(next.outcome, root_player);
    } else {
        if (!edge.child) edge.child = std::make_unique<Node>(next, root_player);
        v_root = simulate_p0(*edge.child, root_player);
    }

    // 3. Root-perspective backup without intermediate sign flips.
    edge.value_sum += v_root;
    ++edge.visits;
    ++node.visits;
    return v_root;
}

Decision Search::choose_p0(const GameState& state, bool policy) {
    if (state.phase != Phase::Main || state.outcome != Outcome::Ongoing) {
        throw std::invalid_argument("Search requires an active main-phase turn");
    }
    const int root_player = state.turn_player;
    const bool legacy = matchup_ && matchup_->legacy_mirror();
    const Mlp* mlp = models_[root_player];
    const attn::AttentionModel* attn_m = attention_models_[root_player];
    const bool widen = options_.exploration && std::bernoulli_distribution(0.01)(rng_);
    const std::size_t beam_w = widen ? 2 * kBeamWidth : kBeamWidth;
    const bool need_trace = options_.record_actions || options_.determinization;

    // Perfect-information mode (`!options_.determinization`): single world with tree reuse & perspective sync.
    if (!options_.determinization) {
        if (!root_ || root_->state != state || widen) {
            root_ = std::make_unique<Node>(state, root_player);
        } else if (root_->perspective_seat != root_player) {
            flip_node_perspective(*root_, root_player);
        }
        if (root_->edges.empty() && !root_->forced) {
            expand_p0(*root_, root_player, beam_w, need_trace);
        }
        if (root_->forced) {
            auto& forced = *root_->forced;
            Decision result{};
            result.next = forced.next;
            result.forced = true;
            result.lethal_status = LethalStatus::Proven;
            result.lethal_evidence = forced.lethal_evidence;
            result.plan = forced.plan;
            if (policy) {
                if (attn_m) result.attention_policy.push_back({forced.attention_features, forced.attention_diff, 1.0F});
                else result.policy.push_back({extract_features(forced.decision, matchup_), 1.0F});
            }
            result.actions = forced.actions;
            if (options_.record_actions) {
                result.ranked.push_back({forced.actions, 0, 1.0F, true, forced.plan, LethalStatus::Proven});
            }
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
            static_cast<void>(simulate_p0(*root_, root_player, options_.exploration ? priors.data() : nullptr));
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
                if (attn_m) result.attention_policy.push_back({edge.candidate.attention_features, edge.candidate.attention_diff, prob});
                else result.policy.push_back({extract_features(edge.candidate.decision, matchup_), prob});
            }
        }
        std::size_t chosen;
        if (options_.exploration && state.turn_number <= 5) {
            chosen = std::discrete_distribution<std::size_t>(visits.begin(), visits.begin() + count)(rng_);
        } else {
            chosen = static_cast<std::size_t>(std::max_element(visits.begin(), visits.begin() + count) - visits.begin());
        }
        const auto& sel = root_->edges[chosen].candidate;
        result.next = sel.next;
        result.actions = sel.actions;
        result.plan = sel.plan;
        result.lethal_status = sel.lethal_status;
        result.lethal_evidence = sel.lethal_evidence;
        if (options_.record_actions) {
            for (std::size_t i = 0; i < count; ++i) {
                auto& edge = root_->edges[i];
                std::optional<float> value;
                if (edge.visits > 0) value = edge.value_sum / static_cast<float>(edge.visits);
                result.ranked.push_back({std::move(edge.candidate.actions), edge.visits, value, i == chosen,
                                         edge.candidate.plan, edge.candidate.lethal_status});
            }
            std::stable_sort(result.ranked.begin(), result.ranked.end(),
                             [](const auto& a, const auto& b) { return a.visits > b.visits; });
        }
        auto next_root = std::move(root_->edges[chosen].child);
        root_ = std::move(next_root);
        return result;
    }

    // Imperfect-information Determinized MCTS (F01, F02, P0-1, P0-2):
    // 1. Sample K determinization worlds strictly from public history & unordered own deck.
    const int num_worlds = std::clamp(
        options_.simulations < options_.determinization_samples ? options_.simulations : options_.determinization_samples,
        1, 8);
    std::vector<GameState> worlds;
    worlds.reserve(static_cast<std::size_t>(num_worlds));
    for (int w = 0; w < num_worlds; ++w) {
        worlds.push_back(sample_hidden_state(state, rng_, root_player));
    }

    // 2. Generate ActionPlan candidates strictly from root observation O (`obs_root`).
    const GameState obs_root = make_clean_observation_state(state, root_player);
    attn::RootContext root_ctx{};
    float root_val = 0.5F;
    if (attn_m) {
        root_val = attn_m->evaluate_root(extract_attention_tokens_for(obs_root, *attn_m), root_ctx);
    }

    const auto generated = legacy
        ? generate_observation_plans<true>(obs_root, mlp, attn_m, attn_m ? &root_ctx : nullptr, root_val, beam_w, matchup_)
        : generate_observation_plans<false>(obs_root, mlp, attn_m, attn_m ? &root_ctx : nullptr, root_val, beam_w, matchup_);

    const auto i_own_b = summarize_board(worlds[0].players[root_player]);
    const auto i_opp_b = summarize_board(worlds[0].players[1 - root_player]);
    const auto init_snap = snapshot_zones(worlds[0], root_player);

    // 3. Only LethalStatus::Proven (100% independent of hidden info & chance) is allowed to force!
    if (generated.proven_lethal) {
        const auto comp = evaluate_plan_in_world(
            worlds[0], i_own_b, i_opp_b, init_snap, *generated.proven_lethal, mlp, attn_m,
            attn_m ? &root_ctx : nullptr, root_val, matchup_, true);
        Decision result{};
        result.next = comp.next;
        result.forced = true;
        result.lethal_status = LethalStatus::Proven;
        result.lethal_evidence = generated.proven_evidence;
        result.actions = comp.full_actions;
        result.plan = *generated.proven_lethal;
        if (policy) {
            if (attn_m) result.attention_policy.push_back({comp.cand_features, comp.cand_diff, 1.0F});
            else result.policy.push_back({extract_features(comp.decision, matchup_), 1.0F});
        }
        if (options_.record_actions) {
            result.sampled_root = std::make_unique<GameState>(worlds[0]);
            result.ranked.push_back({comp.full_actions, 0, 1.0F, true, result.plan, LethalStatus::Proven});
        }
        root_.reset();
        return result;
    }

    // 4. Evaluate all common ActionPlan candidates across all K determinizations (expected value aggregation).
    std::vector<ActionPlan> all_plans = generated.deterministic_plans;
    trim_top_plans(all_plans, generated.deterministic_prescores, kMaxDetPlans);
    if (!generated.chance_plans.empty()) {
        auto cplans = generated.chance_plans;
        trim_top_plans(cplans, generated.chance_prescores, 16);
        for (auto& cp : cplans) all_plans.push_back(std::move(cp));
    }

    struct MultiWorldCandidate {
        Candidate rep;
        std::vector<GameState> world_next;
        float mean_score = 0.0F;
        float mean_logit = 0.0F;
    };
    std::vector<MultiWorldCandidate> mw_cands;
    mw_cands.reserve(all_plans.size());

    for (std::size_t i = 0; i < all_plans.size(); ++i) {
        MultiWorldCandidate mwc{};
        mwc.world_next.resize(static_cast<std::size_t>(num_worlds));
        double score_sum = 0.0;
        double logit_sum = 0.0;
        LethalEvidence agg_ev{};
        bool any_win = false;

        for (int w = 0; w < num_worlds; ++w) {
            const auto comp = evaluate_plan_in_world(
                worlds[w], i_own_b, i_opp_b, init_snap, all_plans[i], mlp, attn_m,
                attn_m ? &root_ctx : nullptr, root_val, matchup_, w == 0 && need_trace);
            mwc.world_next[w] = comp.next;
            score_sum += comp.score;
            logit_sum += comp.policy_logit;
            agg_ev.crossed_unresolved_chance |= comp.evidence.crossed_unresolved_chance;
            agg_ev.depends_on_hidden_information |= comp.evidence.depends_on_hidden_information;
            agg_ev.used_unobserved_card |= comp.evidence.used_unobserved_card;
            any_win |= comp.evidence.terminal_win_verified;

            if (w == 0) {
                mwc.rep.next = comp.next;
                mwc.rep.decision = comp.decision;
                mwc.rep.actions = comp.full_actions;
                mwc.rep.order = i;
                mwc.rep.attention_features = comp.cand_features;
                mwc.rep.attention_diff = comp.cand_diff;
                mwc.rep.plan = all_plans[i];
            }
        }
        agg_ev.terminal_win_verified = any_win;
        if (all_plans[i].requires_replan_after_observation) {
            agg_ev.crossed_unresolved_chance = true;
        }
        mwc.mean_score = static_cast<float>(score_sum / num_worlds);
        mwc.mean_logit = static_cast<float>(logit_sum / num_worlds);
        mwc.rep.score = mwc.mean_score;
        mwc.rep.prediction = {root_val, mwc.mean_logit};
        mwc.rep.lethal_evidence = agg_ev;
        mwc.rep.lethal_status = classify_lethal(agg_ev);
        mw_cands.push_back(std::move(mwc));
    }

    root_ = std::make_unique<Node>(obs_root, root_player);
    root_->evaluated = true;
    root_->context_ready = attn_m != nullptr;
    root_->root_value = root_val;
    root_->attention_context = root_ctx;

    std::vector<std::size_t> order(mw_cands.size());
    std::iota(order.begin(), order.end(), 0);
    const auto limit = std::min(mw_cands.size(), static_cast<std::size_t>(kRankedCandidates));
    std::partial_sort(order.begin(), order.begin() + limit, order.end(), [&](const auto a, const auto b) {
        return mw_cands[a].mean_score != mw_cands[b].mean_score
                   ? mw_cands[a].mean_score > mw_cands[b].mean_score
                   : a < b;
    });
    const auto selected = options_.exploration ? std::min(mw_cands.size(), limit + kRandomCandidates) : limit;
    for (auto i = limit; i < selected; ++i) {
        const auto pick = std::uniform_int_distribution<std::size_t>(i, order.size() - 1)(rng_);
        std::swap(order[i], order[pick]);
    }

    root_->edges.reserve(selected);
    float max_logit = mw_cands[order[0]].mean_logit;
    for (std::size_t i = 1; i < selected; ++i) {
        max_logit = std::max(max_logit, mw_cands[order[i]].mean_logit);
    }
    float prior_sum = 0.0F;
    for (std::size_t i = 0; i < selected; ++i) {
        auto& mwc = mw_cands[order[i]];
        const float prior = std::exp(mwc.mean_logit - max_logit);
        prior_sum += prior;
        Node::Edge edge{};
        edge.candidate = std::move(mwc.rep);
        edge.prior = prior;
        edge.world_next = std::move(mwc.world_next);
        edge.world_children.resize(static_cast<std::size_t>(num_worlds));
        root_->edges.push_back(std::move(edge));
    }
    for (auto& edge : root_->edges) edge.prior /= prior_sum;

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

    // 5. Shared-root MCTS simulations across determinization worlds: per-edge stratified sampling aggregates E_w[V(a, w)]!
    for (int sim = 0; sim < options_.simulations; ++sim) {
        std::size_t best = 0;
        float best_score = -1e9F;
        for (std::size_t i = 0; i < root_->edges.size(); ++i) {
            const auto& edge = root_->edges[i];
            const float q_root = edge.visits > 0 ? (edge.value_sum / static_cast<float>(edge.visits)) : 0.5F;
            const float prior = options_.exploration ? priors[i] : edge.prior;
            const float score = puct_score(q_root, prior, root_->visits, edge.visits, root_player, root_player);
            if (score > best_score) {
                best_score = score;
                best = i;
            }
        }
        auto& edge = root_->edges[best];
        const std::size_t w = static_cast<std::size_t>(edge.visits) % edge.world_next.size();
        const auto& next_w = edge.world_next[w];
        float v_root = 0.5F;
        if (next_w.outcome != Outcome::Ongoing) {
            v_root = outcome_value(next_w.outcome, root_player);
        } else {
            if (!edge.world_children[w]) {
                edge.world_children[w] = std::make_unique<Node>(next_w, root_player);
            }
            v_root = simulate_p0(*edge.world_children[w], root_player);
        }
        edge.value_sum += v_root;
        ++edge.visits;
        ++root_->visits;
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
            if (attn_m) result.attention_policy.push_back({edge.candidate.attention_features, edge.candidate.attention_diff, prob});
            else result.policy.push_back({extract_features(edge.candidate.decision, matchup_), prob});
        }
    }

    std::size_t chosen;
    if (options_.exploration && state.turn_number <= 5) {
        chosen = std::discrete_distribution<std::size_t>(visits.begin(), visits.begin() + count)(rng_);
    } else {
        chosen = static_cast<std::size_t>(std::max_element(visits.begin(), visits.begin() + count) - visits.begin());
    }
    const auto& sel = root_->edges[chosen].candidate;
    result.next = sel.next;
    result.actions = sel.actions;
    result.plan = sel.plan;
    result.lethal_status = sel.lethal_status;
    result.lethal_evidence = sel.lethal_evidence;
    if (options_.record_actions) {
        result.sampled_root = std::make_unique<GameState>(worlds[0]);
        for (std::size_t i = 0; i < count; ++i) {
            auto& edge = root_->edges[i];
            std::optional<float> value;
            if (edge.visits > 0) value = edge.value_sum / static_cast<float>(edge.visits);
            result.ranked.push_back({std::move(edge.candidate.actions), edge.visits, value, i == chosen,
                                     edge.candidate.plan, edge.candidate.lethal_status});
        }
        std::stable_sort(result.ranked.begin(), result.ranked.end(),
                         [](const auto& a, const auto& b) { return a.visits > b.visits; });
    }
    root_.reset();
    return result;
}

Decision Search::choose(const GameState& state, bool policy) {
    if (options_.search_version == SearchVersion::LegacyV2) {
        if (!options_.determinization) return choose_legacy_sample(state, policy);
        auto sampled = sample_hidden_state(state, rng_, state.turn_player);
        auto result = choose_legacy_sample(sampled, policy);
        if (options_.record_actions) result.sampled_root = std::make_unique<GameState>(sampled);
        return result;
    }
    return choose_p0(state, policy);
}

Decision Search::play(const GameState& state, bool policy) {
    if (options_.search_version == SearchVersion::LegacyV2) {
        if (!options_.determinization) return choose_legacy_sample(state, policy);
        auto actual = state;
        const int actor = state.turn_player;
        const auto active = [&] { return actual.outcome == Outcome::Ongoing && actual.turn_player == actor; };
        Decision first;
        std::vector<Action> played;
        while (active() && played.size() < kBeamSteps) {
            const bool opening = played.empty();
            auto sampled = sample_hidden_state(actual, rng_, actor);
            auto plan = choose_legacy_sample(sampled, opening && policy);
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

    if (!options_.determinization) return choose_p0(state, policy);

    auto actual = state;
    const int actor = state.turn_player;
    const auto active = [&] { return actual.outcome == Outcome::Ongoing && actual.turn_player == actor; };
    Decision first;
    std::vector<Action> played;
    while (active() && played.size() < kBeamSteps) {
        const bool opening = played.empty();
        auto step_dec = choose_p0(actual, opening && policy);
        const ActionPlan chosen_plan = step_dec.plan;
        if (opening) {
            first = std::move(step_dec);
        }
        const auto before = played.size();
        for (const auto action : chosen_plan.prefix) {
            if (played.size() == kBeamSteps || !apply_action(actual, action)) break;
            played.push_back(action);
            if (!active()) break;
        }
        if (played.size() == before) break;
        if (active()) {
            // Replan from the newly observed real game state after crossing the chance/observation boundary!
            first.replanned = true;
        }
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

// ============================================================================
// Common Random Numbers (CRN) Mulligan & Physical Instance Exclusion (P0-3)
// ============================================================================

MulliganSampleResult simulate_mulligan_crn(const GameState& state, std::uint8_t mask, std::uint64_t sample_seed) {
    if (state.phase != Phase::Mulligan) {
        throw std::invalid_argument("simulate_mulligan_crn requires Phase::Mulligan");
    }
    MulliganSampleResult res{};
    const int actor = state.turn_player;
    const auto& orig_own = state.players[actor];
    const auto& orig_opp = state.players[1 - actor];

    // 1. Reconstruct physical deck instances (0..N-1) strictly from public_deck without reading true match deck!
    struct PhysicalCard {
        CardId card_id = 0;
        int physical_id = -1;
    };
    std::array<PhysicalCard, kDeckSize> full_deck{};
    int total_cards = 0;
    const auto& src_deck = orig_own.public_deck[0] ? orig_own.public_deck : kMirrorDeck;
    for (int i = 0; i < kDeckSize; ++i) {
        const CardId cid = src_deck[i];
        if (cid && is_non_faith_card(cid)) {
            full_deck[total_cards] = {cid, total_cards};
            ++total_cards;
        }
    }

    // Match the 4 offered cards in orig_own.hand[0..3] to 4 distinct physical instances.
    std::array<bool, kDeckSize> is_offered{};
    for (int slot = 0; slot < kOpeningHand; ++slot) {
        const CardId offered_cid = orig_own.hand[slot];
        res.offered_cards[slot] = offered_cid;
        int matched = -1;
        for (int i = 0; i < total_cards; ++i) {
            if (!is_offered[i] && full_deck[i].card_id == offered_cid) {
                is_offered[i] = true;
                matched = full_deck[i].physical_id;
                break;
            }
        }
        // Fallback if a custom synthetic test put a card in hand not listed in public_deck.
        res.offered_physical_ids[slot] = matched >= 0 ? matched : (1000 + slot);
    }

    // Remaining unoffered pool (36 physical cards): excludes the 4 offered physical IDs, retains other copies of same CardId!
    std::array<PhysicalCard, kDeckSize> unoffered{};
    int unoffered_count = 0;
    for (int i = 0; i < total_cards; ++i) {
        if (!is_offered[i]) unoffered[unoffered_count++] = full_deck[i];
    }

    // Shuffle unoffered pool with `sample_seed` (identical stream across all 16 masks!).
    std::mt19937_64 crn_rng(sample_seed);
    std::shuffle(unoffered.begin(), unoffered.begin() + unoffered_count, crn_rng);

    // 2. Apply SVWB official mulligan redraw: selected physical instances are excluded from redraw!
    res.redrawn_cards = res.offered_cards;
    res.redrawn_physical_ids = res.offered_physical_ids;
    std::array<PhysicalCard, kOpeningHand> returned{};
    int draw_cursor = 0;
    for (int slot = 0; slot < kOpeningHand; ++slot) {
        if ((mask & (1U << slot)) != 0 && draw_cursor < unoffered_count) {
            returned[draw_cursor] = {res.offered_cards[slot], res.offered_physical_ids[slot]};
            res.redrawn_cards[slot] = unoffered[draw_cursor].card_id;
            res.redrawn_physical_ids[slot] = unoffered[draw_cursor].physical_id;
            ++draw_cursor;
        }
    }

    // Build candidate-specific post-mulligan deck Deck(mask, k) via CRN Inside-Out Fisher-Yates insertion:
    // Start with unoffered[kOpeningHand .. unoffered_count-1] so post_deck[0] == unoffered[kOpeningHand] is shared
    // across masks unless a returned card uniformly swaps into slot 0, eliminating Turn-1 topdeck Winner's Curse!
    std::array<CardId, kDeckSize> post_deck{};
    int post_count = 0;
    const int common_start = std::min(kOpeningHand, unoffered_count);
    for (int i = common_start; i < unoffered_count; ++i) post_deck[post_count++] = unoffered[i].card_id;
    for (int i = draw_cursor; i < common_start; ++i) post_deck[post_count++] = unoffered[i].card_id;

    std::array<std::uint64_t, kOpeningHand> crn_insert_rands{};
    for (int i = 0; i < kOpeningHand; ++i) crn_insert_rands[i] = crn_rng();
    const std::uint64_t common_post_rng_state = crn_rng();

    for (int i = 0; i < draw_cursor; ++i) {
        const int pos = static_cast<int>(crn_insert_rands[i] % static_cast<std::uint64_t>(post_count + 1));
        post_deck[post_count] = post_deck[pos];
        post_deck[pos] = returned[i].card_id;
        ++post_count;
    }

    // 3. Construct clean sanitized state (opponent hand/deck also sampled from sample_seed, zero true-deck leakage).
    GameState sim = state;
    sim.rng_state = common_post_rng_state;

    // Opponent setup from public_deck only.
    auto& opp = sim.players[1 - actor];
    std::array<CardId, kDeckSize> opp_pool{};
    int opp_total = 0;
    const auto& opp_src = orig_opp.public_deck[0] ? orig_opp.public_deck : kMirrorDeck;
    for (int i = 0; i < kDeckSize; ++i) {
        if (opp_src[i] && is_non_faith_card(opp_src[i])) opp_pool[opp_total++] = opp_src[i];
    }
    std::shuffle(opp_pool.begin(), opp_pool.begin() + opp_total, crn_rng);
    opp.hand = {};
    opp.hand_modifiers = {};
    opp.deck = {};
    opp.deck_modifiers = {};
    opp.hand_count = kOpeningHand;
    std::copy_n(opp_pool.begin(), std::min(kOpeningHand, opp_total), opp.hand.begin());
    std::copy_n(opp_pool.begin(), opp_total, opp.deck.begin());
    opp.deck_idx = static_cast<std::uint8_t>(std::min(kOpeningHand, opp_total));
    opp.deck_end = static_cast<std::uint8_t>(opp_total);

    // Own post-mulligan setup.
    auto& own = sim.players[actor];
    own.hand = {};
    own.hand_modifiers = {};
    own.deck = {};
    own.deck_modifiers = {};
    own.hand_count = kOpeningHand;
    std::copy_n(res.redrawn_cards.begin(), kOpeningHand, own.hand.begin());
    std::copy_n(res.redrawn_cards.begin(), kOpeningHand, own.deck.begin());
    std::copy_n(post_deck.begin(), post_count, own.deck.begin() + kOpeningHand);
    own.deck_idx = kOpeningHand;
    own.deck_end = static_cast<std::uint8_t>(kOpeningHand + post_count);

    // Advance out of Mulligan phase into `actor`'s Turn 1 main phase.
    sim.turn_player = 1;
    apply_legal_action(sim, {ActionType::Mulligan, kNoTarget, kNoTarget, 0});
    if (actor == 1) {
        apply_legal_action(sim, {ActionType::EndTurn});
    }
    res.turn1_state = sim;
    return res;
}

namespace {

Action choose_mulligan_legacy(const GameState& state, ModelRef model, std::mt19937& rng,
                              const MatchupFeatures* matchup) {
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
            if (next.phase == Phase::Mulligan) apply_legal_action(next, {ActionType::Mulligan});
            if (actor == 1) apply_legal_action(next, {ActionType::EndTurn});
            if (model.attention) {
                values[mask.choice] += model.attention->value(extract_attention_tokens_for(next, *model.attention));
            } else {
                values[mask.choice] += model.mlp->predict(extract_features(next, matchup)).value;
            }
        }
    }
    const auto best = std::max_element(values.begin(), values.end()) - values.begin();
    return {ActionType::Mulligan, kNoTarget, kNoTarget, static_cast<std::uint8_t>(best)};
}

} // namespace

Action choose_mulligan(const GameState& state, ModelRef model, std::mt19937& rng,
                       const MatchupFeatures* matchup, SearchVersion version) {
    if (version == SearchVersion::LegacyV2) {
        return choose_mulligan_legacy(state, model, rng, matchup);
    }
    constexpr int kSamples = 8;
    std::array<std::uint64_t, kSamples> sample_seeds{};
    for (int k = 0; k < kSamples; ++k) {
        const auto hi = static_cast<std::uint64_t>(rng()) << 32;
        sample_seeds[k] = hi | static_cast<std::uint64_t>(rng());
    }

    std::array<float, 1 << kOpeningHand> values{};
    const auto masks = legal_actions(state);
    for (int k = 0; k < kSamples; ++k) {
        for (const auto mask_act : masks) {
            const auto res = simulate_mulligan_crn(state, mask_act.choice, sample_seeds[k]);
            if (model.attention) {
                values[mask_act.choice] += model.attention->value(
                    extract_attention_tokens_for(res.turn1_state, *model.attention));
            } else {
                values[mask_act.choice] += model.mlp->predict(extract_features(res.turn1_state, matchup)).value;
            }
        }
    }
    const auto best = std::max_element(values.begin(), values.end()) - values.begin();
    return {ActionType::Mulligan, kNoTarget, kNoTarget, static_cast<std::uint8_t>(best)};
}

Action choose_mulligan(const GameState& state, const Mlp& model, std::mt19937& rng,
                       const MatchupFeatures* matchup, SearchVersion version) {
    return choose_mulligan(state, ModelRef(model), rng, matchup, version);
}

Action choose_mulligan(const GameState& state, const attn::AttentionModel& model, std::mt19937& rng,
                       const MatchupFeatures* matchup, SearchVersion version) {
    return choose_mulligan(state, ModelRef(model), rng, matchup, version);
}

} // namespace svwb::wb
