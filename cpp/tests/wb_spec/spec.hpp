#pragma once

#include "wb_cards.hpp"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace svwb::wb::spec {

using namespace svwb::wb;
using enum ActionType;
using svwb::CardId;
using svwb::Deck;
using svwb::FLAG_RUSH;
using svwb::FLAG_STORM;
using svwb::FLAG_WARD;
using svwb::kBoardLimit;
using svwb::kDeckSize;
using svwb::kEnemyLeader;
using svwb::kHandLimit;
using svwb::kNoTarget;

inline void check(bool condition, std::string_view expression,
           std::source_location location = std::source_location::current()) {
    if (!condition) {
        throw std::runtime_error(std::string(location.file_name()) + ":" +
                                 std::to_string(location.line()) + " " + std::string(expression));
    }
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__)

struct Scenario {
    GameState state = new_game(kProvisionalMirror, kProvisionalMirror, 27);
    RuleConfig rules;

    Scenario() {
        state.phase = Phase::Main;
        state.turn_number = 7;
        for (auto& player : state.players) {
            player.hand = {};
            player.hand_count = 0;
            player.pp = player.max_pp = 10;
        }
    }

    PlayerState& player(int seat = 0) { return state.players[seat]; }

    void hand(std::initializer_list<CardId> cards, int seat = 0) {
        auto& owner = player(seat);
        CHECK(cards.size() <= owner.hand.size());
        owner.hand = {};
        owner.hand_modifiers = {};
        std::copy(cards.begin(), cards.end(), owner.hand.begin());
        owner.hand_count = static_cast<std::uint8_t>(cards.size());
    }

    Permanent& place(CardId id, int seat = 0) {
        const auto* card = find_card(id);
        auto& owner = player(seat);
        CHECK(card && card->kind != Kind::Spell && owner.board_count < kBoardLimit);
        const auto hp = static_cast<std::int16_t>(card->hp);
        auto& object = owner.board[owner.board_count++];
        object = {state.next_creation_order++, id, static_cast<std::int16_t>(card->atk), hp, hp,
                  card->abilities, static_cast<std::uint16_t>(card->clue),
                  static_cast<std::uint8_t>(card->countdown), 1, Evolution::None, false, true, false, false,
                  false, static_cast<std::uint16_t>(card->earth), {}};
        return object;
    }

    void crest(CrestId id, int countdown, int seat = 0) {
        auto& owner = player(seat);
        CHECK(owner.crest_count < kCrestLimit);
        owner.crests[owner.crest_count++] = {state.next_creation_order++, id, static_cast<std::uint8_t>(countdown)};
    }

    bool legal(Action action) const {
        const auto actions = legal_actions(state, rules);
        return std::find(actions.begin(), actions.end(), action) != actions.end();
    }

    void run(Action action, std::source_location where = std::source_location::current()) {
        check(apply_action(state, action, rules), "action is legal", where);
    }

    void reject(Action action, std::source_location where = std::source_location::current()) {
        const auto before = state;
        check(!apply_action(state, action, rules) && state == before, "illegal action leaves state unchanged", where);
    }

    void next_own_turn() {
        run({EndTurn});
        run({EndTurn});
    }
};

inline CardId official(std::uint32_t id) {
    const auto* card = find_official_card(id);
    CHECK(card && card->implemented);
    return card->id;
}

struct Case {
    std::string name;
    std::function<void()> run;
    std::source_location registration;
};

class Suite {
public:
    void add(std::string name, std::function<void()> run,
             std::source_location registration = std::source_location::current()) {
        cases.push_back({std::move(name), std::move(run), registration});
    }

    std::vector<Case> cases;
};

void catalog(Suite& suite);
void elf(Suite& suite);
void royal(Suite& suite);
void witch(Suite& suite);
void dragon(Suite& suite);
void nightmare(Suite& suite);
void bishop(Suite& suite);
void nemesis(Suite& suite);
void neutral(Suite& suite);
void special(Suite& suite);
void rules(Suite& suite);
void search_keys(Suite& suite);
void phase2_audit(Suite& suite);
void phase2_elf(Suite& suite);
void phase2_royal(Suite& suite);
void phase2_witch(Suite& suite);
void phase2_dragon(Suite& suite);
void phase2_nightmare(Suite& suite);
void phase2_bishop(Suite& suite);
void phase2_nemesis(Suite& suite);
void phase2_neutral(Suite& suite);
void phase2_special(Suite& suite);
void phase2_tokens(Suite& suite);

} // namespace svwb::wb::spec
