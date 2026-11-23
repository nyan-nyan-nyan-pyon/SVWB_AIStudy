#include "card_db.hpp"
#include "engine.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace svwb;
using enum ActionType;

constexpr Action kEndTurn{END_TURN, kNoTarget, kNoTarget};

void check(bool condition, std::string_view expression,
           std::source_location location = std::source_location::current()) {
    if (!condition) {
        throw std::runtime_error(std::string(location.file_name()) + ":" +
                                 std::to_string(location.line()) + ": " +
                                 std::string(expression));
    }
}

#define CHECK(expr) check((expr), #expr)

GameState fixture() {
    GameState state{};
    state.winner = kOngoing;
    state.turn_number = 5;
    for (int owner = 0; owner < kPlayers; ++owner) {
        auto& player = state.players[owner];
        player.leader_hp = kLeaderMaxHp;
        player.ep = static_cast<std::uint8_t>(2 + owner);
        player.deck.fill(1);
        player.pp = player.max_pp = kMaxPp;
    }
    return state;
}

void set_hand(PlayerState& player, std::initializer_list<CardId> cards) {
    CHECK(cards.size() <= player.hand.size());
    player.hand.fill(0);
    std::copy(cards.begin(), cards.end(), player.hand.begin());
    player.hand_count = static_cast<std::uint8_t>(cards.size());
}

void add_follower(PlayerState& player, CardId id, std::uint16_t flags = FLAG_NONE) {
    const auto* card = find_card(id);
    CHECK(card != nullptr && card->type != CardType::SPELL);
    CHECK(player.board_count < kBoardLimit);
    player.board[player.board_count++] =
        {id, card->atk, card->hp, card->hp, 0, static_cast<std::uint16_t>(card->flags | flags)};
}

bool has_action(const GameState& state, Action action) {
    const auto actions = legal_actions(state);
    return std::find(actions.begin(), actions.end(), action) != actions.end();
}

void reject(GameState& state, Action action) {
    const auto before = state;
    CHECK(!has_action(state, action));
    CHECK(!apply_action(state, action));
    CHECK(state == before);
}

