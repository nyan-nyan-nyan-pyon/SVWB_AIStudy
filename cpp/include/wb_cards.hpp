#pragma once

#include "wb_rules.hpp"

#include <filesystem>
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
inline constexpr CardId kCorrector = 121;
inline constexpr CardId kEarthSigil = 123;
inline constexpr CardId kFaithToken = 124;
inline constexpr int kDeckCardKinds = 15;
inline constexpr int kEffectSlots = 8;
inline constexpr std::uint8_t kEnhancedMode = 4;
inline constexpr std::uint8_t kAllModes = 5;
inline constexpr std::uint8_t kBaseModeOnly = 6;

enum class Kind : std::uint8_t { Follower, Amulet, Spell, Faith };
enum class Trigger : std::uint8_t {
    None, OnPlay, OnEnter, OnEvolve, OnSuperEvolve, OnEvolved, LastWords, OnAct,
    TurnStart, TurnEnd, EnemyTurnEnd, OnAttack, OnClash, OnDamage, OnFuse, OnDraw, OnLeaderHeal,
    OnOtherPlay, OnAmuletDestroyed, OnBanish, OnEnemyFollowerBanished,
    OnFollowerEntered, OnEnemyFollowerEntered, OnAllyEvolved, OnBuffed, OnEnhanced,
};
enum class Operation : std::uint8_t {
    Damage, Heal, Draw, AddHand, Summon, SummonModified, SummonEvolved, ReduceCountdown, SpendClue,
    Silence, Banish, Destroy, Transform, Bounce, RestorePp, GrantCrest, ExtendCrest,
    Buff, ReturnToDeck, Copy, IncreaseMaxPp, Necromancy, EarthRite, AddEarth, Reanimate,
    GrantFaith, Discard, Custom, RestoreEp, RemoveAbilities, AutoEvolve, DrawFiltered,
    AddCountdown, SummonEnemy, TemporaryAbilities, AddCombo, SplitDamage, BuffDeck,
    DrawNamed, UpgradeFaith,
};
enum class Target : std::uint8_t {
    None, Self, OwnLeader, EnemyLeader, ChosenEnemyFollower, ChosenEnemyPermanent, AllEnemyFollowers,
    RandomEnemyFollower, BothLeaders, ChosenOwnFollower, ChosenOwnPermanent, ChosenOwnHand,
    ChosenSmallEnemyFollower, ChosenOtherOwnFollower, ChosenOtherPermanent, ChosenOwnHandThree,
    ChosenOtherOwnTwoFollowers, AllOwnFollowers, AllOtherOwnFollowers, AllFollowers,
    AllOtherFollowers, AllEnemyFollowersAndLeader, AllOwnAmulets, AllSmallEnemyFollowers,
    RandomOtherEnemyFollower, StrongestEnemyFollower, AllNonAssailantFollowers,
    AttackingFollower, RandomEnemyTwo, RandomOtherUnevolvedWard,
    EventFollower, EnemyEventFollower, ChosenOwnSpell, ChosenOwnHandTwo,
    AllOwnFairyFollowers, AllOtherOwnRoyalFollowers, AllOwnNamedCards, ChosenUnevolvedOwnFollower, ChosenEnemyOrLeader,
};
enum class Condition : std::uint8_t {
    Always, Combo, Rally, Overflow, CostPaid, AtMaxPp, Fused, NotFused, OwnTurn, OwnAmulets,
    BoardCostAtLeast, OtherBoardCost, FollowerAttack, SelfEvolved, NormalEvolution,
    OwnSuper, OwnEvolvedWithoutSuper, PlayedOneThroughEight,
    BelowCombo, BelowRally, OwnCostOneCards, OtherOwnCostOneCards, BelowOwnCostOneCards, EventFairy,
    EventSpell, EvidenceActed, EvidenceUnused, HandSpells, SuperTurn, EnhanceAtLeast, EffectiveCost,
};
enum class CardHandler : std::uint8_t {
    None, EvolveWithAlly, BanishLowestAndEvolve, MatchPlayedCost, SequenceCity, SequenceOmerio,
    DrawCrest, CopyBanish, RandomKeywords, MarkHoneyTarget, TopHandCosts, ReplaceHandFromEnemyDeck,
    CopyEnemyHand, DiscardThenCopy, DrawDiscounted, DrawWardCount, NeutralHandDamage,
    AmuletHandDamage, DestroyAndReward, SummonFromDeckEvolved, DestroyedToHand, DestroyedToDeck,
    SummonDestroyedFollowers, SummonDestroyedAmulets, GrantAttackTwice, ReturnRandomHand,
    BuddingBuddies, CopyEnemyDeck, SummonDeckDistinct, Marlon, AlmerizeSequence,
    RepeatedAttackDamage, CopySource, NewspaperCopies, DiscardAndHeal, EnhanceTroops,
};
enum class FusionMaterial : std::uint8_t { None, Any, Follower, Spell, Amulet, SameClass, Trait };
enum class DeckFilter : std::uint8_t { Any, Follower, Amulet, Neutral, RoyalFollower };
enum class Magnitude : std::uint8_t { Fixed, SourceAttack, NegativeSourceAttack, Combo, OwnFollowers };

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
    Evolution transformed_evolution = Evolution::None;
    Condition condition = Condition::Always;
    int threshold = 0;
    CardHandler handler = CardHandler::None;
    int spellboost_multiplier = 0;
    DeckFilter filter = DeckFilter::Any;
    Magnitude magnitude = Magnitude::Fixed;
    int cost_override = -1;
    CardId alternate_card = 0;
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
    std::uint32_t official_id = 0;
    std::string_view class_name{};
    std::string_view traits{}; // newline-delimited labels from the catalog
    std::string_view keywords{};
    std::string_view text{};
    std::string_view evolved_text{};
    std::span<const CardId> related{};
    bool implemented = false;
    bool deck_eligible = false;
    bool earth = false;
    bool spellboost = false;
    int spellboost_cost_reduction = 0;
    FusionMaterial fusion = FusionMaterial::None;
    std::string_view fusion_trait{};
    CardId accelerate_form = 0;
    CardId base_form = 0;
    bool all_modes_with_high_cost = false;
    int super_evolve_hand_discount = 0;
    int next_enhance_cost = 0;
    int full_combo_modes = 0;
    int full_board_modes = 0;
    bool enhanced_all_modes = false;
    bool evolve_modes = false;
    int hand_evolve_discount = 0;
    int hand_super_cost = -1;
    int drawn_cost = -1;
    int hand_end_combo = 0;
    bool hand_swap_on_evidence = false;
    bool once_per_own_turn = false;
    CardId faith = 0;
    Trigger faith_trigger = Trigger::None;
};

