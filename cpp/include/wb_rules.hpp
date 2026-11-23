#pragma once

#include "state.hpp"

namespace svwb::wb {

inline constexpr int kCrestLimit = 5;
inline constexpr int kTurnLimit = 40;
inline constexpr int kNameActionLimit = 20;
inline constexpr CardId kFirstCard = 101;
inline constexpr CardId kLastForm = 121;
inline constexpr int kCardSlots = kLastForm - kFirstCard + 1;
using EntityId = std::uint32_t;

enum class MulliganReturn : std::uint8_t { Shuffle, Bottom };
enum class CrestClock : std::uint8_t { Printed, Start, End };
enum class SimultaneousLethal : std::uint8_t { SourceOpponent, NonActive, BothLose };
enum class RandomTargets : std::uint8_t { PerStep, AtAbilityStart };
enum class Priority : std::uint8_t {
    Manual, Transition, LastWords, ActiveCrest, ActiveBoard, OtherCrest, OtherBoard, Deck, Count,
};

struct RuleConfig {
    MulliganReturn mulligan_return = MulliganReturn::Shuffle;              // U01
    bool refresh_before_start_effects = true;                              // U02
    CrestClock crest_clock = CrestClock::Printed;                           // U03
    SimultaneousLethal simultaneous_lethal = SimultaneousLethal::SourceOpponent; // U04
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
};

enum class Evolution : std::uint8_t { None, Normal, Super };
enum class Phase : std::uint8_t { Mulligan, Main };
enum class Outcome : std::uint8_t { Ongoing, FirstWins, SecondWins, BothLose };
enum class EndReason : std::uint8_t { None, Health, EmptyDeck, TurnLimit };
enum class CrestId : std::uint8_t { None, Testimony, Rituel, Rapid };

// Supplemental ability flags used by the extended combat and target rules.
inline constexpr std::uint16_t kBane = 1 << 7;
inline constexpr std::uint16_t kBarrier = 1 << 8;
inline constexpr std::uint16_t kAura = 1 << 9;
inline constexpr std::uint16_t kAmbush = 1 << 10;
inline constexpr std::uint16_t kIntimidate = 1 << 11;
inline constexpr std::uint16_t kAttackBlocked = 1 << 12;
inline constexpr std::uint16_t kDrain = 1 << 13;

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

    bool operator==(const Permanent&) const = default;
};

struct Crest {
    EntityId creation_order;
    CrestId id;
    std::uint8_t countdown;

    bool operator==(const Crest&) const = default;
};

struct NamedActions {
    std::uint8_t play;
    std::uint8_t attack;
    std::uint8_t act;

    bool operator==(const NamedActions&) const = default;
};

struct PlayerState {
    Deck deck;
    std::array<CardId, kHandLimit> hand;
    std::array<Permanent, kBoardLimit> board;
    std::array<Crest, kCrestLimit> crests;
    std::array<NamedActions, kCardSlots> actions;
    std::array<std::uint16_t, kCardSlots> destroyed_followers; // By printed card; independent of shadows.
    std::uint16_t shadows;
    std::int16_t leader_hp;
    std::uint8_t deck_idx;
    std::uint8_t hand_count;
    std::uint8_t board_count;
    std::uint8_t crest_count;
    std::uint8_t pp; // Unspent regular PP; bonus_pp is temporary.
    std::uint8_t max_pp;
    std::uint8_t bonus_pp;
    std::uint8_t ep;
    std::uint8_t sep;
    std::uint8_t extra_pp_uses;
    bool extra_pp_available;
    bool manual_evolved;
    bool leader_barrier;

    [[nodiscard]] int available_pp() const { return pp + bonus_pp; }
    [[nodiscard]] bool overflow() const { return max_pp >= 7; }
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

enum class ActionType : std::uint8_t { EndTurn, Play, Evolve, SuperEvolve, Attack, Act, ExtraPp, Mulligan };
struct Action {
    ActionType type = ActionType::EndTurn;
    std::int8_t src = kNoTarget;
    std::int8_t dst = kNoTarget;
    std::uint8_t choice = 0; // One-based mode or four-bit mulligan mask.

    bool operator==(const Action&) const = default;
};

inline constexpr int kMaxActions =
    2 + 3 * kHandLimit * (kBoardLimit + 1) + kBoardLimit * (2 * kBoardLimit + kBoardLimit + 4);

class ActionList {
public:
    [[nodiscard]] const Action* begin() const { return actions_.data(); }
    [[nodiscard]] const Action* end() const { return begin() + count_; }
    [[nodiscard]] int size() const { return count_; }
    [[nodiscard]] bool empty() const { return count_ == 0; }
    [[nodiscard]] Action operator[](int index) const { return actions_[index]; }

private:
    friend ActionList legal_actions(const GameState&, const RuleConfig&);
    std::array<Action, kMaxActions> actions_;
    int count_ = 0;
};

[[nodiscard]] GameState new_game(const Deck& first, const Deck& second, std::uint64_t seed);
[[nodiscard]] ActionList legal_actions(const GameState& state, const RuleConfig& rules = {});
[[nodiscard]] bool apply_action(GameState& state, Action action, const RuleConfig& rules = {});
// The caller must already know that the action is legal for this position and rule configuration.
void apply_legal_action(GameState& state, Action action, const RuleConfig& rules = {});
[[nodiscard]] bool can_choose_enemy_permanent(const Permanent& object);

static_assert(std::is_trivial_v<GameState> && std::is_standard_layout_v<GameState>);

} // namespace svwb::wb
