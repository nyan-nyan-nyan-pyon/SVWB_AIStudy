#pragma once

#include "state.hpp"

#include <compare>
#include <vector>
namespace svwb::wb {

inline constexpr int kCrestLimit = 5;
inline constexpr int kTurnLimit = 40;
inline constexpr int kNameActionLimit = 20;
inline constexpr CardId kFirstCard = 101;
inline constexpr CardId kLastForm = 121;
inline constexpr int kCardSlots = kLastForm - kFirstCard + 1;
inline constexpr int kDeckCapacity = 80;
inline constexpr int kAdditionalCardCounters = 128;
using EntityId = std::uint32_t;
inline constexpr int kOwnBoardTarget = kBoardLimit;
inline constexpr int kOwnHandTarget = 2 * kBoardLimit;
inline constexpr EntityId kHandTargetFlag = EntityId{1} << 31;
inline constexpr std::uint16_t kBothBoardsSelection = 1 << 15;

enum class MulliganReturn : std::uint8_t { Shuffle, Bottom };
enum class CrestClock : std::uint8_t { Printed, Start, End };
enum class SimultaneousLethal : std::uint8_t { SourceOpponent, NonActive, BothLose };
enum class RandomTargets : std::uint8_t { PerStep, AtAbilityStart };
enum class LalaSummonTiming : std::uint8_t { OpponentTurnEnd, OwnerTurnStart, Disabled };
enum class Priority : std::uint8_t {
    Manual, Transition, LastWords, ActiveCrest, ActiveBoard, OtherCrest, OtherBoard, Deck, Count,
};

struct RuleConfig {
    MulliganReturn mulligan_return = MulliganReturn::Shuffle;              // U01
    bool refresh_before_start_effects = true;                              // U02
    CrestClock crest_clock = CrestClock::Printed;                           // U03
    SimultaneousLethal simultaneous_lethal = SimultaneousLethal::NonActive; // U04
    bool turn_limit_after_end_effects = true;                              // U04
    bool super_damage_per_follower = true;                                 // U05
    bool zero_damage_consumes_barrier = true;                             // U06
    bool zero_damage_triggers = false;                                     // U06
    bool blocked_attacker_can_retaliate = true;                             // U07
    bool act_on_entry_turn = true;                                          // U08
    bool reset_name_counts_on_transform = false;                           // U08
    bool require_live_ability_source = true;                                // U09
    std::array<std::uint8_t, static_cast<int>(Priority::Count)> priority{0, 1, 2, 3, 4, 5, 6, 7}; // U10
    bool replace_oldest_crest_when_full = false;                            // U11
    bool expired_crest_adds_shadow = false;                                 // U12
    bool merge_clue_on_full_board = false;                                  // U12
    RandomTargets random_targets = RandomTargets::PerStep;                 // U14
    bool rabbit_evolves_with_empty_hand = false;
    bool silence_removes_super_abilities = true;
    LalaSummonTiming lala_summon_timing = LalaSummonTiming::OpponentTurnEnd;
    bool refusion_after_hand_transform = true;
    bool fusion_requires_trigger_pp = false;
    bool highlander_counts_card_names = true;
    bool round_half_max_hp_up = false;
    bool burn_crest_heal_on_enemy_turn = false;
    std::uint8_t fanfare_repeat_limit = 64;
};

enum class Evolution : std::uint8_t { None, Normal, Super };
enum class Phase : std::uint8_t { Mulligan, Main };
enum class Outcome : std::uint8_t { Ongoing, FirstWins, SecondWins, BothLose };
enum class EndReason : std::uint8_t { None, Health, EmptyDeck, TurnLimit };
enum class CrestId : std::uint8_t {
    None, Testimony, Rituel, Rapid, Haylem, Erralde, Kukishiro, Sisters,
    Shrink, Expand, Dustdays, DeerKing, Beasts, Unkei, March, Gildaria, Almerize,
    Yube, Wadatsumi, Reflection, Okuyoka, Miltio, IstenDead, Gremory,
    Insomnia, Lincle, Shim, Tico, Sephie, NoinInterrogation, Noin, Lala, Sloth, Cutthroat, Stranger,
    BurnedKnight, ElderDragon, Rezeal,
};

// Supplemental ability flags used by the extended combat and target rules.
inline constexpr std::uint16_t kBane = 1 << 7;
inline constexpr std::uint16_t kBarrier = 1 << 8;
inline constexpr std::uint16_t kAura = 1 << 9;
inline constexpr std::uint16_t kAmbush = 1 << 10;
inline constexpr std::uint16_t kIntimidate = 1 << 11;
inline constexpr std::uint16_t kAttackBlocked = 1 << 12;
inline constexpr std::uint16_t kDrain = 1 << 13;
inline constexpr std::uint16_t kDoubleStrike = 1 << 14;
inline constexpr std::uint16_t kIgnoreWard = 1 << 15;
inline constexpr std::uint8_t kLoseLastWords = 1 << 0;
inline constexpr std::uint8_t kRebirthLastWords = 1 << 1;
inline constexpr std::uint8_t kDeadTrait = 1 << 2;
inline constexpr std::uint8_t kFusedBeta = 1 << 3;
inline constexpr std::uint8_t kFusedGamma = 1 << 4;

struct CardModifiers {
    std::uint16_t abilities;
    std::int8_t attack;
    std::int8_t defense;
    std::int8_t cost;
    std::uint8_t spellboost;
    std::uint8_t fused_ply;
    std::uint8_t fused_materials;
    std::int8_t temporary_cost_delta;
    std::uint8_t effect_flags;

