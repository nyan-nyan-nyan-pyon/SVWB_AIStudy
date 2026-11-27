#include "spec.hpp"

#include <array>

namespace svwb::wb::spec {
namespace {

CardId knight() { return official(90021110); }

void seed_draw(Scenario& s) {
    auto& owner = s.player();
    owner.deck_idx = 0;
    owner.deck_end = 1;
    owner.deck[0] = knight();
}

void add_puppet_expiry(Suite& suite, std::uint32_t id) {
    suite.add("phase2_tokens." + std::to_string(id) + ".destroyed_at_enemy_end", [=] {
        Scenario s;
        s.hand({official(id)});
        s.run({Play, 0});
        s.run({EndTurn});
        CHECK(s.player().board_count == 1 && s.player().shadows == 0);
        s.run({EndTurn});
        CHECK(s.player().board_count == 0 && s.player().shadows == 1);
    });
}

void add_artifact_fusion(Suite& suite, std::uint32_t core, std::uint32_t donor, std::uint32_t form) {
    suite.add("phase2_tokens." + std::to_string(core) + ".core_accepts_only_artifact_amulet", [=] {
        Scenario s;
        s.hand({official(core), official(donor), official(90071130)});
        s.reject({Play, 0});
        s.reject({Fuse, 0, kNoTarget, 2});
        s.run({Fuse, 0, kNoTarget, 1});
        CHECK(s.player().hand_count == 2 && s.player().hand[0] == official(form));
        CHECK(s.player().hand[1] == official(90071130) && s.player().shadows == 0);
    });
    suite.add("phase2_tokens." + std::to_string(form) + ".material_cost_boundaries", [=] {
        constexpr std::array donors{90071130U, 90071130U, 90071150U, 90074110U};
        constexpr std::array masks{1, 3, 4, 8};
        constexpr std::array results{90073110U, 90073120U, 90073130U, 90073130U};
        for (std::size_t i = 0; i < masks.size(); ++i) {
            Scenario s;
            s.hand({official(form), official(donors[0]), official(donors[1]),
                    official(donors[2]), official(donors[3]), knight()});
            s.reject({Fuse, 0, kNoTarget, 16});
            s.run({Fuse, 0, kNoTarget, static_cast<std::uint8_t>(masks[i])});
            CHECK(s.player().hand[0] == official(results[i]));
            CHECK(s.player().pp == 10 && s.player().shadows == 0);
        }
    });
}

} // namespace

void phase2_tokens(Suite& suite) {
    suite.add("phase2_tokens.90011120.evolves_only_at_own_turn_end", [] {
        Scenario s;
        s.place(official(90011120));
        s.state.turn_player = 1;
        const auto ep = s.player().ep;
        s.run({EndTurn});
        CHECK(s.player().board[0].evolution == Evolution::None);
        s.run({EndTurn});
        CHECK(s.player().board[0].evolution == Evolution::Normal && s.player().ep == ep);
    });
    suite.add("phase2_tokens.90051130.own_end_banishes_without_a_shadow", [] {
        Scenario s;
        s.place(official(90051130));
        s.state.turn_player = 1;
        s.run({EndTurn});
        CHECK(s.player().board_count == 1 && s.player().shadows == 0);
        s.run({EndTurn});
        CHECK(s.player().board_count == 0 && s.player().shadows == 0);
    });
    suite.add("phase2_tokens.90051140.replacement_dies_without_another_replacement", [] {
        Scenario s;
        const auto zombie = official(90051140);
        const auto first = s.place(zombie).creation_order;
        s.place(knight(), 1).atk = 10;
        s.place(knight(), 1).atk = 10;
        s.run({EndTurn});
        s.run({Attack, 0, 0});
        CHECK(s.player().board_count == 1 && s.player().shadows == 1);
        CHECK(s.player().board[0].card_id == zombie && s.player().board[0].creation_order != first);
        s.run({Attack, 0, 0});
        CHECK(s.player().board_count == 0 && s.player().shadows == 2);
    });
    add_puppet_expiry(suite, 90071110);
    add_puppet_expiry(suite, 90071120);

    suite.add("phase2_tokens.90001210.act_draws_and_consumes_last_evidence", [] {
        Scenario s;
        seed_draw(s);
        s.place(official(90001210)).clue_count = 1;
        s.player().pp = 0;
        s.reject({Act, 0});
        s.player().pp = 1;
        s.run({Act, 0});
        CHECK(s.player().available_pp() == 0 && s.player().board_count == 0);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == knight());
    });
    suite.add("phase2_tokens.90031210.act_adds_one_earth_and_costs_one_pp", [] {
        Scenario s;
        s.place(official(90031210)).earth_count = 1;
        s.player().pp = 1;
        s.run({Act, 0});
        CHECK(s.player().available_pp() == 0 && s.player().board_count == 1);
        CHECK(s.player().board[0].earth_count == 2);
        s.reject({Act, 0});
    });
    suite.add("phase2_tokens.90014330.evolves_an_unevolved_ally_without_ep", [] {
        Scenario s;
        s.hand({official(90014330)});
        s.reject({Play, 0});
        s.place(knight()).evolution = Evolution::Normal;
        s.place(knight());
        s.player().ep = 0;
        s.reject({Play, 0, kOwnBoardTarget});
        s.run({Play, 0, kOwnBoardTarget + 1});
        CHECK(s.player().board[1].evolution == Evolution::Normal && s.player().ep == 0);
    });
    suite.add("phase2_tokens.90021310.selects_one_follower_or_the_leader", [] {
        for (const auto choice : {std::int8_t{0}, kEnemyLeader}) {
            Scenario s;
            s.hand({official(90021310)});
            s.place(knight(), 1).hp = 3;
            s.place(knight(), 1).hp = 3;
            s.run({Play, 0, choice});
            CHECK(s.player(1).board[0].hp == (choice == 0 ? 2 : 3));
            CHECK(s.player(1).board[1].hp == 3);
            CHECK(s.player(1).leader_hp == (choice == kEnemyLeader ? 19 : 20));
        }
    });
    suite.add("phase2_tokens.90021330.shoes_grant_attack_and_rush", [] {
        Scenario s;
        s.hand({official(90021330)});
        s.reject({Play, 0});
        auto& ally = s.place(knight());
        ally.readied = false;
        s.place(knight(), 1);
        CHECK(!s.legal({Attack, 0, 0}));
        s.run({Play, 0, kOwnBoardTarget});
        CHECK(ally.atk == 2 && ally.hp == 1);
        CHECK(s.legal({Attack, 0, 0}) && !s.legal({Attack, 0, kEnemyLeader}));
    });
    suite.add("phase2_tokens.90021340.necklace_grants_defense_and_ward", [] {
        Scenario s;
        s.hand({official(90021340)});
        s.reject({Play, 0});
        auto& ally = s.place(knight());
        s.place(knight());
        s.place(knight(), 1);
        s.run({Play, 0, kOwnBoardTarget});
        CHECK(ally.atk == 1 && ally.hp == 2 && ally.max_hp == 2);
        s.run({EndTurn});
        CHECK(s.legal({Attack, 0, 0}) && !s.legal({Attack, 0, 1}));
        CHECK(!s.legal({Attack, 0, kEnemyLeader}));
    });
    suite.add("phase2_tokens.90021350.coin_modes_are_exclusive", [] {
        for (const auto mode : std::array<std::uint8_t, 2>{1, 2}) {
            Scenario s;
            seed_draw(s);
            s.hand({official(90021350)});
            s.place(knight(), 1).hp = 5;
            s.run({Play, 0, kNoTarget, mode});
            CHECK(s.player().hand_count == (mode == 1 ? 1 : 0));
            if (mode == 1) CHECK(s.player().hand[0] == knight());
            CHECK(s.player(1).board[0].hp == (mode == 2 ? 3 : 5));
        }
    });
    suite.add("phase2_tokens.90024320.enhance_replaces_base_damage", [] {
        for (const auto pp : std::array<std::uint8_t, 2>{0, 1}) {
            Scenario s;
            s.player().pp = pp;
            s.hand({official(90024320)});
            s.reject({Play, 0});
            s.place(knight(), 1).hp = 8;
            s.run({Play, 0, 0});
            CHECK(s.player(1).board[0].hp == (pp == 1 ? 5 : 7));
            CHECK(s.player().available_pp() == 0);
        }
    });
    suite.add("phase2_tokens.90024330.repeat_retargets_after_destruction", [] {
        Scenario s;
        s.hand({official(90024330)});
        s.place(knight(), 1).hp = 4;
        s.place(knight(), 1).hp = 4;
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 2);
        CHECK(s.player(1).leader_hp == 20);
    });
    suite.add("phase2_tokens.90031310.random_damage_hits_a_follower_only", [] {
        Scenario s;
        s.hand({official(90031310)});
        s.place(knight(), 1).hp = 5;
        s.place(knight(), 1).hp = 5;
        s.place(official(10062210), 1);
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 3 && s.player(1).leader_hp == 20);
        const auto first = s.player(1).board[0].hp;
        const auto second = s.player(1).board[1].hp;
        CHECK((first == 2 && second == 5) || (first == 5 && second == 2));
    });
    suite.add("phase2_tokens.90034350.grants_exactly_two_attacks", [] {
        Scenario s;
        s.hand({official(90034350)});
        s.reject({Play, 0});
        s.place(knight());
        s.run({Play, 0, kOwnBoardTarget});
        s.run({Attack, 0, kEnemyLeader});
        s.run({Attack, 0, kEnemyLeader});
        CHECK(s.player(1).leader_hp == 18);
        s.reject({Attack, 0, kEnemyLeader});
    });
    suite.add("phase2_tokens.90044330.cast_and_discard_both_resolve_once", [] {
        const auto abyss = official(90044330);
        for (const bool discard : {false, true}) {
            Scenario s;
            s.player().leader_hp = 14;
            if (discard) {
                s.hand({official(10641110), abyss});
                s.run({Play, 0, kOwnHandTarget + 1});
            } else {
                s.hand({abyss});
                s.run({Play, 0});
            }
            CHECK(s.player().leader_hp == 15 && s.player(1).leader_hp == 19);
        }
    });
    suite.add("phase2_tokens.90074320.selects_printed_cost_five_and_copies_with_discount", [] {
        Scenario s;
        const auto five_cost = official(90073110);
        s.hand({official(90074320)});
        s.place(knight());
        s.place(five_cost);
        s.reject({Play, 0, kOwnBoardTarget});
        s.run({Play, 0, kOwnBoardTarget + 1});
        CHECK(s.player().board_count == 2 && s.player().hand_count == 1);
        CHECK(s.player().hand[0] == five_cost && s.player().hand_modifiers[0].cost == -3);
    });

    add_artifact_fusion(suite, 90071210, 90071220, 90072110);
    add_artifact_fusion(suite, 90071220, 90071210, 90072120);
    suite.add("phase2_tokens.90073110.requires_two_distinct_materials", [] {
        Scenario s;
        const auto alpha = official(90073110);
        const auto beta = official(90073120);
        const auto gamma = official(90073130);
        s.hand({alpha, beta, beta, gamma});
        s.run({Fuse, 0, kNoTarget, 3});
        CHECK(s.player().hand_count == 2 && s.player().hand[0] == alpha);
        s.next_own_turn();
        s.run({Fuse, 0, kNoTarget, 1});
        CHECK(s.player().hand[0] == official(90074110));
    });
    suite.add("phase2_tokens.90074110.only_play_fanfare_resolves_damage_and_heal", [] {
        const auto omega = official(90074110);
        for (const bool from_hand : {false, true}) {
            Scenario s;
            s.player().leader_hp = 10;
            s.place(knight(), 1).hp = 5;
            s.place(knight(), 1).hp = 6;
            if (from_hand) {
                s.hand({omega});
                s.run({Play, 0});
            } else {
                const Effect summon{.operation = Operation::Summon, .amount = 1, .card = omega};
                resolve_effect_steps(s.state, 0, std::span(&summon, 1), 0, 0, s.rules);
            }
            CHECK(s.player().board_count == 1 && s.player().board[0].card_id == omega);
            CHECK(s.player(1).board_count == (from_hand ? 1 : 2));
            CHECK(s.player(1).board[0].hp == (from_hand ? 1 : 5));
            CHECK(s.player().leader_hp == (from_hand ? 15 : 10) && s.player(1).leader_hp == 20);
        }
    });
}

} // namespace svwb::wb::spec
