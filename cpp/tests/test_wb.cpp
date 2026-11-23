#include "wb_cards.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <random>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace svwb::wb;
using enum ActionType;
using svwb::CardId;
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
                  static_cast<std::uint8_t>(card->countdown), 1, Evolution::None, false, true, false, false};
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
        CHECK(std::is_permutation(first.deck.begin(), first.deck.end(), kProvisionalMirror.begin()));
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

    Scenario late;
    late.state.turn_player = 1;
    late.state.turn_number = 5;
    late.next_own_turn();
    late.run({ExtraPp});
    CHECK(late.player(1).max_pp == 10 && late.player(1).available_pp() == 11);
    late.next_own_turn();
    late.reject({ExtraPp});
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

    for (const auto& [rule, outcome] : {
            std::pair{SimultaneousLethal::SourceOpponent, Outcome::FirstWins},
            std::pair{SimultaneousLethal::NonActive, Outcome::SecondWins},
            std::pair{SimultaneousLethal::BothLose, Outcome::BothLose}}) {
        Scenario simultaneous;
        simultaneous.rules.simultaneous_lethal = rule;
        simultaneous.player().leader_hp = simultaneous.player(1).leader_hp = 3;
        const std::array script{
            Effect{Trigger::None, Operation::Damage, Target::BothLeaders, 3},
            Effect{Trigger::None, Operation::Heal, Target::OwnLeader, 5},
        };
        resolve_effect_steps(simultaneous.state, 1, script, 0, 0, simultaneous.rules);
        CHECK(simultaneous.state.outcome == outcome && simultaneous.state.end_reason == EndReason::Health);
        CHECK(simultaneous.player().leader_hp == 0 && simultaneous.player(1).leader_hp == 0);
    }
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
    std::cout << kUnits.size() - failures << '/' << kUnits.size() << " Phase 4A unit groups passed\n";
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

} // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        return units();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--stress") {
        return stress();
    }
    std::cerr << "Usage: test_wb [--stress]\n";
    return 2;
}
