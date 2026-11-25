#include "wb_cards.hpp"
#include "wb_search.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <random>
#include <ranges>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace {

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

void check(bool condition, std::string_view expression,
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

void winged_statue() {
    Scenario s;
    s.hand({kWingedStatue});
    s.run({Play, 0});
    CHECK(s.player().board[0].countdown == 4 && s.player().pp == 9);
    s.reject({Attack, 0, kEnemyLeader});
    s.run({Act, 0});
    CHECK(s.player().board[0].countdown == 3 && s.player().pp == 8);
    s.reject({Act, 0});
    s.next_own_turn();
    CHECK(s.player().board[0].countdown == 2 && !s.player().board[0].acted_this_turn);
    s.run({Act, 0});
    s.next_own_turn();
    CHECK(s.player().board[0].card_id == kHolyFalcon && s.player().shadows == 1);
    CHECK(s.legal({Attack, 0, kEnemyLeader}));
}

void lilium() {
    Scenario s;
    s.hand({kLilium});
    s.place(kRabbit, 1).abilities |= FLAG_WARD;
    s.run({Play, 0});
    s.run({Evolve, 0, 0});
    CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1 && s.player().leader_hp == 20);
    CHECK(s.player().board[0].atk == 4 && s.player().board[0].hp == 3);

    Scenario survives;
    survives.place(kLilium);
    survives.place(kColette, 1);
    survives.run({Evolve, 0, 0});
    CHECK(survives.player(1).board[0].hp == 2 && survives.player(1).board[0].silenced &&
          survives.player(1).board[0].abilities == 0);

    Scenario death;
    death.place(kLilium);
    death.place(kHolyFalcon, 1);
    death.state.turn_player = 1;
    const auto drawn = death.player().deck[death.player().deck_idx];
    death.run({Attack, 0, 0});
    CHECK(death.player().hand_count == 1 && death.player().hand[0] == drawn);
}

void exias() {
    Scenario s;
    s.hand({kExias});
    s.place(kTrainer, 1);
    s.run({Play, 0});
    CHECK(s.player().pp == 8 && s.player().hand[0] == kEvidence);
    CHECK(s.legal({Attack, 0, 0}));
    s.reject({Attack, 0, kEnemyLeader});
}

void craftsman() {
    Scenario s;
    s.hand({kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer});
    s.place(kCraftsman);
    s.place(kColette, 1);
    s.run({Attack, 0, 0});
    CHECK(s.player().board_count == 0 && s.player(1).board[0].hp == 1);
    CHECK(s.player().hand_count == 9 && s.player().hand[8] == kEvidence);
    CHECK(s.player().shadows == 2); // Destroyed follower and the overflowed second token.
    CHECK(s.player().destroyed_followers[kCraftsman - kFirstCard] == 1 &&
          s.player().destroyed_followers[kEvidence - kFirstCard] == 0);
}

void colette() {
    Scenario s;
    s.place(kTrainer).evolution = Evolution::Normal;
    s.place(kAlmiraj, 1);
    s.place(kExias, 1);
    s.hand({kColette, kTrainer});
    s.run({Play, 0});
    CHECK(s.player().board[1].evolution == Evolution::Normal && s.player().board[1].hp == 6);
    CHECK((s.player().board[1].abilities & (FLAG_WARD | FLAG_RUSH)) == (FLAG_WARD | FLAG_RUSH));
    CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 2);
    CHECK(s.player().ep == 2 && s.player().sep == 2 && !s.player().manual_evolved);
    s.run({Play, 0});
    s.run({Evolve, 2});
    CHECK(s.player().manual_evolved && s.player().ep == 1);

    Scenario no_ally;
    no_ally.hand({kColette});
    no_ally.run({Play, 0});
    CHECK(no_ally.player().board[0].evolution == Evolution::None);
}

void almiraj() {
    Scenario s;
    s.hand({kAlmiraj, kAlmiraj});
    s.player().pp = 2;
    s.run({Play, 0});
    CHECK(s.player().board[0].card_id == kAlmirajCrystal && s.player().board[0].countdown == 3);
    s.run({Act, 0});
    CHECK(s.player().board[0].countdown == 2 && s.player().pp == 0);
    s.player().board[0].countdown = 1;
    s.next_own_turn();
    CHECK(s.player().board[0].card_id == kAlmiraj && s.legal({Attack, 0, kEnemyLeader}));
    s.run({Play, 0});
    CHECK(s.player().pp == 7 && s.player().board[1].card_id == kAlmiraj);
    CHECK(s.legal({Attack, 1, kEnemyLeader}));
}

void dunamis() {
    Scenario s;
    s.hand({kDunamis});
    s.place(kColette, 1);
    s.place(kEvidence, 1);
    s.reject({Play, 0});
    s.reject({Play, 0, 1});
    s.run({Play, 0, 0});
    CHECK(s.player(1).board[0].hp == 2 && s.player().hand[0] == kEvidence);
    s.run({Evolve, 0, 0});
    CHECK(s.player(1).board_count == 1 && s.player(1).board[0].card_id == kEvidence);
    CHECK(s.player().hand_count == 2 && s.player().hand[1] == kEvidence);

    Scenario absent;
    absent.hand({kDunamis});
    absent.place(kHolyFalcon, 1).abilities |= kAura;
    absent.run({Play, 0});
    CHECK(absent.player().hand[0] == kEvidence && absent.player(1).board[0].hp == 2);
}

void trainer() {
    Scenario s;
    s.hand({kTrainer});
    s.run({Play, 0});
    CHECK(s.player().hand[0] == kRabbit && s.player().pp == 7);
    s.run({Evolve, 0});
    CHECK(s.player().pp == 8 && s.player().board[0].hp == 5);
}

void testimony() {
    Scenario s;
    s.hand({kTestimony});
    s.place(kColette, 1).abilities |= kAura; // Random targeting still hits.
    s.player().leader_hp = 17;
    s.reject({Play, 0});
    s.run({Play, 0, kNoTarget, 1});
    CHECK(s.player(1).board_count == 0 && s.player().shadows == 1);
    CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::Testimony);
    s.next_own_turn();
    CHECK(s.player().leader_hp == 18 && s.player().hand[0] == kEvidence && s.player().crests[0].countdown == 1);
    s.next_own_turn();
    CHECK(s.player().leader_hp == 19 && s.player().hand[2] == kEvidence && s.player().crest_count == 0);
    CHECK(s.player().shadows == 1);

    Scenario summon;
    summon.hand({kTestimony, kEvidence});
    summon.run({Play, 0, kNoTarget, 2});
    CHECK(summon.player().board[0].card_id == kRabbit && summon.player().board[0].evolution == Evolution::Normal);
    CHECK(summon.player().crest_count == 0 && summon.player().hand_count == 0 && summon.player().shadows == 1);
}

void projected_statue() {
    Scenario s;
    s.hand({kProjectedStatue});
    s.run({Play, 0});
    CHECK(s.player().board[0].countdown == 2 && s.player().pp == 6);
    s.run({Act, 0});
    CHECK(s.player().board[0].card_id == kSplendidHawk && s.player().pp == 4 && s.player().shadows == 1);
    CHECK(s.legal({Attack, 0, kEnemyLeader}));
}

void rituel() {
    for (int mode = 1; mode <= 3; ++mode) {
        Scenario s;
        s.hand({kRituel});
        s.place(kColette, 1);
        s.place(kWingedStatue, 1);
        s.player().leader_hp = 16;
        s.run({Play, 0, kNoTarget, static_cast<std::uint8_t>(mode)});
        CHECK(s.player().board[0].hp == 1 && s.player(1).board[0].hp == (mode == 1 ? 1 : 4));
        CHECK(s.player(1).board[1].countdown == 4);
        CHECK(s.player(1).leader_hp == (mode == 2 ? 17 : 20) && s.player().leader_hp == (mode == 3 ? 19 : 16));
    }
    Scenario s;
    s.place(kRituel);
    s.run({Evolve, 0});
    CHECK(s.player().crests[0].id == CrestId::Rituel);
    s.next_own_turn();
    CHECK(s.player().crest_count == 0 && s.player().board_count == 2);
    const auto& summoned = s.player().board[1];
    CHECK(summoned.card_id == kRituel && summoned.evolution == Evolution::Normal &&
          summoned.atk == 4 && summoned.hp == 6 && s.player().ep == 1 && !s.player().manual_evolved);
    CHECK(s.player(1).leader_hp == 20 && s.player().leader_hp == 20);

    Scenario lethal;
    lethal.hand({kRituel});
    lethal.player(1).leader_hp = 3;
    lethal.run({Play, 0, kNoTarget, 2});
    CHECK(lethal.state.outcome == Outcome::FirstWins && lethal.player().board[0].hp == 4);
    CHECK(legal_actions(lethal.state).empty());
}

void propagation() {
    for (const int pp : {5, 7, 8, 10}) {
        Scenario s;
        s.hand({kPropagation});
        s.player().pp = static_cast<std::uint8_t>(pp);
        s.run({Play, 0});
        CHECK(s.player().board_count == (pp >= 8 ? 4 : 2));
        CHECK(s.player().pp == pp - (pp >= 8 ? 8 : 5) && s.player().shadows == 1);
        CHECK(std::all_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                          [](auto object) { return object.card_id == kRabbit; }));
    }
    Scenario full;
    for (int i = 0; i < kBoardLimit; ++i) {
        full.place(kColette);
    }
    full.hand({kPropagation, kEvidence});
    full.run({Play, 0});
    CHECK(full.player().board_count == 5 && full.player().shadows == 1 && full.player().hand[0] == kEvidence);
}

void rapid() {
    Scenario s;
    s.hand({kRapid, kEvidence});
    s.place(kRabbit, 1);
    s.run({Play, 0, 0});
    CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 0 && s.player().leader_hp == 20);
    CHECK(s.player(1).destroyed_followers[kRabbit - kFirstCard] == 0);
    CHECK(s.player().board[1].card_id == kRabbit && s.player().board[1].evolution == Evolution::Normal);
    s.run({SuperEvolve, 0});
    CHECK(s.player().board[0].atk == 7 && s.player().board[0].hp == 7);
    CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::Rapid &&
          s.player().crests[0].countdown == 2 && s.player().sep == 1);
    const auto original = s.player().crests[0].creation_order;
    s.next_own_turn();
    CHECK(s.player().board_count == 3 && s.player().crests[0].countdown == 1);
    s.place(kRapid);
    s.run({Evolve, 3});
    CHECK(s.player().crest_count == 1 && s.player().crests[0].countdown == 1 &&
          s.player().crests[0].creation_order == original);
}

void teresa() {
    Scenario s;
    s.player().leader_hp = 8;
    s.hand({kTeresa});
    s.run({Play, 0});
    const auto& sister = s.player().board[1];
    CHECK(sister.card_id == kHealingSister && sister.atk == 5 && sister.hp == 3 && sister.max_hp == 3);
    CHECK((sister.abilities & (FLAG_WARD | FLAG_STORM)) == (FLAG_WARD | FLAG_STORM));
    CHECK(s.player().leader_hp == 8 && s.legal({Attack, 1, kEnemyLeader}));

    Scenario crowded;
    for (int i = 0; i < 4; ++i) {
        crowded.place(kColette);
    }
    crowded.hand({kTeresa});
    crowded.run({Play, 0});
    CHECK(crowded.player().board[4].card_id == kTeresa && crowded.player().shadows == 0);
}

void rabbit() {
    Scenario s;
    s.hand({kRabbit, kTrainer, kEvidence, kTestimony});
    s.run({Play, 0});
    CHECK(s.player().hand_count == 2 && s.player().hand[0] == kTrainer && s.player().hand[1] == kTestimony);
    CHECK(s.player().board[0].evolution == Evolution::Normal && s.player().board[0].hp == 3);
    CHECK(!s.player().manual_evolved && s.player().shadows == 0);

    for (const bool evolve : {false, true}) {
        Scenario empty;
        empty.rules.rabbit_evolves_with_empty_hand = evolve;
        empty.hand({kRabbit});
        empty.run({Play, 0});
        CHECK(empty.player().board[0].evolution == (evolve ? Evolution::Normal : Evolution::None));
    }
    Scenario lethal;
    lethal.place(kRabbit, 1);
    lethal.place(kRabbit, 1);
    lethal.hand({kRituel});
    lethal.player().leader_hp = 2;
    lethal.run({Play, 0, kNoTarget, 1});
    CHECK(lethal.player(1).shadows == 2 && lethal.player().leader_hp == 0);
    CHECK(lethal.state.outcome == Outcome::SecondWins && lethal.player().board[0].hp == 1);
}

void evidence() {
    Scenario s;
    s.hand({kEvidence, kEvidence});
    s.run({Play, 0});
    const auto previous = s.player().board[0].creation_order;
    s.run({Play, 0});
    CHECK(s.player().board_count == 1 && s.player().board[0].clue_count == 2 &&
          s.player().board[0].creation_order > previous && s.player().shadows == 0);
    CHECK(!can_choose_enemy_permanent(s.player().board[0]));
    s.run({Act, 0});
    CHECK(s.player().board[0].clue_count == 1 && s.player().hand_count == 1);
    s.reject({Act, 0});
    s.next_own_turn();
    s.run({Act, 0});
    CHECK(s.player().board_count == 0 && s.player().shadows == 1 && s.player().hand_count == 3);

    Scenario exhausted;
    exhausted.place(kEvidence);
    exhausted.player().deck_idx = kDeckSize;
    exhausted.run({Act, 0});
    CHECK(exhausted.state.outcome == Outcome::SecondWins && exhausted.state.end_reason == EndReason::EmptyDeck);
    CHECK(exhausted.player().board[0].clue_count == 1 && exhausted.player().shadows == 0);
}

void falcon(CardId id, int atk, int hp) {
    Scenario s;
    auto& object = s.place(id);
    object.readied = false;
    CHECK(object.atk == atk && object.hp == hp && s.legal({Attack, 0, kEnemyLeader}));
    s.run({Attack, 0, kEnemyLeader});
    CHECK(s.player(1).leader_hp == 20 - atk);
}

void sister() {
    Scenario s;
    s.hand({kHealingSister});
    s.player().leader_hp = 16;
    s.run({Play, 0});
    CHECK(s.player().leader_hp == 20 && s.player().board[0].hp == 5);
    CHECK(s.player().board[0].abilities == FLAG_WARD && s.player().pp == 4);
}

void corrector_and_mirror_deck() {
    const auto* def = find_card(kCorrector);
    CHECK(def && def->official_id == 11063110 && def->cost == 8 && def->atk == 3 && def->hp == 5);
    CHECK(kMirrorDeck.size() == 40);
    for (const auto id : kMirrorCardIds) {
        const bool two = id == kCraftsman || id == kColette || id == kDunamis ||
                         id == kProjectedStatue || id == kTeresa;
        CHECK(std::count(kMirrorDeck.begin(), kMirrorDeck.end(), id) == (two ? 2 : 3));
    }

    Scenario s;
    s.place(kRabbit, 1);
    s.hand({kCorrector, kEvidence, kExias, kRapid});
    s.reject({Play, 0});
    s.run({Play, 0, 0});
    CHECK(s.player().pp == 2 && s.player(1).board_count == 0 && s.player(1).shadows == 0);
    CHECK(s.player().leader_hp == 20 && s.player().shadows == 0);
    CHECK(s.player().board_count == 3 && s.player().hand_count == 1 && s.player().hand[0] == kRapid);
    for (const auto slot : {1, 2}) {
        const auto& rabbit = s.player().board[slot];
        CHECK(rabbit.card_id == kRabbit && rabbit.evolution == Evolution::Normal && rabbit.hp == 3);
    }
    CHECK(!s.player().manual_evolved);
    s.player().leader_hp = 15;
    const auto cursor = s.player().deck_idx;
    s.run({SuperEvolve, 0});
    CHECK(s.player().leader_hp == 17 && s.player().hand_count == 3 && s.player().deck_idx == cursor + 2);
    CHECK(s.player().hand[1] == s.player().deck[cursor] && s.player().hand[2] == s.player().deck[cursor + 1]);
    CHECK(s.player().board[0].evolution == Evolution::Super && s.player().sep == 1);

    Scenario absent;
    absent.hand({kCorrector});
    absent.run({Play, 0});
    CHECK(absent.player().board_count == 3 && absent.player().board[1].evolution == Evolution::None);
    absent.player().leader_hp = 15;
    absent.player().deck_idx = kDeckSize - 1;
    absent.run({SuperEvolve, 0});
    CHECK(absent.state.end_reason == EndReason::EmptyDeck && absent.player().leader_hp == 15);
}

