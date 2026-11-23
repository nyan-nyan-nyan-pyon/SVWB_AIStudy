#pragma once

#include "wb_rules.hpp"

#include <span>
#include <string_view>

namespace svwb::wb {

inline constexpr CardId kWingedStatue = 101;
inline constexpr CardId kLilium = 102;
inline constexpr CardId kExias = 103;
inline constexpr CardId kCraftsman = 104;
inline constexpr CardId kColette = 105;
inline constexpr CardId kAlmiraj = 106;
inline constexpr CardId kDunamis = 107;
inline constexpr CardId kTrainer = 108;
inline constexpr CardId kTestimony = 109;
inline constexpr CardId kProjectedStatue = 110;
inline constexpr CardId kRituel = 111;
inline constexpr CardId kPropagation = 112;
inline constexpr CardId kRapid = 113;
inline constexpr CardId kTeresa = 114;
inline constexpr CardId kRabbit = 115;
inline constexpr CardId kEvidence = 116;
inline constexpr CardId kHolyFalcon = 117;
inline constexpr CardId kSplendidHawk = 118;
inline constexpr CardId kHealingSister = 119;
inline constexpr CardId kAlmirajCrystal = 120;
inline constexpr int kDeckCardKinds = 14;
inline constexpr int kEffectSlots = 8;
inline constexpr std::uint8_t kEnhancedMode = 4;

enum class Kind : std::uint8_t { Follower, Amulet, Spell };
enum class Trigger : std::uint8_t {
    None, OnPlay, OnEnter, OnEvolve, OnSuperEvolve, OnEvolved, LastWords, OnAct,
    TurnStart, TurnEnd, EnemyTurnEnd, OnAttack, OnClash, OnDamage,
};
enum class Operation : std::uint8_t {
    Damage, Heal, Draw, AddHand, Summon, SummonModified, SummonEvolved, ReduceCountdown, SpendClue,
    Silence, Banish, Destroy, Transform, Bounce, RestorePp, GrantCrest, ExtendCrest,
    EvolveWithAlly, BanishLowestAndEvolve,
};
enum class Target : std::uint8_t {
    None, Self, OwnLeader, EnemyLeader, ChosenEnemyFollower, ChosenEnemyPermanent, AllEnemyFollowers,
    RandomEnemyFollower, BothLeaders,
};

struct Effect {
    Trigger trigger = Trigger::None;
    Operation operation{};
    Target target{};
    int amount = 0;
    CardId card = 0;
    int health_modifier = 0;
    std::uint8_t mode = 0;
    CrestId crest = CrestId::None;
    std::uint16_t granted_abilities = 0;
};

struct CardDef {
    CardId id;
    std::string_view name;
    Kind kind;
    int cost;
    int atk = 0;
    int hp = 0;
    std::uint16_t abilities = 0;
    int countdown = 0;
    int act_cost = -1;
    int modes = 0;
    int enhance_cost = 0;
    CardId crystal_form = 0;
    bool clue = false;
    std::array<Effect, kEffectSlots> effects{};
};

struct CrestDef {
    CrestId id;
    std::uint8_t countdown;
    CrestClock clock;
    std::array<Effect, kEffectSlots> effects;
};

[[nodiscard]] const CardDef* find_card(CardId id);
[[nodiscard]] const CrestDef* find_crest(CrestId id);
[[nodiscard]] CardId printed_card(CardId id);
[[nodiscard]] bool is_deck_card(CardId id);

// Resolves already-selected effect steps in order, using Trigger::None for immediate steps.
// Source and selected target are stable creation IDs, or zero when absent.
void resolve_effect_steps(GameState& state, int owner, std::span<const Effect> effects,
                          EntityId source = 0, EntityId target = 0, const RuleConfig& rules = {});

inline constexpr Deck kProvisionalMirror = [] {
    Deck deck{};
    std::size_t cursor = 0;
    for (int i = 0; i < kDeckCardKinds; ++i) {
        const int copies = i < kDeckSize - 2 * kDeckCardKinds ? 3 : 2;
        for (int copy = 0; copy < copies; ++copy) {
            deck[cursor++] = static_cast<CardId>(kFirstCard + i);
        }
    }
    return deck;
}();

} // namespace svwb::wb