struct CrestDef {
    CrestId id;
    std::uint8_t countdown;
    CrestClock clock;
    std::array<Effect, kEffectSlots> effects;
    std::string_view name;
    std::uint32_t official_id;
};

[[nodiscard]] const CardDef* find_card(CardId id);
[[nodiscard]] const CardDef* find_official_card(std::uint32_t id);
[[nodiscard]] const CrestDef* find_crest(CrestId id);
[[nodiscard]] CardId printed_card(CardId id);
[[nodiscard]] bool is_deck_card(CardId id);
[[nodiscard]] Deck read_deck(const std::filesystem::path& path);
[[nodiscard]] int card_cost(const CardDef& card, CardModifiers modifiers);
[[nodiscard]] int enhance_payment(const CardDef& card, int pp);
[[nodiscard]] std::span<const CardId> neutral_bishop_rotation();
[[nodiscard]] std::span<const CardId> elf_rotation();
[[nodiscard]] std::span<const CardId> royal_rotation();

// Resolves already-selected effect steps in order, using Trigger::None for immediate steps.
// Source and selected target are stable creation IDs, or zero when absent.
void resolve_effect_steps(GameState& state, int owner, std::span<const Effect> effects,
                          EntityId source = 0, EntityId target = 0, const RuleConfig& rules = {});

inline constexpr Deck kProvisionalMirror = [] {
    Deck deck{};
    std::size_t cursor = 0;
    for (int i = 0; i < 14; ++i) {
        const int copies = i < 12 ? 3 : 2;
        for (int copy = 0; copy < copies; ++copy) {
            deck[cursor++] = static_cast<CardId>(kFirstCard + i);
        }
    }
    return deck;
}();

inline constexpr std::array<CardId, kDeckCardKinds> kMirrorCardIds{
    kWingedStatue, kLilium, kExias, kCraftsman, kColette, kAlmiraj, kDunamis, kTrainer,
    kTestimony, kProjectedStatue, kRituel, kPropagation, kRapid, kTeresa, kCorrector,
};

inline constexpr Deck kMirrorDeck = [] {
    constexpr std::array<int, kDeckCardKinds> copies{3, 3, 3, 2, 2, 3, 2, 3, 3, 2, 3, 3, 3, 2, 3};
    Deck deck{};
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < copies.size(); ++i) {
        for (int copy = 0; copy < copies[i]; ++copy) {
            deck[cursor++] = kMirrorCardIds[i];
        }
    }
    if (cursor != deck.size()) {
        throw "The mirror deck must contain exactly 40 cards";
    }
    return deck;
}();

} // namespace svwb::wb