void mulligan_and_first_draw() {
    for (int mask = 0; mask < 16; ++mask) {
        RuleConfig rules;
        rules.mulligan_return = MulliganReturn::Bottom;
        auto state = new_game(kProvisionalMirror, kProvisionalMirror, 111);
        const auto original = state;
        CHECK(state.phase == Phase::Mulligan && legal_actions(state).size() == 16);
        CHECK(!apply_action(state, {EndTurn}, rules) && state == original);
        CHECK(state.players[0].ep == 2 && state.players[1].ep == 2 &&
              state.players[0].sep == 2 && state.players[1].sep == 2);
        CHECK(apply_action(state, {Mulligan, kNoTarget, kNoTarget, static_cast<std::uint8_t>(mask)}, rules));
        const auto& first = state.players[0];
        int replacement = 4;
        for (int i = 0; i < 4; ++i) {
            CHECK(first.hand[i] == original.players[0].deck[mask & (1 << i) ? replacement++ : i]);
        }
        CHECK(first.deck_idx == 4 && state.players[1] == original.players[1]);
        CHECK(std::is_permutation(first.deck.begin(), first.deck.begin() + first.deck_end, kProvisionalMirror.begin()));
        CHECK(first.deck[4] == original.players[0].deck[replacement]);
        CHECK(apply_action(state, {Mulligan, kNoTarget, kNoTarget, 15}, rules));
        CHECK(state.phase == Phase::Main && state.turn_player == 0 && state.players[0].hand_count == 5);
        CHECK(state.players[1].hand_count == 4 && state.players[0].pp == 1);
        CHECK(apply_action(state, {EndTurn}, rules));
        CHECK(state.players[1].hand_count == 5 && state.players[1].deck_idx == 5 && state.players[1].pp == 1);
    }
    auto shuffled = new_game(kProvisionalMirror, kProvisionalMirror, 111);
    auto copy = shuffled;
    for (const Action action : {Action{Mulligan, kNoTarget, kNoTarget, 15}, Action{Mulligan}}) {
        CHECK(apply_action(shuffled, action) && apply_action(copy, action) && shuffled == copy);
    }
    auto invalid_deck = kProvisionalMirror;
    invalid_deck.back() = kWingedStatue;
    bool rejected = false;
    try {
        static_cast<void>(new_game(invalid_deck, kProvisionalMirror, 0));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

void extra_pp() {
    Scenario early;
    early.state.turn_number = 5;
    early.reject({ExtraPp});
    early.state.turn_player = 1;
    auto& second = early.player(1);
    second.pp = second.max_pp = 6;
    early.run({ExtraPp});
    CHECK(second.available_pp() == 7 && !second.overflow() && second.extra_pp_uses == 1);
    early.reject({ExtraPp});
    early.hand({kTeresa}, 1);
    early.run({Play, 0});
    CHECK(second.pp == 0 && second.bonus_pp == 0);
    early.next_own_turn();
    CHECK(early.state.turn_number == 6 && second.pp == 7 && second.extra_pp_available && second.overflow());
    early.run({ExtraPp});
    CHECK(second.extra_pp_uses == 2 && second.bonus_pp == 1);
    early.run({EndTurn});
    CHECK(second.bonus_pp == 0);
    early.run({EndTurn});
    early.reject({ExtraPp});

    for (const int first_use_turn : {6, 7}) {
        Scenario saved;
        saved.state.turn_player = 1;
        saved.state.turn_number = 5;
        CHECK(saved.legal({ExtraPp}));
        while (saved.state.turn_number < first_use_turn) {
            saved.run({EndTurn});
            CHECK(saved.player(1).extra_pp_available && saved.player(1).extra_pp_uses == 0);
            saved.reject({ExtraPp}); // Only the second player can spend it.
            saved.run({EndTurn});
        }
        CHECK(saved.player(1).available_pp() == saved.player(1).max_pp);
        saved.run({ExtraPp});
        CHECK(saved.player(1).extra_pp_uses == 1 && saved.player(1).bonus_pp == 1);
        saved.reject({ExtraPp});
        saved.next_own_turn();
        CHECK(saved.player(1).bonus_pp == 0);
        saved.reject({ExtraPp});
    }

    Scenario deferred;
    deferred.state.turn_player = 1;
    deferred.state.turn_number = 5;
    deferred.run({ExtraPp});
    deferred.next_own_turn();
    CHECK(deferred.player(1).extra_pp_available && deferred.player(1).bonus_pp == 0);
    deferred.next_own_turn();
    deferred.run({ExtraPp});
    CHECK(deferred.player(1).extra_pp_uses == 2 &&
          deferred.player(1).max_pp == 10 && deferred.player(1).available_pp() == 11);
    deferred.next_own_turn();
    deferred.reject({ExtraPp});
}

void evolution_rights_and_super_combat() {
    Scenario gates;
    gates.place(kTrainer);
    gates.place(kTrainer, 1);
    gates.state.turn_number = 4;
    gates.reject({Evolve, 0});
    gates.state.turn_player = 1;
    CHECK(gates.legal({Evolve, 0}));
    gates.reject({SuperEvolve, 0});
    gates.state.turn_number = 6;
    CHECK(gates.legal({SuperEvolve, 0}));
    gates.state.turn_player = 0;
    gates.reject({SuperEvolve, 0});

    Scenario s;
    s.place(kTrainer);
    s.place(kColette, 1).abilities |= kBane;
    s.hand({kColette});
    s.run({SuperEvolve, 0});
    CHECK(s.player().board[0].evolution == Evolution::Super && s.player().board[0].hp == 6);
    CHECK(s.player().ep == 2 && s.player().sep == 1 && s.player().manual_evolved);
    s.run({Play, 0});
    CHECK(s.player().board[1].evolution == Evolution::Normal); // Auto-evolve bypasses the used manual slot.
    s.place(kDunamis);
    s.reject({Evolve, 2});
    s.reject({SuperEvolve, 2});
    s.run({Attack, 0, 0});
    CHECK(s.player().board[0].hp == 6); // Both combat damage and Bane fail.
    CHECK(s.player(1).board_count == 0 && s.player(1).leader_hp == 19);
    s.player().board[0].hp = 2;
    s.place(kAlmiraj, 1);
    s.run({EndTurn});
    s.place(kAlmiraj, 1);
    s.run({Attack, 0, 1}); // Ward blocks attacks on the super-evolved follower.
    s.player().board[1].abilities = 0;
    s.run({Attack, 0, 0}); // The first enemy attacker died, exposing the second in slot 0.
    CHECK(s.player().board[0].card_id != kTrainer); // No protection during the opponent's attack.

    Scenario no_rider;
    no_rider.place(kTrainer).evolution = Evolution::Super;
    no_rider.run({Attack, 0, kEnemyLeader});
    CHECK(no_rider.player(1).leader_hp == 17);
}

void turn_start_order_and_crests() {
    Scenario s;
    s.player().leader_hp = 17;
    for (int i = 0; i < 4; ++i) {
        s.place(kHolyFalcon);
    }
    s.place(kWingedStatue).countdown = 1;
    s.crest(CrestId::Testimony, 2);
    s.crest(CrestId::Rapid, 1);
    const auto top = s.player().deck[s.player().deck_idx];
    s.next_own_turn();
    CHECK(s.player().leader_hp == 18 && s.player().board_count == 5 && s.player().shadows == 1);
    CHECK(s.player().board[4].card_id == kRabbit && s.player().board[4].evolution == Evolution::Normal);
    CHECK(s.player().hand_count == 1 && s.player().hand[0] == top);
    CHECK(s.player().crest_count == 1 && s.player().crests[0].countdown == 1);

    Scenario by_creation;
    by_creation.place(kWingedStatue).countdown = 1;
    by_creation.place(kProjectedStatue).countdown = 1;
    by_creation.next_own_turn();
    CHECK(by_creation.player().board[0].card_id == kHolyFalcon &&
          by_creation.player().board[1].card_id == kSplendidHawk && by_creation.player().shadows == 2);

    Scenario alternate;
    alternate.rules.crest_clock = CrestClock::End;
    alternate.rules.expired_crest_adds_shadow = true;
    alternate.crest(CrestId::Rituel, 1);
    alternate.run({EndTurn});
    CHECK(alternate.player().crest_count == 0 && alternate.player().shadows == 1);
    CHECK(alternate.player().board[0].card_id == kRituel &&
          alternate.player().board[0].evolution == Evolution::Normal);
}

void timeout_and_deck_out() {
    Scenario final_round;
    final_round.state.turn_number = 40;
    final_round.run({EndTurn});
    CHECK(final_round.state.outcome == Outcome::Ongoing && final_round.state.turn_player == 1);

    Scenario timeout;
    timeout.state.turn_number = 40;
    timeout.state.turn_player = 1;
    timeout.crest(CrestId::Rituel, 1, 1);
    timeout.rules.crest_clock = CrestClock::End;
    timeout.run({EndTurn});
    CHECK(timeout.state.outcome == Outcome::BothLose && timeout.state.end_reason == EndReason::TurnLimit);
    CHECK(timeout.player(1).board[0].card_id == kRituel && legal_actions(timeout.state).empty());
    Scenario early_timeout;
    early_timeout.state.turn_number = 40;
    early_timeout.state.turn_player = 1;
    early_timeout.crest(CrestId::Rituel, 1, 1);
    early_timeout.rules.crest_clock = CrestClock::End;
    early_timeout.rules.turn_limit_after_end_effects = false;
    early_timeout.run({EndTurn});
    CHECK(early_timeout.state.outcome == Outcome::BothLose && early_timeout.player(1).board_count == 0);

    Scenario deck;
    deck.player(1).deck_idx = kDeckSize - 1;
    deck.run({EndTurn});
    CHECK(deck.state.outcome == Outcome::Ongoing && deck.player(1).deck_idx == kDeckSize);
    deck.next_own_turn();
    CHECK(deck.state.outcome == Outcome::FirstWins && deck.state.end_reason == EndReason::EmptyDeck);
}

void rule_variants_and_limits() {
    Scenario clue;
    clue.place(kEvidence);
    for (int i = 0; i < 4; ++i) {
        clue.place(kHolyFalcon);
    }
    clue.hand({kEvidence});
    clue.reject({Play, 0});
    clue.rules.merge_clue_on_full_board = true;
    clue.run({Play, 0});
    CHECK(clue.player().board[4].clue_count == 2 && clue.player().shadows == 0);

    Scenario limits;
    limits.place(kWingedStatue);
    limits.place(kAlmiraj);
    limits.hand({kAlmiraj});
    limits.player().actions[kWingedStatue - kFirstCard].act = kNameActionLimit;
    limits.player().actions[kAlmiraj - kFirstCard].play = kNameActionLimit;
    limits.player().actions[kAlmiraj - kFirstCard].attack = kNameActionLimit;
    limits.player().pp = 2;
    limits.reject({Play, 0});
    limits.reject({Act, 0});
    limits.reject({Attack, 1, kEnemyLeader});
    limits.next_own_turn();
    CHECK(limits.legal({Play, 0}) && limits.legal({Act, 0}) && limits.legal({Attack, 1, kEnemyLeader}));
    CHECK(limits.player().actions[kAlmiraj - kFirstCard] == NamedActions{});

    for (const bool consumes : {false, true}) {
        Scenario barrier;
        barrier.rules.zero_damage_consumes_barrier = consumes;
        barrier.place(kColette).atk = 0;
        barrier.place(kColette, 1).abilities |= kBarrier;
        barrier.run({Attack, 0, 0});
        CHECK(((barrier.player(1).board[0].abilities & kBarrier) == 0) == consumes);
        CHECK(barrier.player(1).board[0].hp == 4 && barrier.player().board[0].hp == 2);
    }

    bool found_preselected_repeat = false;
    for (std::uint64_t seed = 0; seed < 16; ++seed) {
        Scenario coins;
        coins.rules.random_targets = RandomTargets::AtAbilityStart;
        coins.state.rng_state = seed;
        coins.place(kColette);
        coins.place(kAlmiraj, 1);
        coins.place(kExias, 1);
        auto duplicate = coins;
        coins.run({Evolve, 0});
        duplicate.run({Evolve, 0});
        CHECK(coins.state == duplicate.state);
        found_preselected_repeat |= coins.player(1).board_count == 1;
    }
    CHECK(found_preselected_repeat);
}

void effect_transitions() {
    Scenario s;
    auto& super = s.place(kTrainer);
    super.evolution = Evolution::Super;
    super.hp = super.max_hp = 6;
    super.abilities |= kBarrier;
    const auto intact = super;
    const auto id = super.creation_order;
    s.rules.zero_damage_consumes_barrier = false;
    constexpr auto enemy = Target::ChosenEnemyFollower;
    const std::array harm{
        Effect{Trigger::None, Operation::Damage, enemy, 9},
        Effect{Trigger::None, Operation::Destroy, enemy},
    };
    resolve_effect_steps(s.state, 1, harm, 0, id, s.rules);
    CHECK(s.player().board[0] == intact);
    CHECK(s.player().shadows == 0);

    auto banished = s;
    resolve_effect_steps(banished.state, 1, std::array{Effect{Trigger::None, Operation::Banish, enemy}}, 0, id);
    CHECK(banished.player().board_count == 0 && banished.player().shadows == 0);

    auto transformed = s;
    transformed.player().board[0].attacked_this_turn = true;
    transformed.player().board[0].hp = 1;
    auto& named = transformed.player().actions[kExias - kFirstCard];
    named.play = 2;
    resolve_effect_steps(transformed.state, 1,
                         std::array{Effect{Trigger::None, Operation::Transform, enemy, 0, kExias}}, 0, id);
    const auto& replacement = transformed.player().board[0];
    CHECK(replacement.creation_order == id && replacement.card_id == kExias && replacement.hp == 1);
    CHECK(replacement.evolution == Evolution::None && !replacement.attacked_this_turn &&
          !replacement.readied && replacement.abilities == FLAG_RUSH);
    CHECK(transformed.player().shadows == 0 && transformed.player().hand_count == 0 && named.play == 2);
    transformed.rules.reset_name_counts_on_transform = true;
    resolve_effect_steps(transformed.state, 1,
                         std::array{Effect{Trigger::None, Operation::Transform, enemy, 0, kExias}},
                         0, id, transformed.rules);
    CHECK(named == NamedActions{});

    auto bounced = s;
    bounced.hand({kRapid, kRapid, kRapid, kRapid, kRapid, kRapid, kRapid, kRapid, kRapid});
    resolve_effect_steps(bounced.state, 1, std::array{Effect{Trigger::None, Operation::Bounce, enemy}}, 0, id);
    CHECK(bounced.player().board_count == 0 && bounced.player().hand_count == 9 && bounced.player().shadows == 1);
    CHECK(bounced.player().destroyed_followers[kTrainer - kFirstCard] == 0);

    s.state.turn_player = 1; // Ability destruction works on the other player's turn.
    resolve_effect_steps(s.state, 1, std::array{Effect{Trigger::None, Operation::Destroy, enemy}}, 0, id);
    CHECK(s.player().board_count == 0 && s.player().shadows == 1);
    CHECK(s.player().destroyed_followers[kTrainer - kFirstCard] == 1);

    for (const bool requires_live : {true, false}) {
        Scenario queued;
        queued.rules.require_live_ability_source = requires_live;
        queued.hand({kEvidence});
        const auto incoming = queued.state.next_creation_order;
        const std::array script{
            Effect{Trigger::None, Operation::Summon, Target::None, 1, kRabbit},
            Effect{Trigger::None, Operation::Silence, Target::Self},
        };
        resolve_effect_steps(queued.state, 0, script, incoming, 0, queued.rules);
        CHECK(queued.player().board[0].silenced);
        CHECK(queued.player().hand_count == (requires_live ? 1 : 0));
        CHECK(queued.player().board[0].evolution == (requires_live ? Evolution::None : Evolution::Normal));
    }

    const std::array lethal{
        Effect{Trigger::None, Operation::Damage, Target::BothLeaders, 3},
        Effect{Trigger::None, Operation::Heal, Target::OwnLeader, 5},
    };
    for (const auto& [rule, outcome] : {
            std::pair{SimultaneousLethal::SourceOpponent, Outcome::FirstWins},
            std::pair{SimultaneousLethal::NonActive, Outcome::SecondWins},
            std::pair{SimultaneousLethal::BothLose, Outcome::BothLose}}) {
        Scenario simultaneous;
        simultaneous.rules.simultaneous_lethal = rule;
        simultaneous.player().leader_hp = simultaneous.player(1).leader_hp = 3;
        resolve_effect_steps(simultaneous.state, 1, lethal, 0, 0, simultaneous.rules);
        CHECK(simultaneous.state.outcome == outcome && simultaneous.state.end_reason == EndReason::Health);
        CHECK(simultaneous.player().leader_hp == 0 && simultaneous.player(1).leader_hp == 0);
    }
    for (const int active : {0, 1}) {
        Scenario simultaneous;
        simultaneous.state.turn_player = static_cast<std::uint8_t>(active);
        simultaneous.player().leader_hp = simultaneous.player(1).leader_hp = 3;
        resolve_effect_steps(simultaneous.state, 1 - active, lethal);
        CHECK(simultaneous.state.outcome == (active == 0 ? Outcome::SecondWins : Outcome::FirstWins));
        CHECK(simultaneous.state.end_reason == EndReason::Health && simultaneous.player(1 - active).leader_hp == 0);
    }
}

void rabbit_collision_lethal() {
    for (const int active : {0, 1}) {
        const int defender = 1 - active;
        Scenario s;
        s.state.turn_player = static_cast<std::uint8_t>(active);
        s.place(kRabbit, defender); // The opponent's rabbit was on the board first.
        s.place(kRabbit, active);
        s.player(active).leader_hp = s.player(defender).leader_hp = 2;
        s.run({Attack, 0, 0});
        CHECK(s.player(active).board_count == 0 && s.player(defender).board_count == 0);
        CHECK(s.state.outcome == (active == 0 ? Outcome::FirstWins : Outcome::SecondWins));
        CHECK(s.state.end_reason == EndReason::Health);
        CHECK(s.player(active).leader_hp == 2 && s.player(defender).leader_hp == 0);
    }
}

void attack_after_transform() {
    Scenario attacked;
    const auto source = attacked.place(kTrainer).creation_order;
    attacked.place(kTrainer, 1);
    attacked.run({Attack, 0, kEnemyLeader});
    CHECK(attacked.player().board[0].attacked_this_turn);

    struct Form {
        CardId card;
        Evolution evolution;
        int bonus;
        bool board_attack;
        bool leader_attack;
    };
    for (const Form form : {
            Form{kTrainer, Evolution::None, 0, false, false},
            Form{kExias, Evolution::None, 0, true, false},
            Form{kAlmiraj, Evolution::None, 0, true, true},
            Form{kColette, Evolution::Normal, 2, true, false},
            Form{kColette, Evolution::Super, 3, true, false}}) {
        auto s = attacked;
        const Effect transform{.operation = Operation::Transform, .target = Target::Self,
                               .card = form.card, .transformed_evolution = form.evolution};
        resolve_effect_steps(s.state, 0, std::array{transform}, source);
        const auto& result = s.player().board[0];
        const auto& printed = *find_card(form.card);
        CHECK(result.creation_order == source && result.card_id == form.card && result.evolution == form.evolution);
        CHECK(result.atk == printed.atk + form.bonus && result.hp == printed.hp + form.bonus);
        CHECK(!result.attacked_this_turn && !result.readied);
        CHECK(s.player(1).board[0].hp == 3); // Changing form does not trigger Colette's OnEvolved ability.
        CHECK(s.legal({Attack, 0, kEnemyLeader}) == form.leader_attack);
        CHECK(s.legal({Attack, 0, 0}) == form.board_attack);
        if (form.board_attack) {
            s.run({Attack, 0, 0});
            s.reject({Attack, 0, kEnemyLeader});
        }
    }
}

void phase5_hand_and_rally() {
    const auto* metadata = find_official_card(10031210);
    CHECK(metadata && metadata->id == 122 && metadata->earth && metadata->implemented && !metadata->text.empty());
    CHECK(find_official_card(10042310)->kind == Kind::Spell);
    const auto id = [](std::uint32_t official) { return find_official_card(official)->id; };
    Scenario combo;
    combo.hand({kWingedStatue, kWingedStatue, id(10911110)});
    combo.run({Play, 0});
    combo.run({Play, 0});
    combo.run({Play, 0});
    CHECK(combo.player().plays_this_turn == 3 && combo.player().hand_count == 1 && combo.player().followers_entered == 1);
    combo.next_own_turn();
    CHECK(combo.player().plays_this_turn == 0 && combo.player().followers_entered == 1);
    const std::array rally{
        Effect{.operation=Operation::Summon, .amount=2, .card=kHolyFalcon},
        Effect{.operation=Operation::Heal, .target=Target::OwnLeader, .amount=3,
               .condition=Condition::Rally, .threshold=3},
    };
    combo.player().leader_hp = 14;
    resolve_effect_steps(combo.state, 0, rally);
    CHECK(combo.player().followers_entered == 3 && combo.player().leader_hp == 17);
    resolve_effect_steps(combo.state, 0, std::array{rally[0]});
    CHECK(combo.player().followers_entered == 3 && combo.player().board_count == 5);

    Scenario boost;
    const auto storm = id(10131320);
    boost.hand({storm, kTestimony, storm});
    boost.player().hand_modifiers[0].spellboost = 2;
    boost.player().hand_modifiers[0].cost = -1;
    boost.run({Play, 1, kNoTarget, 1});
    CHECK(boost.player().hand_modifiers[0].spellboost == 3 && boost.player().hand_modifiers[1].spellboost == 1);
    const auto carried = boost.player().hand_modifiers[0];
    boost.player().deck_idx = boost.player().deck_end;
    const std::array send{
        Effect{.operation=Operation::ReturnToDeck, .target=Target::ChosenOwnHand},
        Effect{.operation=Operation::Draw, .amount=1},
    };
    resolve_effect_steps(boost.state, 0, send, 0, kHandTargetFlag);
    CHECK(boost.player().hand[1] == storm && boost.player().hand_modifiers[1] == carried);
    CHECK(boost.player().hand_modifiers[0].spellboost == 1 && boost.player().deck_remaining() == 0);
    boost.place(kCorrector, 1);
    boost.run({Play, 1, 0});
    CHECK(boost.player(1).board_count == 0 && boost.player().hand_modifiers[0].spellboost == 2);
    const Effect discard{.operation=Operation::Discard, .amount=2};
    boost.hand({kColette, kTestimony, kExias});
    const auto shadows = boost.player().shadows;
    resolve_effect_steps(boost.state, 0, std::array{discard});
    CHECK(boost.player().hand_count == 1 && boost.player().shadows == shadows + 2);
    CHECK(std::all_of(boost.player().destroyed_followers.begin(), boost.player().destroyed_followers.end(),
                      [](auto value) { return value == 0; }));
}

void phase5_attacks_copy_and_earth() {
    Scenario combat;
    const auto monk = find_official_card(11061120)->id;
    auto& original = combat.place(monk);
    combat.run({Attack, 0, kEnemyLeader});
    CHECK(original.attacked_this_turn && !original.attacked_twice);
    combat.run({Attack, 0, kEnemyLeader});
    CHECK(original.attacked_twice);
    combat.reject({Attack, 0, kEnemyLeader});
    original.hp -= 2;
    const auto snapshot = original;
    const auto helper = combat.place(kTrainer, 1).creation_order;
    const std::array make_copy{Effect{.operation=Operation::Copy, .target=Target::Self}};
    resolve_effect_steps(combat.state, 0, make_copy, snapshot.creation_order);
    const auto& copy = combat.player().board[1];
    CHECK(copy.creation_order != snapshot.creation_order && copy.hp == snapshot.hp && copy.max_hp == snapshot.max_hp);
    CHECK(copy.atk == snapshot.atk && copy.abilities == snapshot.abilities && copy.modifiers == snapshot.modifiers);
    CHECK(!copy.attacked_this_turn && !copy.attacked_twice && !copy.acted_this_turn);
    CHECK(combat.player().followers_entered == 1 && combat.legal({Attack, 1, 0}) && helper > 0);

    Scenario earth;
    const std::array sigils{Effect{.operation=Operation::AddEarth, .amount=2},
                           Effect{.operation=Operation::Summon, .amount=1, .card=122}};
    resolve_effect_steps(earth.state, 0, sigils);
    CHECK(earth.player().board_count == 1 && earth.player().board[0].earth_count == 3);
    CHECK(!can_choose_enemy_permanent(earth.player().board[0]) && earth.player().shadows == 0);
    const auto order = earth.player().board[0].creation_order;
    std::array destroy{Effect{.operation=Operation::Destroy, .target=Target::ChosenEnemyPermanent}};
    resolve_effect_steps(earth.state, 1, destroy, 0, order);
    CHECK(earth.player().board_count == 1 && earth.player().board[0].earth_count == 3);
    const std::array rite{Effect{.operation=Operation::EarthRite, .amount=3},
                          Effect{.operation=Operation::Draw, .amount=1, .condition=Condition::CostPaid}};
    resolve_effect_steps(earth.state, 0, rite);
    CHECK(earth.player().board_count == 0 && earth.player().shadows == 1 && earth.player().hand_count == 1);
    resolve_effect_steps(earth.state, 0, rite);
    CHECK(earth.player().shadows == 1 && earth.player().hand_count == 1);
}

void phase5_pp_necromancy_and_reanimate() {
    Scenario dragon;
    dragon.player().max_pp = dragon.player().pp = 6;
    dragon.player().bonus_pp = 1;
    dragon.hand({find_official_card(11041310)->id, find_official_card(10042310)->id});
    CHECK(dragon.player().available_pp() == 7 && !dragon.player().overflow());
    dragon.run({Play, 0});
    CHECK(dragon.player().max_pp == 6);
    dragon.run({Play, 0});
    CHECK(dragon.player().max_pp == 7 && dragon.player().overflow() && dragon.player().available_pp() == 1);
    dragon.player().max_pp = dragon.player().pp = 9;
    dragon.player().bonus_pp = 0;
    dragon.hand({find_official_card(10042310)->id, find_official_card(10042310)->id});
    dragon.run({Play, 0});
    CHECK(dragon.player().max_pp == 10 && dragon.player().pp == 6 && dragon.player().hand_count == 2);
    dragon.run({Play, 0});
    CHECK(dragon.player().max_pp == 10 && dragon.player().pp == 3 && dragon.player().hand_count == 2);

    Scenario shadow;
    const auto mummy = find_official_card(10051130)->id;
    shadow.hand({mummy, mummy});
    shadow.player().shadows = 4;
    shadow.run({Play, 0});
    CHECK(shadow.player().shadows == 0 && shadow.legal({Attack, 0, kEnemyLeader}));
    shadow.run({Play, 0});
    CHECK(!shadow.legal({Attack, 1, kEnemyLeader}));

    Scenario history;
    history.player().destroyed_followers[kLilium - kFirstCard] = 1;
    history.player().destroyed_followers[kExias - kFirstCard] = 3;
    history.player().destroyed_followers[kTrainer - kFirstCard] = 9;
    int older = 0;
    for (int seed = 0; seed < 512; ++seed) {
        auto sample = history;
        sample.state.rng_state = static_cast<std::uint64_t>(seed);
        resolve_effect_steps(sample.state, 0, std::array{Effect{.operation=Operation::Reanimate, .amount=2}});
        CHECK(sample.player().board_count == 1 && sample.player().shadows == 0);
        const auto summoned = sample.player().board[0].card_id;
        CHECK(summoned == kLilium || summoned == kExias);
        older += summoned == kLilium;
    }
    CHECK(older > 95 && older < 158);
}

void phase5_fusion_and_faith() {
    Scenario fusion;
    const auto host = find_official_card(11072310)->id;
    fusion.hand({kEvidence, host, kWingedStatue, kEvidence, host});
    fusion.run({Fuse, 1, kNoTarget, 3});
    CHECK(fusion.player().hand_count == 3 && fusion.player().hand[0] == host && fusion.player().shadows == 0);
    CHECK(fusion.player().hand_modifiers[0].fused_materials == 2 && fusion.player().board[0].card_id == kEvidence);
    fusion.reject({Fuse, 0, kNoTarget, 1});
    CHECK(fusion.legal({Fuse, 2, kNoTarget, 2}));
    fusion.next_own_turn();
    CHECK(fusion.legal({Fuse, 0, kNoTarget, 1}));

    Scenario faith;
    faith.crest(CrestId::Testimony, 2);
    const std::array grant{Effect{.operation=Operation::GrantFaith, .card=kFaithToken}};
    resolve_effect_steps(faith.state, 0, grant);
    resolve_effect_steps(faith.state, 0, grant);
    CHECK(faith.player().faith_count == 1 && faith.player().crest_count == 1);
    // Populate four different faith slots to check the shared boundary with one crest.
    faith.player().faiths = {kFaithToken, kLilium, kDunamis, kRapid, {}};
    faith.player().faith_count = 4;
    resolve_effect_steps(faith.state, 0, std::array{Effect{.operation=Operation::GrantCrest, .crest=CrestId::Rapid}});
    CHECK(faith.player().faith_count == 4 && faith.player().crest_count == 1);
}
using Unit = std::pair<std::string_view, void (*)()>;
constexpr std::array kUnits{
    Unit{"winged statue", winged_statue}, Unit{"Lilium", lilium}, Unit{"Exias", exias},
    Unit{"craftsman", craftsman}, Unit{"Colette", colette}, Unit{"Almiraj", almiraj},
    Unit{"Dunamis", dunamis}, Unit{"trainer", trainer}, Unit{"testimony", testimony},
    Unit{"projected statue", projected_statue}, Unit{"Rituel", rituel}, Unit{"propagation", propagation},
    Unit{"Rapid", rapid}, Unit{"Teresa", teresa}, Unit{"rabbit", rabbit}, Unit{"evidence", evidence},
    Unit{"holy falcon", [] { falcon(kHolyFalcon, 2, 2); }},
    Unit{"splendid hawk", [] { falcon(kSplendidHawk, 4, 4); }}, Unit{"healing sister", sister},
    Unit{"corrector and exact mirror deck", corrector_and_mirror_deck},
    Unit{"mulligan and first draw", mulligan_and_first_draw}, Unit{"extra PP", extra_pp},
    Unit{"evolution rights and super combat", evolution_rights_and_super_combat},
    Unit{"turn start and crest ordering", turn_start_order_and_crests},
    Unit{"timeout and deck-out", timeout_and_deck_out},
    Unit{"rule variants and action limits", rule_variants_and_limits},
    Unit{"ability protection, zone changes, queued silence, and simultaneous lethal", effect_transitions},
    Unit{"lethal Last Words when two rabbits trade", rabbit_collision_lethal},
    Unit{"attack after transforming a follower that already attacked", attack_after_transform},
    Unit{"JSON catalog, combo, rally, spellboost, deck return and discard", phase5_hand_and_rally},
    Unit{"two attacks, copying and Earth Rite", phase5_attacks_copy_and_earth},
    Unit{"overflow, PP cap, necromancy and weighted reanimate", phase5_pp_necromancy_and_reanimate},
    Unit{"per-card fusion and shared faith slots", phase5_fusion_and_faith},
};

int units() {
    int failures = 0;
    for (const auto& [name, unit] : kUnits) {
        try {
            unit();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << kUnits.size() - failures << '/' << kUnits.size() << " WB unit groups passed\n";
    return failures == 0 ? 0 : 1;
}

void validate(const GameState& state, const GameState& before) {
    CHECK(state.turn_player < 2 && state.turn_number >= 1 && state.turn_number <= kTurnLimit);
    CHECK(state.next_creation_order >= before.next_creation_order);
    std::array<EntityId, 2 * (kBoardLimit + kCrestLimit)> objects{};
    int found = 0;
    const auto record = [&](EntityId id) {
        CHECK(id > 0 && id < state.next_creation_order);
        CHECK(std::find(objects.begin(), objects.begin() + found, id) == objects.begin() + found);
        objects[found++] = id;
    };
    for (int seat = 0; seat < 2; ++seat) {
        const auto& player = state.players[seat];
        const auto& prior = before.players[seat];
        CHECK(player.pp <= player.max_pp && player.max_pp <= 10 && player.bonus_pp <= 1);
        CHECK(seat == 1 || (!player.bonus_pp && !player.extra_pp_available && !player.extra_pp_uses));
        CHECK(player.bonus_pp == 0 || (seat == state.turn_player && !player.extra_pp_available));
        CHECK(player.extra_pp_uses <= 2 && player.extra_pp_uses >= prior.extra_pp_uses);
        if (seat == 1 && player.extra_pp_uses == 0) {
            CHECK(player.extra_pp_available);
        }
        CHECK(player.hand_count <= kHandLimit && player.board_count <= kBoardLimit && player.crest_count <= kCrestLimit);
        CHECK(player.deck_idx >= prior.deck_idx && player.deck_idx <= kDeckSize && player.shadows >= prior.shadows);
        CHECK(player.ep <= 2 && player.sep <= 2 && player.ep <= prior.ep && player.sep <= prior.sep);
        CHECK(player.leader_hp <= 20 && (state.outcome != Outcome::Ongoing || player.leader_hp > 0));
        if (before.phase == Phase::Main) {
            CHECK(player.deck == prior.deck);
        }
        int destroyed = 0;
        for (int i = 0; i < kCardSlots; ++i) {
            CHECK(player.destroyed_followers[i] >= prior.destroyed_followers[i]);
            destroyed += player.destroyed_followers[i];
        }
        CHECK(destroyed <= player.shadows);
        for (int i = 0; i < kHandLimit; ++i) {
            CHECK(i < player.hand_count ? find_card(player.hand[i]) != nullptr : player.hand[i] == 0);
        }
        int clues = 0;
        for (int i = 0; i < kBoardLimit; ++i) {
            const auto& object = player.board[i];
            if (i >= player.board_count) {
                CHECK(object == Permanent{});
                continue;
            }
            record(object.creation_order);
            CHECK(i == 0 || player.board[i - 1].creation_order < object.creation_order);
            const auto* card = find_card(object.card_id);
            CHECK(card && card->kind != Kind::Spell);
            if (card->kind == Kind::Follower) {
                CHECK(object.hp > 0 && object.hp <= object.max_hp && object.atk >= 0 && object.countdown == 0);
            } else {
                CHECK(object.evolution == Evolution::None && !object.readied && !object.attacked_this_turn);
                CHECK(card->clue ? object.clue_count > 0 : object.countdown > 0);
            }
            clues += object.clue_count > 0;
        }
        CHECK(clues <= 1);
        for (int i = 0; i < kCrestLimit; ++i) {
            const auto& crest = player.crests[i];
            if (i >= player.crest_count) {
                CHECK(crest == Crest{});
                continue;
            }
            record(crest.creation_order);
            CHECK(i == 0 || player.crests[i - 1].creation_order < crest.creation_order);
            CHECK(find_crest(crest.id) != nullptr && crest.countdown > 0);
            CHECK(std::none_of(player.crests.begin(), player.crests.begin() + i,
                               [&](const Crest& other) { return other.id == crest.id; }));
        }
    }
    if (state.outcome == Outcome::Ongoing) {
        CHECK(state.end_reason == EndReason::None);
    } else if (state.outcome == Outcome::BothLose) {
        CHECK(state.end_reason == EndReason::TurnLimit && state.turn_number == 40);
    } else {
        const auto& loser = state.players[state.outcome == Outcome::FirstWins ? 1 : 0];
        CHECK((state.end_reason == EndReason::Health && loser.leader_hp <= 0) ||
              (state.end_reason == EndReason::EmptyDeck && loser.deck_idx == kDeckSize));
    }
}

int stress() {
    constexpr int games = 10'000;
    constexpr std::uint64_t seed = 0x57'42'340AULL;
    std::mt19937_64 rng(seed);
    std::array<int, 4> outcomes{};
    std::array<int, 4> reasons{};
    std::array<std::uint64_t, 8> action_counts{};
    int max_actions = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int game = 0; game < games; ++game) {
        const auto game_seed = rng();
        auto state = new_game(kMirrorDeck, kMirrorDeck, game_seed);
        int actions = 0;
        Action selected{};
        try {
            while (state.outcome == Outcome::Ongoing) {
                CHECK(actions < 2048);
                const auto choices = legal_actions(state);
                CHECK(!choices.empty());
                selected = choices[std::uniform_int_distribution<int>(0, choices.size() - 1)(rng)];
                const auto before = state;
                apply_legal_action(state, selected);
                ++action_counts[static_cast<int>(selected.type)];
                ++actions;
                validate(state, before);
            }
            CHECK(legal_actions(state).empty());
        } catch (const std::exception& error) {
            std::cerr << "Game " << game + 1 << " seed=" << game_seed << " step=" << actions
                      << " round=" << static_cast<int>(state.turn_number)
                      << " action=" << static_cast<int>(selected.type) << ": " << error.what() << '\n';
            return 1;
        }
        ++outcomes[static_cast<int>(state.outcome)];
        ++reasons[static_cast<int>(state.end_reason)];
        max_actions = std::max(max_actions, actions);
    }
    CHECK(std::all_of(action_counts.begin(), action_counts.end(), [](auto count) { return count > 0; }));
    std::uint64_t total = 0;
    for (const auto count : action_counts) {
        total += count;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << "PASS exact_mirror games=" << games << " seed=" << seed << " actions=" << total
              << " max_actions=" << max_actions << " invariant_failures=0\n"
              << "first_wins=" << outcomes[1] << " second_wins=" << outcomes[2] << " both_lose=" << outcomes[3]
              << " lethal=" << reasons[1] << " deck_out=" << reasons[2] << " turn_limit=" << reasons[3] << '\n'
              << std::fixed << std::setprecision(3) << "elapsed_seconds=" << secs << " games_per_second="
              << games / secs << " (single thread, all states checked)\n";
    return 0;
}

CardId official(std::uint32_t id) {
    const auto* card = find_official_card(id);
    CHECK(card && card->implemented);
    return card->id;
}

void all_rotation_cards() {
    const auto cards = neutral_bishop_rotation();
    CHECK(cards.size() == 101);
    int neutral = 0;
    int bishop = 0;
    for (const auto id : cards) {
        const auto& card = *find_card(id);
        try {
            CHECK(is_deck_card(id));
            CHECK(official(card.official_id) == id && printed_card(id) == id);
            neutral += card.class_name == "ニュートラル";
            bishop += card.class_name == "ビショップ";
            // Verify every printed card is accepted by the same deck loader rules used in a live game.
            Deck deck{};
            deck[0] = id;
            std::copy_n(std::views::filter(cards, [id](CardId item) { return item != id; }).begin(), kDeckSize - 1,
                        deck.begin() + 1);
            const auto opening = new_game(deck, deck, card.official_id);
            CHECK(std::find(opening.players[0].deck.begin(), opening.players[0].deck.begin() + kDeckSize, id) !=
                  opening.players[0].deck.begin() + kDeckSize);

            Scenario s;
            s.place(kWingedStatue);
            s.place(kTrainer);
            s.place(kLilium).evolution = Evolution::Normal;
            s.place(kHolyFalcon, 1);
            s.hand({id, kTrainer, kRapid, kEvidence, kColette});
            s.player().leader_hp = 10;
            const auto legal = legal_actions(s.state);
            const auto action = std::find_if(legal.begin(), legal.end(), [](const auto& move) {
                return move.type == Play && move.src == 0;
            });
            CHECK(action != legal.end());
            s.run(*action);
            CHECK(s.state.outcome == Outcome::Ongoing && s.player().plays_this_turn == 1);
            CHECK(s.player().hand_count <= kHandLimit && s.player().board_count <= kBoardLimit);
            CHECK(s.player().pp <= 10 - (card.enhance_cost ? card.enhance_cost : card.cost)
                  || card.official_id == 10663210); // The book can refund PP.
            if (card.kind != Kind::Spell) {
                const bool found = std::any_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                    [id](const auto& object) { return object.card_id == id; });
                CHECK(found);
            } else {
                CHECK(s.player().shadows >= 1);
            }
            std::cout << "PASS rotation_card " << card.official_id << '\n';
        } catch (const std::exception& error) {
            throw std::runtime_error(std::to_string(card.official_id) + " " + std::string(card.name) + ": " + error.what());
        }
    }
    CHECK(neutral == 41 && bishop == 60);
}

void rotation_forms_and_tokens() {
    struct Form {
        std::uint32_t base;
        std::uint32_t crystal;
        int cost;
    };
    for (const auto [base, crystal, cost] : std::array<Form, 4>{{
            {10962120, 10962122, 1}, {10661110, 10661112, 2}, {10662110, 10662112, 1}, {10663110, 10663112, 1}}}) {
        Scenario s;
        s.hand({official(base)});
        s.player().leader_hp = 10;
        s.player().pp = static_cast<std::uint8_t>(cost);
        s.run({Play, 0});
        const auto form = official(crystal);
        CHECK(s.player().pp == 0 && s.player().board[0].card_id == form &&
              s.player().board[0].countdown == 3 && printed_card(form) == official(base));
        CHECK(!is_deck_card(form) && find_card(form)->act_cost == (base == 10962120 ? 1 : -1));
        s.next_own_turn();
        CHECK(s.player().board[0].countdown == 2);
        if (base == 10962120) {
            s.run({Act, 0});
            CHECK(s.player().board[0].countdown == 1);
        } else {
            s.next_own_turn();
            CHECK(s.player().board[0].countdown == 1);
        }
        s.next_own_turn();
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(base));
        CHECK(s.player().board[0].evolution == Evolution::None && s.player().shadows == 1);
        CHECK(s.player().leader_hp == (base == 10661110 ? 12 : 10));
    }
    Scenario accelerate;
    accelerate.hand({official(10901110)});
    accelerate.place(kTrainer, 1);
    accelerate.player().pp = 1;
    const auto form = official(10901112);
    CHECK(printed_card(form) == official(10901110) && find_card(form)->kind == Kind::Spell && !is_deck_card(form));
    accelerate.run({Play, 0});
    CHECK(accelerate.player().board_count == 0 && accelerate.player().pp == 0 &&
          accelerate.player().shadows == 1 && accelerate.player(1).board[0].hp == 1);

    Scenario tiger;
    tiger.hand({official(10961210)});
    tiger.run({Play, 0});
    tiger.run({Act, 0});
    const auto& token = tiger.player().board[0];
    CHECK(token.card_id == official(90061120) && token.atk == 4 && token.hp == 4 && (token.abilities & FLAG_RUSH));
    CHECK(!is_deck_card(token.card_id));
    const auto fox = official(10061120);
    CHECK(find_card(fox)->abilities & FLAG_WARD);
    CHECK(official(90001210) == kEvidence && official(90061110) == kHolyFalcon &&
          official(90061130) == kSplendidHawk && official(90061140) == kRabbit);
}

void rotation_crests() {
    using enum CrestId;
    for (const auto [id, code, cd] : std::array<std::tuple<CrestId, std::uint32_t, int>, 7>{{
             {Testimony, 11063312, 2}, {Rituel, 10864122, 1}, {Rapid, 11064112, 1},
             {Haylem, 10704112, 2}, {Erralde, 10964112, 0}, {Kukishiro, 10564122, 0}, {Sisters, 10864112, 0}}}) {
        const auto* crest = find_crest(id);
        CHECK(crest && crest->official_id == code && crest->countdown == cd && !crest->name.empty());
    }
    Scenario honey;
    const auto haylem = official(10704110);
    honey.place(haylem).atk = 0;
    honey.place(kTrainer, 1);
    honey.run({Attack, 0, 0});
    CHECK(honey.player().board_count == 1 && honey.player(1).board[0].banish_at_end);
    honey.run({EndTurn});
    honey.reject({Attack, 0, kEnemyLeader});
    honey.run({EndTurn});
    CHECK(honey.player(1).board_count == 0 && honey.player(1).shadows == 0);
    honey.place(kProjectedStatue, 1);
    honey.place(kTrainer, 1).atk = 20;
    honey.state.turn_player = 1;
    honey.run({Attack, 1, 0});
    CHECK(honey.player().crest_count == 1 && honey.player().crests[0].id == Haylem);
    honey.run({EndTurn});
    CHECK(honey.player().crest_count == 1 && honey.player().crests[0].countdown == 1);
    honey.next_own_turn();
    CHECK(honey.player().crest_count == 0 && honey.player().board[0].card_id == haylem &&
          honey.player().board[0].evolution == Evolution::Normal);

    Scenario erralde;
    erralde.player().leader_hp = 15;
    erralde.place(official(10964110));
    erralde.run({Evolve, 0});
    CHECK(erralde.player().crests[0].id == Erralde);
    erralde.next_own_turn();
    CHECK(erralde.player().leader_hp == 16 && erralde.player(1).leader_hp == 19 &&
          erralde.player().crests[0].countdown == 0);

    Scenario kukishiro;
    kukishiro.crest(Kukishiro, 0);
    auto& side = kukishiro.player();
    side.deck[side.deck_idx] = kWingedStatue;
    side.deck[side.deck_idx + 1] = kLilium;
    side.deck[side.deck_idx + 2] = kWingedStatue;
    kukishiro.next_own_turn();
    CHECK(side.board_count == 1 && kukishiro.player(1).board_count == 0);
    kukishiro.next_own_turn();
    CHECK(side.board_count == 1 && kukishiro.player(1).board_count == 1);
    kukishiro.hand({kExias, kExias, kExias, kExias, kExias, kExias, kExias, kExias, kExias});
    kukishiro.next_own_turn();
    CHECK(side.board_count == 1 && kukishiro.player(1).board_count == 1 &&
          side.shadows == 1 && side.hand_count == 9);

    Scenario sisters;
    const auto card = official(10864110);
    sisters.place(card);
    sisters.hand({official(10862120)});
    sisters.place(kTrainer, 1);
    sisters.run({SuperEvolve, 0});
    CHECK(sisters.player().crests[0].id == Sisters && sisters.player().hand_modifiers[0].cost == -3);
    sisters.run({Attack, 0, 0});
    CHECK(sisters.player().board[0].abilities & kDoubleStrike);
    sisters.run({Attack, 0, kEnemyLeader});
    sisters.reject({Attack, 0, kEnemyLeader});
    sisters.next_own_turn();
    CHECK(!(sisters.player().board[0].abilities & kDoubleStrike) &&
          sisters.player().crests[0].id == Sisters);
    sisters.run({Attack, 0, kEnemyLeader});
    sisters.reject({Attack, 0, kEnemyLeader});

    Scenario deck_summon;
    auto& deck = deck_summon.player();
    std::fill(deck.deck.begin() + deck.deck_idx, deck.deck.begin() + deck.deck_end, kRapid);
    deck.deck[deck.deck_idx + 1] = official(10661110);
    deck.deck_modifiers[deck.deck_idx + 1].cost = -3;
    CHECK(card_cost(*find_card(official(10661110)), deck.deck_modifiers[deck.deck_idx + 1]) <= 2);
    deck_summon.hand({card, official(10862120)});
    deck_summon.player().leader_hp = 12;
    deck_summon.run({Play, 0});
    CHECK(deck.board_count == 2 && deck.board[1].card_id == official(10661110) &&
          deck.board[1].evolution == Evolution::Super && deck.leader_hp == 14);
    CHECK(deck.hand_modifiers[0].cost == -3 && deck.crest_count == 0 && !deck.manual_evolved);
}

void rotation_sequences() {
    Scenario city;
    city.hand({official(10703210), kTrainer, kRapid});
    city.place(kTrainer, 1);
    city.player().leader_hp = 15;
    city.run({Play, 0});
    CHECK(city.player().board[0].countdown == 1);
    city.run({Act, 0, kOwnHandTarget});
    CHECK(city.player().board[0].countdown == 2 && city.player().shadows == 1);
    city.next_own_turn();
    CHECK(city.player(1).board[0].hp == 1 && city.player().board[0].sequence == 1);
    city.run({Act, 0, kOwnHandTarget});
    city.next_own_turn();
    CHECK(city.player().leader_hp == 17 && city.player().board[0].sequence == 2);
    city.run({EndTurn});
    CHECK(city.player(1).leader_hp == 18 && city.player().board_count == 0 && city.player().shadows == 3);

    Scenario omerio;
    omerio.place(official(10964120));
    omerio.place(kWingedStatue);
    omerio.place(kProjectedStatue);
    omerio.place(kEvidence);
    omerio.place(kCorrector, 1);
    omerio.place(kCorrector, 1);
    omerio.player().leader_hp = 15;
    omerio.run({Evolve, 0});
    CHECK(omerio.player(1).board[0].hp == 2 && omerio.player(1).board[1].hp == 2);
    CHECK(omerio.player().leader_hp == 17 && omerio.player().board[0].sequence == 0);
    CHECK(std::count_if(omerio.player().board.begin(), omerio.player().board.begin() + omerio.player().board_count,
                        [](const auto& pc) { return pc.card_id == kHolyFalcon; }) == 2);
    omerio.hand({official(10562210)});
    omerio.run({Play, 0});
    omerio.run({Act, 4});
    CHECK(omerio.player(1).board_count == 0 && omerio.player().board[0].sequence == 1);

    Scenario costs;
    costs.place(official(10503210));
    costs.hand({kTrainer, kTrainer, kTrainer});
    costs.run({Play, 0});
    CHECK(costs.player().board[0].countdown == 5);
    costs.run({Play, 0});
    CHECK(costs.player().board[0].countdown == 4);
    costs.place(official(10903210));
    costs.player().played_cost_mask = 0x1fc;
    costs.player().destroyed_followers[kTrainer - kFirstCard] = 2;
    costs.hand({kWingedStatue});
    costs.run({Play, 0});
    costs.run({EndTurn});
    CHECK(std::none_of(costs.player().board.begin(), costs.player().board.begin() + costs.player().board_count,
                       [](const auto& pc) { return pc.card_id == official(10903210); }));
    CHECK(costs.player().board_count == 5 && costs.player().board[1].atk == 6 &&
          costs.player().board[4].card_id == kTrainer && costs.player().board[4].max_hp == 6);
}

void rotation_targets_and_copies() {
    Scenario goddess;
    goddess.place(official(10502110));
    goddess.hand({kTrainer, kRapid, kEvidence, kWingedStatue, kColette, kTeresa});
    goddess.player().hand_modifiers[3].cost = -1;
    goddess.reject({Evolve, 0});
    goddess.run({Evolve, 0, kNoTarget, 0, 0b000111});
    CHECK(goddess.player().shadows == 3 && goddess.player().hand_count == 6);
    CHECK(goddess.player().hand[0] == kWingedStatue && goddess.player().hand[3] == kWingedStatue &&
          goddess.player().hand_modifiers[3].cost == -1);

    Scenario angel;
    angel.hand({official(10902110)});
    angel.place(kTrainer);
    angel.place(kColette);
    angel.place(kLilium);
    angel.run({Play, 0, kNoTarget, 0, 0b101});
    CHECK(angel.player().board[0].atk == 6 && angel.player().board[1].atk == 2 &&
          angel.player().board[2].atk == 5 && angel.player().board[3].card_id == official(10902110));

    Scenario spores;
    spores.hand({official(10862310)});
    spores.place(kColette, 1);
    spores.place(kTrainer, 1);
    spores.player().pp = 3;
    spores.reject({Play, 0, 0});
    spores.run({Play, 0, 1});
    CHECK(spores.player(1).board_count == 1 && spores.player(1).board[0].card_id == kColette);
    spores.hand({official(10862310)});
    spores.player().pp = 5;
    spores.place(kTrainer, 1).abilities |= kAura;
    spores.place(kTrainer, 1).abilities |= kAmbush;
    spores.run({Play, 0});
    CHECK(spores.player(1).board_count == 1 && spores.player(1).shadows == 0);

    Scenario copy;
    const auto idol = official(11064120);
    copy.place(idol);
    const auto donor = copy.place(kTrainer, 1).creation_order;
    copy.player(1).board[0].atk = 9;
    copy.player(1).board[0].hp = 1;
    copy.run({Evolve, 0, 0});
    CHECK(copy.player(1).board_count == 0 && copy.player(1).shadows == 0 && copy.player().board_count == 2);
    CHECK(copy.player().board[1].card_id == kTrainer && copy.player().board[1].hp == 1 &&
          copy.player().board[1].atk == 9 && copy.player().board[1].creation_order > donor);

    Scenario hand;
    hand.place(official(10602210));
    hand.hand({kTrainer});
    auto& rival = hand.player(1);
    rival.deck_end = rival.deck_idx + 1;
    rival.deck[rival.deck_idx] = kRapid;
    rival.deck_modifiers[rival.deck_idx].cost = -2;
    hand.run({Act, 0, kOwnHandTarget});
    CHECK(hand.player().hand[0] == kRapid && hand.player().hand_modifiers[0].cost == -2 &&
          rival.deck_remaining() == 1 && hand.player().shadows == 0);
    hand.hand({official(10802310)});
    hand.hand({kRapid}, 1);
    rival.hand_modifiers[0].cost = -2;
    hand.run({Play, 0});
    CHECK(hand.player().hand_count == 2 && hand.player().hand[0] == kRapid &&
          hand.player().hand_modifiers[0].cost == -3 && rival.hand_modifiers[0].cost == -2);
}

void rotation_board_conditions() {
    Scenario evidence;
    const auto clue = official(11003210);
    evidence.player().leader_hp = 14;
    evidence.hand({clue, kEvidence});
    evidence.run({Play, 0});
    evidence.run({Play, 0});
    CHECK(evidence.player().leader_hp == 16 && evidence.player().board[0].card_id == kEvidence &&
          evidence.player().board[0].clue_count == 2 && evidence.player().shadows == 0);

    Scenario book;
    book.hand({official(10663210)});
    book.place(kWingedStatue);
    book.place(kEvidence, 1);
    book.reject({Play, 0, 0});
    book.run({Play, 0, kOwnBoardTarget});
    CHECK(book.player().pp == 8 && book.player().board_count == 2 &&
          book.player().board[1].card_id == kHolyFalcon);
    book.next_own_turn();
    book.next_own_turn();
    CHECK(book.player().board[1].card_id == kWingedStatue);
    CHECK(book.player().destroyed_amulets[kWingedStatue - kFirstCard] == 1);

    Scenario promise;
    promise.place(kRapid);
    promise.place(kTrainer, 1);
    promise.player().leader_hp = 10;
    promise.hand({official(10962310)});
    promise.reject({Play, 0, kNoTarget, 1});
    promise.run({Play, 0, kNoTarget, kAllModes});
    CHECK(promise.player().leader_hp == 12 && promise.player(1).board_count == 0);

    Scenario heal;
    heal.player().leader_hp = 14;
    heal.place(official(10961110));
    heal.place(official(10563110));
    heal.place(official(10963110));
    heal.place(kTrainer, 1);
    heal.hand({kHealingSister});
    heal.run({Play, 0});
    CHECK(heal.player().board[0].evolution == Evolution::Normal && !heal.player().manual_evolved &&
          heal.player().leader_hp == 19 && heal.player().board[4].card_id == official(10061120) &&
          heal.player(1).board_count == 0);
}

int rotation_units() {
    try {
        all_rotation_cards();
        rotation_forms_and_tokens();
        rotation_crests();
        rotation_sequences();
        rotation_targets_and_copies();
        rotation_board_conditions();
        std::cout << "PASS rotation card cases=101, plus forms, tokens, crests and interactions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

void elf_royal_card_cases() {
    CHECK(elf_rotation().size() == 63 && royal_rotation().size() == 60);
    for (const auto group : {elf_rotation(), royal_rotation()}) {
        for (const auto id : group) {
            const auto& card = *find_card(id);
            try {
                CHECK(is_deck_card(id) && official(card.official_id) == id && printed_card(id) == id);
                Deck deck{};
                deck[0] = id;
                auto others = std::views::filter(group, [id](CardId entry) { return entry != id; });
                std::copy_n(others.begin(), kDeckSize - 1, deck.begin() + 1);
                const auto fresh = new_game(deck, deck, card.official_id);
                CHECK(fresh.players[0].deck_end == kDeckSize && fresh.players[0].hand_count == 4);
                const auto& side = fresh.players[0];
                if (card.faith) CHECK(std::find(side.faiths.begin(), side.faiths.begin() + side.faith_count, card.faith) !=
                                      side.faiths.begin() + side.faith_count);

                Scenario s;
                s.place(kWingedStatue);
                s.place(kTrainer);
                s.place(kTrainer, 1);
                s.hand({id, kRapid, kPropagation, kEvidence});
                s.player().leader_hp = 10;
                const auto moves = legal_actions(s.state);
                const auto action = std::find_if(moves.begin(), moves.end(), [](Action move) {
                    return move.type == Play && move.src == 0;
                });
                CHECK(action != moves.end());
                s.run(*action);
                CHECK(s.state.outcome == Outcome::Ongoing && s.player().plays_this_turn > 0);
                CHECK(s.player().hand_count <= kHandLimit && s.player().board_count <= kBoardLimit);
                if (card.kind != Kind::Spell) {
                    CHECK(std::any_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                                      [id](const auto& object) { return object.card_id == id; }));
                } else {
                    CHECK(s.player().shadows > 0);
                }
                std::cout << "PASS elf_royal_card " << card.official_id << '\n';
            } catch (const std::exception& error) {
                throw std::runtime_error(std::to_string(card.official_id) + " " + std::string(card.name) + ": " + error.what());
            }
        }
    }
}

void elf_royal_faith() {
    const auto sas = official(10614120);
    const auto idmeta = official(10624120);
    const auto sas_faith = official(10614122);
    const auto id_faith = official(10624122);
    auto deck = kMirrorDeck;
    deck[0] = deck[1] = sas;
    deck[2] = idmeta;
    const auto fresh = new_game(deck, deck, 13);
    for (const auto& side : fresh.players) {
        CHECK(side.faith_count == 2 && side.faiths[0] == sas_faith && side.faiths[1] == id_faith &&
              side.crest_count == 0 && side.faith_values[0] == 0 && side.faith_values[1] == 0 && side.deck_end == 40);
    }
    CHECK(!is_deck_card(sas_faith) && !is_deck_card(id_faith));
    Scenario s;
    auto& p = s.player();
    p.faiths[0] = sas_faith;
    p.faiths[1] = id_faith;
    p.faith_count = 2;
    p.faith_values[0] = 8;
    p.faith_values[1] = 4;
    s.hand({sas, sas, idmeta, official(10522310), official(10621110), official(10612310)});
    s.run({Play, 0});
    CHECK(p.faith_values[0] == 8 && p.faith_upgrades[0] == 0 && p.hand_count == 5);
    s.run({Evolve, 0});
    CHECK(p.faith_values[0] == 9 && p.hand_modifiers[p.hand_count - 1].cost == -1);
    constexpr Effect make_evolved{.operation=Operation::SummonEvolved, .amount=1, .card=kTrainer};
    resolve_effect_steps(s.state, 0, std::array{make_evolved});
    CHECK(p.faith_values[0] == 10 && p.hand_modifiers[p.hand_count - 1].cost == -2);
    s.run({Play, 0});
    CHECK(p.faith_values[0] == 0 && p.faith_upgrades[0] == 1 && p.hand[p.hand_count - 1] == official(90014330));
    s.run({Play, static_cast<std::int8_t>(p.hand_count - 1), kOwnBoardTarget + 2});
    CHECK(p.faith_values[0] == 1 && s.player(1).leader_hp == 19 &&
          p.board[2].evolution == Evolution::Normal && p.hand_modifiers[p.hand_count - 1].cost == -3);
    s.next_own_turn();
    s.run({Play, 0});
    CHECK(p.hand[p.hand_count - 1] == official(90024320));
    s.run({Play, 1});
    CHECK(p.faith_values[1] == 5);
    s.run({Evolve, 3});
    CHECK(p.faith_values[1] == 0 && p.faith_upgrades[1] == 1);
    const int before = p.board[0].atk;
    s.hand({official(10522310)});
    p.pp = 4;
    s.run({Play, 0});
    CHECK(p.faith_values[1] == 1 && p.board[0].atk == before + 1);
}

void elf_royal_combos_and_modes() {
    Scenario combo;
    combo.hand({official(10011120), official(10912310), official(10714110), official(10712120)});
    combo.place(kCorrector, 1);
    combo.run({Play, 0});
    CHECK(combo.player().plays_this_turn == 2);
    combo.reject({Play, 0, kNoTarget, 1});
    combo.run({Play, 0, kNoTarget, kAllModes});
    CHECK(combo.player().hand[2] == official(90011310) && combo.player().hand[3] == official(90011110) &&
          combo.player(1).board[0].hp == 1);
    combo.run({Play, 0, 0});
    CHECK(combo.player().plays_this_turn == 5 && combo.player(1).board_count == 0);
    combo.player().pp = 5;
    combo.run({Play, 0});
    CHECK(combo.player().plays_this_turn == 7);
    combo.run({Evolve, 2});
    CHECK(combo.player().plays_this_turn == 8);

    Scenario damage;
    damage.hand({official(10511310), official(10914110)});
    damage.place(kLilium, 1);
    damage.place(kWingedStatue, 1);
    damage.place(kCorrector, 1);
    damage.run({Play, 0});
    CHECK(damage.player(1).board_count == 2 && damage.player(1).board[1].hp == 3);
    damage.player().plays_this_turn = 2;
    damage.run({Play, 0});
    CHECK(damage.player(1).board_count == 1 && damage.player(1).board[0].card_id == kWingedStatue);

    Scenario mode;
    mode.hand({official(10514120), official(10623310), official(10823310)});
    mode.place(kCorrector, 1);
    mode.run({Play, 0, kNoTarget, 2});
    CHECK(mode.player().pp == 9);
    mode.run({Evolve, 0, kNoTarget, 3});
    CHECK(mode.player(1).board[0].hp == 2);
    mode.run({Play, 0, kNoTarget, kAllModes});
    CHECK(mode.player().pp == 6 && mode.player(1).board_count == 0);
    mode.place(kTrainer);
    mode.player().leader_hp = 14;
    mode.run({Play, 0, kNoTarget, kAllModes});
    CHECK(mode.player().leader_hp == 16);

    for (const int pp : {6, 7, 8, 10}) {
        Scenario noel;
        auto& p = noel.player();
        p.pp = static_cast<std::uint8_t>(pp);
        noel.hand({official(10624110)});
        noel.run({Play, 0});
        CHECK(p.board_count == (pp >= 8 ? 4 : pp >= 7 ? 3 : 2) && (p.board[1].abilities & kBane));
        if (pp >= 7) CHECK(p.board[2].abilities & kDrain);
        if (pp >= 8) CHECK(p.board[3].abilities & FLAG_STORM);
        CHECK(p.pp == (pp == 10 ? 2 : 0));
    }

    Scenario rally;
    auto& p = rally.player();
    p.followers_entered = 19;
    rally.hand({official(10724110), official(10722310)});
    rally.run({Play, 0});
    CHECK(p.crest_count == 1 && p.crests[0].id == CrestId::Gildaria && p.board[0].evolution == Evolution::Normal);
    CHECK(p.followers_entered == 22 && p.board_count == 3 &&
          (p.board[1].abilities & FLAG_RUSH) && rally.player(1).leader_hp == 18);
    rally.run({Play, 0});
    CHECK(p.hand_count == 2 && p.hand[0] == official(90021120) &&
          (p.hand_modifiers[0].abilities & FLAG_RUSH) && p.hand[1] == p.hand[0]);
}

void elf_royal_hand_and_copies() {
    Scenario evidence;
    const auto seller = official(11021110);
    const auto designer = official(11022110);
    const auto collector = official(11023110);
    auto& p = evidence.player();
    evidence.place(kEvidence).clue_count = 2;
    evidence.place(kEvidence, 1);
    evidence.hand({seller, designer, collector, official(11022310)});
    evidence.place(kCorrector, 1);
    evidence.run({Act, 0});
    CHECK(p.acted_evidence && p.hand_modifiers[0].attack == 0 && p.hand_modifiers[0].defense == 1 &&
          p.hand_modifiers[1].attack == 4 && p.hand_modifiers[1].defense == -3 &&
          p.hand_modifiers[2].attack == -3 && p.hand_modifiers[2].defense == 4);
    evidence.run({Play, 0});
    CHECK(p.board[1].card_id == seller && p.board[2].card_id == seller &&
          p.board[1].atk == 2 && p.board[1].hp == 2 && p.board[2].hp == 2);
    evidence.run({Play, 2, 1});
    CHECK(evidence.player(1).board_count == 1 && evidence.player(1).board[0].card_id == kEvidence);
    evidence.next_own_turn();
    CHECK(!p.acted_evidence);

    Scenario costs;
    costs.hand({official(10913310), official(10721110)});
    auto& side = costs.player();
    side.plays_this_turn = 3;
    const auto next_cost = official(10522110);
    side.deck[side.deck_idx] = next_cost;
    costs.place(kTrainer);
    costs.run({SuperEvolve, 0});
    CHECK(card_cost(*find_card(side.hand[1]), side.hand_modifiers[1]) == 1);
    costs.next_own_turn();
    CHECK(side.hand_modifiers[0].cost == -1 && side.hand[2] == next_cost &&
          card_cost(*find_card(next_cost), side.hand_modifiers[2]) == 3);
    costs.next_own_turn();
    CHECK(card_cost(*find_card(next_cost), side.hand_modifiers[2]) == 6);

    Scenario copies;
    const auto low = official(10514110);
    const auto spa = official(10912120);
    copies.hand({low, kEvidence, kTrainer});
    copies.player().plays_this_turn = 2;
    copies.run({Play, 0});
    CHECK(copies.player().board[0].atk == 4 && copies.player().board[0].hp == 4);
    auto& other = copies.player(1);
    copies.hand({kColette}, 1);
    other.hand_modifiers[0].cost = -1;
    other.deck_end = other.deck_idx + 3;
    for (int i = other.deck_idx; i < other.deck_end; ++i) {
        other.deck[i] = spa;
        other.deck_modifiers[i].attack = 2;
    }
    copies.run({Evolve, 0});
    CHECK(copies.player().hand_count == 3 && copies.player().shadows == 2 && other.deck_remaining() == 3);
    CHECK(copies.player().hand[0] == spa && copies.player().hand_modifiers[0].attack == 2);
    copies.run({Play, 0});
    CHECK(copies.player().hand[2] == kColette && copies.player().hand_modifiers[2].cost == -1 &&
          other.hand_count == 1 && other.deck_remaining() == 3);

    Scenario mind;
    const auto shift = official(10711310);
    mind.hand({kTrainer, shift, kExias});
    mind.reject({Play, 1, kNoTarget, 0, 0b001, true});
    mind.run({Play, 1, kNoTarget, 0, 0b101, true});
    CHECK(mind.player().hand_count == 2 && mind.player().shadows == 1);
    mind.hand({shift, kTrainer});
    const auto moves = legal_actions(mind.state);
    CHECK(std::none_of(moves.begin(), moves.end(),
                       [](Action action) { return action.type == Play && action.src == 0; }));
}

void elf_royal_crests() {
    using enum CrestId;
    for (const auto [id, official_id, cd] : std::array<std::tuple<CrestId, std::uint32_t, int>, 9>{{
             {Shrink, 10712312, 1}, {Expand, 10713312, 1}, {Dustdays, 10714112, 3}, {DeerKing, 10714122, 3},
             {Beasts, 11012312, 2}, {Unkei, 10524122, 4}, {March, 10622312, 2},
             {Gildaria, 10724112, 1}, {Almerize, 11024122, 2}}}) {
        const auto* crest = find_crest(id);
        CHECK(crest && crest->official_id == official_id && crest->countdown == cd && !crest->name.empty());
    }
    Scenario elf;
    elf.hand({official(10712310), official(10713310), official(11012310)});
    elf.player().plays_this_turn = 2;
    elf.place(official(10714110));
    elf.place(official(10714120));
    elf.run({Play, 0});
    elf.run({Play, 0});
    elf.run({Play, 0});
    elf.run({Evolve, 0});
    CHECK(elf.player().crest_count == 4 && elf.player().crests[3].id == Dustdays);
    elf.next_own_turn();
    CHECK(elf.player().crest_count == 2 && elf.player().board[3].card_id == official(11012110));
    CHECK(elf.player().hand[0] == official(10713310) && elf.player().hand[1] == official(10712310));
    elf.run({SuperEvolve, 1});
    CHECK(elf.player().crests[2].id == DeerKing);
    elf.player().plays_this_turn = 3;
    elf.run({EndTurn});
    CHECK(elf.player().hand[elf.player().hand_count - 1] == official(90011310));
    CHECK(elf.player().deck_modifiers[elf.player().deck_idx].attack == 2);

    Scenario royal;
    royal.place(official(11024120));
    royal.place(official(10524120));
    royal.run({SuperEvolve, 0});
    CHECK(royal.player().crests[0].id == Almerize && royal.player().crests[0].countdown == 4);
    royal.next_own_turn();
    CHECK(royal.player().hand[0] == official(90021310) && royal.player().hand_modifiers[0].cost == -1);
    royal.run({SuperEvolve, 1});
    CHECK(royal.player().crests[1].id == Unkei);
    royal.next_own_turn();
    CHECK(royal.player().hand[2] == official(90021320) && royal.player().hand_modifiers[2].cost == -1 &&
          royal.player().hand[3] == official(90021350));
    royal.next_own_turn();
    CHECK(royal.player().hand[5] == official(90021310));

    Scenario march;
    march.hand({official(10622310), official(10621110)});
    march.player().pp = 3;
    march.run({Play, 0});
    CHECK(march.player().crests[0].id == March && march.player().crests[0].countdown == 4);
    march.next_own_turn();
    const int before = march.player().board_count;
    march.run({Play, 0});
    CHECK(march.player().board_count == before + 2 && march.player().board[march.player().board_count - 1].card_id == official(10621110));
}

void elf_royal_tokens() {
    constexpr std::array<std::uint32_t, 17> tokens{90011110,90011120,90011310,90014110,90014330,
        90021110,90021120,90021130,90021210,90021310,90021320,90021330,90021340,90021350,90022110,90024320,90024330};
    for (const auto id : tokens) {
        const auto internal = official(id);
        const auto& token = *find_card(internal);
        CHECK(!is_deck_card(internal) && !token.class_name.empty());
        Scenario s;
        s.hand({internal});
        s.place(kTrainer);
        s.place(kCorrector, 1);
        s.player().leader_hp = 12;
        const auto choices = legal_actions(s.state);
        const auto move = std::find_if(choices.begin(), choices.end(), [](Action a) { return a.type == Play; });
        CHECK(move != choices.end());
        s.run(*move);
        CHECK(s.state.outcome == Outcome::Ongoing);
        if (token.kind != Kind::Spell) CHECK(s.player().board[1].card_id == internal);
        else CHECK(s.player().shadows == 1);
    }
    Scenario flag;
    flag.place(official(90021210)).countdown = 1;
    flag.hand({official(90021310), official(90021320)});
    flag.run({Play, 0, kEnemyLeader});
    CHECK(flag.player(1).leader_hp == 17 && flag.player().board_count == 0 && flag.player().shadows == 2);
    flag.place(official(90011120));
    flag.run({EndTurn});
    CHECK(flag.player().board[0].evolution == Evolution::Normal);
}

int elf_royal_units() {
    try {
        elf_royal_card_cases();
        elf_royal_faith();
        elf_royal_combos_and_modes();
        elf_royal_hand_and_copies();
        elf_royal_crests();
        elf_royal_tokens();
        std::cout << "PASS elf_royal card cases=123 token cases=17 crests=9 faiths=2\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

void dragon_nightmare_cards() {
    CHECK(dragon_rotation().size() == 58 && nightmare_rotation().size() == 61);
    for (const auto group : {dragon_rotation(), nightmare_rotation()}) {
        for (const auto id : group) {
            const auto& card = *find_card(id);
            try {
                CHECK(is_deck_card(id) && official(card.official_id) == id && printed_card(id) == id);
                Deck deck{};
                deck[0] = id;
                auto others = std::views::filter(group, [id](CardId entry) { return entry != id; });
                std::copy_n(others.begin(), kDeckSize - 1, deck.begin() + 1);
                const auto fresh = new_game(deck, deck, card.official_id);
                CHECK(fresh.players[0].deck_end == kDeckSize && fresh.players[0].hand_count == 4);
                Scenario s;
                s.place(kWingedStatue);
                s.place(kTrainer);
                s.place(kLilium);
                s.place(kTrainer, 1);
                s.place(kLilium, 1);
                s.hand({id, kRapid, kPropagation, kEvidence});
                s.player().leader_hp = 12;
                s.player().shadows = 5;
                const auto choices = legal_actions(s.state);
                const auto action = std::find_if(choices.begin(), choices.end(), [](Action move) {
                    return move.type == Play && move.src == 0;
                });
                CHECK(action != choices.end());
                s.run(*action);
                CHECK(s.state.outcome == Outcome::Ongoing && s.player().plays_this_turn == 1);
                if (card.kind != Kind::Spell) {
                    CHECK(std::any_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                                      [id](const auto& object) { return object.card_id == id; }));
                }
                std::cout << "PASS dragon_nightmare_card " << card.official_id << '\n';
            } catch (const std::exception& error) {
                throw std::runtime_error(std::to_string(card.official_id) + " " + std::string(card.name) + ": " + error.what());
            }
        }
    }
}

void dragon_nightmare_rulings() {
    Scenario miltio;
    const auto soldier = official(10621110);
    miltio.crest(CrestId::Miltio, 0);
    miltio.hand({soldier, official(10642110), kTrainer});
    miltio.player().pp = 4;
    miltio.run({Play, 0});
    CHECK(miltio.player().pp == 2 && miltio.player().board[0].evolution == Evolution::Normal &&
          miltio.player().board[0].atk == 4 && miltio.player().board[0].hp == 4);
    miltio.player().pp = 10;
    miltio.reject({Play, 0, kOwnHandTarget + 1});
    miltio.run({Play, 0});
    CHECK(miltio.player().hand_count == 1 && miltio.player().hand[0] == kTrainer &&
          miltio.player().board[1].evolution == Evolution::Normal);
    const auto cannon = official(10941310);
    miltio.place(kCorrector, 1);
    miltio.hand({cannon});
    miltio.run({Play, 0});
    CHECK(miltio.player(1).board[0].hp == 2); // Spell Enhance still works.

    Scenario gremory;
    const auto card = official(11054120);
    const auto target = gremory.place(card).creation_order;
    gremory.player().shadows = 1;
    constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
    resolve_effect_steps(gremory.state, 0, std::array{destroy}, target);
    CHECK(gremory.player().board_count == 0 && gremory.player().shadows == 0 && gremory.player().hand_count == 1);
    gremory.place(card);
    gremory.run({Evolve, 0});
    CHECK(gremory.player().crests[0].id == CrestId::Gremory);
    gremory.run({EndTurn});
    CHECK(gremory.player().shadows == 2);

    Scenario drake;
    auto& p = drake.player();
    drake.hand({official(10952110)});
    p.pp = 2;
    drake.run({Play, 0});
    const auto crystal = official(10952112);
    CHECK(p.board[0].card_id == crystal && p.board[0].countdown == 4 &&
          p.pp == 0 && !is_deck_card(crystal) && printed_card(crystal) == official(10952110));
    for (int count = 3; count >= 1; --count) {
        drake.next_own_turn();
        CHECK(p.board[0].card_id == crystal && p.board[0].countdown == count);
    }
    drake.next_own_turn();
    CHECK(p.board[0].card_id == official(10952110) && (p.board[0].abilities & FLAG_WARD));
}

void dragon_discard_and_deck_changes() {
    const auto val = official(10644120);
    Scenario discard;
    const auto red = official(10642310);
    discard.hand({val, red});
    discard.place(kTrainer, 1);
    discard.reject({Play, 1, 0});
    discard.run({Play, 1, 0, 0, 0, false, false, 0});
    CHECK(discard.player().shadows == 2 && discard.player().board[0].card_id == val &&
          discard.player(1).board_count == 0);
    discard.run({SuperEvolve, 0});
    const auto abyss = official(90044330);
    CHECK(discard.player().hand_count == 3 &&
          std::all_of(discard.player().hand.begin(), discard.player().hand.begin() + 3, [abyss](auto id) { return id == abyss; }));
    discard.player().leader_hp = 14;
    discard.hand({official(10641110), abyss});
    discard.run({Play, 0, kOwnHandTarget + 1});
    CHECK(discard.player().leader_hp == 15 && discard.player(1).leader_hp == 19);

    Scenario blades;
    const auto giver = official(10641310);
    const auto knife = official(10643310);
    const auto warrior = official(10642110);
    auto& p = blades.player();
    for (const auto [source, cost, expected] : std::array<std::tuple<CardId,int,int>, 3>{{{giver,4,2},{knife,7,5},{knife,5,3}}}) {
        blades.hand({warrior, source});
        p.hand_modifiers[1].cost = static_cast<std::int8_t>(cost - find_card(source)->cost);
        p.pp = 10;
        blades.run({Play, 0, kOwnHandTarget + 1});
        CHECK(p.hand_count == 1 && p.hand[0] == source && card_cost(*find_card(source), p.hand_modifiers[0]) == expected);
    }

    Scenario transform;
    auto& side = transform.player();
    const auto glance = official(10741310);
    const auto sleep = official(10742310);
    transform.hand({glance, glance, kRapid});
    side.deck[side.deck_idx] = glance;
    side.deck_modifiers[side.deck_idx].cost = -2;
    side.hand_modifiers[1].cost = -1;
    side.max_pp = side.pp = 9;
    transform.run({Play, 0});
    CHECK(side.max_pp == 10 && side.hand[0] == sleep && side.deck[side.deck_idx] == sleep &&
          side.hand_modifiers[0].cost == 0 && side.deck_modifiers[side.deck_idx].cost == 0);

    Scenario exile;
    auto& deck = exile.player();
    deck.deck_idx = 0;
    deck.deck_end = 4;
    deck.deck = {kWingedStatue, kLilium, kTrainer, kRapid};
    exile.place(official(10543110));
    exile.place(kLilium, 1);
    exile.place(kCorrector, 1);
    exile.run({SuperEvolve, 0});
    CHECK(deck.deck_remaining() == 2 && deck.deck[0] == kLilium && deck.deck[1] == kRapid && deck.shadows == 0);
    CHECK(exile.player(1).board_count == 1 && exile.player(1).board[0].hp == 4);
}

void dragon_attack_modes_and_crests() {
    Scenario previous;
    auto& p = previous.player();
    const auto thief = official(10941110);
    previous.place(kHolyFalcon);
    previous.run({Attack, 0, kEnemyLeader});
    CHECK(p.attacked_leader_this_turn && !p.attacked_leader_last_turn);
    previous.next_own_turn();
    CHECK(!p.attacked_leader_this_turn && p.attacked_leader_last_turn);
    previous.hand({thief, official(10944110)});
    previous.run({Play, 0});
    CHECK(p.board[1].abilities & FLAG_STORM);
    previous.place(kColette, 1);
    previous.run({Play, 0});
    CHECK(p.board[2].abilities & FLAG_STORM);
    CHECK(previous.legal({Attack, 2, kEnemyLeader}) && !previous.legal({Attack, 1, kEnemyLeader}));

    Scenario modes;
    const auto four_modes = official(11043310);
    modes.hand({four_modes});
    modes.place(kCorrector, 1);
    const auto choices = legal_actions(modes.state);
    CHECK(std::count_if(choices.begin(), choices.end(), [](Action action) { return action.type == Play; }) == 6);
    modes.run({Play, 0, kNoTarget, kModeMask | 0b1100});
    CHECK(modes.player().hand_count == 3 && modes.player().hand[0] == kEvidence &&
          modes.player(1).board[0].hp == 1 && modes.player().board_count == 0);

    Scenario two_targets;
    two_targets.place(official(10541110));
    two_targets.place(kCorrector, 1);
    two_targets.place(kTrainer, 1);
    two_targets.place(kLilium, 1);
    two_targets.reject({SuperEvolve, 0, 0});
    two_targets.run({SuperEvolve, 0, kNoTarget, 0, 0b110, false, true});
    CHECK(two_targets.player(1).board_count == 1 && two_targets.player(1).board[0].card_id == kCorrector);

    Scenario waves;
    const auto orca = official(90041130);
    waves.crest(CrestId::Yube, 0);
    waves.crest(CrestId::Wadatsumi, 0);
    waves.hand({orca, orca});
    waves.run({Play, 0});
    waves.run({Play, 0});
    CHECK(waves.player().board[0].atk == 3 && waves.player().board[1].hp == 3);
    waves.place(kCorrector, 1).atk = 0;
    waves.run({Attack, 0, 0});
    waves.run({Attack, 1, 0});
    CHECK(waves.player().hand_count == 1 && waves.player().hand[0] == orca &&
          waves.player().board[0].atk == 4 && waves.player().board[1].atk == 4);
    waves.run({EndTurn});
    CHECK(waves.player().board[0].atk == 3 && waves.player().board[1].atk == 3);

    Scenario reflection;
    const auto duo = official(10844110);
    for (int i = 0; i < 3; ++i) {
        reflection.hand({duo});
        reflection.player().pp = 10;
        reflection.run({Play, 0});
        CHECK(reflection.player().board[i].atk == 4 + i + (i == 2 ? 2 : 0));
    }
    const auto target = reflection.player().board[0].creation_order;
    constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
    resolve_effect_steps(reflection.state, 0, std::array{destroy}, target);
    CHECK(reflection.player().crests[0].id == CrestId::Reflection && reflection.player().crests[0].countdown == 2);
    reflection.next_own_turn();
    reflection.next_own_turn();
    CHECK(reflection.player().hand[1] == duo && reflection.player().hand_modifiers[1].cost == -2);
}

void nightmare_banish_last_words_and_reanimate() {
    const auto ghost = official(90051130);
    const auto skeleton = official(90051110);
    constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
    Scenario dead;
    dead.place(official(11052110));
    dead.place(official(11054110));
    dead.place(official(11053110));
    dead.place(skeleton);
    const auto spirit = dead.place(ghost).creation_order;
    dead.player().leader_hp = 12;
    resolve_effect_steps(dead.state, 0, std::array{destroy}, spirit);
    CHECK(dead.player().shadows == 0 && dead.player().leader_hp == 14 && dead.player().board_count == 5 &&
          dead.player().board[3].atk == 3 && dead.player().board[4].card_id == skeleton &&
          dead.player().board[4].hp == 5 && (dead.player().board[4].abilities & FLAG_WARD));
    Scenario bounce;
    const auto ghost_target = bounce.place(ghost).creation_order;
    constexpr Effect bounce_self{.operation=Operation::Bounce, .target=Target::Self};
    resolve_effect_steps(bounce.state, 0, std::array{bounce_self}, ghost_target);
    CHECK(bounce.player().hand_count == 0 && bounce.player().board_count == 0 && bounce.player().shadows == 0);
    bounce.place(ghost);
    bounce.run({EndTurn});
    CHECK(bounce.player().board_count == 0 && bounce.player().shadows == 0);

    for (const auto id : {official(10951120), official(90051140)}) {
        Scenario lost;
        const auto body = lost.place(id).creation_order;
        resolve_effect_steps(lost.state, 0, std::array{destroy}, body);
        CHECK(lost.player().board_count == 1 && lost.player().board[0].card_id == id &&
              (lost.player().board[0].modifiers.effect_flags & kLoseLastWords));
        resolve_effect_steps(lost.state, 0, std::array{destroy}, lost.player().board[0].creation_order);
        CHECK(lost.player().board_count == 0 && lost.player().shadows == 2);
    }
    Scenario grant;
    const auto victim = grant.place(kTrainer).creation_order;
    grant.hand({official(10953310)});
    grant.run({Play, 0, kOwnBoardTarget});
    resolve_effect_steps(grant.state, 0, std::array{destroy}, victim);
    CHECK(grant.player().board_count == 1 && grant.player().board[0].card_id == kTrainer &&
          !(grant.player().board[0].modifiers.effect_flags & kRebirthLastWords));

    Scenario reanimation;
    const auto old_one = reanimation.place(kExias).creation_order;
    const auto old_two = reanimation.place(kLilium).creation_order;
    resolve_effect_steps(reanimation.state, 0, std::array{destroy}, old_one);
    resolve_effect_steps(reanimation.state, 0, std::array{destroy}, old_two);
    reanimation.hand({official(10954110)});
    reanimation.run({Play, 0});
    CHECK(reanimation.player().board_count == 4);
    CHECK(std::all_of(reanimation.player().board.begin() + 1, reanimation.player().board.begin() + 4,
        [](const auto& follower) { return follower.modifiers.effect_flags & kDeadTrait; }));
    reanimation.run({SuperEvolve, 0});
    CHECK(reanimation.player().crests[0].id == CrestId::IstenDead);
    reanimation.place(kLilium);
    reanimation.place(kTrainer, 1);
    reanimation.run({EndTurn});
    CHECK(reanimation.player(1).board_count == 0);

    Scenario mixed_cost;
    const auto expensive = mixed_cost.place(kTrainer).creation_order;
    const auto cheap = mixed_cost.place(kLilium).creation_order;
    resolve_effect_steps(mixed_cost.state, 0, std::array{destroy}, expensive);
    resolve_effect_steps(mixed_cost.state, 0, std::array{destroy}, cheap);
    mixed_cost.hand({official(10554110)});
    mixed_cost.run({Play, 0});
    CHECK(mixed_cost.player().board_count == 3 && mixed_cost.player().board[1].card_id == kTrainer &&
          mixed_cost.player().board[2].card_id == kLilium);

    Scenario steal;
    steal.place(kTrainer, 1).hp = 1;
    steal.hand({official(10652310)});
    steal.run({Play, 0, 0});
    CHECK(steal.player(1).board_count == 0 && steal.player(1).shadows == 0 &&
          steal.player().board[0].card_id == kTrainer && steal.player().board[0].hp == 1);
}

void dragon_nightmare_extras() {
    for (const auto [id, code, cd] : std::array<std::tuple<CrestId, std::uint32_t, int>, 7>{{
          {CrestId::Yube,10544122,0},{CrestId::Wadatsumi,10841132,0},{CrestId::Reflection,10844112,2},
          {CrestId::Okuyoka,10553312,2},{CrestId::Miltio,10554112,0},{CrestId::IstenDead,10954112,0},{CrestId::Gremory,11054122,0}}}) {
        const auto* crest = find_crest(id);
        CHECK(crest && crest->official_id == code && crest->countdown == cd);
    }
    Scenario flower;
    flower.hand({official(10553310), kTrainer, kTrainer, kTrainer, kTrainer});
    flower.run({Play, 0});
    flower.run({EndTurn});
    CHECK(flower.player().board[0].card_id == official(90051110) && flower.player().hand_count == 5 &&
          (flower.player().board[0].abilities & FLAG_WARD));
    for (const auto code : {90041110U,90041120U,90041130U,90044330U,90051110U,90051120U,90051130U,90051140U,90054330U}) {
        const auto id = official(code);
        CHECK(!is_deck_card(id));
        Scenario s;
        s.hand({id});
        s.run({Play, 0});
        const auto& card = *find_card(id);
        if (card.kind == Kind::Spell) CHECK(s.player().shadows == 1);
        else CHECK(s.player().board[0].card_id == id && s.player().board[0].hp == card.hp);
    }
}

int dragon_nightmare_units() {
    try {
        dragon_nightmare_cards();
        dragon_nightmare_rulings();
        dragon_discard_and_deck_changes();
        dragon_attack_modes_and_crests();
        nightmare_banish_last_words_and_reanimate();
        dragon_nightmare_extras();
        std::cout << "PASS dragon_nightmare card cases=119 token cases=9 crests=7 crystallize=1 official_QA=2\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

void witch_nemesis_cards() {
    CHECK(witch_rotation().size() == 62 && nemesis_rotation().size() == 64);
    for (const auto group : {witch_rotation(), nemesis_rotation()}) {
        for (const auto id : group) {
            const auto& card = *find_card(id);
            try {
                CHECK(is_deck_card(id) && official(card.official_id) == id && printed_card(id) == id);
                Deck deck{};
                deck[0] = id;
                auto others = std::views::filter(group, [id](CardId entry) { return entry != id; });
                std::copy_n(others.begin(), kDeckSize - 1, deck.begin() + 1);
                const auto fresh = new_game(deck, deck, card.official_id);
                CHECK(fresh.players[0].deck_end == kDeckSize && fresh.players[0].hand_count == 4);
                Scenario s;
                s.place(kEarthSigil).earth_count = 3;
                s.place(official(10631110));
                s.place(kTrainer);
                s.place(kCorrector, 1);
                s.place(kLilium, 1);
                s.hand({id, kRapid, kPropagation, kEvidence});
                s.player().leader_hp = 12;
                const auto choices = legal_actions(s.state);
                const auto action = std::find_if(choices.begin(), choices.end(), [](Action move) {
                    return move.type == Play && move.src == 0;
                });
                CHECK(action != choices.end());
                s.run(*action);
                CHECK(s.state.outcome == Outcome::Ongoing && s.player().plays_this_turn == 1);
                if (card.kind != Kind::Spell) {
                    CHECK(std::any_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                                      [id](const auto& object) { return object.card_id == id; }));
                }
                std::cout << "PASS witch_nemesis_card " << card.official_id << '\n';
            } catch (const std::exception& error) {
                throw std::runtime_error(std::to_string(card.official_id) + " " + std::string(card.name) + ": " + error.what());
            }
        }
    }
}

void witch_costs_and_transformation() {
    const auto initial = official(10131320);
    const auto reprint = official(10831310);
    const auto insight = official(10031310);
    const auto sammy = official(10832110);
    CHECK(initial != reprint && find_card(initial)->name == find_card(reprint)->name);
    for (const auto storm : {initial, reprint}) {
        Scenario s;
        s.place(kCorrector, 1);
        s.hand({insight, storm, sammy});
        s.run({Play, 0});
        CHECK(s.player().hand_modifiers[0].spellboost == 1 && s.player().hand_modifiers[1].spellboost == 1 &&
              card_cost(*find_card(sammy), s.player().hand_modifiers[1]) == find_card(sammy)->cost - 1);
        s.run({Play, 0, 0});
        CHECK(s.player(1).board[0].hp == 2);
    }
    {
        Scenario s;
        constexpr Effect earth{.operation=Operation::AddEarth, .amount=2};
        resolve_effect_steps(s.state, 0, std::array{earth});
        s.hand({official(10731110), official(10731310), official(10733310)});
        s.run({Act, 0});
        CHECK(s.player().board[0].earth_count == 3 && s.player().acted_amulet);
        s.run({Play, 0});
        CHECK(s.player().board[0].earth_count == 2 && s.player().board[1].evolution == Evolution::Normal);
        CHECK(s.player().hand_modifiers[0].cost == -1 && s.player().hand_modifiers[1].cost == -1);
    }
    {
        Scenario s;
        auto& p = s.player();
        const auto target = s.place(kTrainer).creation_order;
        s.place(kLilium);
        s.run({Attack, 0, kEnemyLeader});
        p.deck_idx = 0;
        p.deck_end = 2;
        p.deck = {kRapid, insight};
        p.deck_modifiers[0].attack = 2;
        p.deck_modifiers[0].abilities = FLAG_STORM;
        s.hand({official(10533310)});
        s.run({Play, 0});
        CHECK(p.board[0].creation_order == target && p.board[0].card_id == kRapid && p.board[1].card_id == kRapid &&
              p.board[0].atk == find_card(kRapid)->atk + 2 && p.board[0].evolution == Evolution::None);
        CHECK(p.deck_remaining() == 2 && !p.board[0].attacked_this_turn && s.legal({Attack, 0, kEnemyLeader}));
        p.pp = 10;
        p.deck_idx = 0;
        p.deck_end = 0;
        s.hand({official(10932120), kTrainer, kLilium});
        s.run({Play, 0});
        CHECK(p.hand_count == 2 && p.deck_remaining() == 0 && s.state.outcome == Outcome::Ongoing);
    }
    {
        Scenario s;
        s.place(official(11032110));
        s.place(kEarthSigil).earth_count = 1;
        s.hand({official(11032110)});
        s.player().leader_hp = 12;
        s.run({Act, 1});
        CHECK(s.player().leader_hp == 13 && s.player().hand_modifiers[0].cost == -1);
    }
}

void artifact_fusion_chain() {
    const auto future = official(90071210), past = official(90071220);
    const auto attack = official(90072110), castle = official(90072120);
    const auto alpha = official(90073110), beta = official(90073120), gamma = official(90073130);
    const auto omega = official(90074110);
    const auto analyze = official(90071130), mystic = official(90071150);
    for (const auto [core, donor, form] : std::array<std::tuple<CardId, CardId, CardId>, 2>{{
             {future, past, attack}, {past, future, castle}}}) {
        for (const auto [cost, token] : std::array<std::pair<int, CardId>, 3>{{{1, alpha}, {2, beta}, {3, gamma}}}) {
            Scenario s;
            auto& p = s.player();
            s.hand({core, donor, analyze, analyze, mystic});
            s.reject({Play, 0});
            s.reject({Fuse, 0, kNoTarget, 2}); // An AF follower may not be fed directly to a Core.
            s.run({Fuse, 0, kNoTarget, 1});
            CHECK(p.hand[0] == form && p.hand_count == 4 && p.shadows == 0);
            const auto materials = static_cast<std::uint8_t>(cost == 1 ? 1 : cost == 2 ? 3 : 4);
            s.run({Fuse, 0, kNoTarget, materials});
            CHECK(p.hand[0] == token && p.shadows == 0 && p.pp == 10);
        }
    }
    {
        Scenario s;
        s.hand({alpha, beta, beta, gamma});
        s.run({Fuse, 0, kNoTarget, 3});
        CHECK(s.player().hand[0] == alpha && s.player().hand[1] == gamma &&
              (s.player().hand_modifiers[0].effect_flags & kFusedBeta));
        s.reject({Fuse, 0, kNoTarget, 1});
        s.next_own_turn();
        s.run({Fuse, 0, kNoTarget, 1});
        CHECK(s.player().hand[0] == omega && s.player().shadows == 0);
    }
    {
        Scenario s;
        s.rules.refusion_after_hand_transform = false;
        s.hand({future, past, analyze});
        s.run({Fuse, 0, kNoTarget, 1});
        s.reject({Fuse, 0, kNoTarget, 1});
        s.next_own_turn();
        s.run({Fuse, 0, kNoTarget, 1});
        CHECK(s.player().hand[0] == alpha);
    }
    {
        Scenario s;
        s.hand({official(10934110), kEvidence, kTrainer});
        s.player().pp = 2;
        s.crest(CrestId::Sephie, 0);
        s.run({Fuse, 0, kNoTarget, 1});
        CHECK(s.player().pp == 0 && s.player().board_count == 1 &&
              s.player().board[0].card_id == official(10931110) && (s.player().board[0].abilities & FLAG_STORM));
        s.player().pp = 10;
        s.run({Play, 0});
        CHECK(s.player().board_count == 4 && !(s.player().board[2].abilities & FLAG_STORM) &&
              !(s.player().board[3].abilities & FLAG_STORM));
    }
    {
        Scenario s;
        s.hand({official(10934110), kTrainer});
        s.player().pp = 1;
        s.rules.fusion_requires_trigger_pp = true;
        s.reject({Fuse, 0, kNoTarget, 1});
        s.rules.fusion_requires_trigger_pp = false;
        s.run({Fuse, 0, kNoTarget, 1});
        CHECK(s.player().hand_count == 1 && s.player().board_count == 0 && s.player().pp == 1 && s.player().shadows == 0);
    }
}

void nemesis_artifact_and_highlander() {
    const auto analyze = official(90071130), ancient = official(90071140), mystic = official(90071150);
    {
        Scenario s;
        auto& p = s.player();
        s.hand({analyze, analyze, ancient, mystic, official(10771310), official(10773310)});
        s.place(kCorrector, 1);
        s.run({Play, 0});
        s.run({Play, 0});
        CHECK(p.artifact_types_entered == 1);
        s.run({Play, 0});
        s.run({Play, 0});
        CHECK(p.artifact_types_entered == 3);
        s.reject({Play, 0, kNoTarget, 1});
        s.run({Play, 0, kNoTarget, kAllModes});
        CHECK(std::find(p.hand.begin(), p.hand.begin() + p.hand_count, ancient) != p.hand.begin() + p.hand_count);
        p.pp = 10;
        s.run({Play, 0});
        CHECK(s.player(1).board[0].hp == 2 && s.player(1).leader_hp == 19);
    }
    {
        Scenario s;
        auto& p = s.player();
        p.deck_idx = 0;
        p.deck_end = 2;
        p.deck = {official(10131320), official(10831310)};
        s.hand({official(10973310)});
        s.place(kCorrector, 1);
        s.place(kTrainer, 1);
        s.reject({Play, 0});
        s.rules.highlander_counts_card_names = false;
        s.reject({Play, 0, 0});
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 0);
    }
    {
        Scenario s;
        auto& p = s.player();
        p.pp = 9;
        s.hand({official(10874110)});
        s.place(kCorrector, 1);
        s.place(kTrainer, 1);
        s.run({Play, 0, 0});
        CHECK(p.board[0].evolution == Evolution::Normal && (p.board[0].abilities & FLAG_STORM) &&
              s.player(1).board_count == 1 && s.player(1).board[0].card_id == kTrainer);
    }
    {
        Scenario s;
        const auto containment = official(11073310), arm = official(11072310);
        s.place(official(11072110));
        s.place(kCorrector, 1);
        s.hand({containment, kTrainer, kEvidence, arm, kEvidence});
        s.reject({Fuse, 0, kNoTarget, 1});
        s.run({Fuse, 0, kNoTarget, 2});
        CHECK(s.player().board[1].clue_count == 1 && s.player(1).board[0].hp == 4);
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 1 && s.player().board[1].clue_count == 1);
        s.run({Fuse, 1, kNoTarget, 2});
        CHECK(s.player(1).board[0].hp == 3 && s.player().board[1].clue_count == 2);
        s.run({Play, 1});
        CHECK(s.player(1).board_count == 0);
    }
}

void witch_nemesis_crests() {
    using enum CrestId;
    for (const auto [id, code, cd] : std::array<std::tuple<CrestId, std::uint32_t, int>, 11>{{
             {Insomnia,10532112,2},{Lincle,10534112,0},{Shim,10634112,0},{Tico,10833112,0},{Sephie,10934112,0},
             {NoinInterrogation,11033312,0},{Noin,11034112,0},{Lala,10734112,1},{Sloth,10574112,3},
             {Cutthroat,10974112,0},{Stranger,11074122,2}}}) {
        const auto* crest = find_crest(id);
        CHECK(crest && crest->official_id == code && crest->countdown == cd);
    }
    {
        Scenario s;
        s.hand({official(10534110), official(10534110), official(10534110)});
        s.run({Play, 0});
        s.run({Play, 0});
        s.run({Play, 0});
        CHECK(s.player(1).leader_max_hp == 16 && s.player(1).leader_hp == 16);
        const auto before = s.player().deck_remaining();
        s.run({SuperEvolve, 0});
        CHECK(s.player().deck_remaining() == before + 10);
    }
    {
        Scenario s;
        s.place(official(10532110));
        s.place(kTrainer, 1);
        s.crest(Insomnia, 2);
        s.run({Evolve, 0});
        CHECK(s.player(1).board_count == 0 && s.player().board[0].hp == 2 && s.player().crest_count == 0);
    }
    {
        Scenario s;
        s.crest(NoinInterrogation, 0);
        s.crest(Noin, 0);
        s.place(kEarthSigil).earth_count = 1;
        s.place(kCorrector, 1);
        s.player().leader_hp = 12;
        s.run({Act, 0});
        CHECK(s.player(1).board[0].hp == 2 && s.player().pp == 10);
        s.next_own_turn();
        s.run({Act, 0});
        CHECK(s.player().leader_hp == 14);
        s.next_own_turn();
        s.run({Act, 0});
        CHECK(s.player().ep == 3 && s.player().crest_count == 1 && s.player().crests[0].id == Noin);
    }
    {
        Scenario s;
        const auto hand_of = official(10631110);
        s.crest(Shim, 0);
        s.crest(Tico, 0);
        s.place(hand_of);
        s.hand({official(90031310)});
        s.run({Attack, 0, kEnemyLeader});
        s.run({Play, 0});
        CHECK(s.player().board[0].atk == find_card(hand_of)->atk + 1 &&
              s.player(1).leader_hp == 20 - s.player().board[0].atk - 1);
    }
    for (const auto timing : {LalaSummonTiming::OpponentTurnEnd, LalaSummonTiming::OwnerTurnStart, LalaSummonTiming::Disabled}) {
        Scenario s;
        s.rules.lala_summon_timing = timing;
        s.crest(Lala, 1);
        s.run({EndTurn});
        CHECK(s.player().board_count == 0);
        s.run({EndTurn});
        CHECK(s.player().crest_count == 0);
        if (timing == LalaSummonTiming::Disabled) CHECK(s.player().board_count == 0);
        else CHECK(s.player().board[0].card_id == official(10734110) &&
                  s.player().board[0].evolution == Evolution::Normal &&
                  s.player().board[0].readied == (timing == LalaSummonTiming::OpponentTurnEnd));
    }
    {
        Scenario s;
        s.crest(Sloth, 3);
        s.place(kCorrector);
        s.hand({kTrainer});
        int effects_seen = 0;
        for (int turn = 0; turn < 3; ++turn) {
            s.next_own_turn();
            if (s.player().hand_modifiers[0].temporary_cost_delta == 1) ++effects_seen;
        }
        CHECK(effects_seen == 1 && s.player().crest_count == 0 && s.player().leader_hp == 17 && s.player().board[0].hp == 3);
    }
    {
        Scenario s;
        s.crest(Cutthroat, 0);
        s.crest(Stranger, 2);
        s.hand({kTrainer, kLilium, official(11073310), kEvidence, official(11072310), kEvidence});
        s.run({Play, 0});
        s.run({Play, 0});
        CHECK(s.player().board[0].evolution == Evolution::Normal && s.player().board[1].evolution == Evolution::None);
        s.run({Fuse, 0, kNoTarget, 1});
        CHECK(s.player().crests[1].countdown == 1);
        s.run({Fuse, 1, kNoTarget, 2});
        CHECK(s.player().crest_count == 1 && s.player().board[3].card_id == official(11074120));
    }
}

void witch_nemesis_tokens() {
    for (const auto [base, form_code, cost, count] : std::array<std::tuple<std::uint32_t, std::uint32_t, int, int>, 3>{{
             {10671110,10671112,2,1}, {10672110,10672112,3,2}, {10673110,10673112,1,1}}}) {
        Scenario s;
        const auto id = official(base);
        const auto form = official(form_code);
        CHECK(find_card(form)->kind == Kind::Spell && printed_card(form) == id && !is_deck_card(form));
        s.player().pp = static_cast<std::uint8_t>(cost);
        s.hand({id});
        s.run({Play, 0});
        CHECK(s.player().pp == 0 && s.player().hand_count == 0 && s.player().board_count == count &&
              s.player().shadows == 1);
        for (int slot = 0; slot < count; ++slot)
            CHECK(s.player().board[slot].card_id == id && s.player().board[slot].evolution == Evolution::None);
    }
    constexpr std::array<std::uint32_t, 24> tokens{
        90001210,90031110,90031120,90031210,90031310,90034340,90034350,90071110,90071120,
        90071130,90071140,90071150,90071160,90071210,90071220,90072110,90072120,90073110,
        90073120,90073130,90074110,90074140,90074150,90074320,
    };
    for (const auto code : tokens) {
        const auto id = official(code);
        CHECK(!is_deck_card(id));
        Scenario s;
        s.place(kRapid);
        s.hand({id});
        const auto actions = legal_actions(s.state);
        const auto play = std::find_if(actions.begin(), actions.end(), [](Action a) { return a.type == Play; });
        if (code == 90071210 || code == 90071220) CHECK(play == actions.end());
        else {
            CHECK(play != actions.end());
            s.run(*play);
            if (find_card(id)->kind == Kind::Follower) CHECK(s.player().board[1].card_id == id);
        }
    }
    for (const auto id : {official(90071110), official(90071120)}) {
        Scenario s;
        s.place(id);
        s.run({EndTurn});
        CHECK(s.player().board_count == 1);
        s.run({EndTurn});
        CHECK(s.player().board_count == 0 && s.player().shadows == 1);
    }
}

int witch_nemesis_units() {
    try {
        witch_nemesis_cards();
        witch_costs_and_transformation();
        artifact_fusion_chain();
        nemesis_artifact_and_highlander();
        witch_nemesis_crests();
        witch_nemesis_tokens();
        std::cout << "PASS witch_nemesis card cases=126 token cases=24 crests=11 accelerate=3 AF chain=verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

int verify_rotation_registry() {
    const auto pool = all_rotation();
    CHECK(pool.size() == 516 && high_cost_rotation().size() == 47);
    std::vector<std::uint32_t> ids;
    for (const auto card : pool) {
        const auto* def = find_card(card);
        CHECK(def && def->implemented && def->deck_eligible && def->may_play_from_hand && def->official_id != 0);
        CHECK(find_official_card(def->official_id) == def && is_deck_card(card));
        ids.push_back(def->official_id);
    }
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    std::cout << "PASS rotation_catalog implemented=" << pool.size() << " accepted=" << ids.size() << " missing=0\n";
    return 0;
}

void high_cost_cards() {
    for (const auto id : high_cost_rotation()) {
        const auto& card = *find_card(id);
        try {
            CHECK(card.cost >= 8 && card.cost <= 10 && is_deck_card(id) && official(card.official_id) == id);
            auto deck = kMirrorDeck;
            const auto replace = std::find_if(deck.begin(), deck.end(), [id](CardId entry) { return entry != id; });
            if (std::count(deck.begin(), deck.end(), id) < 3) *replace = id;
            CHECK(new_game(deck, deck, card.official_id).players[0].deck_end == kDeckSize);
            Scenario s;
            s.place(kTrainer, 1);
            s.hand({id, kTrainer, kLilium});
            const auto moves = legal_actions(s.state);
            const auto play = std::find_if(moves.begin(), moves.end(), [](Action move) {
                return move.type == Play && move.src == 0;
            });
            CHECK(play != moves.end());
            s.run(*play);
            CHECK(s.player().plays_this_turn == 1 && s.state.outcome == Outcome::Ongoing);
            CHECK(s.player().board[0].card_id == id);
            std::cout << "PASS high_cost_card " << card.official_id << '\n';
        } catch (const std::exception& error) {
            throw std::runtime_error(std::to_string(card.official_id) + " " + std::string(card.name) + ": " + error.what());
        }
    }
}

void high_cost_combat() {
    {
        Scenario s;
        s.hand({official(10924120)});
        s.run({Play, 0});
        for (int i = 0; i < 3; ++i) s.run({Attack, 0, kEnemyLeader});
        s.reject({Attack, 0, kEnemyLeader});
        CHECK(s.player(1).leader_hp == 14 && s.player().board[0].attacked_thrice);
        s.next_own_turn();
        CHECK(s.legal({Attack, 0, kEnemyLeader}) && !s.player().board[0].attacked_thrice);
        const auto order = s.player().board[0].creation_order;
        resolve_effect_steps(s.state, 0, std::array{Effect{.operation=Operation::Silence,
            .target=Target::ChosenOwnFollower}}, 0, order);
        s.run({Attack, 0, kEnemyLeader});
        s.reject({Attack, 0, kEnemyLeader});
    }
    {
        Scenario s;
        s.hand({official(10654110)});
        for (int i = 0; i < 3; ++i) s.place(kTrainer, 1);
        s.run({Play, 0});
        resolve_effect_steps(s.state, 1, std::array{Effect{.operation=Operation::Destroy,
            .target=Target::AllEnemyFollowers}});
        CHECK(s.player().board_count == 1 && s.player().board[0].hp == 10);
        s.run({SuperEvolve, 0});
        for (int i = 0; i < 3; ++i) s.run({Attack, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).leader_hp == 17);
        CHECK(s.player().board[0].hp == 13 && s.player().board[0].attacked_thrice);
    }
    {
        Scenario s;
        s.hand({official(10711110)});
        s.place(kTrainer, 1).atk = 8;
        s.run({Play, 0});
        CHECK(s.player().board_count == 2);
        s.run({Attack, 0, 0});
        CHECK(s.player().board[0].hp == 3 && s.player().board[1].hp == 6);
        resolve_effect_steps(s.state, 1, std::array{Effect{.operation=Operation::Damage,
            .target=Target::AllEnemyFollowers, .amount=9}});
        CHECK(s.player().board_count == 1 && s.player().board[0].hp == 3);
    }
}

void high_cost_leader_and_summon() {
    const auto zerael = official(10904110);
    {
        Scenario s;
        s.player().played_cost_mask = 0x0fe;
        s.player().deck = {zerael, zerael, kTrainer};
        s.player().deck_idx = 0;
        s.player().deck_end = 3;
        s.place(kCorrector, 1);
        s.hand({official(10862110)});
        s.player().hand_modifiers[0].cost = -3;
        s.run({Play, 0});
        CHECK(s.player().played_cost_mask == 0x1fe);
        s.run({EndTurn});
        CHECK(s.player().deck_remaining() == 2 && s.player().board[1].card_id == zerael);
        CHECK(s.player(1).board[0].hp == 5); // Direct summons do not fire Fanfare.
    }
    {
        Scenario s;
        s.player().played_cost_mask = 0x0fe;
        s.player().deck = {zerael};
        s.player().deck_idx = 0;
        s.player().deck_end = 1;
        s.run({EndTurn});
        CHECK(s.player().board_count == 0 && s.player().deck_remaining() == 1);
    }
    {
        Scenario s;
        s.player().played_cost_mask = 0x1fe;
        s.player().deck = {zerael};
        s.player().deck_idx = 0;
        s.player().deck_end = 1;
        for (int i = 0; i < 4; ++i) s.place(kTrainer);
        s.place(official(10903210));
        s.run({EndTurn});
        CHECK(s.player().board_count == 5 && s.player().deck_remaining() == 0);
        CHECK(s.player().board[4].card_id == zerael && s.player().board[4].atk == 12);
    }
    {
        Scenario s;
        s.hand({official(10804110)});
        s.crest(CrestId::Rituel, 1);
        s.crest(CrestId::Rapid, 2, 1);
        s.player().faiths[0] = official(10664122);
        s.player().faith_count = 1;
        s.run({Play, 0, kNoTarget, 3});
        CHECK(s.player().crest_count == 0 && s.player(1).crest_count == 0);
        CHECK(s.player().board_count == 1 && s.player().shadows == 0 && s.player().faith_count == 1);
    }
    {
        Scenario s;
        s.hand({official(10804120)});
        s.player().sep = 0;
        s.run({Play, 0});
        CHECK(s.player().sep == 2);
    }
    for (const bool round_up : {false, true}) {
        Scenario s;
        s.rules.round_half_max_hp_up = round_up;
        s.hand({official(11074110), kEvidence});
        s.place(kTrainer, 1);
        s.place(kLilium, 1);
        s.place(kTrainer, 1);
        s.player(1).leader_hp = 15;
        s.player(1).leader_max_hp = 17;
        s.run({Fuse, 0, kNoTarget, 1});
        CHECK(s.player().board[0].card_id == kEvidence && s.player().shadows == 0);
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].card_id == kLilium);
        CHECK(s.player(1).leader_max_hp == (round_up ? 9 : 8) && s.player(1).leader_hp == s.player(1).leader_max_hp);
    }
    {
        const auto omega = official(10604110);
        bool replayed = false;
        for (std::uint64_t seed = 0; seed < 64 && !replayed; ++seed) {
            Scenario limited;
            limited.rules.fanfare_repeat_limit = 0;
            limited.state.rng_state = seed;
            limited.hand({omega});
            auto normal = limited;
            normal.rules.fanfare_repeat_limit = 64;
            limited.run({Play, 0});
            normal.run({Play, 0});
            CHECK(limited.player().board[0].atk <= 8);
            replayed = limited.state.rng_state != normal.state.rng_state;
            if (replayed) CHECK(limited.player().board[0].atk == 8);
        }
        CHECK(replayed);
    }
}

void high_cost_supplements() {
    for (const auto [id, code, cd] : std::array<std::tuple<CrestId, std::uint32_t, int>, 3>{{
        {CrestId::BurnedKnight, 10744112, 0}, {CrestId::ElderDragon, 10744122, 2}, {CrestId::Rezeal, 11004112, 0},
    }}) {
        const auto* crest = find_crest(id);
        CHECK(crest && crest->official_id == code && crest->countdown == cd);
    }
    {
        Scenario s;
        s.hand({official(10744110)});
        s.run({Play, 0});
        s.run({SuperEvolve, 0});
        CHECK(s.player(1).crests[0].id == CrestId::BurnedKnight);
        s.run({EndTurn});
        CHECK(s.player(1).leader_hp == 18);
        constexpr Effect heal{.operation=Operation::Heal, .target=Target::OwnLeader, .amount=1};
        resolve_effect_steps(s.state, 1, std::array{heal});
        CHECK(s.player(1).leader_hp == 18);
        resolve_effect_steps(s.state, 1, std::array{heal});
        CHECK(s.player(1).leader_hp == 19);
    }
    {
        Scenario s;
        s.hand({official(10744120)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().crests[0].countdown == 2);
        s.run({SuperEvolve, 0});
        CHECK(s.player().crests[0].countdown == 4);
        s.run({EndTurn});
        CHECK(s.player().board_count == 3);
    }
    {
        Scenario s;
        s.crest(CrestId::Rezeal, 0);
        s.hand({official(10021130)});
        s.run({Play, 0});
        CHECK(s.player().board[0].modifiers.effect_flags & kRebirthLastWords);
        s.reject({Attack, 0, kEnemyLeader});
        const auto before = s.player().board[0].creation_order;
        resolve_effect_steps(s.state, 1, std::array{Effect{.operation=Operation::Destroy, .target=Target::AllEnemyFollowers}});
        CHECK(s.player().board_count == 1 && s.player().board[0].creation_order != before &&
              !(s.player().board[0].modifiers.effect_flags & kRebirthLastWords));
    }
    {
        Scenario s;
        s.place(kWingedStatue);
        s.place(kProjectedStatue);
        s.hand({official(10764110)});
        s.run({Play, 0});
        CHECK(s.player().board[3].card_id == official(90064210) && s.player().board[3].countdown == 1 &&
              !can_choose_enemy_permanent(s.player().board[3]));
        s.run({Evolve, 2});
        CHECK(s.player().board[3].countdown == 2);
        s.run({EndTurn});
        CHECK(s.player(1).leader_hp == 17);
    }
    {
        const auto liant = official(10664120);
        const auto book = official(90064320);
        Scenario s;
        auto deck = kMirrorDeck;
        deck[0] = liant;
        CHECK(new_game(deck, deck, 1).players[0].faiths[0] == official(10664122));
        s.player().faiths[0] = official(10664122);
        s.player().faith_count = 1;
        s.player().faith_values[0] = 9;
        s.place(kWingedStatue);
        s.place(kTrainer, 1);
        s.place(kLilium, 1);
        s.hand({liant});
        s.run({Play, 0, kNoTarget, 0, kBothBoardsSelection | 1 | (3 << kBoardLimit)});
        CHECK(s.player().board_count == 2 && s.player(1).board_count == 0 && s.player().faith_values[0] == 10);
        s.next_own_turn();
        CHECK(s.player().faith_values[0] == 0 && s.player().hand[0] == book);
        s.place(kWingedStatue);
        s.run({Play, 0, kOwnBoardTarget + 2});
        CHECK(s.player(1).leader_hp == 18 && s.player().hand[1] == book && s.player().faith_values[0] == 1);
    }
    {
        const auto kald = official(10634120);
        const auto gem = official(10631110);
        const auto abyss = official(90034330);
        Scenario s;
        s.player().faiths[0] = official(10634122);
        s.player().faith_count = 1;
        s.player().faith_values[0] = 3;
        s.player().leader_hp = 10;
        s.hand({abyss, kald});
        s.run({Play, 0});
        CHECK(s.player().board[0].card_id == gem && s.player().faith_values[0] == 4);
        CHECK(s.player().hand_modifiers[0].cost == -1);
        CHECK(s.player().board[0].atk - find_card(gem)->atk + s.player().leader_hp - 10 + 20 - s.player(1).leader_hp == 3);
    }
    {
        const auto macmillan = official(10754120);
        const auto zombie = official(90051140);
        Scenario s;
        s.player().shadows = 10;
        s.hand({macmillan});
        s.run({Play, 0});
        CHECK(s.player().board_count == 4 && s.player(1).leader_hp == 17);
        CHECK(s.player().board[1].atk == find_card(zombie)->atk + 1 &&
              (s.player().board[1].abilities & (FLAG_RUSH | FLAG_WARD)) == (FLAG_RUSH | FLAG_WARD));
        s.run({EndTurn});
        resolve_effect_steps(s.state, 0, std::array{Effect{.operation=Operation::Summon, .amount=1, .card=zombie}});
        CHECK(s.player().board_count == 5 && s.player().board[4].atk == find_card(zombie)->atk &&
              s.player(1).leader_hp == 17);
    }
}

void validate_rotation(const GameState& state) {
    CHECK(state.turn_player < 2 && state.turn_number >= 1 && state.turn_number <= kTurnLimit);
    std::array<EntityId, 4 * kBoardLimit> orders{};
    int order_count = 0;
    const auto record = [&](EntityId order) {
        CHECK(order > 0 && order < state.next_creation_order);
        CHECK(std::find(orders.begin(), orders.begin() + order_count, order) == orders.begin() + order_count);
        orders[order_count++] = order;
    };
    for (const auto& player : state.players) {
        CHECK(player.max_pp <= 10 && player.pp <= player.max_pp && player.bonus_pp <= 1);
        CHECK(player.sep <= 2 && player.extra_pp_uses <= 2);
        CHECK(player.hand_count <= kHandLimit && player.board_count <= kBoardLimit &&
              player.crest_count + player.faith_count <= kCrestLimit);
        CHECK(player.deck_idx <= player.deck_end && player.deck_end <= kDeckCapacity);
        CHECK(player.leader_max_hp <= 20 && player.leader_max_hp >= 0 && player.leader_hp <= player.leader_max_hp &&
              (state.outcome != Outcome::Ongoing || player.leader_hp > 0));
        const auto card_ok = [](CardId id) { const auto* card = find_card(id); return card && card->implemented; };
        CHECK(std::all_of(player.deck.begin() + player.deck_idx, player.deck.begin() + player.deck_end, card_ok));
        CHECK(std::all_of(player.hand.begin(), player.hand.begin() + player.hand_count, card_ok));
        const auto hand_end = player.hand.begin() + player.hand_count;
        CHECK(std::all_of(hand_end, player.hand.end(), [](CardId id) { return id == 0; }));
        int clues = 0;
        for (int i = 0; i < player.board_count; ++i) {
            const auto& pc = player.board[i];
            CHECK(card_ok(pc.card_id));
            record(pc.creation_order);
            CHECK(i == 0 || player.board[i - 1].creation_order < pc.creation_order);
            const auto& card = *find_card(pc.card_id);
            if (card.kind == Kind::Follower) {
                // Lethal leader damage stops the queue before simultaneous follower damage is cleaned up.
                CHECK((state.outcome != Outcome::Ongoing || pc.hp > 0) && pc.hp <= pc.max_hp &&
                      pc.atk >= 0 && pc.clue_count == 0 && pc.countdown == 0);
            } else {
                CHECK(card.kind == Kind::Amulet && pc.evolution == Evolution::None &&
                      (card.countdown == 0 || pc.countdown > 0));
            }
            clues += pc.clue_count > 0;
        }
        CHECK(clues <= 1);
        for (int i = 0; i < player.faith_count; ++i) {
            CHECK(find_card(player.faiths[i])->kind == Kind::Faith);
            CHECK(std::find(player.faiths.begin(), player.faiths.begin() + i, player.faiths[i]) == player.faiths.begin() + i);
        }
        for (int i = 0; i < player.crest_count; ++i) {
            const auto& crest = player.crests[i];
            record(crest.creation_order);
            const auto* definition = find_crest(crest.id);
            CHECK(definition && (definition->countdown == 0 || crest.countdown > 0));
            CHECK(std::none_of(player.crests.begin(), player.crests.begin() + i,
                               [&](const auto& other) { return other.id == crest.id; }));
        }
    }
    if (state.outcome == Outcome::Ongoing) {
        CHECK(state.end_reason == EndReason::None);
    } else if (state.end_reason == EndReason::Health || state.end_reason == EndReason::EmptyDeck) {
        CHECK(state.outcome == Outcome::FirstWins || state.outcome == Outcome::SecondWins);
        const auto& loser = state.players[state.outcome == Outcome::FirstWins ? 1 : 0];
        CHECK(state.end_reason == EndReason::Health ? loser.leader_hp <= 0 : loser.deck_remaining() == 0);
    } else {
        CHECK(state.end_reason == EndReason::TurnLimit && state.outcome == Outcome::BothLose && state.turn_number == kTurnLimit);
    }
}

int stress_decks(std::span<const CardId> rotation, int games, std::uint64_t seed, std::string_view label,
                 auto&& decks_for_game) {
    std::mt19937_64 rng(seed);
    std::vector<CardId> played;
    int max_actions = 0;
    std::uint64_t total_actions = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int game = 0; game < games; ++game) {
        const auto [first, second] = decks_for_game(game, rng);
        const auto game_seed = rng();
        int actions = 0;
        try {
            auto state = new_game(first, second, game_seed);
            validate_rotation(state);
            while (state.outcome == Outcome::Ongoing) {
                CHECK(actions < 2048);
                const auto choices = legal_actions(state);
                CHECK(!choices.empty());
                const auto move = choices[std::uniform_int_distribution<int>(0, choices.size() - 1)(rng)];
                if (!rotation.empty() && move.type == Play) {
                    const auto id = state.players[state.turn_player].hand[move.src];
                    if (std::find(played.begin(), played.end(), id) == played.end()) played.push_back(id);
                }
                apply_legal_action(state, move);
                ++actions;
                validate_rotation(state);
            }
            CHECK(legal_actions(state).empty());
        } catch (const std::exception& error) {
            std::cerr << label << " game " << game + 1 << " seed=" << game_seed << " step=" << actions << ": "
                      << error.what() << '\n';
            return 1;
        }
        total_actions += actions;
        max_actions = std::max(max_actions, actions);
    }
    CHECK(std::all_of(rotation.begin(), rotation.end(), [&](const auto id) {
        return std::find(played.begin(), played.end(), id) != played.end();
    }));
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << "PASS " << label << " games=" << games << " seed=" << seed;
    if (!rotation.empty()) std::cout << " cards_played=" << rotation.size();
    std::cout << " actions=" << total_actions << " max_actions=" << max_actions << " invariant_failures=0\n"
              << std::fixed << std::setprecision(3) << "elapsed_seconds=" << secs
              << " games_per_second=" << games / secs << " (single thread, all states checked)\n";
    return 0;
}

int stress_rotation(std::span<const CardId> rotation, int games, std::uint64_t seed, std::string_view label) {
    std::vector<CardId> pool;
    for (int copy = 0; copy < 3; ++copy) pool.insert(pool.end(), rotation.begin(), rotation.end());
    return stress_decks(rotation, games, seed, label, [&](int, auto& rng) {
        std::shuffle(pool.begin(), pool.end(), rng);
        Deck deck{};
        std::copy_n(pool.begin(), kDeckSize, deck.begin());
        return std::pair{deck, deck};
    });
}

int stress_deck_pair(std::string_view deck_a, std::string_view deck_b, std::string_view count) {
    int games = 0;
    const auto [end, error] = std::from_chars(count.data(), count.data() + count.size(), games);
    if (error != std::errc{} || end != count.data() + count.size() || games < 2 || games % 2 != 0) {
        throw std::invalid_argument("Deck-pair stress requires a positive even game count");
    }
    const auto a = read_deck(std::filesystem::path(deck_a));
    const auto b = read_deck(std::filesystem::path(deck_b));
    return stress_decks({}, games, 2026, "fixed_deck_pair", [&](int game, auto&) {
        return game % 2 == 0 ? std::pair{a, b} : std::pair{b, a};
    });
}

int stress_with_neutral(std::span<const CardId> faction, std::uint64_t seed, std::string_view label) {
    std::vector<CardId> pool(faction.begin(), faction.end());
    for (const auto id : neutral_bishop_rotation()) {
        if (find_card(id)->class_name == "ニュートラル") pool.push_back(id);
    }
    CHECK(pool.size() == faction.size() + 41);
    return stress_rotation(pool, 5'000, seed, label);
}

void actor_audit() {
    const auto sammy = official(10832110);
    auto witch = kMirrorDeck;
    witch[0] = sammy;
    const MatchupFeatures mirror(kMirrorDeck, kMirrorDeck);
    const MatchupFeatures custom(witch, kMirrorDeck);
    const auto model = svwb::Mlp::random(12);
    for (const auto* matchup : {&mirror, &custom}) {
        auto initial = new_game(matchup->legacy_mirror() ? kMirrorDeck : witch, kMirrorDeck, 2026);
        apply_legal_action(initial, {Mulligan});
        apply_legal_action(initial, {Mulligan});
        for (int seat = 0; seat < svwb::kPlayers; ++seat) {
            auto state = initial;
            state.turn_player = static_cast<std::uint8_t>(seat);
            const auto features = extract_features(state, matchup);
            auto hidden = state;
            auto& opposing = hidden.players[1 - seat];
            std::fill_n(opposing.hand.begin(), opposing.hand_count, sammy);
            opposing.hand_modifiers[0].spellboost = 8;
            opposing.hand_modifiers[1].cost = -2;
            for (auto& player : hidden.players) {
                // Audit both unknown order and unknown remaining identities, keeping public counts fixed.
                std::reverse(player.deck.begin() + player.deck_idx, player.deck.begin() + player.deck_end);
                player.deck[player.deck_idx] = sammy;
                player.deck_modifiers[player.deck_idx].attack = 5;
            }
            ++hidden.rng_state;
            CHECK(extract_features(hidden, matchup) == features);
            --hidden.players[1 - seat].hand_count;
            CHECK(extract_features(hidden, matchup) != features);
            auto visible = state;
            visible.players[seat].hand[0] = visible.players[seat].hand[0] == kLilium ? kTrainer : kLilium;
            CHECK(extract_features(visible, matchup) != features);

            // Both paths share the seed, model, state, and search settings. Logging must not change any feature.
            std::mt19937 training_rng(129);
            std::mt19937 inference_rng(129);
            Search training(model, model, training_rng, {4, false, true}, matchup);
            Search inference(model, model, inference_rng, {4, false, true}, matchup);
            const auto logged = training.choose(state, true);
            const auto inferred = inference.choose(state, false);
            CHECK(logged.next == inferred.next && logged.visits == inferred.visits);
            CHECK(logged.ranked.size() == logged.policy.size() && logged.ranked.size() == inferred.ranked.size());
            for (std::size_t i = 0; i < logged.ranked.size(); ++i) {
                const auto& candidate = inferred.ranked[i];
                CHECK(candidate.actions == logged.ranked[i].actions && candidate.selected == logged.ranked[i].selected);
                auto end_of_moves = *inferred.sampled_root;
                for (const auto action : candidate.actions) {
                    if (action.type == EndTurn) break;
                    CHECK(apply_action(end_of_moves, action));
                }
                const auto inference_features = extract_features(end_of_moves, matchup);
                const auto probability = static_cast<float>(candidate.visits) / inferred.visits;
                CHECK(std::any_of(logged.policy.begin(), logged.policy.end(), [&](const auto& row) {
                    return row.features == inference_features && row.probability == probability;
                }));
            }
        }
    }
    std::cout << "PASS actor audit: both seats, mirror and custom layouts; inference and replay features identical\n";
}

void search_canonicalization() {
    Scenario copies;
    copies.hand({kTestimony, kTrainer, kTestimony, kTestimony});
    copies.player().hand_modifiers[3].spellboost = 1;
    const auto raw = legal_actions(copies.state);
    const auto canonical = legal_actions(copies.state, {}, PlayCopies::Canonical);
    const auto testimony_count = [](const ActionList& actions) {
        return std::count_if(actions.begin(), actions.end(), [](Action action) {
            return action.type == Play && action.src != 1;
        });
    };
    CHECK(testimony_count(raw) == 6 && testimony_count(canonical) == 4);
    CHECK(std::none_of(canonical.begin(), canonical.end(), [](Action action) {
        return action.type == Play && action.src == 2;
    }));
    CHECK(std::any_of(canonical.begin(), canonical.end(), [](Action action) {
        return action.type == Play && action.src == 3;
    }));

    Scenario commute;
    commute.place(kTrainer);
    commute.place(kLilium);
    commute.player().manual_evolved = true;
    const Action left{Attack, 0, kEnemyLeader};
    const Action right{Attack, 1, kEnemyLeader};
    auto left_then_right = commute.state;
    auto right_then_left = commute.state;
    for (const auto action : {left, right}) CHECK(apply_action(left_then_right, action));
    for (const auto action : {right, left}) CHECK(apply_action(right_then_left, action));
    CHECK(left_then_right == right_then_left);
    const MatchupFeatures mirror(kMirrorDeck, kMirrorDeck);
    const MatchupFeatures general(kProvisionalMirror, kMirrorDeck);
    auto model = svwb::Mlp::random(5);
    for (const auto* matchup : {&mirror, &general}) {
        std::mt19937 rng(5);
        Search search(model, model, rng, {1, false, true}, matchup);
        const auto result = search.choose(commute.state, true);
        CHECK(result.policy.size() == 4 && result.ranked.size() == 4);
        CHECK(std::count_if(result.ranked.begin(), result.ranked.end(), [](const auto& plan) {
            return plan.actions.size() == 3;
        }) == 1);
    }
    std::cout << "PASS canonicalization: identical-copy play branches 6->4, commuting turn paths 5->4\n";
}

void search_forced_lethal() {
    const MatchupFeatures mirror(kMirrorDeck, kMirrorDeck);
    const MatchupFeatures general(kProvisionalMirror, kMirrorDeck);
    for (std::uint32_t seed = 0; seed < 16; ++seed) {
        const auto model = svwb::Mlp::random(seed);
        std::mt19937 rng(seed);
        Search search(model, model, rng, {64, seed % 2 == 0, true}, seed < 8 ? &mirror : &general);
        Scenario s;
        const int actor = seed % 2;
        s.state.turn_player = static_cast<std::uint8_t>(actor);
        s.player(actor).manual_evolved = true;
        s.player(1 - actor).leader_hp = 5;
        s.place(kTrainer, actor);
        s.place(kLilium, actor);
        const auto decision = search.play(s.state, true);
        CHECK(decision.forced && decision.visits == 0 && decision.policy.size() == 1);
        CHECK(decision.policy[0].probability == 1.0F && decision.ranked.size() == 1 &&
              decision.ranked[0].selected && decision.ranked[0].actions.size() == 2);
        for (const auto action : decision.ranked[0].actions) CHECK(apply_action(s.state, action));
        CHECK(s.state == decision.next && s.player(1 - actor).leader_hp <= 0 &&
              s.player(actor).leader_hp > 0 && outcome_value(s.state.outcome, actor) == 1.0F);
    }
    const auto model = svwb::Mlp::random(1);
    std::mt19937 rng(1);
    Search search(model, model, rng, {4, false, true}, &general);
    {
        Scenario s;
        s.hand({official(10031310)});
        s.player().deck[s.player().deck_idx] = kHolyFalcon;
        s.player(1).leader_hp = 2;
        auto draw = s.state;
        CHECK(!apply_observed_action(draw, {Play, 0}) && draw.players[0].hand[0] == kHolyFalcon);
        CHECK(!search.choose(s.state, false).forced);
    }
    {
        Scenario s;
        s.hand({official(10852120)});
        s.player().leader_hp = s.player(1).leader_hp = 1;
        auto trade = s.state;
        CHECK(apply_observed_action(trade, {Play, 0}) && trade.outcome == Outcome::SecondWins);
        CHECK(!search.choose(s.state, false).forced);
    }
    {
        Scenario s;
        s.player(1).deck_idx = s.player(1).deck_end;
        CHECK(!search.choose(s.state, false).forced);
    }
    std::cout << "PASS FORCE: 16/16 random weight sets; unknown draws, mutual lethal and deck-out excluded\n";
}

void determinization_audit() {
    const auto model = svwb::Mlp::random(83);
    auto witch = kMirrorDeck;
    witch.front() = official(10031310);
    const MatchupFeatures mirror(kMirrorDeck, kMirrorDeck);
    const MatchupFeatures general(witch, kMirrorDeck);
    for (int actor = 0; actor < svwb::kPlayers; ++actor) {
        for (const auto* matchup : {&mirror, &general}) {
            auto state = new_game(matchup->legacy_mirror() ? kMirrorDeck : witch, kMirrorDeck, 51);
            apply_legal_action(state, {Mulligan});
            apply_legal_action(state, {Mulligan});
            state.turn_player = static_cast<std::uint8_t>(actor);
            auto& own = state.players[actor];
            own.pp = own.max_pp = 2;
            own.deck_modifiers[own.deck_idx].cost = -1;
            auto other = state;
            auto& altered_own = other.players[actor];
            std::reverse(altered_own.deck.begin() + own.deck_idx, altered_own.deck.begin() + own.deck_end);
            std::reverse(altered_own.deck_modifiers.begin() + own.deck_idx, altered_own.deck_modifiers.begin() + own.deck_end);
            auto& altered_opponent = other.players[1 - actor];
            std::reverse(altered_opponent.deck.begin() + altered_opponent.deck_idx, altered_opponent.deck.begin() + altered_opponent.deck_end);
            std::fill_n(altered_opponent.hand.begin(), altered_opponent.hand_count, kCorrector);
            altered_opponent.hand_modifiers[0].cost = -4;
            altered_opponent.deck[altered_opponent.deck_idx] = kEvidence;
            other.rng_state ^= 1518;
            std::mt19937 left_rng(115), right_rng(115);
            Search left(model, model, left_rng, {8, true, true}, matchup);
            Search right(model, model, right_rng, {8, true, true}, matchup);
            const auto a = left.choose(state, true);
            const auto b = right.choose(other, true);
            CHECK(a.sampled_root && b.sampled_root && *a.sampled_root == *b.sampled_root);
            CHECK(a.next == b.next && a.actions == b.actions && a.visits == b.visits && a.forced == b.forced);
            CHECK(a.policy.size() == b.policy.size() && a.ranked.size() == b.ranked.size());
            for (std::size_t i = 0; i < a.policy.size(); ++i) {
                CHECK(a.policy[i].features == b.policy[i].features && a.policy[i].probability == b.policy[i].probability);
            }
            for (std::size_t i = 0; i < a.ranked.size(); ++i) {
                const auto& l = a.ranked[i];
                const auto& r = b.ranked[i];
                CHECK(l.actions == r.actions && l.visits == r.visits && l.value == r.value && l.selected == r.selected);
            }
            CHECK(left_rng() == right_rng());

            const auto live = left.play(state, false);
            for (const auto action : live.actions) CHECK(apply_action(state, action));
            CHECK(state == live.next && (state.outcome != Outcome::Ongoing || state.turn_player != actor));
        }
    }

    // The declared deck and its publicly revealed plays constrain the hypothesis.
    Scenario public_cards;
    public_cards.player(1).public_deck.fill(kExias);
    public_cards.player(1).public_deck[0] = kLilium;
    public_cards.state.turn_player = 1;
    public_cards.hand({kLilium}, 1);
    public_cards.run({Play, 0});
    CHECK(public_cards.player(1).public_spent == 1);
    public_cards.player(1).deck_idx = 1;
    public_cards.player(1).deck_end = kDeckSize;
    public_cards.state.turn_player = 0;
    std::mt19937 rng(97);
    Search search(model, model, rng, {1, false, true}, &mirror);
    const auto plan = search.choose(public_cards.state, false);
    const auto& hidden = plan.sampled_root->players[1];
    CHECK(hidden.deck_remaining() == 39);
    CHECK(std::all_of(hidden.deck.begin(), hidden.deck.begin() + hidden.deck_end, [](auto card) { return card == kExias; }));

    Scenario uncertain_draw;
    uncertain_draw.hand({official(10031310)});
    uncertain_draw.player().pp = 4;
    uncertain_draw.player().deck = {kTrainer, kHolyFalcon};
    uncertain_draw.player().deck_idx = 0;
    uncertain_draw.player().deck_end = 2;
    uncertain_draw.player(1).leader_hp = 2;
    bool tested_replan = false;
    for (unsigned seed = 0; seed < 20 && !tested_replan; ++seed) {
        std::mt19937 plan_rng(seed), live_rng(seed);
        Search planner(model, model, plan_rng, {16, false, true}, &general);
        const auto predicted = planner.choose(uncertain_draw.state, false);
        if (predicted.next.outcome != Outcome::FirstWins) continue;
        Search player(model, model, live_rng, {16, false, true}, &general);
        const auto actual = player.play(uncertain_draw.state, false);
        CHECK(actual.replanned && actual.next.outcome != Outcome::FirstWins);
        auto replay = uncertain_draw.state;
        for (const auto action : actual.actions) CHECK(apply_action(replay, action));
        CHECK(replay == actual.next);
        tested_replan = true;
    }
    CHECK(tested_replan);
    std::cout << "PASS determinization: both seats, identical sampled states, plans, values, visits and policy targets; live replay verified\n";
}

void attention_audit() {
    const auto sammy = official(10832110);
    auto witch = kMirrorDeck;
    witch[0] = sammy;
    witch[1] = official(10031310);
    const MatchupFeatures mirror(kMirrorDeck, kMirrorDeck);
    const MatchupFeatures custom(witch, kMirrorDeck);
    const auto model = svwb::attn::AttentionModel::random(202611);

    for (const auto* matchup : {&mirror, &custom}) {
        auto initial = new_game(matchup->legacy_mirror() ? kMirrorDeck : witch, kMirrorDeck, 4096);
        apply_legal_action(initial, {Mulligan});
        apply_legal_action(initial, {Mulligan});
        // Advance several turns and populate board, crests, faiths, earth sigils, and hand modifiers.
        for (int step = 0; step < 6 && initial.outcome == Outcome::Ongoing; ++step) {
            apply_legal_action(initial, {EndTurn});
        }
        initial.players[0].crests[0] = {101, CrestId::Testimony, 2, 0, 0};
        initial.players[0].crest_count = 1;
        initial.players[1].crests[0] = {102, CrestId::Rituel, 3, 0, 0};
        initial.players[1].crest_count = 1;
        initial.players[0].faiths[0] = kFaithToken;
        initial.players[0].faith_values[0] = 7;
        initial.players[0].faith_upgrades[0] = 2;
        initial.players[0].faith_count = 1;

        for (int seat = 0; seat < svwb::kPlayers; ++seat) {
            auto state = initial;
            state.turn_player = static_cast<std::uint8_t>(seat);
            state.players[seat].pp = state.players[seat].max_pp = 5;
            state.players[seat].ep = 1;
            state.players[seat].sep = 1;
            if (state.players[seat].board_count == 0) {
                state.players[seat].board[0] = {201, kLilium, 2, 1, 1, FLAG_RUSH, 0, 0, 1, Evolution::Normal};
                state.players[seat].board_count = 1;
            }
            if (state.players[1 - seat].board_count == 0) {
                state.players[1 - seat].board[0] = {202, kColette, 2, 4, 4, FLAG_WARD, 0, 0, 1, Evolution::None};
                state.players[1 - seat].board_count = 1;
            }

            const auto tokens = extract_attention_tokens(state);
            svwb::attn::RootContext ctx_state{};
            const float val_state = model.evaluate_root(tokens, ctx_state);
            const float logit_state = model.value_logit(tokens);

            // Rewrite opponent's hand contents/modifiers and both players' deck orders/contents.
            auto hidden = state;
            auto& opposing = hidden.players[1 - seat];
            std::fill_n(opposing.hand.begin(), opposing.hand_count, sammy);
            opposing.hand_modifiers[0].spellboost = 11;
            opposing.hand_modifiers[0].cost = -3;
            opposing.hand_modifiers[1].attack = 4;
            for (auto& player : hidden.players) {
                std::reverse(player.deck.begin() + player.deck_idx, player.deck.begin() + player.deck_end);
                std::reverse(player.deck_modifiers.begin() + player.deck_idx, player.deck_modifiers.begin() + player.deck_end);
                player.deck[player.deck_idx] = sammy;
                player.deck_modifiers[player.deck_idx].attack = 7;
            }
            hidden.rng_state ^= 0xDEADBEEFCAFEBABEULL;

            const auto hidden_tokens = extract_attention_tokens(hidden);
            CHECK(hidden_tokens == tokens);

            svwb::attn::RootContext ctx_hidden{};
            const float val_hidden = model.evaluate_root(hidden_tokens, ctx_hidden);
            const float logit_hidden = model.value_logit(hidden_tokens);
            CHECK(val_hidden == val_state && logit_hidden == logit_state);
            CHECK(ctx_hidden.cls == ctx_state.cls && ctx_hidden.policy_base == ctx_state.policy_base);

            // Verify public opponent hand size and own hand content changes ARE reflected.
            auto diff_public = hidden;
            --diff_public.players[1 - seat].hand_count;
            CHECK(extract_attention_tokens(diff_public) != tokens);
            auto diff_own_hand = state;
            diff_own_hand.players[seat].hand[0] = diff_own_hand.players[seat].hand[0] == kLilium ? kTrainer : kLilium;
            CHECK(extract_attention_tokens(diff_own_hand) != tokens);

            // Verify full Attention Search (determinized MCTS + candidate features + policy targets) matches identically
            // when opponent hand contents, opponent deck order/contents, and own deck order are rewritten.
            auto search_hidden = state;
            auto& altered_own = search_hidden.players[seat];
            std::reverse(altered_own.deck.begin() + altered_own.deck_idx, altered_own.deck.begin() + altered_own.deck_end);
            std::reverse(altered_own.deck_modifiers.begin() + altered_own.deck_idx,
                         altered_own.deck_modifiers.begin() + altered_own.deck_end);
            auto& altered_opp = search_hidden.players[1 - seat];
            std::reverse(altered_opp.deck.begin() + altered_opp.deck_idx, altered_opp.deck.begin() + altered_opp.deck_end);
            std::fill_n(altered_opp.hand.begin(), altered_opp.hand_count, sammy);
            altered_opp.hand_modifiers[0].spellboost = 11;
            altered_opp.hand_modifiers[0].cost = -3;
            altered_opp.deck[altered_opp.deck_idx] = kEvidence;
            search_hidden.rng_state ^= 0xDEADBEEFCAFEBABEULL;

            std::mt19937 left_rng(314), right_rng(314);
            Search left(model, model, left_rng, {8, true, true}, matchup);
            Search right(model, model, right_rng, {8, true, true}, matchup);
            const auto a = left.choose(state, true);
            const auto b = right.choose(search_hidden, true);
            CHECK(a.sampled_root && b.sampled_root && *a.sampled_root == *b.sampled_root);
            CHECK(a.next == b.next && a.actions == b.actions && a.visits == b.visits && a.forced == b.forced);
            CHECK(a.attention_policy.size() == b.attention_policy.size() && !a.attention_policy.empty());
            for (std::size_t i = 0; i < a.attention_policy.size(); ++i) {
                CHECK(a.attention_policy[i].features == b.attention_policy[i].features);
                CHECK(a.attention_policy[i].emb_diff == b.attention_policy[i].emb_diff);
                CHECK(a.attention_policy[i].probability == b.attention_policy[i].probability);
                const float pol_a = model.policy_logit(ctx_state, a.attention_policy[i].features.data(),
                                                       &a.attention_policy[i].emb_diff);
                const float pol_b = model.policy_logit(ctx_hidden, b.attention_policy[i].features.data(),
                                                       &b.attention_policy[i].emb_diff);
                CHECK(pol_a == pol_b && std::isfinite(pol_a));
            }
            for (std::size_t i = 0; i < a.ranked.size(); ++i) {
                const auto& candidate = a.ranked[i];
                auto end_of_moves = *a.sampled_root;
                for (const auto action : candidate.actions) {
                    if (action.type == EndTurn) break;
                    CHECK(apply_action(end_of_moves, action));
                }
                auto ended = end_of_moves;
                if (ended.outcome == Outcome::Ongoing && ended.turn_player == a.sampled_root->turn_player) {
                    CHECK(apply_action(ended, {EndTurn}));
                }
                const auto replayed_feat = extract_candidate_features(*a.sampled_root, end_of_moves, ended);
                const auto replayed_diff = extract_candidate_sparse_diff(*a.sampled_root, end_of_moves);
                const float prob = static_cast<float>(candidate.visits) / a.visits;
                CHECK(std::any_of(a.attention_policy.begin(), a.attention_policy.end(), [&](const auto& row) {
                    return row.features == replayed_feat && row.emb_diff == replayed_diff && row.probability == prob;
                }));
            }

            // Verify token permutation invariance (no positional encoding).
            auto permuted = tokens;
            std::swap(permuted.vocab[0], permuted.vocab[1]);
            for (int d = 0; d < svwb::attn::kAttrDim; ++d) {
                std::swap(permuted.attrs[d], permuted.attrs[svwb::attn::kAttrDim + d]);
            }
            CHECK(std::abs(model.value_logit(permuted) - logit_state) < 1e-5F);
        }
    }
    std::cout << "PASS attention info leak audit: hidden hand and deck order masked; tokens, logits, search and replay identical\n";
}

// ============================================================================
// P0 & Attention v3 Unit Tests (F01 ~ F12)
// ============================================================================

void mcts_root_perspective_backup() {
    // 1. Verify outcome_value zero-sum definition: Win=1.0, Lose=0.0, BothLose=0.5.
    CHECK(outcome_value(Outcome::FirstWins, 0) == 1.0F && outcome_value(Outcome::FirstWins, 1) == 0.0F);
    CHECK(outcome_value(Outcome::SecondWins, 0) == 0.0F && outcome_value(Outcome::SecondWins, 1) == 1.0F);
    CHECK(outcome_value(Outcome::BothLose, 0) == 0.5F && outcome_value(Outcome::BothLose, 1) == 0.5F);

    // 2. Construct 3-level fixed-value game tree:
    //    Root (actor=0) -> Edge0 (prior=0.6) -> OppNode A (actor=1):
    //       Edge A0 (prior=0.5) -> RootLeaf A00 (actor=0, leaf_actor_value=0.80 => root=0.80)
    //       Edge A1 (prior=0.5) -> RootLeaf A01 (actor=0, leaf_actor_value=0.20 => root=0.20)
    //    Root (actor=0) -> Edge1 (prior=0.4) -> OppNode B (actor=1, leaf_actor_value=0.30 => root=1.0-0.30=0.70):
    //       Edge B0 (prior=0.7) -> Terminal BothLose (bypasses NN => root=0.50)
    //       Edge B1 (prior=0.3) -> SameActorNode (actor=0, leaf_actor_value=0.90 => root=0.90)
    auto make_leaf = [](int actor_seat, float actor_val, Outcome term = Outcome::Ongoing) {
        auto n = std::make_unique<ExplicitMctsNode>();
        n->actor = actor_seat;
        n->leaf_actor_value = actor_val;
        n->terminal = term;
        return n;
    };

    ExplicitMctsNode root{};
    root.actor = 0;

    auto opp_a = std::make_unique<ExplicitMctsNode>();
    opp_a->actor = 1;
    opp_a->leaf_actor_value = 0.60F; // actor=1 => root perspective = 1.0 - 0.60 = 0.40
    {
        ExplicitMctsNode::Edge ea0{0.5F, 0.0F, 0, {}};
        ea0.world_children.push_back(make_leaf(0, 0.80F));
        opp_a->edges.push_back(std::move(ea0));
        ExplicitMctsNode::Edge ea1{0.5F, 0.0F, 0, {}};
        ea1.world_children.push_back(make_leaf(0, 0.20F));
        opp_a->edges.push_back(std::move(ea1));
    }

    auto opp_b = std::make_unique<ExplicitMctsNode>();
    opp_b->actor = 1;
    opp_b->leaf_actor_value = 0.30F; // actor=1 => root perspective = 1.0 - 0.30 = 0.70
    {
        ExplicitMctsNode::Edge eb0{0.7F, 0.0F, 0, {}};
        eb0.world_children.push_back(make_leaf(0, 0.99F, Outcome::BothLose)); // terminal => must return 0.50 & skip NN!
        opp_b->edges.push_back(std::move(eb0));
        ExplicitMctsNode::Edge eb1{0.3F, 0.0F, 0, {}};
        eb1.world_children.push_back(make_leaf(0, 0.90F));
        opp_b->edges.push_back(std::move(eb1));
    }

    {
        ExplicitMctsNode::Edge e0{0.6F, 0.0F, 0, {}};
        e0.world_children.push_back(std::move(opp_a));
        root.edges.push_back(std::move(e0));
        ExplicitMctsNode::Edge e1{0.4F, 0.0F, 0, {}};
        e1.world_children.push_back(std::move(opp_b));
        root.edges.push_back(std::move(e1));
    }

    int nn_evals = 0;

    // Sim 1: root(v=0): score(e0)=0.5 + 1.5*1*0.6 = 1.4 > score(e1)=0.5 + 0.6 = 1.1 => picks e0 -> evaluates opp_a leaf (actor=1, val=0.60 => root=0.40)
    run_explicit_mcts_simulation(root, 0, 0, &nn_evals);
    CHECK(nn_evals == 1 && root.visits == 1 && root.edges[0].visits == 1);
    CHECK(std::abs(root.edges[0].value_sum - 0.40F) < 1e-6F);

    // Sim 2: root(v=1): score(e0)=0.40 + 1.5*sqrt(2)*0.6/2 = 1.036396; score(e1)=0.50 + 1.5*sqrt(2)*0.4/1 = 1.348528 => picks e1 -> evaluates opp_b leaf (root=0.70)
    run_explicit_mcts_simulation(root, 0, 0, &nn_evals);
    CHECK(nn_evals == 2 && root.visits == 2 && root.edges[1].visits == 1);
    CHECK(std::abs(root.edges[1].value_sum - 0.70F) < 1e-6F);

    // Sim 3: root(v=2): score(e0)=0.40 + 1.5*sqrt(3)*0.6/2 = 1.179423; score(e1)=0.70 + 1.5*sqrt(3)*0.4/2 = 1.219615 => picks e1 -> inside opp_b(actor=1, v=0):
    //   eb0(prior=0.7): (1-0.5) + 1.5*1*0.7 = 1.55 > eb1: 0.5 + 0.45 = 0.95 => picks eb0 (Terminal BothLose => root=0.50, nn_evals unchanged!)
    run_explicit_mcts_simulation(root, 0, 0, &nn_evals);
    CHECK(nn_evals == 2); // Terminal BothLose bypassed NN!
    CHECK(root.edges[1].visits == 2 && std::abs(root.edges[1].value_sum - 1.20F) < 1e-6F);
    CHECK(root.edges[1].world_children[0]->edges[0].visits == 1);
    CHECK(std::abs(root.edges[1].world_children[0]->edges[0].value_sum - 0.50F) < 1e-6F);

    // Sim 4: root(v=3): score(e0)=0.40 + 1.5*2*0.6/2 = 1.30 > score(e1)=0.60 + 1.5*2*0.4/3 = 1.00 => picks e0 -> inside opp_a(actor=1, v=0):
    //   ea0(prior=0.5) vs ea1(prior=0.5): tie goes to ea0 (leaf=0.80 => root=0.80)
    run_explicit_mcts_simulation(root, 0, 0, &nn_evals);
    CHECK(nn_evals == 3 && root.edges[0].visits == 2);
    CHECK(std::abs(root.edges[0].value_sum - 1.20F) < 1e-6F); // 0.40 + 0.80
    CHECK(std::abs(root.edges[0].world_children[0]->edges[0].value_sum - 0.80F) < 1e-6F);

    // Sim 5: root(v=4): score(e0)=0.60 + 1.5*sqrt(5)*0.6/3 = 1.270820 > score(e1)=0.60 + 1.5*sqrt(5)*0.4/3 = 1.047214 => picks e0 -> inside opp_a(actor=1, v=1):
    //   Check one-sided flip at opp_a (actor=1 != root_player=0):
    //   ea0 has Q_root=0.80 => (1.0 - 0.80) + 1.5*sqrt(2)*0.5/2 = 0.20 + 0.530330 = 0.730330
    //   ea1 has Q_root=0.50 => (1.0 - 0.50) + 1.5*sqrt(2)*0.5/1 = 0.50 + 1.060660 = 1.560660 => MUST pick ea1 (leaf=0.20 => root=0.20)!
    run_explicit_mcts_simulation(root, 0, 0, &nn_evals);
    CHECK(nn_evals == 4 && root.edges[0].visits == 3);
    CHECK(root.edges[0].world_children[0]->edges[1].visits == 1);
    CHECK(std::abs(root.edges[0].world_children[0]->edges[1].value_sum - 0.20F) < 1e-6F);
    CHECK(std::abs(root.edges[0].value_sum - 1.40F) < 1e-6F); // 0.40 + 0.80 + 0.20

    // Sim 6..12: keep visiting opp_a; opponent (actor=1) minimizes Q_root by preferring ea1 (Q_root=0.20 -> 1-Q=0.80) over ea0 (Q_root=0.80 -> 1-Q=0.20)!
    for (int s = 0; s < 8; ++s) run_explicit_mcts_simulation(root, 0, 0, &nn_evals);
    CHECK(root.edges[0].world_children[0]->edges[1].visits > root.edges[0].world_children[0]->edges[0].visits);

    // Verify puct_score actor check directly (same actor at child does NOT flip Q, different actor DOES flip Q, U never flips):
    const float u_only = puct_score(0.0F, 0.5F, 3, 1, 0, 0) - 0.0F;
    CHECK(std::abs(puct_score(0.8F, 0.5F, 3, 1, 0, 0) - (0.8F + u_only)) < 1e-6F);
    CHECK(std::abs(puct_score(0.8F, 0.5F, 3, 1, 1, 0) - (0.2F + u_only)) < 1e-6F);

    std::cout << "PASS wb_mcts_root_perspective_backup\n";
}

void mulligan_crn_and_exclusion() {
    // Build a 40-card deck where kCorrector has exactly 1 copy, kLilium has 3 copies, and rest are valid cards.
    Deck custom_deck = kMirrorDeck;
    // Count kCorrector in kMirrorDeck and replace extra copies so exactly 1 copy of kCorrector exists.
    int corrector_seen = 0;
    for (auto& cid : custom_deck) {
        if (cid == kCorrector) {
            if (++corrector_seen > 1) cid = kWingedStatue; // wait: check copy limit of kWingedStatue!
        }
    }
    // Construct a clean 40-card deck respecting <= 3 copies per card with exactly 1 copy of kCorrector and 3 copies of kLilium.
    const auto pool = neutral_bishop_rotation();
    Deck valid_deck{};
    int cursor = 0;
    valid_deck[cursor++] = kCorrector; // 1 copy only (singleton!)
    for (int c = 0; c < 3; ++c) valid_deck[cursor++] = kLilium; // 3 copies
    for (CardId id : pool) {
        if (id == kCorrector || id == kLilium || !is_deck_card(id) || find_card(id)->kind == Kind::Faith) continue;
        for (int c = 0; c < 3 && cursor < kDeckSize; ++c) valid_deck[cursor++] = id;
        if (cursor == kDeckSize) break;
    }
    CHECK(cursor == kDeckSize);

    auto base_state = new_game(valid_deck, valid_deck, 424242);
    // Force opening hand to [kCorrector (1-copy), kLilium (3-copy), valid_deck[4], valid_deck[7]].
    base_state.players[0].hand[0] = kCorrector;
    base_state.players[0].hand[1] = kLilium;
    base_state.players[0].hand[2] = valid_deck[4];
    base_state.players[0].hand[3] = valid_deck[7];

    bool saw_other_copy_of_lilium = false;
    for (std::uint64_t seed = 1; seed <= 64; ++seed) {
        const std::uint64_t sample_seed = 0x9E3779B97F4A7C15ULL * seed;
        // Mask 15 replaces all 4 slots -> exposes the first 4 draws of the CRN stream.
        const auto ref_all = simulate_mulligan_crn(base_state, 15, sample_seed);
        for (int mask = 0; mask < 16; ++mask) {
            const auto res = simulate_mulligan_crn(base_state, static_cast<std::uint8_t>(mask), sample_seed);
            int draw_ord = 0;
            for (int slot = 0; slot < 4; ++slot) {
                if (mask & (1 << slot)) {
                    // 1. Selected physical instance ID must NEVER appear anywhere in redrawn_physical_ids!
                    const int excluded_pid = res.offered_physical_ids[slot];
                    for (int r = 0; r < 4; ++r) {
                        CHECK(res.redrawn_physical_ids[r] != excluded_pid);
                    }
                    // 2. Common Random Numbers: k-th replacement draw matches ref_all's k-th replacement draw!
                    CHECK(res.redrawn_physical_ids[slot] == ref_all.redrawn_physical_ids[draw_ord]);
                    CHECK(res.redrawn_cards[slot] == ref_all.redrawn_cards[draw_ord]);
                    ++draw_ord;
                } else {
                    CHECK(res.redrawn_physical_ids[slot] == res.offered_physical_ids[slot]);
                    CHECK(res.redrawn_cards[slot] == res.offered_cards[slot]);
                }
            }
            // Singleton kCorrector (slot 0): if replaced, kCorrector can NEVER appear in the redrawn 4-card hand!
            if (mask & 1) {
                for (int r = 0; r < 4; ++r) CHECK(res.redrawn_cards[r] != kCorrector);
            }
            // 3-copy kLilium (slot 1): if replaced, another copy (different physical_id) CAN appear!
            if ((mask & 2) && res.redrawn_cards[1] == kLilium) {
                CHECK(res.redrawn_physical_ids[1] != res.offered_physical_ids[1]);
                saw_other_copy_of_lilium = true;
            }
        }
    }
    CHECK(saw_other_copy_of_lilium);

    // 3. Verify choose_mulligan never reads the true match deck or opponent hidden hand.
    const auto attn_model = svwb::attn::AttentionModel::random(777);
    const MatchupFeatures matchup(valid_deck, valid_deck);
    auto corrupted = base_state;
    std::fill(corrupted.players[0].deck.begin(), corrupted.players[0].deck.end(), kCorrector);
    std::fill(corrupted.players[1].deck.begin(), corrupted.players[1].deck.end(), kCorrector);
    std::fill(corrupted.players[1].hand.begin(), corrupted.players[1].hand.end(), kCorrector);
    corrupted.rng_state ^= 0x123456789ABCDEF0ULL;

    std::mt19937 rng1(2026), rng2(2026);
    const auto m1 = choose_mulligan(base_state, attn_model, rng1, &matchup);
    const auto m2 = choose_mulligan(corrupted, attn_model, rng2, &matchup);
    CHECK(m1 == m2);

    std::cout << "PASS wb_mulligan_crn_and_exclusion\n";
}

void attention_hidden_state_invariance() {
    const auto sammy = official(10832110);
    auto witch = kMirrorDeck;
    witch[0] = sammy;
    witch[1] = official(10031310);
    const MatchupFeatures matchup(witch, kMirrorDeck);
    const auto model = svwb::attn::AttentionModel::random(20261125);

    auto base = new_game(witch, kMirrorDeck, 8192);
    apply_legal_action(base, {Mulligan});
    apply_legal_action(base, {Mulligan});
    for (int step = 0; step < 6 && base.outcome == Outcome::Ongoing; ++step) {
        apply_legal_action(base, {EndTurn});
    }
    base.players[0].crests[0] = {301, CrestId::Testimony, 2, 0, 0};
    base.players[0].crest_count = 1;
    base.players[1].faiths[0] = kFaithToken;
    base.players[1].faith_values[0] = 4;
    base.players[1].faith_upgrades[0] = 1;
    base.players[1].faith_count = 1;

    for (int seat = 0; seat < svwb::kPlayers; ++seat) {
        auto s1 = base;
        s1.turn_player = static_cast<std::uint8_t>(seat);
        s1.players[seat].pp = s1.players[seat].max_pp = 5;
        s1.players[seat].ep = 1;
        s1.players[seat].sep = 1;

        auto s2 = s1;
        auto& own2 = s2.players[seat];
        std::reverse(own2.deck.begin() + own2.deck_idx, own2.deck.begin() + own2.deck_end);
        std::reverse(own2.deck_modifiers.begin() + own2.deck_idx, own2.deck_modifiers.begin() + own2.deck_end);
        auto& opp2 = s2.players[1 - seat];
        std::reverse(opp2.deck.begin() + opp2.deck_idx, opp2.deck.begin() + opp2.deck_end);
        std::fill_n(opp2.hand.begin(), opp2.hand_count, sammy);
        opp2.hand_modifiers[0].spellboost = 9;
        opp2.hand_modifiers[0].cost = -3;
        s2.rng_state ^= 0xFEEDFACECAFEBEEFULL;

        // Item 1: Root Token
        const auto tok1 = extract_attention_tokens(s1);
        const auto tok2 = extract_attention_tokens(s2);
        CHECK(tok1 == tok2);

        // Item 2: RootContext CLS
        svwb::attn::RootContext ctx1{}, ctx2{};
        const float v1 = model.evaluate_root(tok1, ctx1);
        const float v2 = model.evaluate_root(tok2, ctx2);
        CHECK(std::abs(v1 - v2) <= 1e-6F);
        for (int i = 0; i < svwb::attn::kModel; ++i) {
            CHECK(std::abs(ctx1.cls[i] - ctx2.cls[i]) <= 1e-6F);
        }

        // Item 3: Legal actions & Proven lethal status
        const auto leg1 = legal_actions(s1, {}, PlayCopies::Canonical);
        const auto leg2 = legal_actions(s2, {}, PlayCopies::Canonical);
        CHECK(leg1.size() == leg2.size());
        for (int i = 0; i < leg1.size(); ++i) CHECK(leg1[i] == leg2[i]);

        // Item 4: sample_hidden sequence
        std::mt19937 hrng1(999), hrng2(999);
        for (int k = 0; k < 4; ++k) {
            CHECK(sample_hidden_state(s1, hrng1, seat) == sample_hidden_state(s2, hrng2, seat));
        }

        // Items 5, 6, 7: Candidate features/diffs, MCTS visit distribution, and final selected action sequence
        std::mt19937 srng1(20261125), srng2(20261125);
        Search search1(model, model, srng1, {16, false, true, true}, &matchup);
        Search search2(model, model, srng2, {16, false, true, true}, &matchup);
        const auto d1 = search1.choose(s1, true);
        const auto d2 = search2.choose(s2, true);
        CHECK(d1.forced == d2.forced && d1.lethal_status == d2.lethal_status);
        CHECK(d1.attention_policy.size() == d2.attention_policy.size());
        for (std::size_t i = 0; i < d1.attention_policy.size(); ++i) {
            for (std::size_t j = 0; j < svwb::attn::kCandidateDim; ++j) {
                CHECK(std::abs(d1.attention_policy[i].features[j] - d2.attention_policy[i].features[j]) <= 1e-6F);
            }
            CHECK(d1.attention_policy[i].emb_diff == d2.attention_policy[i].emb_diff);
            CHECK(std::abs(d1.attention_policy[i].probability - d2.attention_policy[i].probability) <= 1e-6F);
        }
        CHECK(d1.ranked.size() == d2.ranked.size());
        for (std::size_t i = 0; i < d1.ranked.size(); ++i) {
            CHECK(d1.ranked[i].visits == d2.ranked[i].visits);
            CHECK(d1.ranked[i].actions == d2.ranked[i].actions);
            if (d1.ranked[i].value && d2.ranked[i].value) {
                CHECK(std::abs(*d1.ranked[i].value - *d2.ranked[i].value) <= 1e-6F);
            }
        }
        CHECK(d1.plan == d2.plan && d1.actions == d2.actions);
    }
    std::cout << "PASS wb_attention_hidden_state_invariance\n";
}

void mcts_strategy_fusion_expected_value() {
    // Artificial 2-world game tree from F02:
    //   Candidate A (uncertain): Sample 0 -> 100% (1.0), Sample 1 -> 0% (0.0) => E[V(A)] = 50% (0.50)
    //   Candidate B (safe):      Sample 0 -> 60% (0.6),  Sample 1 -> 60% (0.6) => E[V(B)] = 60% (0.60)
    //   Per-world optimum average (Strategy Fusion) would be 0.5*(1.0 + 0.6) = 80% (0.80).
    //   Shared-root expected-value MCTS must converge to Q(A)~0.50, Q(B)=0.60 and choose B!
    ExplicitMctsNode root{};
    root.actor = 0;

    ExplicitMctsNode::Edge edge_a{0.5F, 0.0F, 0, {}};
    {
        auto w0 = std::make_unique<ExplicitMctsNode>();
        w0->actor = 1;
        w0->terminal = Outcome::FirstWins; // 1.0 for root_player=0
        auto w1 = std::make_unique<ExplicitMctsNode>();
        w1->actor = 1;
        w1->terminal = Outcome::SecondWins; // 0.0 for root_player=0
        edge_a.world_children.push_back(std::move(w0));
        edge_a.world_children.push_back(std::move(w1));
    }

    ExplicitMctsNode::Edge edge_b{0.5F, 0.0F, 0, {}};
    {
        for (int w = 0; w < 2; ++w) {
            auto leaf = std::make_unique<ExplicitMctsNode>();
            leaf->actor = 0;
            leaf->leaf_actor_value = 0.60F; // 0.60 in both worlds
            edge_b.world_children.push_back(std::move(leaf));
        }
    }

    root.edges.push_back(std::move(edge_a));
    root.edges.push_back(std::move(edge_b));

    for (int sim = 0; sim < 128; ++sim) {
        run_explicit_mcts_simulation(root, 0);
    }

    const float q_a = root.edges[0].value_sum / static_cast<float>(root.edges[0].visits);
    const float q_b = root.edges[1].value_sum / static_cast<float>(root.edges[1].visits);
    const float root_ev = (root.edges[0].value_sum + root.edges[1].value_sum) / static_cast<float>(root.visits);
    CHECK(std::abs(q_a - 0.50F) < 0.05F);
    CHECK(std::abs(q_b - 0.60F) < 1e-5F);
    CHECK(root_ev < 0.62F); // Strictly avoids the 0.80 Strategy Fusion overestimate!
    CHECK(root.edges[1].visits > root.edges[0].visits); // Selects Safe Action B (60%)!

    std::cout << "PASS wb_mcts_strategy_fusion_expected_value\n";
}

void attention_hidden_pool_consistency() {
    Scenario s;
    Deck custom = kMirrorDeck;
    s.player(1).public_deck = custom;
    std::sort(s.player(1).public_deck.begin(), s.player(1).public_deck.end());
    // Mark first 5 cards in public_deck as spent, add 1 generated kEvidence, set hand_count=3, deck_remaining=33 (total=36).
    s.player(1).public_spent = 0b11111ULL;
    s.player(1).public_generated[0] = kEvidence;
    s.player(1).public_generated_count = 1;
    s.player(1).hand_count = 3;
    s.player(1).deck_idx = 0;
    s.player(1).deck_end = 33;

    // Build expected multiset of unseen cards (35 unspent from public_deck + 1 generated kEvidence = 36).
    std::vector<CardId> expected_pool;
    for (std::size_t i = 0; i < s.player(1).public_deck.size(); ++i) {
        if (!(s.player(1).public_spent & (std::uint64_t{1} << i))) {
            expected_pool.push_back(s.player(1).public_deck[i]);
        }
    }
    expected_pool.push_back(kEvidence);
    std::sort(expected_pool.begin(), expected_pool.end());
    CHECK(expected_pool.size() == 36);

    std::mt19937 rng(12345);
    for (int trial = 0; trial < 32; ++trial) {
        const auto sampled = sample_hidden_state(s.state, rng, 0);
        const auto& opp = sampled.players[1];
        CHECK(opp.hand_count == 3 && opp.deck_remaining() == 33);
        std::vector<CardId> actual_pool;
        for (int i = 0; i < opp.hand_count; ++i) {
            CHECK(find_card(opp.hand[i]) && find_card(opp.hand[i])->kind != Kind::Faith);
            actual_pool.push_back(opp.hand[i]);
        }
        for (int i = opp.deck_idx; i < opp.deck_end; ++i) {
            CHECK(find_card(opp.deck[i]) && find_card(opp.deck[i])->kind != Kind::Faith);
            actual_pool.push_back(opp.deck[i]);
        }
        std::sort(actual_pool.begin(), actual_pool.end());
        CHECK(actual_pool == expected_pool);
    }
    std::cout << "PASS wb_attention_hidden_pool_consistency\n";
}

void attention_opponent_private_info_isolation() {
    const auto attn_model = svwb::attn::AttentionModel::random(314159);
    const auto mlp_model = svwb::Mlp::random(271828);
    const MatchupFeatures matchup(kMirrorDeck, kMirrorDeck);

    for (int root_player = 0; root_player < svwb::kPlayers; ++root_player) {
        const int opp_seat = 1 - root_player;
        Scenario s_a;
        s_a.state.turn_player = static_cast<std::uint8_t>(opp_seat);
        s_a.state.turn_number = 6;
        s_a.player(opp_seat).pp = s_a.player(opp_seat).max_pp = 6;
        s_a.player(opp_seat).ep = 1;
        s_a.player(opp_seat).sep = 1;
        s_a.hand({kLilium, kExias, kTestimony, kRituel}, opp_seat);
        s_a.place(kTrainer, root_player);
        s_a.place(kColette, opp_seat);

        // State A vs State B: ONLY root_player's private hand contents & modifiers differ (hand_count=3 stays identical!).
        s_a.hand({kWingedStatue, kWingedStatue, kTrainer}, root_player);
        Scenario s_b = s_a;
        s_b.hand({kCorrector, kRapid, kTeresa}, root_player);
        s_b.player(root_player).hand_modifiers[0].cost = -5;
        s_b.player(root_player).hand_modifiers[1].spellboost = 12;

        for (const ModelRef mref : {ModelRef(attn_model), ModelRef(mlp_model)}) {
            const auto pol_a = evaluate_opponent_turn_policy(s_a.state, mref, root_player, 0xCAFEBABEULL, &matchup);
            const auto pol_b = evaluate_opponent_turn_policy(s_b.state, mref, root_player, 0xCAFEBABEULL, &matchup);
            CHECK(!pol_a.plans.empty() && pol_a.plans == pol_b.plans);
            CHECK(pol_a.priors.size() == pol_b.priors.size());
            for (std::size_t i = 0; i < pol_a.priors.size(); ++i) {
                CHECK(pol_a.priors[i] == pol_b.priors[i]);
            }
            CHECK(pol_a.greedy_choice == pol_b.greedy_choice);
        }
    }
    std::cout << "PASS wb_attention_opponent_private_info_isolation\n";
}

void attention_stochastic_lethal_not_forced() {
    auto witch = kMirrorDeck;
    witch[0] = official(10031310); // Insight (1PP Draw 1)
    const MatchupFeatures matchup(witch, kMirrorDeck);
    const auto model = svwb::attn::AttentionModel::random(2026);

    // 1. Draw-dependent lethal (Insight draws HolyFalcon at 4PP vs 2HP enemy): Probabilistic, NEVER Proven/forced!
    {
        Scenario s;
        s.hand({official(10031310)});
        s.player(0).pp = s.player(0).max_pp = 4;
        s.player(0).deck = {};
        s.player(0).deck[0] = kHolyFalcon;
        s.player(0).deck_idx = 0;
        s.player(0).deck_end = 1;
        s.player(1).leader_hp = 2;

        auto sim = s.state;
        std::uint16_t unobs = 0;
        LethalEvidence ev{};
        ev.merge(apply_action_with_evidence(sim, {Play, 0}, 0, &unobs));      // Draw HolyFalcon (unobserved!)
        ev.merge(apply_action_with_evidence(sim, {Play, 0}, 0, &unobs));      // Play drawn HolyFalcon
        ev.merge(apply_action_with_evidence(sim, {Attack, 0, kEnemyLeader}, 0, &unobs));
        CHECK(ev.terminal_win_verified && ev.crossed_unresolved_chance && ev.depends_on_hidden_information && ev.used_unobserved_card);
        CHECK(!ev.is_proven() && classify_lethal(ev) == LethalStatus::Probabilistic);

        std::mt19937 rng(42);
        Search search(model, model, rng, {16, false, true, true}, &matchup);
        const auto dec = search.choose(s.state, true);
        CHECK(!dec.forced && dec.lethal_status != LethalStatus::Proven);
        CHECK(dec.plan.requires_replan_after_observation || dec.plan.prefix.back().type == EndTurn);
    }

    // 2. Indirect unobserved card effect: draw unobserved card then play Rabbit (BanishLowestAndEvolve inspects hand)!
    {
        Scenario s;
        s.hand({official(10031310), kRabbit});
        s.player(0).pp = s.player(0).max_pp = 10;
        s.player(0).deck[0] = kWingedStatue;
        s.player(0).deck_idx = 0;
        s.player(0).deck_end = 1;
        std::uint16_t unobs = 0;
        LethalEvidence ev{};
        ev.merge(apply_action_with_evidence(s.state, {Play, 0}, 0, &unobs)); // Draws into slot 1
        ev.merge(apply_action_with_evidence(s.state, {Play, 0}, 0, &unobs)); // Plays Rabbit -> BanishLowestAndEvolve
        CHECK(ev.crossed_unresolved_chance && ev.depends_on_hidden_information && ev.used_unobserved_card);
    }

    // 3. Random-target lethal (Colette evolve 1 dmg x2 when enemy has 2 followers, one of which is Lilium or blocks):
    {
        Scenario s;
        s.place(kTrainer, 0).evolution = Evolution::Normal;
        s.place(kAlmiraj, 1);
        s.place(kExias, 1);
        s.hand({kColette});
        std::uint16_t unobs = 0;
        const auto ev = apply_action_with_evidence(s.state, {Play, 0}, 0, &unobs);
        CHECK(ev.crossed_unresolved_chance && !ev.is_proven());
    }

    // 4. Contrast with true deterministic Proven lethal (HolyFalcon already observed in initial hand vs 2HP enemy):
    {
        Scenario s;
        s.hand({kHolyFalcon});
        s.player(0).pp = s.player(0).max_pp = 4;
        s.player(1).leader_hp = 2;
        std::mt19937 rng(42);
        Search search(model, model, rng, {16, false, true, true}, &matchup);
        const auto dec = search.choose(s.state, true);
        CHECK(dec.forced && dec.lethal_status == LethalStatus::Proven && dec.lethal_evidence.is_proven());
    }

    std::cout << "PASS wb_attention_stochastic_lethal_not_forced\n";
}

void attention_global64_turn_boundaries() {
    CHECK(compute_card_vocab_hash() == svwb::attn::kCardVocabHash);

    const auto check_budget = [](int turn_num, int active_seat, int p0_max_pp, int p1_max_pp,
                                 bool p1_extra_avail, int p1_extra_uses,
                                 int expected_own_next_pp, int expected_opp_next_pp) {
        Scenario s;
        s.state.turn_number = static_cast<std::uint8_t>(turn_num);
        s.state.turn_player = static_cast<std::uint8_t>(active_seat);
        s.player(0).max_pp = s.player(0).pp = static_cast<std::uint8_t>(p0_max_pp);
        s.player(0).extra_pp_available = false;
        s.player(1).max_pp = s.player(1).pp = static_cast<std::uint8_t>(p1_max_pp);
        s.player(1).extra_pp_available = p1_extra_avail;
        s.player(1).extra_pp_uses = static_cast<std::uint8_t>(p1_extra_uses);

        const auto tok = extract_attention_tokens(s.state);
        CHECK(std::abs(tok.global[8] - expected_own_next_pp / 11.0F) < 1e-6F);
        CHECK(std::abs(tok.global[36] - expected_opp_next_pp / 11.0F) < 1e-6F);
        CHECK(tok.global[63] == 0.0F);
    };

    // First player boundaries: 3T, 6T, 9T
    check_budget(3, 0, 3, 2, true, 0, 4, 4);   // 1st 3T: own->4, opp(2nd 3T with extra avail)->3+1=4
    check_budget(3, 0, 3, 2, false, 1, 4, 3);  // 1st 3T: opp already spent extra PP -> 3
    check_budget(6, 0, 6, 5, false, 1, 7, 7);  // 1st 6T: opp next refresh is 2nd 6T -> refill extra PP -> 6+1=7!
    check_budget(9, 0, 9, 8, false, 2, 10, 9); // 1st 9T: own next=10, opp next=9

    // Second player boundaries: 3T, 5T, 6T, 10T
    check_budget(3, 1, 3, 3, true, 0, 5, 4);   // 2nd 3T (extra unused): own next(4T)=4+1=5, opp next(4T)=4
    check_budget(5, 1, 5, 5, false, 1, 7, 6);  // 2nd 5T (extra spent): own next refresh is 6T -> refill -> 6+1=7!
    check_budget(6, 1, 6, 6, true, 1, 8, 7);   // 2nd 6T (refilled extra unused): own next(7T)=7+1=8
    check_budget(6, 1, 6, 6, false, 2, 7, 7);  // 2nd 6T (refilled extra used): own next(7T)=7+0=7
    check_budget(10, 1, 10, 10, false, 2, 10, 10); // 2nd 10T (extra used): capped at 10
    check_budget(10, 1, 10, 10, true, 1, 11, 10);  // 2nd 10T (extra held): 10 + 1 = 11

    // Opponent evolution/super-evolution unlock flags ([40], [41]) based on opp_next_turn:
    {
        Scenario s;
        s.state.turn_player = 0;
        s.state.turn_number = 4; // 1st 4T -> opp next action is 2nd 4T (normal evo unlocked, super locked)
        auto tok = extract_attention_tokens(s.state);
        CHECK(tok.global[12] == 0.0F && tok.global[13] == 0.0F);
        CHECK(tok.global[40] == 1.0F && tok.global[41] == 0.0F);

        s.state.turn_number = 6; // 1st 6T -> opp next action is 2nd 6T (both normal & super unlocked!)
        tok = extract_attention_tokens(s.state);
        CHECK(tok.global[12] == 1.0F && tok.global[13] == 0.0F);
        CHECK(tok.global[40] == 1.0F && tok.global[41] == 1.0F);

        s.state.turn_player = 1;
        s.state.turn_number = 6; // 2nd 6T -> opp next action is 1st 7T (both normal & super unlocked!)
        tok = extract_attention_tokens(s.state);
        CHECK(tok.global[12] == 1.0F && tok.global[13] == 1.0F);
        CHECK(tok.global[40] == 1.0F && tok.global[41] == 1.0F);
    }

    // Non-active reset, unclipped (>1.0f) counters, and [25..27]/[53..55] evidence/crest/faith separation:
    {
        Scenario s;
        s.state.turn_player = 0;
        s.player(0).shadows = 80;
        s.player(0).plays_this_turn = 25;
        s.player(0).followers_entered = 80;
        auto& own_earth = s.place(kEarthSigil, 0);
        own_earth.earth_count = 20;
        auto& own_ev = s.place(kEvidence, 0);
        own_ev.clue_count = 20;
        s.crest(CrestId::Testimony, 2, 0);
        s.crest(CrestId::Rituel, 3, 0);
        s.player(0).faiths[0] = kFaithToken;
        s.player(0).faith_count = 1;

        // Contaminate non-active opponent's turn-local fields to verify forced 0.0f reset:
        s.player(1).pp = 7;
        s.player(1).manual_evolved = true;
        s.player(1).plays_this_turn = 9;
        s.player(1).acted_evidence = true;
        s.player(1).acted_amulet = true;
        s.player(1).shadows = 96;
        s.player(1).followers_entered = 96;
        auto& opp_ev = s.place(kEvidence, 1);
        opp_ev.clue_count = 24;
        s.crest(CrestId::Rapid, 2, 1);
        s.player(1).faiths[0] = kFaithToken;
        s.player(1).faiths[1] = kFaithToken;
        s.player(1).faith_count = 2;

        const auto tok = extract_attention_tokens(s.state);
        // Unclipped > 1.0f
        CHECK(std::abs(tok.global[14] - 80.0F / 64.0F) < 1e-6F);
        CHECK(std::abs(tok.global[15] - 25.0F / 20.0F) < 1e-6F);
        CHECK(std::abs(tok.global[16] - 80.0F / 64.0F) < 1e-6F);
        CHECK(std::abs(tok.global[17] - 20.0F / 16.0F) < 1e-6F);
        CHECK(std::abs(tok.global[25] - 20.0F / 16.0F) < 1e-6F);
        CHECK(std::abs(tok.global[22] - 3.0F / 5.0F) < 1e-6F);
        CHECK(std::abs(tok.global[26] - 2.0F / 5.0F) < 1e-6F);
        CHECK(std::abs(tok.global[27] - 1.0F / 5.0F) < 1e-6F);

        // Non-active turn-local resets == 0.0f
        CHECK(tok.global[31] == 0.0F);
        CHECK(tok.global[39] == 0.0F);
        CHECK(tok.global[43] == 0.0F);
        CHECK(tok.global[51] == 0.0F);

        // Opponent unclipped & crest/faith separation
        CHECK(std::abs(tok.global[42] - 96.0F / 64.0F) < 1e-6F);
        CHECK(std::abs(tok.global[44] - 96.0F / 64.0F) < 1e-6F);
        CHECK(std::abs(tok.global[53] - 24.0F / 16.0F) < 1e-6F);
        CHECK(std::abs(tok.global[50] - 3.0F / 5.0F) < 1e-6F);
        CHECK(std::abs(tok.global[54] - 1.0F / 5.0F) < 1e-6F);
        CHECK(std::abs(tok.global[55] - 2.0F / 5.0F) < 1e-6F);
    }

    std::cout << "PASS wb_attention_global64_turn_boundaries\n";
}

void attention_embdiff160_and_migration() {
    using svwb::attn::ArchVariant;
    using svwb::attn::AttentionModel;

    // 1. Parameter count verification (AC-04)
    const auto m_v2 = AttentionModel::random(101, ArchVariant::LegacyV2);
    const auto m_v3g = AttentionModel::random(102, ArchVariant::V3Global);
    const auto m_v3gd = AttentionModel::random(103, ArchVariant::V3GlobalEmbDiff);
    CHECK(m_v2.parameters().size() == 461'826);
    CHECK(m_v3g.parameters().size() == 465'922);
    CHECK(m_v3gd.parameters().size() == 486'402);

    // 2. CandidateEmbDiff strict initial -> decision separation & 5-zone signed cancellation & vocab_id==0 masking
    {
        Scenario init_s;
        init_s.hand({kLilium, kLilium, kTrainer});
        init_s.place(kWingedStatue, 0);
        init_s.place(kColette, 1);
        init_s.crest(CrestId::Testimony, 2, 0);

        Scenario dec_s = init_s;
        dec_s.hand({kLilium, kLilium, kEvidence}); // kLilium count 2 -> 2 (net 0!), -kTrainer, +kEvidence
        dec_s.player(0).board_count = 0;
        dec_s.place(kExias, 0);
        dec_s.player(1).board_count = 0;
        dec_s.player(0).crest_count = 0;
        dec_s.crest(CrestId::Rituel, 3, 1);

        const auto diff = extract_candidate_sparse_diff(init_s.state, dec_s.state);
        CHECK(diff.total_entries() == 7);
        CHECK(diff.zone_offsets[1] - diff.zone_offsets[0] == 2); // kLilium cancelled out!
        CHECK(diff.zone_offsets[2] - diff.zone_offsets[1] == 2);
        CHECK(diff.zone_offsets[3] - diff.zone_offsets[2] == 1);
        CHECK(diff.zone_offsets[4] - diff.zone_offsets[3] == 1);
        CHECK(diff.zone_offsets[5] - diff.zone_offsets[4] == 1);

        // Verify vocab_id == 0 explicit zero-masking in materialize_embdiff, DirectMatMul, and PreprojectedTable
        auto synthetic = diff;
        synthetic.entries[synthetic.total_entries()] = {0, 5, 0};
        synthetic.zone_offsets[5] = static_cast<std::uint8_t>(synthetic.total_entries() + 1);
        CHECK(m_v3gd.materialize_embdiff(diff) == m_v3gd.materialize_embdiff(synthetic));

        svwb::attn::RootContext ctx{};
        static_cast<void>(m_v3gd.evaluate_root(extract_attention_tokens(init_s.state), ctx));
        const auto cfeat = extract_candidate_features(init_s.state, dec_s.state, dec_s.state);
        const float l_dir_clean = m_v3gd.policy_logit_direct(ctx, cfeat.data(), &diff);
        const float l_dir_zero = m_v3gd.policy_logit_direct(ctx, cfeat.data(), &synthetic);
        const float l_pre_clean = m_v3gd.policy_logit_preprojected(ctx, cfeat.data(), &diff);
        const float l_pre_zero = m_v3gd.policy_logit_preprojected(ctx, cfeat.data(), &synthetic);
        CHECK(l_dir_clean == l_dir_zero && l_pre_clean == l_pre_zero);
        CHECK(std::abs(l_dir_clean - l_pre_clean) < 1e-5F);
    }

    // 3. v2 -> v3-G and v2 -> v3-GD warm-start weight migration equivalence (< 1e-5)
    std::vector<AttentionModel> v2_sources;
    v2_sources.push_back(m_v2);
    const std::filesystem::path real_v2("data/phase5c-attn-v2/bishop__bishop/generation_040/attn_weights.bin");
    if (std::filesystem::is_regular_file(real_v2)) {
        v2_sources.push_back(AttentionModel::load(real_v2));
    }

    for (const auto& src_v2 : v2_sources) {
        const auto mig_v3g = AttentionModel::migrate_v2_to_v3(src_v2, ArchVariant::V3Global);
        const auto mig_v3gd = AttentionModel::migrate_v2_to_v3(src_v2, ArchVariant::V3GlobalEmbDiff);
        CHECK(mig_v3g.parameters().size() == 465'922);
        CHECK(mig_v3gd.parameters().size() == 486'402);

        auto state = new_game(kMirrorDeck, kMirrorDeck, 5555);
        apply_legal_action(state, {Mulligan});
        apply_legal_action(state, {Mulligan});
        for (int t = 0; t < 8 && state.outcome == Outcome::Ongoing; ++t) {
            const auto tok_v2 = extract_attention_tokens_v2(state);
            const auto tok_v3 = extract_attention_tokens(state);
            svwb::attn::RootContext ctx_v2{}, ctx_g{}, ctx_gd{};
            const float val_v2 = src_v2.evaluate_root(tok_v2, ctx_v2);
            const float val_g = mig_v3g.evaluate_root(tok_v3, ctx_g);
            const float val_gd = mig_v3gd.evaluate_root(tok_v3, ctx_gd);
            CHECK(std::abs(val_v2 - val_g) < 1e-5F && std::abs(val_v2 - val_gd) < 1e-5F);
            CHECK(std::abs(src_v2.value_logit(tok_v2) - mig_v3gd.value_logit(tok_v3)) < 1e-5F);

            auto dec_state = state;
            for (const auto act : legal_actions(state, {}, PlayCopies::Canonical)) {
                if (act.type != EndTurn) {
                    apply_legal_action(dec_state, act);
                    break;
                }
            }
            auto next_state = dec_state;
            if (next_state.outcome == Outcome::Ongoing && next_state.turn_player == state.turn_player) {
                apply_legal_action(next_state, {EndTurn});
            }
            const auto cfeat = extract_candidate_features(state, dec_state, next_state);
            const auto cdiff = extract_candidate_sparse_diff(state, dec_state);
            const float p_v2 = src_v2.policy_logit(ctx_v2, cfeat.data(), nullptr);
            const float p_g = mig_v3g.policy_logit(ctx_g, cfeat.data(), nullptr);
            const float p_gd = mig_v3gd.policy_logit(ctx_gd, cfeat.data(), &cdiff);
            CHECK(std::abs(p_v2 - p_g) < 1e-5F && std::abs(p_v2 - p_gd) < 1e-5F);

            state = next_state;
        }
    }

    std::cout << "PASS wb_attention_embdiff160_and_migration\n";
}

void p0_post_chance_beam_completion() {
    auto witch = kMirrorDeck;
    witch[0] = official(10031310); // Insight (1PP Spell: Draw 1)
    const MatchupFeatures matchup(witch, kMirrorDeck);
    const auto model = svwb::attn::AttentionModel::random(20261125);

    // Scenario: 5 PP, 1 EP, Insight (1PP) in hand, remaining own deck is all kHolyFalcon (4PP 2/2 Storm).
    // Enemy leader is at 4 HP. Winning requires crossing the Insight draw boundary AND completing the
    // 3-step post-chance chain: Play HolyFalcon (4PP) -> Evolve HolyFalcon (4 atk) -> Attack Enemy Leader (4 dmg)!
    Scenario s;
    s.state.turn_number = 5;
    s.state.turn_player = 0;
    s.player(0).pp = s.player(0).max_pp = 5;
    s.player(0).ep = 1;
    s.hand({official(10031310)});
    s.player(0).deck = {};
    for (int i = 0; i < 10; ++i) s.player(0).deck[i] = kHolyFalcon;
    s.player(0).deck_idx = 0;
    s.player(0).deck_end = 10;
    s.player(1).leader_hp = 4;

    std::mt19937 rng1(777);
    Search search_choose(model, model, rng1, {32, false, true, true}, &matchup);
    const auto dec = search_choose.choose(s.state, true);
    // Must NOT be forced as Proven (depends on drawing from deck), yet beam completion across worlds finds the 4-damage win!
    CHECK(!dec.forced && dec.lethal_status != LethalStatus::Proven);
    CHECK(dec.plan.requires_replan_after_observation);
    CHECK(dec.plan.prefix.size() == 1 && dec.plan.prefix[0].type == Play && dec.plan.prefix[0].src == 0);
    CHECK(!dec.ranked.empty() && dec.ranked.front().value.has_value() && *dec.ranked.front().value > 0.95F);

    // Live execution via Search::play must stop after Insight, observe drawn kHolyFalcon, replan, evolve, and win!
    std::mt19937 rng2(777);
    Search search_play(model, model, rng2, {32, false, false, true}, &matchup);
    const auto live = search_play.play(s.state, false);
    CHECK(live.replanned);
    CHECK(live.next.outcome == Outcome::FirstWins);
    CHECK(live.next.players[1].leader_hp <= 0);

    std::cout << "PASS wb_p0_post_chance_beam_completion\n";
}

void mulligan_crn_post_deck_coupling() {
    const auto pool = neutral_bishop_rotation();
    Deck valid_deck{};
    int cursor = 0;
    valid_deck[cursor++] = kCorrector; // singleton
    for (CardId id : pool) {
        if (id == kCorrector || !is_deck_card(id) || find_card(id)->kind == Kind::Faith) continue;
        for (int c = 0; c < 3 && cursor < kDeckSize; ++c) valid_deck[cursor++] = id;
        if (cursor == kDeckSize) break;
    }
    CHECK(cursor == kDeckSize);

    auto base = new_game(valid_deck, valid_deck, 314159);
    base.players[0].hand[0] = kCorrector;
    base.players[0].hand[1] = valid_deck[1];
    base.players[0].hand[2] = valid_deck[4];
    base.players[0].hand[3] = valid_deck[7];

    int coupled_m1 = 0;
    int coupled_m15 = 0;
    int returned_singleton_drawn_t1 = 0;
    constexpr int kTrials = 256;

    for (int t = 1; t <= kTrials; ++t) {
        const std::uint64_t seed = 0xD1B54A32D192ED03ULL * static_cast<std::uint64_t>(t);
        const auto r0 = simulate_mulligan_crn(base, 0, seed);
        const auto r1 = simulate_mulligan_crn(base, 1, seed);
        const auto r15 = simulate_mulligan_crn(base, 15, seed);

        // Opponent hand/deck and post-mulligan RNG state must be 100% identical across all 16 masks
        for (int mask = 1; mask < 16; ++mask) {
            const auto rm = simulate_mulligan_crn(base, static_cast<std::uint8_t>(mask), seed);
            CHECK(rm.turn1_state.rng_state == r0.turn1_state.rng_state);
            CHECK(rm.turn1_state.players[1].hand == r0.turn1_state.players[1].hand);
            CHECK(rm.turn1_state.players[1].deck == r0.turn1_state.players[1].deck);
            CHECK(rm.turn1_state.players[0].hand_count == 5 && rm.turn1_state.players[0].deck_remaining() == 35);
        }

        // Turn-1 draw is hand[4] (5th card drawn at Turn 1 start)
        const CardId draw_m0 = r0.turn1_state.players[0].hand[4];
        const CardId draw_m1 = r1.turn1_state.players[0].hand[4];
        const CardId draw_m15 = r15.turn1_state.players[0].hand[4];
        if (draw_m1 == draw_m0) ++coupled_m1;
        if (draw_m15 == draw_m0) ++coupled_m15;
        if (draw_m1 == kCorrector) ++returned_singleton_drawn_t1;
    }

    // Theoretical coupling: 35/36 (~97.2%) for mask=1, >= 32/36 (~88.9%) for mask=15; old uncoupled was < 10%!
    CHECK(coupled_m1 >= 235);  // > 91.8%
    CHECK(coupled_m15 >= 210); // > 82.0%
    // Returned singleton can still uniformly appear on Turn 1 (~1/36 = ~7.1 out of 256)
    CHECK(returned_singleton_drawn_t1 <= 24);

    std::cout << "PASS wb_mulligan_crn_post_deck_coupling\n";
}

int attention_microbench() {
    auto model = svwb::attn::AttentionModel::random(20261125, svwb::attn::ArchVariant::V3GlobalEmbDiff);
    auto witch = kMirrorDeck;
    witch[0] = official(10832110);
    witch[1] = official(10031310);
    witch[2] = official(10131320);
    const MatchupFeatures matchup(witch, kMirrorDeck);

    struct BenchItem {
        svwb::attn::RootContext ctx;
        svwb::attn::CandidateFeatures feat;
        svwb::attn::CandidateSparseDiff diff;
    };
    std::vector<BenchItem> items;
    std::vector<GameState> turn_states;
    items.reserve(1024);

    std::mt19937 rng(7070);
    for (int g = 0; g < 16 && items.size() < 800; ++g) {
        auto state = new_game(g % 2 == 0 ? kMirrorDeck : witch, kMirrorDeck, 5000 + g);
        apply_legal_action(state, {Mulligan});
        apply_legal_action(state, {Mulligan});
        Search search(model, model, rng, {8, false, true, true}, &matchup);
        for (int t = 0; t < 16 && state.outcome == Outcome::Ongoing && items.size() < 800; ++t) {
            turn_states.push_back(state);
            const auto dec = search.choose(state, true);
            if (!dec.attention_policy.empty()) {
                svwb::attn::RootContext ctx{};
                static_cast<void>(model.evaluate_root(extract_attention_tokens(state), ctx));
                for (const auto& s : dec.attention_policy) {
                    items.push_back({ctx, s.features, s.emb_diff});
                }
            }
            state = dec.next;
        }
    }

    // Verify numerical agreement between Method A (DirectMatMul) and Method B (PreprojectedTable)
    float max_diff = 0.0F;
    double mean_entries = 0.0;
    for (const auto& it : items) {
        const float a = model.policy_logit_direct(it.ctx, it.feat.data(), &it.diff);
        const float b = model.policy_logit_preprojected(it.ctx, it.feat.data(), &it.diff);
        max_diff = std::max(max_diff, std::abs(a - b));
        mean_entries += it.diff.total_entries();
    }
    mean_entries /= static_cast<double>(items.size());
    CHECK(max_diff < 1e-5F);

    constexpr int kWarmup = 400;
    constexpr int kRepeats = 5000;
    volatile float sink = 0.0F;

    for (int r = 0; r < kWarmup; ++r) {
        float acc = 0.0F;
        for (const auto& it : items) acc += model.policy_logit_direct(it.ctx, it.feat.data(), &it.diff);
        sink = acc;
    }
    const auto t0 = std::chrono::steady_clock::now();
    float sum_direct = 0.0F;
    for (int r = 0; r < kRepeats; ++r) {
        for (const auto& it : items) sum_direct += model.policy_logit_direct(it.ctx, it.feat.data(), &it.diff);
    }
    const auto t1 = std::chrono::steady_clock::now();
    sink = sum_direct;

    for (int r = 0; r < kWarmup; ++r) {
        float acc = 0.0F;
        for (const auto& it : items) acc += model.policy_logit_preprojected(it.ctx, it.feat.data(), &it.diff);
        sink = acc;
    }
    const auto t2 = std::chrono::steady_clock::now();
    float sum_preproj = 0.0F;
    for (int r = 0; r < kRepeats; ++r) {
        for (const auto& it : items) sum_preproj += model.policy_logit_preprojected(it.ctx, it.feat.data(), &it.diff);
    }
    const auto t3 = std::chrono::steady_clock::now();
    sink = sum_preproj;
    static_cast<void>(sink);

    const double total_evals = static_cast<double>(kRepeats) * static_cast<double>(items.size());
    const double direct_sec = std::chrono::duration<double>(t1 - t0).count();
    const double preproj_sec = std::chrono::duration<double>(t3 - t2).count();
    const double direct_ns = (direct_sec * 1e9) / total_evals;
    const double preproj_ns = (preproj_sec * 1e9) / total_evals;

    // Also measure end-to-end MCTS search turn latency (16 simulations) for both kernels
    const auto bench_turn_search = [&](svwb::attn::PolicyScorerKernel kernel) {
        model.set_scorer_kernel(kernel);
        std::mt19937 srng(999);
        Search s(model, model, srng, {16, false, false, true}, &matchup);
        const auto s0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < std::min<std::size_t>(turn_states.size(), 64); ++i) {
            const auto d = s.choose(turn_states[i], false);
            sink = static_cast<float>(d.visits);
        }
        const auto s1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(s1 - s0).count() /
               static_cast<double>(std::min<std::size_t>(turn_states.size(), 64));
    };
    const double turn_ms_direct = bench_turn_search(svwb::attn::PolicyScorerKernel::DirectMatMul);
    const double turn_ms_preproj = bench_turn_search(svwb::attn::PolicyScorerKernel::PreprojectedTable);

    std::cout << std::fixed << std::setprecision(2)
              << "MICROBENCH candidates=" << items.size()
              << " avg_sparse_diff_entries=" << mean_entries
              << " total_evals=" << static_cast<std::uint64_t>(total_evals)
              << " max_abs_diff=" << std::scientific << max_diff << std::fixed << '\n'
              << "Method_A_DirectMatMul_ns_per_cand=" << direct_ns
              << " cands_per_sec=" << (total_evals / direct_sec)
              << " search16_ms_per_turn=" << turn_ms_direct << '\n'
              << "Method_B_Preprojected_ns_per_cand=" << preproj_ns
              << " cands_per_sec=" << (total_evals / preproj_sec)
              << " search16_ms_per_turn=" << turn_ms_preproj
              << " speedup_x=" << (direct_ns / preproj_ns) << '\n';
    return 0;
}

int dump_attention_parity(const std::filesystem::path& weights_path, const std::filesystem::path& out_path) {
    const auto model = svwb::attn::AttentionModel::load(weights_path);
    auto witch = kMirrorDeck;
    witch[0] = official(10832110);
    witch[1] = official(10031310);
    witch[2] = official(10131320);
    const MatchupFeatures matchup(witch, kMirrorDeck);

    struct Record {
        svwb::attn::Tokens tokens{};
        std::array<float, svwb::attn::kModel> global_cls_init{};
        std::array<float, svwb::attn::kCardTokens * svwb::attn::kModel> token_proj{};
        std::array<float, svwb::attn::kModel> final_cls{};
        float value_logit = 0.0F;
        float value_prob = 0.5F;
        std::int32_t candidate_count = 0;
        std::array<svwb::attn::CandidateFeatures, svwb::kMaxCandidates> candidates{};
        std::array<std::array<std::uint16_t, svwb::attn::kMaxSparseDiffEntries>, svwb::kMaxCandidates> diff_ids{};
        std::array<std::array<std::int8_t, svwb::attn::kMaxSparseDiffEntries>, svwb::kMaxCandidates> diff_signs{};
        std::array<std::array<std::uint8_t, svwb::attn::kDiffZones + 1>, svwb::kMaxCandidates> zone_counts{};
        std::array<svwb::attn::CandidateEmbDiff, svwb::kMaxCandidates> embdiff_dense{};
        std::array<std::array<float, svwb::attn::kPolicyHidden>, svwb::kMaxCandidates> policy_h1{};
        std::array<float, svwb::kMaxCandidates> policy_logits{};
    };

    const auto pack_record = [&](const GameState& root_state,
                                 std::span<const svwb::attn::CandidateFeatures> c_feats,
                                 std::span<const svwb::attn::CandidateSparseDiff> c_diffs) {
        Record rec{};
        rec.tokens = extract_attention_tokens_for(root_state, model);
        svwb::attn::DebugStageOutputs dbg{};
        model.debug_encode(rec.tokens, dbg);
        rec.global_cls_init = dbg.global_cls_init;
        rec.token_proj = dbg.token_proj;
        rec.final_cls = dbg.final_cls;
        rec.value_logit = dbg.value_logit;
        rec.value_prob = dbg.value_prob;

        svwb::attn::RootContext ctx{};
        const float eval_p = model.evaluate_root(rec.tokens, ctx);
        CHECK(std::abs(eval_p - rec.value_prob) < 1e-6F);

        rec.candidate_count = static_cast<std::int32_t>(
            std::min<std::size_t>(c_feats.size(), svwb::kMaxCandidates));
        for (int k = 0; k < rec.candidate_count; ++k) {
            rec.candidates[k] = c_feats[k];
            const auto& sp = c_diffs[k];
            int total = 0;
            for (int z = 0; z < svwb::attn::kDiffZones; ++z) {
                const int b = sp.zone_offsets[z];
                const int e = sp.zone_offsets[z + 1];
                const int zc = e - b;
                rec.zone_counts[k][z] = static_cast<std::uint8_t>(zc);
                const int base_off = svwb::attn::kZoneDiffSliceOffsets[z];
                for (int j = 0; j < zc; ++j) {
                    rec.diff_ids[k][base_off + j] = sp.entries[b + j].vocab_id;
                    rec.diff_signs[k][base_off + j] = sp.entries[b + j].signed_count;
                }
                total += zc;
            }
            rec.zone_counts[k][svwb::attn::kDiffZones] = static_cast<std::uint8_t>(total);
            model.debug_policy(ctx, rec.candidates[k].data(), &sp,
                               rec.embdiff_dense[k], rec.policy_h1[k], rec.policy_logits[k]);
        }
        return rec;
    };

    std::vector<Record> records;

    // Add all 8 mandatory boundary test cases explicitly (Spec 4.4 / AC-D-4):
    // (1) Multiple copies of same card in hand + (3) Empty diff (full pass) + (4) Normal evolve + (5) Super evolve
    {
        Scenario s;
        s.state.turn_number = 7;
        s.player(0).pp = s.player(0).max_pp = 10;
        s.player(0).ep = 1;
        s.player(0).sep = 1;
        s.hand({kLilium, kLilium, kLilium, kTrainer});
        s.place(kTrainer, 0);
        s.place(kColette, 1);

        std::vector<svwb::attn::CandidateFeatures> feats;
        std::vector<svwb::attn::CandidateSparseDiff> diffs;
        // Pass (empty diff)
        {
            auto next = s.state;
            apply_legal_action(next, {EndTurn});
            feats.push_back(extract_candidate_features(s.state, s.state, next));
            diffs.push_back(extract_candidate_sparse_diff(s.state, s.state));
        }
        // Normal evolve
        {
            auto dec = s.state;
            apply_legal_action(dec, {Evolve, 0});
            auto next = dec;
            apply_legal_action(next, {EndTurn});
            feats.push_back(extract_candidate_features(s.state, dec, next));
            diffs.push_back(extract_candidate_sparse_diff(s.state, dec));
        }
        // Super evolve
        {
            auto dec = s.state;
            apply_legal_action(dec, {SuperEvolve, 0});
            auto next = dec;
            apply_legal_action(next, {EndTurn});
            feats.push_back(extract_candidate_features(s.state, dec, next));
            diffs.push_back(extract_candidate_sparse_diff(s.state, dec));
        }
        // Play 1 copy of 3-copy kLilium
        {
            auto dec = s.state;
            apply_legal_action(dec, {Play, 0});
            auto next = dec;
            apply_legal_action(next, {EndTurn});
            feats.push_back(extract_candidate_features(s.state, dec, next));
            diffs.push_back(extract_candidate_sparse_diff(s.state, dec));
        }
        records.push_back(pack_record(s.state, feats, diffs));
    }

    // (2) Consume 1 copy + draw 1 copy of same card + (6) Transform + (7) Crest disappearance + (8) Random draw
    {
        Scenario s;
        s.state.turn_number = 7;
        s.player(0).pp = s.player(0).max_pp = 10;
        s.hand({kEvidence, official(10031310), kTrainer});
        s.player(0).deck[0] = kEvidence;
        s.player(0).deck[1] = kLilium;
        s.player(0).deck_idx = 0;
        s.player(0).deck_end = 2;
        s.place(kColette, 1);
        s.crest(CrestId::Testimony, 1, 0);

        std::vector<svwb::attn::CandidateFeatures> feats;
        std::vector<svwb::attn::CandidateSparseDiff> diffs;
        // Consume 1 kEvidence + draw 1 kEvidence (net 0 for kEvidence in hand!)
        {
            auto dec = s.state;
            apply_legal_action(dec, {Play, 0}); // plays kEvidence -> board
            apply_legal_action(dec, {Act, 0});  // acts kEvidence -> draws deck[0]==kEvidence & destroys board kEvidence!
            auto next = dec;
            apply_legal_action(next, {EndTurn});
            feats.push_back(extract_candidate_features(s.state, dec, next));
            diffs.push_back(extract_candidate_sparse_diff(s.state, dec));
        }
        // Transform enemy permanent + Crest disappearance + Random draw
        {
            auto dec = s.state;
            dec.players[1].board[0].card_id = kRabbit; // transformed on board
            dec.players[0].crest_count = 0;            // crest disappeared before EndTurn
            apply_legal_action(dec, {Play, 1});        // Insight random draw
            auto next = dec;
            apply_legal_action(next, {EndTurn});
            feats.push_back(extract_candidate_features(s.state, dec, next));
            diffs.push_back(extract_candidate_sparse_diff(s.state, dec));
        }
        records.push_back(pack_record(s.state, feats, diffs));
    }

    // Full-game positions across multiple matchups
    std::mt19937 rng(20261124);
    for (int game_idx = 0; game_idx < 6 && records.size() < 32; ++game_idx) {
        auto state = new_game(game_idx % 2 == 0 ? kMirrorDeck : witch, kMirrorDeck, 1000 + game_idx);
        apply_legal_action(state, {Mulligan});
        apply_legal_action(state, {Mulligan});
        if (game_idx == 1) {
            state.players[0].crests[0] = {500, CrestId::Testimony, 2, 0, 0};
            state.players[0].crest_count = 1;
            state.players[1].faiths[0] = kFaithToken;
            state.players[1].faith_values[0] = 5;
            state.players[1].faith_upgrades[0] = 1;
            state.players[1].faith_count = 1;
        }
        Search search(model, model, rng, {8, false, true, true}, &matchup);
        for (int turn = 0; turn < 14 && state.outcome == Outcome::Ongoing && records.size() < 32; ++turn) {
            const auto decision = search.choose(state, true);
            if (!decision.forced && !decision.attention_policy.empty()) {
                const auto& root_state = decision.sampled_root ? *decision.sampled_root : state;
                std::vector<svwb::attn::CandidateFeatures> c_feats;
                std::vector<svwb::attn::CandidateSparseDiff> c_diffs;
                for (const auto& s : decision.attention_policy) {
                    c_feats.push_back(s.features);
                    c_diffs.push_back(s.emb_diff);
                }
                records.push_back(pack_record(root_state, c_feats, c_diffs));
            }
            for (const auto action : decision.actions) {
                if (!apply_action(state, action)) break;
                if (state.outcome != Outcome::Ongoing) break;
            }
            if (state.outcome == Outcome::Ongoing && state.turn_player == (decision.sampled_root ? decision.sampled_root->turn_player : 0)) {
                apply_legal_action(state, {EndTurn});
            }
        }
    }
    CHECK(records.size() >= 24);
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    CHECK(static_cast<bool>(out));
    const std::array<std::int32_t, 4> hdr{
        static_cast<std::int32_t>(records.size()),
        model.global_dim(),
        svwb::attn::kCandidateDim,
        model.candidate_embdiff_dim(),
    };
    out.write(reinterpret_cast<const char*>(hdr.data()), sizeof(hdr));
    for (const auto& rec : records) {
        out.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
    }
    out.close();
    std::cout << "Dumped " << records.size() << " attention 3-stage parity positions\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::cout << "GameState_size_bytes=" << sizeof(GameState) << '\n';
    if (argc == 4 && std::string_view(argv[1]) == "--attention-parity-dump") {
        try {
            return dump_attention_parity(argv[2], argv[3]);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 5 && std::string_view(argv[1]) == "--deck-pair-stress") {
        try {
            return stress_deck_pair(argv[2], argv[3], argv[4]);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 1) {
        return units();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--stress") {
        return stress();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--rotation-unit") return rotation_units();
    if (argc == 2 && std::string_view(argv[1]) == "--rotation-stress")
        return stress_rotation(neutral_bishop_rotation(), 10'000, 0x535657425742ULL, "rotation_mirror");
    if (argc == 2 && std::string_view(argv[1]) == "--elf-royal-unit") return elf_royal_units();
    if (argc == 2 && std::string_view(argv[1]) == "--elf-stress")
        return stress_with_neutral(elf_rotation(), 0x454c46ULL, "elf_neutral_mirror");
    if (argc == 2 && std::string_view(argv[1]) == "--royal-stress")
        return stress_with_neutral(royal_rotation(), 0x524f59414cULL, "royal_neutral_mirror");
    if (argc == 2 && std::string_view(argv[1]) == "--dragon-nightmare-unit") return dragon_nightmare_units();
    if (argc == 2 && std::string_view(argv[1]) == "--dragon-stress")
        return stress_with_neutral(dragon_rotation(), 0x445241474f4eULL, "dragon_neutral_mirror");
    if (argc == 2 && std::string_view(argv[1]) == "--nightmare-stress")
        return stress_with_neutral(nightmare_rotation(), 0x4e49474854ULL, "nightmare_neutral_mirror");
    if (argc == 2 && std::string_view(argv[1]) == "--witch-nemesis-unit") return witch_nemesis_units();
    if (argc == 2 && std::string_view(argv[1]) == "--witch-stress")
        return stress_with_neutral(witch_rotation(), 0x5749544348ULL, "witch_neutral_mirror");
    if (argc == 2 && std::string_view(argv[1]) == "--nemesis-stress")
        return stress_with_neutral(nemesis_rotation(), 0x4e454d4553ULL, "nemesis_neutral_mirror");
    if (argc == 2 && std::string_view(argv[1]) == "--rotation-registry") return verify_rotation_registry();
    if (argc == 2 && std::string_view(argv[1]) == "--full-rotation-stress")
        return stress_rotation(all_rotation(), 10'000, 0x35364234ULL, "full_rotation_mirror");
    if (argc == 2 && std::string_view(argv[1]) == "--attention-microbench") return attention_microbench();
    if (argc == 2) {
        const std::string_view mode(argv[1]);
        try {
            if (mode == "--actor-audit") actor_audit();
            else if (mode == "--attention-audit") attention_audit();
            else if (mode == "--canonicalization") search_canonicalization();
            else if (mode == "--forced-lethal") search_forced_lethal();
            else if (mode == "--determinization") determinization_audit();
            else if (mode == "--mcts-backup") mcts_root_perspective_backup();
            else if (mode == "--mulligan-crn") mulligan_crn_and_exclusion();
            else if (mode == "--hidden-invariance") attention_hidden_state_invariance();
            else if (mode == "--strategy-fusion") mcts_strategy_fusion_expected_value();
            else if (mode == "--hidden-pool") attention_hidden_pool_consistency();
            else if (mode == "--opponent-isolation") attention_opponent_private_info_isolation();
            else if (mode == "--stochastic-lethal") attention_stochastic_lethal_not_forced();
            else if (mode == "--global64-unit") attention_global64_turn_boundaries();
            else if (mode == "--embdiff-migration-unit") attention_embdiff160_and_migration();
            else if (mode == "--post-chance-beam") p0_post_chance_beam_completion();
            else if (mode == "--mulligan-crn-coupling") mulligan_crn_post_deck_coupling();
            else if (mode == "--high-cost-unit") {
                high_cost_cards();
                high_cost_combat();
                high_cost_leader_and_summon();
                high_cost_supplements();
                std::cout << "PASS high_cost cards=47 tokens=3 crests=3 faiths=2\n";
            }
            else throw std::invalid_argument("Unknown test_wb mode");
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    std::cerr << "Usage: test_wb [--stress|--rotation-unit|--rotation-stress|--elf-royal-unit|--elf-stress|--royal-stress"
                 "|--dragon-nightmare-unit|--dragon-stress|--nightmare-stress|--witch-nemesis-unit|--witch-stress"
                 "|--nemesis-stress|--rotation-registry|--full-rotation-stress|--actor-audit|--canonicalization"
                 "|--forced-lethal|--high-cost-unit|--deck-pair-stress DECK_A DECK_B GAMES]\n";
    return 2;
}