    auto operator<=>(const CardModifiers&) const = default;
};

struct Permanent {
    EntityId creation_order;
    CardId card_id;
    std::int16_t atk;
    std::int16_t hp;
    std::int16_t max_hp;
    std::uint16_t abilities;
    std::uint16_t clue_count;
    std::uint8_t countdown;
    std::uint8_t entry_ply;
    Evolution evolution;
    bool silenced;
    bool readied;
    bool attacked_this_turn;
    bool acted_this_turn;
    bool attacked_twice;
    std::uint16_t earth_count;
    CardModifiers modifiers;
    std::uint16_t temporary_abilities;
    std::uint8_t expires_end_ply;
    std::uint8_t sequence;
    bool banish_at_end;
    std::uint8_t last_trigger_ply;
    std::int16_t temporary_attack;
    bool attacked_thrice;
    std::uint8_t attack_limit;

    bool operator==(const Permanent&) const = default;
};

struct Crest {
    EntityId creation_order;
    CrestId id;
    std::uint8_t countdown;
    std::uint8_t sequence;
    std::uint8_t last_trigger_round;

    bool operator==(const Crest&) const = default;
};

struct NamedActions {
    std::uint8_t play;
    std::uint8_t attack;
    std::uint8_t act;

    bool operator==(const NamedActions&) const = default;
};

struct CardCounters {
    CardId id;
    std::uint16_t destroyed;
    NamedActions actions;
    std::uint8_t entered;

    bool operator==(const CardCounters&) const = default;
};

struct PlayerState {
    std::array<CardId, kDeckCapacity> deck;
    std::array<CardModifiers, kDeckCapacity> deck_modifiers;
    std::array<CardId, kHandLimit> hand;
    std::array<CardModifiers, kHandLimit> hand_modifiers;
    std::array<Permanent, kBoardLimit> board;
    std::array<Crest, kCrestLimit> crests;
    std::array<CardId, kCrestLimit> faiths;
    std::array<std::uint16_t, kCrestLimit> faith_values;
    std::array<std::uint8_t, kCrestLimit> faith_upgrades;
    std::array<NamedActions, kCardSlots> actions;
    std::array<std::uint16_t, kCardSlots> destroyed_followers; // By printed card; independent of shadows.
    std::array<std::uint16_t, kCardSlots> destroyed_amulets;
    std::array<CardCounters, kAdditionalCardCounters> other_counters;
    std::array<CardId, kDeckSize> public_deck;
    std::array<CardId, kDeckSize> public_generated;
    std::uint64_t public_spent;
    std::uint16_t shadows;
    std::uint16_t followers_entered;
    std::uint16_t plays_this_turn;
    std::uint16_t played_cost_mask;
    std::int16_t leader_hp;
    std::int16_t leader_max_hp;
    std::uint16_t artifact_types_entered;
    std::uint8_t deck_idx;
    std::uint8_t deck_end;
    std::uint8_t hand_count;
    std::uint8_t board_count;
    std::uint8_t crest_count;
    std::uint8_t faith_count;
    std::uint8_t other_counter_count;
    std::uint8_t public_generated_count;
    std::uint8_t pp; // Unspent regular PP; bonus_pp is temporary.
    std::uint8_t max_pp;
    std::uint8_t bonus_pp;
    std::uint8_t ep;
    std::uint8_t sep;
    std::uint8_t extra_pp_uses;
    bool extra_pp_available;
    bool manual_evolved;
    bool leader_barrier;
    bool acted_evidence;
    bool acted_amulet;
    bool attacked_leader_this_turn;
    bool attacked_leader_last_turn;