void test_cards_and_decks() {
    constexpr std::array<std::array<int, 3>, 11> stats{{
        {1, 1, 2}, {2, 2, 2}, {2, 1, 3}, {3, 2, 2}, {3, 3, 2}, {2, 1, 1},
        {4, 2, 2}, {4, 3, 4}, {2, 0, 0}, {5, 0, 0}, {1, 1, 1},
    }};
    for (CardId id = 1; id <= stats.size(); ++id) {
        const auto* card = find_card(id);
        CHECK(card != nullptr && card->id == id && !card->name.empty());
        const auto& expected = stats[id - 1];
        CHECK(card->cost == expected[0] && card->atk == expected[1] && card->hp == expected[2]);
        const auto type = id == kKnightId ? CardType::TOKEN
                         : id >= 9 ? CardType::SPELL : CardType::FOLLOWER;
        CHECK(card->type == type);
    }
    CHECK(find_card(0) == nullptr && find_card(12) == nullptr);
    constexpr std::array<int, 11> copies_a{6, 6, 0, 8, 4, 6, 4, 0, 6, 0, 0};
    constexpr std::array<int, 11> copies_b{0, 4, 8, 0, 6, 6, 0, 6, 4, 6, 0};
    for (CardId id = 1; id <= copies_a.size(); ++id) {
        CHECK(std::count(kDeckA.begin(), kDeckA.end(), id) == copies_a[id - 1]);
        CHECK(std::count(kDeckB.begin(), kDeckB.end(), id) == copies_b[id - 1]);
    }
    auto invalid = kDeckA;
    std::mt19937 rng(13);
    for (CardId id : {CardId{0}, kKnightId, CardId{12}}) {
        invalid[0] = id;
        bool rejected = false;
        try {
            static_cast<void>(new_game(invalid, kDeckB, rng));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
    }
}

void test_setup_and_turns() {
    std::mt19937 rng(42);
    auto state = new_game(kDeckA, kDeckB, rng);
    std::mt19937 replay_rng(42);
    CHECK(state == new_game(kDeckA, kDeckB, replay_rng));
    CHECK(state.turn_player == 0 && state.turn_number == 1 && state.winner == kOngoing);
    const auto& first = state.players[0];
    const auto& second = state.players[1];
    CHECK(first.leader_hp == kLeaderMaxHp && second.leader_hp == kLeaderMaxHp);
    CHECK(first.pp == 1 && first.max_pp == 1 && first.ep == 2);
    CHECK(second.pp == 0 && second.max_pp == 0 && second.ep == 3);
    CHECK(first.hand_count == kOpeningHand + 1 && first.deck_idx == first.hand_count);
    CHECK(second.hand_count == kOpeningHand && second.deck_idx == second.hand_count);
    CHECK(std::equal(first.hand.begin(), first.hand.begin() + first.hand_count, first.deck.begin()));
    CHECK(std::equal(second.hand.begin(), second.hand.begin() + second.hand_count, second.deck.begin()));
    CHECK(std::is_permutation(first.deck.begin(), first.deck.end(), kDeckA.begin()));
    CHECK(std::is_permutation(second.deck.begin(), second.deck.end(), kDeckB.begin()));

    CHECK(apply_action(state, kEndTurn));
    CHECK(state.turn_player == 1 && state.turn_number == 1);
    CHECK(second.hand_count == kOpeningHand + 1 && second.pp == 1);
    CHECK(apply_action(state, kEndTurn));
    CHECK(state.turn_player == 0 && state.turn_number == 2);
    CHECK(first.hand_count == kOpeningHand + 2 && first.pp == 2);
    for (int turn = 0; turn < 20; ++turn) {
        CHECK(apply_action(state, kEndTurn));
    }
    CHECK(first.max_pp == kMaxPp && first.pp == kMaxPp);
    CHECK(second.max_pp == kMaxPp && second.pp == kMaxPp);
    CHECK(first.hand_count == kHandLimit && second.hand_count == kHandLimit);
    CHECK(first.shadows == first.deck_idx - kHandLimit);
    CHECK(second.shadows == second.deck_idx - kHandLimit);
}

void test_play_and_refresh() {
    auto state = fixture();
    auto& first = state.players[0];
    set_hand(first, {2, 1});
    first.pp = 1;
    reject(state, {PLAY_CARD, 0, kNoTarget});
    reject(state, {PLAY_CARD, -1, kNoTarget});
    reject(state, {PLAY_CARD, 2, kNoTarget});
    reject(state, {PLAY_CARD, 1, kEnemyLeader});
    reject(state, {END_TURN, 0, kNoTarget});
    reject(state, {static_cast<ActionType>(255), 0, 0});
    CHECK(apply_action(state, {PLAY_CARD, 1, kNoTarget}));
    CHECK(first.hand_count == 1 && first.hand[0] == 2 && first.hand[1] == 0);
    CHECK(first.pp == 0 && first.board_count == 1);
    const auto& goblin = first.board[0];
    CHECK(goblin.card_id == 1 && goblin.atk == 1 && goblin.hp == 2);
    reject(state, {ATTACK, 0, kEnemyLeader});
    CHECK(apply_action(state, kEndTurn));
    CHECK(apply_action(state, kEndTurn));
    CHECK(first.pp == kMaxPp);
    CHECK(apply_action(state, {ATTACK, 0, kEnemyLeader}));
    CHECK(state.players[1].leader_hp == 19);
    reject(state, {ATTACK, 0, kEnemyLeader});
    CHECK(apply_action(state, {PLAY_CARD, 0, kNoTarget}));
    CHECK(first.hand_count == 1 && first.hand[0] == 1 && first.hand[1] == 0);
    CHECK(first.board_count == 2 && first.board[1].card_id == 2);
}

void test_storm_rush_and_ward() {
    auto state = fixture();
    auto& first = state.players[0];
    auto& enemy = state.players[1];
    set_hand(first, {4, 5, 9});
    CHECK(apply_action(state, {PLAY_CARD, 0, kNoTarget}));
    CHECK(apply_action(state, {PLAY_CARD, 0, kNoTarget}));
    CHECK(has_action(state, {ATTACK, 0, kEnemyLeader}));
    reject(state, {ATTACK, 1, kEnemyLeader});

    add_follower(enemy, 2);
    add_follower(enemy, 3);
    add_follower(enemy, 3);
    reject(state, {ATTACK, 0, kEnemyLeader});
    reject(state, {ATTACK, 0, 0});
    CHECK(has_action(state, {ATTACK, 0, 1}) && has_action(state, {ATTACK, 0, 2}));
    CHECK(has_action(state, {ATTACK, 1, 1}) && has_action(state, {ATTACK, 1, 2}));
    CHECK(has_action(state, {PLAY_CARD, 0, 0}));
    CHECK(has_action(state, {PLAY_CARD, 0, kEnemyLeader}));
    CHECK(apply_action(state, {ATTACK, 1, 1}));
    CHECK(enemy.board_count == 2 && enemy.board[0].card_id == 2 && enemy.board[1].card_id == 3);
    CHECK(enemy.board[2] == Follower{} && enemy.shadows == 1 && first.board[1].hp == 1);
    reject(state, {ATTACK, 1, 1});
    CHECK(apply_action(state, {PLAY_CARD, 0, 1}));
    CHECK(enemy.board_count == 1 && enemy.shadows == 2);
    CHECK(apply_action(state, {ATTACK, 0, kEnemyLeader}));
    CHECK(enemy.leader_hp == 18);
    CHECK(apply_action(state, kEndTurn));
    CHECK(apply_action(state, kEndTurn));
    CHECK(apply_action(state, {ATTACK, 1, kEnemyLeader}));
    CHECK(enemy.leader_hp == 15);
}

void test_summon_and_board_limit() {
    for (int occupied = 0; occupied <= kBoardLimit; ++occupied) {
        auto state = fixture();
        auto& player = state.players[0];
        for (int slot = 0; slot < occupied; ++slot) {
            add_follower(player, 1);
        }
        set_hand(player, {7, 9});
        if (occupied == kBoardLimit) {
            reject(state, {PLAY_CARD, 0, kNoTarget});
            CHECK(apply_action(state, {PLAY_CARD, 1, kEnemyLeader}));
            CHECK(player.board_count == kBoardLimit);
            continue;
        }
        CHECK(apply_action(state, {PLAY_CARD, 0, kNoTarget}));
        CHECK(player.pp == 6 && player.board[occupied].card_id == 7);
        CHECK(player.board_count == std::min(kBoardLimit, occupied + 3));
        for (int slot = occupied + 1; slot < player.board_count; ++slot) {
            const auto& knight = player.board[slot];
            CHECK(knight.card_id == kKnightId && knight.atk == 1 && knight.hp == 1);
            reject(state, {ATTACK, static_cast<std::int8_t>(slot), kEnemyLeader});
        }
    }
}

void test_evolution_rights() {
    auto state = fixture();
    auto& first = state.players[0];
    auto& second = state.players[1];
    add_follower(first, 1);
    add_follower(first, 2);
    add_follower(second, 2);
    add_follower(second, 1);
    state.turn_number = 3;
    reject(state, {EVOLVE, 0, kNoTarget});
    CHECK(apply_action(state, kEndTurn));
    reject(state, {EVOLVE, 0, kNoTarget});
    CHECK(apply_action(state, kEndTurn));
    reject(state, {EVOLVE, 0, kNoTarget});
    CHECK(apply_action(state, kEndTurn));
    second.board[0].hp = 1;
    CHECK(apply_action(state, {EVOLVE, 0, kNoTarget}));
    CHECK(second.ep == 2 && second.evolved_this_turn == 1);
    CHECK(second.board[0].atk == 4 && second.board[0].hp == 3 && second.board[0].max_hp == 4);
    reject(state, {EVOLVE, 1, kNoTarget});
    CHECK(apply_action(state, kEndTurn));
    CHECK(apply_action(state, {EVOLVE, 0, kNoTarget}));
    CHECK(first.ep == 1);
    CHECK(apply_action(state, kEndTurn));
    reject(state, {EVOLVE, 0, kNoTarget});
    CHECK(has_action(state, {EVOLVE, 1, kNoTarget}));
    CHECK(apply_action(state, kEndTurn));
    CHECK(apply_action(state, {EVOLVE, 1, kNoTarget}));
    CHECK(first.ep == 0);
    add_follower(first, 1);
    CHECK(apply_action(state, kEndTurn));
    CHECK(apply_action(state, kEndTurn));
    reject(state, {EVOLVE, 2, kNoTarget});
}

void test_evolve_effect_and_attack_flags() {
    auto state = fixture();
    auto& first = state.players[0];
    auto& enemy = state.players[1];
    set_hand(first, {8, 4});
    add_follower(enemy, 1);
    add_follower(enemy, 2);
    CHECK(apply_action(state, {PLAY_CARD, 0, kNoTarget}));
    reject(state, {ATTACK, 0, 0});
    reject(state, {EVOLVE, 0, kNoTarget});
    reject(state, {EVOLVE, 0, kEnemyLeader});
    CHECK(has_action(state, {EVOLVE, 0, 0}));
    CHECK(apply_action(state, {EVOLVE, 0, 1}));
    CHECK(first.board[0].atk == 5 && first.board[0].hp == 6);
    CHECK(enemy.board_count == 1 && enemy.board[0].card_id == 1 && enemy.shadows == 1);
    reject(state, {ATTACK, 0, kEnemyLeader});
    CHECK(apply_action(state, {ATTACK, 0, 0}));
    CHECK(first.board[0].hp == 5);

    CHECK(apply_action(state, {PLAY_CARD, 0, kNoTarget}));
    CHECK(apply_action(state, {ATTACK, 1, kEnemyLeader}));
    CHECK(apply_action(state, kEndTurn));
    CHECK(apply_action(state, kEndTurn));
    CHECK(apply_action(state, {ATTACK, 1, kEnemyLeader}));
    CHECK(apply_action(state, {EVOLVE, 1, kNoTarget}));
    reject(state, {ATTACK, 1, kEnemyLeader});

    auto empty_enemy = fixture();
    set_hand(empty_enemy.players[0], {8});
    CHECK(apply_action(empty_enemy, {PLAY_CARD, 0, kNoTarget}));
    CHECK(apply_action(empty_enemy, {EVOLVE, 0, kNoTarget}));
    CHECK(empty_enemy.players[0].board[0].atk == 5);
    CHECK(empty_enemy.players[1].leader_hp == kLeaderMaxHp);
}

void test_fireball_and_lethal() {
    auto state = fixture();
    auto& first = state.players[0];
    auto& enemy = state.players[1];
    set_hand(first, {9, 9});
    add_follower(enemy, 8);
    add_follower(enemy, 3);
    reject(state, {PLAY_CARD, 0, kNoTarget});
    reject(state, {PLAY_CARD, 0, 2});
    CHECK(apply_action(state, {PLAY_CARD, 0, 0}));
    CHECK(enemy.board[0].hp == 1 && enemy.board[0].max_hp == 4);
    CHECK(enemy.leader_hp == kLeaderMaxHp && first.pp == 8 && first.shadows == 1);
    enemy.leader_hp = 2;
    CHECK(apply_action(state, {PLAY_CARD, 0, kEnemyLeader}));
    CHECK(enemy.leader_hp == -1 && state.winner == 0);
    CHECK(legal_actions(state).empty());
    reject(state, kEndTurn);

    auto attack_lethal = fixture();
    add_follower(attack_lethal.players[0], 4, FLAG_CAN_ATTACK | FLAG_CAN_HIT_FACE);
    attack_lethal.players[1].leader_hp = 2;
    CHECK(apply_action(attack_lethal, {ATTACK, 0, kEnemyLeader}));
    CHECK(attack_lethal.winner == 0);
}

void test_area_damage_heal_and_death_queue() {
    auto state = fixture();
    auto& first = state.players[0];
    auto& enemy = state.players[1];
    set_hand(first, {10, 10});
    first.leader_hp = 16;
    add_follower(first, 1);
    add_follower(enemy, 6);
    add_follower(enemy, 3);
    add_follower(enemy, 6);
    add_follower(enemy, 2);
    add_follower(enemy, 8);
    enemy.deck[0] = 5;
    enemy.deck[1] = 9;
    CHECK(apply_action(state, {PLAY_CARD, 0, kNoTarget}));
    CHECK(first.leader_hp == 19 && first.board[0].hp == 2 && first.pp == 5);
    CHECK(enemy.leader_hp == kLeaderMaxHp && enemy.board_count == 2 && enemy.shadows == 3);
    CHECK(enemy.board[0].card_id == 3 && enemy.board[0].hp == 1);
    CHECK(enemy.board[1].card_id == 8 && enemy.board[1].hp == 2);
    CHECK(enemy.board[2] == Follower{} && enemy.board[4] == Follower{});
    CHECK(enemy.hand_count == 2 && enemy.hand[0] == 5 && enemy.hand[1] == 9 && enemy.deck_idx == 2);
    CHECK(apply_action(state, {PLAY_CARD, 0, kNoTarget}));
    CHECK(first.leader_hp == 20 && first.shadows == 2 && enemy.board_count == 0);
    CHECK(enemy.shadows == 5 && enemy.hand_count == 2);

    auto lethal_draw = fixture();
    set_hand(lethal_draw.players[0], {10});
    lethal_draw.players[0].leader_hp = 5;
    auto& exhausted = lethal_draw.players[1];
    add_follower(exhausted, 6);
    add_follower(exhausted, 6);
    exhausted.deck_idx = kDeckSize - 1;
    CHECK(apply_action(lethal_draw, {PLAY_CARD, 0, kNoTarget}));
    CHECK(lethal_draw.winner == 0 && lethal_draw.players[0].leader_hp == 8);
    CHECK(exhausted.hand_count == 1 && exhausted.board_count == 0 && exhausted.shadows == 2);
}

void test_simultaneous_combat_and_deck_out() {
    auto state = fixture();
    auto& first = state.players[0];
    auto& enemy = state.players[1];
    add_follower(first, 6, FLAG_CAN_ATTACK | FLAG_CAN_HIT_FACE);
    add_follower(enemy, 6);
    first.deck[0] = 4;
    enemy.deck[0] = 10;
    CHECK(apply_action(state, {ATTACK, 0, 0}));
    CHECK(first.board_count == 0 && enemy.board_count == 0);
    CHECK(first.shadows == 1 && enemy.shadows == 1);
    CHECK(first.hand_count == 1 && first.hand[0] == 4);
    CHECK(enemy.hand_count == 1 && enemy.hand[0] == 10);

    enemy.deck_idx = kDeckSize;
    CHECK(apply_action(state, kEndTurn));
    CHECK(state.winner == 0 && enemy.leader_hp == kLeaderMaxHp);
    CHECK(enemy.deck_idx == kDeckSize && enemy.hand_count == 1);
    reject(state, kEndTurn);
}

using TestCase = std::pair<std::string_view, void (*)()>;
constexpr std::array kTests{
    TestCase{"cards and deck compositions", test_cards_and_decks},
    TestCase{"opening, PP refresh and full-hand draw", test_setup_and_turns},
    TestCase{"vanilla play, invalid actions and attack refresh", test_play_and_refresh},
    TestCase{"storm, rush, ward and retaliation", test_storm_rush_and_ward},
    TestCase{"fanfare tokens and board capacity", test_summon_and_board_limit},
    TestCase{"evolution turn, EP and repeat evolution limits", test_evolution_rights},
    TestCase{"evolution target, attack flags and no-target fallback", test_evolve_effect_and_attack_flags},
    TestCase{"fireball targets and immediate lethal", test_fireball_and_lethal},
    TestCase{"area damage, capped healing and queued last words", test_area_damage_heal_and_death_queue},
    TestCase{"simultaneous combat deaths and deck exhaustion", test_simultaneous_combat_and_deck_out},
};

int run_units() {
    std::size_t passed = 0;
    for (const auto& [name, test] : kTests) {
        try {
            test();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << passed << '/' << kTests.size() << " unit cases passed\n";
    return passed == kTests.size() ? 0 : 1;
}

int summoned_tokens(const PlayerState& player) {
    // Every drawn card is in hand, on board, or accounted for by shadows.
    // Summoned tokens are the only cards that enter this accounting without a draw.
    return player.hand_count + player.board_count + player.shadows - player.deck_idx;
}

void check_state(const GameState& state, const GameState& before, int deck_a_seat) {
    CHECK(state.turn_player < kPlayers);
    CHECK(state.turn_number >= 1 && state.turn_number <= kDeckSize - kOpeningHand + 1);
    CHECK(state.winner >= kOngoing && state.winner <= kDraw);
    for (int owner = 0; owner < kPlayers; ++owner) {
        const auto& player = state.players[owner];
        const auto& previous = before.players[owner];
        CHECK(player.leader_hp <= kLeaderMaxHp);
        CHECK(player.leader_hp > 0 || state.winner == 1 - owner || state.winner == kDraw);
        CHECK(player.max_pp == std::min(kMaxPp, state.turn_number - (owner > state.turn_player)));
        CHECK(player.pp <= player.max_pp);
        CHECK(player.ep <= previous.ep && player.ep <= 2 + owner);
        CHECK(player.evolved_this_turn <= 1);
        CHECK(owner == state.turn_player || player.evolved_this_turn == 0);
        CHECK(player.hand_count <= kHandLimit && player.board_count <= kBoardLimit);
        CHECK(player.deck_idx >= previous.deck_idx && player.deck_idx <= kDeckSize);
        CHECK(player.shadows >= previous.shadows);
        CHECK(summoned_tokens(player) >= summoned_tokens(previous));
        CHECK(summoned_tokens(player) <= (owner == deck_a_seat ? 8 : 0));
        CHECK(player.deck == previous.deck);

        for (int slot = 0; slot < kHandLimit; ++slot) {
            if (slot < player.hand_count) {
                const auto* card = find_card(player.hand[slot]);
                CHECK(card != nullptr && card->type != CardType::TOKEN);
            } else {
                CHECK(player.hand[slot] == 0);
            }
        }
        for (int slot = 0; slot < kBoardLimit; ++slot) {
            const auto& follower = player.board[slot];
            if (slot >= player.board_count) {
                CHECK(follower == Follower{});
                continue;
            }
            const auto* card = find_card(follower.card_id);
            CHECK(card != nullptr && card->type != CardType::SPELL);
            const int bonus = (follower.flags & FLAG_EVOLVED) ? 2 : 0;
            CHECK(follower.atk == card->atk + bonus && follower.max_hp == card->hp + bonus);
            CHECK(follower.hp > 0 && follower.hp <= follower.max_hp);
            CHECK(!(follower.flags & FLAG_CAN_HIT_FACE) || (follower.flags & FLAG_CAN_ATTACK));
            CHECK(!(follower.flags & FLAG_ATTACKED_THIS_TURN) || !(follower.flags & FLAG_CAN_ATTACK));
            constexpr auto transient = FLAG_CAN_ATTACK | FLAG_CAN_HIT_FACE | FLAG_ATTACKED_THIS_TURN;
            CHECK(owner == state.turn_player || !(follower.flags & transient));
        }
    }
    if (state.winner == kDraw) {
        CHECK(state.players[0].leader_hp <= 0 && state.players[1].leader_hp <= 0);
    } else if (state.winner != kOngoing) {
        const auto& loser = state.players[1 - state.winner];
        CHECK(loser.leader_hp <= 0 || loser.deck_idx == kDeckSize);
    }
}

int run_stress() {
    constexpr int games = 10'000;
    constexpr int action_limit = 1'024;
    constexpr std::uint32_t seed = 0x5A17'2026;
    std::mt19937 rng(seed);
    int total_actions = 0;
    int max_actions = 0;
    std::array<int, 3> deck_results{};
    const auto start = std::chrono::steady_clock::now();

    for (int game = 0; game < games; ++game) {
        const int a_seat = game % kPlayers;
        auto state = a_seat == 0 ? new_game(kDeckA, kDeckB, rng) : new_game(kDeckB, kDeckA, rng);
        int actions_taken = 0;
        Action action = kEndTurn;
        try {
            check_state(state, state, a_seat);
            while (state.winner == kOngoing) {
                CHECK(actions_taken < action_limit);
                const auto actions = legal_actions(state);
                CHECK(!actions.empty() && actions[0] == kEndTurn);
                std::uniform_int_distribution<std::size_t> pick(0, actions.size() - 1);
                action = actions[pick(rng)];
                const auto before = state;
                CHECK(apply_action(state, action));
                ++actions_taken;
                check_state(state, before, a_seat);
            }
            CHECK(legal_actions(state).empty());
        } catch (const std::exception& error) {
            std::cerr << "FAIL seed=" << seed << " game=" << game + 1
                      << " actions_applied=" << actions_taken
                      << " round=" << static_cast<int>(state.turn_number)
                      << " action=(" << static_cast<int>(action.type) << ','
                      << static_cast<int>(action.src) << ',' << static_cast<int>(action.dst)
                      << "): " << error.what() << '\n';
            return 1;
        }
        total_actions += actions_taken;
        max_actions = std::max(max_actions, actions_taken);
        const int result = state.winner == kDraw ? kDraw : state.winner == a_seat ? 0 : 1;
        ++deck_results[result];
    }

    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    const double seconds = elapsed.count();
    std::cout << "PASS random endurance: games=" << games << " seed=" << seed
              << " actions=" << total_actions << " max_actions_per_game=" << max_actions << '\n'
              << "A_wins=" << deck_results[0] << " B_wins=" << deck_results[1]
              << " draws=" << deck_results[2] << " invariant_failures=0\n"
              << std::fixed << std::setprecision(3)
              << "elapsed=" << seconds << " s, " << games / seconds << " games/s, "
              << seconds * 1000 / games << " ms/game (single-threaded, validation included)\n"
              << "sizeof(GameState)=" << sizeof(GameState) << " bytes\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        return run_units();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--stress") {
        return run_stress();
    }
    std::cerr << "Usage: test_engine [--stress]\n";
    return 2;
}
