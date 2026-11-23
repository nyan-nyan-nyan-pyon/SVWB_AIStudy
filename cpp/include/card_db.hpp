#pragma once

#include "state.hpp"

#include <initializer_list>
#include <string_view>

namespace svwb {

enum class CardType : std::uint8_t { FOLLOWER, SPELL, TOKEN };
enum class Trigger : std::uint8_t { ON_PLAY, ON_EVOLVE, ON_DEATH };
enum class EffectType : std::uint8_t { DAMAGE, HEAL, BUFF, DRAW, SUMMON };
enum class TargetType : std::uint8_t {
    NONE,
    CHOSEN_ENEMY_FOLLOWER,
    CHOSEN_ANY_ENEMY,
    ALL_ENEMY_FOLLOWERS,
    ENEMY_LEADER,
    SELF_LEADER,
    SELF_FOLLOWER,
};

struct Effect {
    Trigger trigger;
    EffectType type;
    TargetType target;
    std::int8_t amount;
    CardId summon_id = 0;
    std::int8_t hp_buff = 0; // For BUFF, amount is the attack adjustment.
};

inline constexpr int kEffectsPerCard = 2;

struct CardDef {
    CardId id;
    std::string_view name;
    CardType type;
    std::uint8_t cost;
    std::int8_t atk;
    std::int8_t hp;
    std::uint16_t flags = FLAG_NONE;
    std::array<Effect, kEffectsPerCard> effects{};
    std::uint8_t effect_count = 0;
};

inline constexpr CardId kKnightId = 11;
inline constexpr std::array<CardDef, 12> kCardDb{{
    {},
    {1, "ゴブリン", CardType::FOLLOWER, 1, 1, 2},
    {2, "ファイター", CardType::FOLLOWER, 2, 2, 2},
    {3, "盾の騎士", CardType::FOLLOWER, 2, 1, 3, FLAG_WARD},
    {4, "速攻の騎手", CardType::FOLLOWER, 3, 2, 2, FLAG_STORM},
    {5, "突撃兵", CardType::FOLLOWER, 3, 3, 2, FLAG_RUSH},
    {6, "知識の精霊", CardType::FOLLOWER, 2, 1, 1, FLAG_NONE, {{
        {Trigger::ON_DEATH, EffectType::DRAW, TargetType::NONE, 1},
    }}, 1},
    {7, "召喚師", CardType::FOLLOWER, 4, 2, 2, FLAG_NONE, {{
        {Trigger::ON_PLAY, EffectType::SUMMON, TargetType::NONE, 2, kKnightId},
    }}, 1},
    {8, "炎の魔導師", CardType::FOLLOWER, 4, 3, 4, FLAG_NONE, {{
        {Trigger::ON_EVOLVE, EffectType::DAMAGE, TargetType::CHOSEN_ENEMY_FOLLOWER, 2},
    }}, 1},
    {9, "火球", CardType::SPELL, 2, 0, 0, FLAG_NONE, {{
        {Trigger::ON_PLAY, EffectType::DAMAGE, TargetType::CHOSEN_ANY_ENEMY, 3},
    }}, 1},
    {10, "聖なる光", CardType::SPELL, 5, 0, 0, FLAG_NONE, {{
        {Trigger::ON_PLAY, EffectType::DAMAGE, TargetType::ALL_ENEMY_FOLLOWERS, 2},
        {Trigger::ON_PLAY, EffectType::HEAL, TargetType::SELF_LEADER, 3},
    }}, 2},
    {kKnightId, "ナイト", CardType::TOKEN, 1, 1, 1},
}};

[[nodiscard]] constexpr const CardDef* find_card(CardId id) noexcept {
    return id > 0 && id < kCardDb.size() ? &kCardDb[id] : nullptr;
}

namespace detail {

struct DeckEntry {
    CardId id;
    int copies;
};

consteval Deck make_deck(std::initializer_list<DeckEntry> entries) {
    Deck deck{};
    std::size_t size = 0;
    for (const auto& [id, copies] : entries) {
        const auto* card = find_card(id);
        if (card == nullptr || card->type == CardType::TOKEN ||
            copies <= 0 || static_cast<std::size_t>(copies) > deck.size() - size) {
            throw "Invalid test deck entry";
        }
        for (int i = 0; i < copies; ++i) {
            deck[size++] = id;
        }
    }
    if (size != deck.size()) {
        throw "Test deck must contain 40 cards";
    }
    return deck;
}

} // namespace detail

inline constexpr Deck kDeckA = detail::make_deck({
    {1, 6}, {2, 6}, {6, 6}, {4, 8}, {5, 4}, {7, 4}, {9, 6},
});
inline constexpr Deck kDeckB = detail::make_deck({
    {2, 4}, {3, 8}, {6, 6}, {5, 6}, {8, 6}, {9, 4}, {10, 6},
});

} // namespace svwb
