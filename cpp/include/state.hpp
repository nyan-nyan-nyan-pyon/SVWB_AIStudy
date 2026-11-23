#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

namespace svwb {

inline constexpr int kPlayers = 2;
inline constexpr int kDeckSize = 40;
inline constexpr int kHandLimit = 9;
inline constexpr int kBoardLimit = 5;
inline constexpr int kLeaderMaxHp = 20;
inline constexpr int kMaxPp = 10;
inline constexpr int kOpeningHand = 4;
inline constexpr std::int8_t kOngoing = -1;
inline constexpr std::int8_t kDraw = 2;

using CardId = std::uint16_t;
using Deck = std::array<CardId, kDeckSize>;

enum FollowerFlags : std::uint16_t {
    FLAG_NONE = 0,
    FLAG_WARD = 1 << 0,
    FLAG_STORM = 1 << 1,
    FLAG_RUSH = 1 << 2,
    FLAG_EVOLVED = 1 << 3,
    FLAG_CAN_ATTACK = 1 << 4,
    FLAG_CAN_HIT_FACE = 1 << 5,
    FLAG_ATTACKED_THIS_TURN = 1 << 6,
};

struct Follower {
    CardId card_id;
    std::int8_t atk;
    std::int8_t hp;
    std::int8_t max_hp;
    std::uint8_t reserved;
    std::uint16_t flags;

    bool operator==(const Follower&) const = default;
};

struct PlayerState {
    std::int8_t leader_hp;
    std::uint8_t pp;
    std::uint8_t max_pp;
    std::uint8_t ep;
    std::uint8_t shadows;
    std::uint8_t hand_count;
    std::uint8_t board_count;
    std::uint8_t deck_idx;
    std::uint8_t evolved_this_turn;
    std::array<CardId, kHandLimit> hand;
    std::array<Follower, kBoardLimit> board;
    Deck deck;

    bool operator==(const PlayerState&) const = default;
};

struct GameState {
    std::array<PlayerState, kPlayers> players;
    std::uint8_t turn_player;
    std::uint8_t turn_number; // Both players take turn N before this increments.
    std::int8_t winner;       // kOngoing, player index, or kDraw.

    bool operator==(const GameState&) const = default;
};

enum class ActionType : std::uint8_t { END_TURN = 0, PLAY_CARD, EVOLVE, ATTACK };

inline constexpr std::int8_t kNoTarget = -2;
inline constexpr std::int8_t kEnemyLeader = -1;

struct Action {
    ActionType type;
    std::int8_t src; // Hand slot for PLAY_CARD, own board slot for EVOLVE or ATTACK.
    std::int8_t dst; // Enemy board slot, kEnemyLeader, or kNoTarget.

    bool operator==(const Action&) const = default;
};

static_assert(std::is_trivial_v<GameState> && std::is_standard_layout_v<GameState>);
static_assert(sizeof(Follower) == 8);
static_assert(sizeof(GameState) == 300);
static_assert(sizeof(Action) == 3);

} // namespace svwb