    [[nodiscard]] int available_pp() const { return pp + bonus_pp; }
    [[nodiscard]] bool overflow() const { return max_pp >= 7; }
    [[nodiscard]] int deck_remaining() const { return deck_end - deck_idx; }
    bool operator==(const PlayerState&) const = default;
};

struct GameState {
    std::array<PlayerState, kPlayers> players;
    std::uint64_t rng_state;
    EntityId next_creation_order;
    std::uint8_t turn_player;
    std::uint8_t turn_number;
    Phase phase;
    Outcome outcome;
    EndReason end_reason;

    bool operator==(const GameState&) const = default;
};

enum class ActionType : std::uint8_t { EndTurn, Play, Evolve, SuperEvolve, Attack, Act, ExtraPp, Mulligan, Fuse };
struct Action {
    ActionType type = ActionType::EndTurn;
    std::int8_t src = kNoTarget;
    std::int8_t dst = kNoTarget;
    std::uint8_t choice = 0; // One-based mode or a mulligan/fusion mask. Fusion skips the source slot.
    std::uint16_t selection = 0; // Slot mask for effects selecting multiple cards from one zone.
    bool selection_in_hand = false;
    bool selection_on_enemy = false;
    std::int8_t secondary_slot = kNoTarget;

    bool operator==(const Action&) const = default;
};

enum class PlayCopies : std::uint8_t { All, Canonical };

inline constexpr int kInlineActions = 2 + 3 * kHandLimit * (kBoardLimit + 1) +
    kBoardLimit * (3 * kBoardLimit + 4);

class ActionList {
public:
    [[nodiscard]] const Action* begin() const { return extended_.empty() ? actions_.data() : extended_.data(); }
    [[nodiscard]] const Action* end() const { return begin() + count_; }
    [[nodiscard]] int size() const { return count_; }
    [[nodiscard]] bool empty() const { return count_ == 0; }
    [[nodiscard]] Action operator[](int index) const { return begin()[index]; }

private:
    friend ActionList legal_actions(const GameState&, const RuleConfig&, PlayCopies);
    void push(Action action) {
        if (count_ < kInlineActions) {
            actions_[count_] = action;
        } else {
            if (extended_.empty()) extended_.assign(actions_.begin(), actions_.end());
            extended_.push_back(action);
        }
        ++count_;
    }
    std::array<Action, kInlineActions> actions_;
    std::vector<Action> extended_;
    int count_ = 0;
};

[[nodiscard]] GameState new_game(const Deck& first, const Deck& second, std::uint64_t seed);
[[nodiscard]] ActionList legal_actions(const GameState& state, const RuleConfig& rules = {},
                                      PlayCopies copies = PlayCopies::All);
[[nodiscard]] bool apply_action(GameState& state, Action action, const RuleConfig& rules = {});
// The caller must already know that the action is legal for this position and rule configuration.
void apply_legal_action(GameState& state, Action action, const RuleConfig& rules = {});
// True if the action resolved without relying on hidden card identities or a random choice.
[[nodiscard]] bool apply_observed_action(GameState& state, Action action, const RuleConfig& rules = {});
[[nodiscard]] bool can_choose_enemy_permanent(const Permanent& object);
[[nodiscard]] bool follower_fanfare_suppressed(const PlayerState& player);
[[nodiscard]] inline bool attack_ready(const Permanent& object) {
    const int limit = object.attack_limit >= 3 ? object.attack_limit : 1 + ((object.abilities & kDoubleStrike) != 0);
    return !object.attacked_thrice && (!object.attacked_twice || limit == 3) &&
        (!object.attacked_this_turn || limit >= 2) &&
        (object.abilities & kAttackBlocked) == 0 &&
        (object.readied || (object.abilities & (FLAG_RUSH | FLAG_STORM)) != 0);
}

static_assert(std::is_trivial_v<GameState> && std::is_standard_layout_v<GameState>);
static_assert(sizeof(GameState) <= 5632);

} // namespace svwb::wb
