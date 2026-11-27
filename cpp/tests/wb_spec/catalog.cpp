#include "spec.hpp"

#include <cstdint>

namespace svwb::wb::spec {
namespace {

struct Printed {
    std::uint32_t id;
    Kind kind;
    std::uint16_t abilities;
    int cost;
    int attack;
    int defense;
    bool rotation;
};

enum class Event { Play, Death, Evolve, Super, Enter, TurnEnd, TurnStart };
enum class Check { Draw, Heal, FaceDamage, ChosenDamage, AllEnemyDamage, AddHand, Summon };

CardId filler() {
    return official(90021110); // Knight, with no triggered abilities.
}

bool can_play(const Scenario& scenario) {
    return std::ranges::any_of(legal_actions(scenario.state, scenario.rules), [](Action action) {
        return action.type == Play && action.src == 0;
    });
}

void play(Scenario& scenario) {
    const auto actions = legal_actions(scenario.state, scenario.rules);
    const auto action = std::ranges::find_if(actions, [](Action candidate) {
        return candidate.type == Play && candidate.src == 0;
    });
    CHECK(action != actions.end());
    scenario.run(*action);
}

void add_inventory(Suite& suite, int expected_cards, int expected_crests) {
    suite.add("catalog.inventory", [=] {
        int cards = 0;
        for (int id = kFirstCard; id <= UINT16_MAX; ++id) {
            const auto* card = find_card(static_cast<CardId>(id));
            if (!card) {
                break;
            }
            cards += card->implemented;
        }
        int crests = 0;
        for (int id = 1; id <= UINT8_MAX; ++id) {
            crests += find_crest(static_cast<CrestId>(id)) != nullptr;
        }
        CHECK(cards == expected_cards && crests == expected_crests);
    });
}

void add_registry(Suite& suite, std::string name, CardId engine_id, std::uint32_t official_id) {
    suite.add(name + ".registry", [=] {
        const auto* card = find_card(engine_id);
        CHECK(card && card->implemented && card->id == engine_id);
        CHECK(card->official_id == official_id);
        if (official_id != 0) {
            CHECK(find_official_card(official_id) == card);
        }
    });
}

void add_crest(Suite& suite, std::string name, CrestId id, std::uint32_t official_id,
               int countdown, bool verified_countdown) {
    suite.add(name + ".registry", [=] {
        const auto* crest = find_crest(id);
        CHECK(crest && crest->id == id && crest->official_id == official_id);
    });
    if (verified_countdown) {
        suite.add(name + ".printed-countdown", [=] {
            const auto* crest = find_crest(id);
            CHECK(crest && crest->countdown == countdown);
        });
    }
}

void add_printed_data(Suite& suite, std::string name, Printed printed) {
    suite.add(std::move(name), [=] {
        const auto* card = find_official_card(printed.id);
        CHECK(card);
        CHECK(card->kind == printed.kind);
        CHECK(card->cost == printed.cost);
        CHECK(card->atk == printed.attack);
        CHECK(card->hp == printed.defense);
        CHECK((card->abilities & printed.abilities) == printed.abilities);
        CHECK(is_deck_card(card->id) == printed.rotation);
    });
}

void add_below_cost(Suite& suite, std::string name, Printed printed, int minimum) {
    suite.add(std::move(name), [=] {
        Scenario scenario;
        scenario.hand({official(printed.id)});
        scenario.place(filler());
        scenario.place(filler(), 1);
        scenario.player().pp = static_cast<std::uint8_t>(minimum - 1);
        CHECK(!can_play(scenario));
        scenario.reject({Play, 0});
    });
}

void add_printed_amulet_countdown(Suite& suite, std::string name, Printed printed, int count) {
    suite.add(std::move(name), [=] {
        CHECK(find_official_card(printed.id)->countdown == count);
    });
}

void add_countdown_advance(Suite& suite, std::string name, Printed printed, int count) {
    suite.add(std::move(name), [=] {
        Scenario scenario;
        const auto source = scenario.place(official(printed.id)).creation_order;
        const auto& player = scenario.player();
        const auto locate = [&] {
            return std::ranges::find_if(player.board.begin(), player.board.begin() + player.board_count,
                                       [source](const Permanent& object) {
                                           return object.creation_order == source;
                                       });
        };
        for (int remaining = count; remaining > 0; --remaining) {
            const auto entry = locate();
            CHECK(entry != player.board.begin() + player.board_count);
            CHECK(entry->countdown == remaining);
            scenario.next_own_turn();
        }
        CHECK(locate() == player.board.begin() + player.board_count);
    });
}

void add_amulet_combat(Suite& suite, std::string name, Printed printed) {
    suite.add(std::move(name), [=] {
        Scenario scenario;
        scenario.place(official(printed.id));
        scenario.place(filler(), 1);
        scenario.reject({Attack, 0, 0});
        scenario.reject({Attack, 0, kEnemyLeader});
        scenario.state.turn_player = 1;
        scenario.reject({Attack, 0, 0});
        CHECK(scenario.legal({Attack, 0, kEnemyLeader}));
    });
}

void add_form(Suite& suite, std::string name, std::uint32_t form_id, std::uint32_t base_id,
              Kind kind, int cost, int countdown) {
    suite.add(name + ".form-data", [=] {
        const auto form = official(form_id);
        const auto* definition = find_card(form);
        CHECK(definition->kind == kind && definition->cost == cost);
        CHECK(definition->countdown == countdown);
        CHECK(printed_card(form) == official(base_id));
        CHECK(!is_deck_card(form));
    });
    suite.add(name + ".form-play", [=] {
        Scenario scenario;
        scenario.hand({official(base_id)});
        scenario.place(filler(), 1);
        scenario.player().pp = static_cast<std::uint8_t>(cost);
        play(scenario);
        CHECK(scenario.player().plays_this_turn == 1);
        CHECK(scenario.player().available_pp() == 0);
        if (kind == Kind::Amulet) {
            CHECK(scenario.player().board_count == 1);
            CHECK(scenario.player().board[0].card_id == official(form_id));
            CHECK(scenario.player().board[0].countdown == countdown);
        } else {
            CHECK(scenario.player().shadows >= 1);
        }
    });
}

void add_full_board(Suite& suite, std::string name, Printed printed) {
    suite.add(std::move(name), [=] {
        Scenario scenario;
        scenario.hand({official(printed.id)});
        scenario.player().pp = static_cast<std::uint8_t>(printed.cost);
        for (int i = 0; i < kBoardLimit; ++i) {
            scenario.place(filler());
        }
        CHECK(!can_play(scenario));
        scenario.reject({Play, 0});
    });
}

void add_play(Suite& suite, std::string name, Printed printed) {
    suite.add(std::move(name), [=] {
        Scenario scenario;
        const auto id = official(printed.id);
        scenario.hand({id});
        scenario.place(filler());
        scenario.place(filler(), 1);
        scenario.player().pp = static_cast<std::uint8_t>(printed.cost);
        play(scenario);
        CHECK(scenario.player().plays_this_turn >= 1);
        CHECK(scenario.player().board_count <= kBoardLimit);
        CHECK(std::ranges::find(scenario.player().hand.begin(),
                               scenario.player().hand.begin() + scenario.player().hand_count,
                               id) == scenario.player().hand.begin() + scenario.player().hand_count);
    });
}

void add_cannot_play(Suite& suite, std::string name, Printed printed) {
    suite.add(std::move(name), [=] {
        Scenario scenario;
        scenario.hand({official(printed.id)});
        CHECK(!can_play(scenario));
        scenario.reject({Play, 0});
    });
}

void add_play_spell(Suite& suite, std::string name, Printed printed) {
    suite.add(std::move(name), [=] {
        Scenario scenario;
        const auto id = official(printed.id);
        scenario.hand({id, filler()});
        scenario.place(filler());
        scenario.place(filler(), 1);
        scenario.player().pp = static_cast<std::uint8_t>(printed.cost);
        play(scenario);
        CHECK(scenario.player().plays_this_turn >= 1);
        CHECK(scenario.player().board_count <= kBoardLimit);
    });
}

void add_no_target(Suite& suite, std::string name, Printed printed) {
    suite.add(std::move(name), [=] {
        Scenario scenario;
        scenario.hand({official(printed.id)});
        scenario.player().pp = static_cast<std::uint8_t>(printed.cost);
        if (printed.kind == Kind::Spell) {
            CHECK(!can_play(scenario));
            scenario.reject({Play, 0});
        } else {
            scenario.run({Play, 0});
            CHECK(scenario.player().board_count == 1);
            CHECK(scenario.player().board[0].card_id == official(printed.id));
        }
        CHECK(scenario.player(1).leader_hp == 20);
    });
}

void add_keyword(Suite& suite, std::string name, Printed printed, std::uint16_t keyword) {
    // The printed keyword and the common behavior are documented in the Cygames help.
    // https://shadowverse-wb.com/ja/help/?tab=tab0
    suite.add(std::move(name), [=] {
        Scenario scenario;
        auto& subject = scenario.place(official(printed.id));
        switch (keyword) {
        case FLAG_STORM:
        case FLAG_RUSH:
            subject.readied = false;
            scenario.place(filler(), 1);
            CHECK(scenario.legal({Attack, 0, 0}));
            CHECK(scenario.legal({Attack, 0, kEnemyLeader}) == ((printed.abilities & FLAG_STORM) != 0));
            break;
        case FLAG_WARD: {
            scenario.place(filler());
            scenario.place(filler(), 1);
            scenario.state.turn_player = 1;
            const bool targetable = !(printed.abilities & (kAmbush | kIntimidate));
            CHECK(scenario.legal({Attack, 0, 0}) == targetable);
            CHECK(scenario.legal({Attack, 0, 1}) != targetable);
            CHECK(scenario.legal({Attack, 0, kEnemyLeader}) != targetable);
            break;
        }
        case kBane: {
            auto& defender = scenario.place(filler(), 1);
            defender.hp = defender.max_hp = 30;
            scenario.run({Attack, 0, 0});
            CHECK(scenario.player(1).board_count == 0);
            CHECK(scenario.player(1).shadows == 1);
            break;
        }
        case kDrain:
            scenario.player().leader_hp = 1;
            scenario.run({Attack, 0, kEnemyLeader});
            CHECK(scenario.player(1).leader_hp == 20 - printed.attack);
            CHECK(scenario.player().leader_hp == 1 + printed.attack);
            break;
        case kAmbush:
        case kIntimidate:
        case kAura: {
            scenario.place(filler());
            scenario.place(filler(), 1);
            scenario.hand({official(90021310)}, 1); // Golden Dagger chooses a follower or leader.
            scenario.state.turn_player = 1;
            const bool targetable = !(printed.abilities & (kAmbush | kIntimidate));
            CHECK(scenario.legal({Attack, 0, 0}) == targetable);
            CHECK(scenario.legal({Attack, 0, 1}) == (!targetable || !(printed.abilities & FLAG_WARD)));
            CHECK(scenario.legal({Play, 0, 0}) == !(printed.abilities & (kAura | kAmbush)));
            CHECK(scenario.legal({Play, 0, 1}));
            break;
        }
        }
    });
}

void add_attack_window(Suite& suite, std::string name, Printed printed) {
    suite.add(std::move(name), [=] {
        Scenario scenario;
        scenario.hand({official(printed.id)});
        scenario.place(filler(), 1);
        play(scenario);
        CHECK(scenario.player().board_count == 1);
        const auto& follower = scenario.player().board[0];
        CHECK(follower.atk == printed.attack && follower.hp == printed.defense);
        const bool storm = (printed.abilities & FLAG_STORM) != 0;
        const bool rush = (printed.abilities & FLAG_RUSH) != 0;
        CHECK(scenario.legal({Attack, 0, kEnemyLeader}) == storm);
        CHECK(scenario.legal({Attack, 0, 0}) == (storm || rush));
        scenario.next_own_turn();
        CHECK(scenario.legal({Attack, 0, kEnemyLeader}));
        scenario.run({Attack, 0, kEnemyLeader});
        CHECK(scenario.player(1).leader_hp == 20 - printed.attack);
        scenario.reject({Attack, 0, kEnemyLeader});
    });
}

void add_combat_destruction(Suite& suite, std::string name, Printed printed) {
    suite.add(std::move(name), [=] {
        Scenario scenario;
        scenario.hand({official(printed.id)});
        play(scenario);
        auto& attacker = scenario.place(filler(), 1);
        attacker.atk = static_cast<std::int16_t>(printed.defense);
        attacker.hp = attacker.max_hp = 20;
        scenario.run({EndTurn});
        scenario.run({Attack, 0, 0});
        CHECK(scenario.player().board_count == 0 && scenario.player().shadows == 1);
        if (!(printed.abilities & kBane)) {
            CHECK(scenario.player(1).board[0].hp == 20 - printed.attack);
        }
    });
}

void add_intimidate(Suite& suite, std::string name, Printed printed) {
    suite.add(std::move(name), [=] {
        Scenario scenario;
        scenario.hand({official(printed.id)});
        play(scenario);
        scenario.place(filler(), 1);
        scenario.run({EndTurn});
        scenario.reject({Attack, 0, 0});
        CHECK(scenario.legal({Attack, 0, kEnemyLeader}));
    });
}

void add_summon_board_cap(Suite& suite, std::string name, Printed printed) {
    suite.add(std::move(name), [=] {
        Scenario scenario;
        scenario.hand({official(printed.id)});
        scenario.player().pp = static_cast<std::uint8_t>(printed.cost);
        const int initial = kBoardLimit - (printed.kind != Kind::Spell);
        for (int slot = 0; slot < initial; ++slot) scenario.place(filler());
        const auto before = scenario.player().board;
        play(scenario);
        CHECK(scenario.player().board_count == kBoardLimit);
        for (int slot = 0; slot < initial; ++slot) {
            CHECK(scenario.player().board[slot].creation_order == before[slot].creation_order);
        }
        if (printed.kind != Kind::Spell) {
            CHECK(scenario.player().board[kBoardLimit - 1].card_id == official(printed.id));
        }
        CHECK(scenario.player().shadows == (printed.kind == Kind::Spell ? 1 : 0));
    });
}

void add_hand_cap(Suite& suite, std::string name, Printed printed, Check effect, int amount, std::uint32_t recipient) {
    suite.add(std::move(name), [=] {
        Scenario scenario;
        auto& player = scenario.player();
        const bool draw = effect == Check::Draw;
        const auto incoming = draw ? kAlmiraj : official(recipient);
        const auto other = incoming == filler() ? kTrainer : filler();
        player.hand.fill(other);
        player.hand[0] = official(printed.id);
        player.hand_count = kHandLimit;
        player.pp = static_cast<std::uint8_t>(printed.cost);
        const auto cursor = player.deck_idx;
        player.deck[cursor] = incoming;
        play(scenario);
        CHECK(player.hand_count == kHandLimit && player.hand[kHandLimit - 1] == incoming);
        CHECK(player.deck_idx == cursor + (draw ? amount : 0));
        CHECK(player.shadows == (printed.kind == Kind::Spell ? 1 : 0) + amount - 1);
    });
}

void add_heal_cap(Suite& suite, std::string name, Printed printed) {
    suite.add(std::move(name), [=] {
        Scenario scenario;
        scenario.hand({official(printed.id)});
        scenario.player().pp = static_cast<std::uint8_t>(printed.cost);
        scenario.player().leader_hp = 19;
        play(scenario);
        CHECK(scenario.player().leader_hp == 20);
    });
}

void fire(Scenario& scenario, CardId id, Event event, bool choose_enemy) {
    if (event == Event::Play) {
        scenario.hand({id});
        scenario.run({Play, 0, choose_enemy ? std::int8_t{0} : kNoTarget});
        return;
    }
    if (event == Event::Enter) {
        const Effect summon{.trigger = Trigger::None, .operation = Operation::Summon,
                            .amount = 1, .card = id};
        resolve_effect_steps(scenario.state, 0, std::span(&summon, 1), 0, 0, scenario.rules);
        return;
    }
    const auto source = scenario.place(id).creation_order;
    if (event == Event::TurnStart || event == Event::TurnEnd) {
        if (event == Event::TurnStart) {
            scenario.state.turn_player = 1;
        }
        scenario.run({EndTurn});
        return;
    }
    if (event == Event::Death) {
        const Effect destruction{.trigger = Trigger::None, .operation = Operation::Destroy,
                                 .target = Target::ChosenEnemyPermanent};
        resolve_effect_steps(scenario.state, 1, std::span(&destruction, 1), 0, source,
                             scenario.rules);
        return;
    }
    scenario.run({event == Event::Evolve ? Evolve : SuperEvolve, 0,
                  choose_enemy ? std::int8_t{0} : kNoTarget});
}

void add_effect(Suite& suite, std::string name, Printed printed, Event event,
                Check effect, int amount, std::uint32_t recipient) {
    suite.add(std::move(name), [=] {
        Scenario scenario;
        const auto id = official(printed.id);
        scenario.player().pp = static_cast<std::uint8_t>(printed.cost);
        scenario.player().leader_hp = 10;
        if (effect == Check::ChosenDamage || effect == Check::AllEnemyDamage) {
            auto& target = scenario.place(filler(), 1);
            target.hp = target.max_hp = 20; // Observe the full damage rather than saturating at death.
            if (effect == Check::ChosenDamage && event != Event::Super) {
                auto& other = scenario.place(filler(), 1);
                other.hp = other.max_hp = 20;
            }
        }
        const auto deck_top = scenario.player().deck_idx;
        fire(scenario, id, event, effect == Check::ChosenDamage);
        switch (effect) {
        case Check::Draw:
            CHECK(scenario.player().deck_idx == deck_top + amount);
            CHECK(scenario.player().hand_count == amount);
            for (int i = 0; i < amount; ++i) {
                CHECK(scenario.player().hand[i] == scenario.player().deck[deck_top + i]);
            }
            break;
        case Check::Heal:
            CHECK(scenario.player().leader_hp == std::min(20, 10 + amount));
            break;
        case Check::FaceDamage:
            CHECK(scenario.player(1).leader_hp == 20 - amount);
            break;
        case Check::ChosenDamage:
        case Check::AllEnemyDamage:
            CHECK(scenario.player(1).board_count >= 1 && scenario.player(1).board[0].hp == 20 - amount);
            if (effect == Check::ChosenDamage && event != Event::Super) {
                CHECK(scenario.player(1).board_count == 2 && scenario.player(1).board[1].hp == 20);
            }
            CHECK(scenario.player(1).leader_hp == 20);
            break;
        case Check::AddHand: {
            const auto result = official(recipient);
            CHECK(std::count(scenario.player().hand.begin(),
                             scenario.player().hand.begin() + scenario.player().hand_count, result) == amount);
            break;
        }
        case Check::Summon: {
            const auto result = official(recipient);
            const auto& board = scenario.player().board;
            const int original = result == id && printed.kind != Kind::Spell && event != Event::Death ? 1 : 0;
            CHECK(std::count_if(board.begin(), board.begin() + scenario.player().board_count,
                                [result](const Permanent& card) { return card.card_id == result; }) == amount + original);
            break;
        }
        }
    });
}

} // namespace

void catalog(Suite& suite) {
#include "official_spec.inc"
}

} // namespace svwb::wb::spec
