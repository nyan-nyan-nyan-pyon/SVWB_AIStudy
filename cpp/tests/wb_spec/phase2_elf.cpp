#include "spec.hpp"

#include <numeric>

namespace svwb::wb::spec {

void phase2_elf(Suite& suite) {
    suite.add("phase2_elf.10011120.play_adds_one_combo", [] {
        Scenario s;
        s.hand({official(10011120)});
        s.run({Play, 0});
        // The play itself counts toward Combo, and the printed fanfare adds one more.
        CHECK(s.player().plays_this_turn == 2);
    });

    suite.add("phase2_elf.10012110.combo_three_target_damage_boundary", [] {
        const auto target = official(10011120);
        for (const auto [prior, expected_hp] : {std::pair{1, 5}, std::pair{2, 2}}) {
            Scenario s;
            s.player().plays_this_turn = static_cast<std::uint16_t>(prior);
            auto& victim = s.place(target, 1);
            victim.hp = victim.max_hp = 5;
            s.hand({official(10012110)});
            if (prior == 1) {
                s.run({Play, 0});
                CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == expected_hp);
            } else {
                s.run({Play, 0, 0});
                CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == expected_hp);
            }
        }
    });

    suite.add("phase2_elf.10012310.bounce_own_card_then_damage_enemy", [] {
        Scenario s;
        const auto own = official(10011120);
        auto& enemy = s.place(official(10011120), 1);
        enemy.hp = enemy.max_hp = 3;
        s.place(own);
        s.hand({official(10012310)});
        s.run({Play, 0, static_cast<std::int8_t>(kOwnBoardTarget)});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == own);
        CHECK(s.player().board_count == 0);
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 1);
    });

    suite.add("phase2_elf.10511310.split_three_damage_and_add_fairy", [] {
        Scenario s;
        for (const int hp : {1, 2, 3}) {
            auto& victim = s.place(official(10011120), 1);
            victim.hp = victim.max_hp = static_cast<std::int16_t>(hp);
        }
        s.hand({official(10511310)});
        s.run({Play, 0});
        int remaining_hp = 0;
        for (int i = 0; i < s.player(1).board_count; ++i) remaining_hp += s.player(1).board[i].hp;
        CHECK(remaining_hp == 3); // The official effect distributes exactly three damage among enemy followers.
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90011110));
    });

    suite.add("phase2_elf.10512120.rush_bane_and_drain_combat", [] {
        Scenario s;
        s.player().leader_hp = 10;
        auto& victim = s.place(official(10011120), 1);
        victim.hp = victim.max_hp = 6;
        s.hand({official(10512120)});
        s.run({Play, 0});
        CHECK((s.player().board[0].abilities & FLAG_RUSH) != 0);
        CHECK((s.player().board[0].abilities & kBane) != 0);
        CHECK((s.player().board[0].abilities & kDrain) != 0);
        CHECK(s.legal({Attack, 0, 0}));
        s.run({Attack, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player().leader_hp == 12);
    });

    suite.add("phase2_elf.10514110.combo_buff_and_evolve_replaces_hand_from_enemy_deck", [] {
        Scenario s;
        s.player().plays_this_turn = 2;
        auto& enemy = s.player(1);
        enemy.deck_idx = 0;
        enemy.deck_end = 5;
        enemy.deck[0] = official(10011120);
        enemy.deck[1] = official(10012110);
        enemy.deck[2] = official(10712110);
        enemy.deck[3] = official(10712120);
        enemy.deck[4] = official(10811130);
        s.hand({official(10514110), kTrainer});
        s.run({Play, 0});
        CHECK(s.player().board[0].atk == 4 && s.player().board[0].hp == 4);
        s.run({Evolve, 0});
        CHECK(s.player().hand_count == 5);
        for (int i = 0; i < s.player().hand_count; ++i) CHECK(s.player().hand[i] != kTrainer);
    });

    suite.add("phase2_elf.10514120.fanfare_and_evolve_modes", [] {
        const auto card = official(10514120);
        const auto fairy = official(90011110);
        {
            Scenario s;
            s.hand({card});
            s.run({Play, 0, kNoTarget, 1});
            CHECK(s.player().hand_count == 2 && s.player().hand[0] == fairy && s.player().hand[1] == fairy);
        }
        {
            Scenario s;
            s.hand({card});
            s.run({Play, 0, kNoTarget, 2});
            CHECK(s.player().pp == 9);
            s.run({Evolve, 0, kNoTarget, 2});
            CHECK(s.player().pp == 10);
        }
        {
            Scenario s;
            auto& victim = s.place(official(10011120), 1);
            victim.hp = victim.max_hp = 5;
            s.hand({card});
            s.run({Play, 0, kNoTarget, 3});
            CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 2);
        }
    });

    suite.add("phase2_elf.10611120.combo_three_auto_evolves", [] {
        for (const auto [prior, expected] : {std::pair{1, Evolution::None}, std::pair{2, Evolution::Normal}}) {
            Scenario s;
            s.player().plays_this_turn = static_cast<std::uint16_t>(prior);
            s.hand({official(10611120)});
            s.run({Play, 0});
            CHECK(s.player().board[0].evolution == expected);
        }
    });

    suite.add("phase2_elf.10611310.damage_all_and_summon_green_fairies", [] {
        Scenario s;
        for (int i = 0; i < 2; ++i) {
            auto& victim = s.place(official(10011120), 1);
            victim.hp = victim.max_hp = 4;
        }
        s.hand({official(10611310)});
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 0);
        CHECK(s.player().board_count == 2);
        for (int i = 0; i < 2; ++i) CHECK(s.player().board[i].card_id == official(90011120));
    });

    suite.add("phase2_elf.10612310.hand_discount_triggers_on_follower_evolution", [] {
        Scenario s;
        s.hand({official(10612310), official(10011120)});
        s.run({Play, 1});
        CHECK(card_cost(*find_card(official(10612310)), s.player().hand_modifiers[0]) == 5);
        s.run({Evolve, 0});
        CHECK(card_cost(*find_card(official(10612310)), s.player().hand_modifiers[0]) == 4);
        s.run({Play, 0});
        CHECK(s.player().hand_count == 2);
    });

    suite.add("phase2_elf.10613310.mode_destroys_strongest_or_summons_fairy", [] {
        const auto spell = official(10613310);
        {
            Scenario s;
            auto& weak = s.place(official(10011120), 1);
            weak.atk = 1;
            auto& strong = s.place(official(10011120), 1);
            strong.atk = 4;
            s.hand({spell});
            s.run({Play, 0, kNoTarget, 1});
            CHECK(s.player(1).board_count == 1 && s.player(1).board[0].atk == 1);
        }
        {
            Scenario s;
            s.hand({spell});
            s.run({Play, 0, kNoTarget, 2});
            CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(90011120));
        }
    });

    suite.add("phase2_elf.10614120.drain_and_faith_fanfare", [] {
        Scenario s;
        s.player().leader_hp = 10;
        s.player().faiths[0] = official(10614122);
        s.player().faith_values[0] = 10;
        s.player().faith_count = 1;
        s.player().ep = 1;
        auto& victim = s.place(official(10011120), 1);
        victim.hp = victim.max_hp = 5;
        victim.atk = 0;
        s.hand({official(10614120)});
        s.run({Play, 0});
        CHECK(s.player().faith_values[0] == 0);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90014330));
        CHECK((s.player().board[0].abilities & kDrain) != 0);
        s.player().board[0].readied = true;
        s.run({Attack, 0, 0});
        CHECK(s.player().leader_hp == 11);
        CHECK(s.player(1).board[0].hp == 4 && s.player().board[0].card_id == official(10614120));
        s.run({Evolve, 0});
        CHECK(s.player(1).leader_hp == 19);
    });

    suite.add("phase2_elf.10711120.return_hand_card_and_add_two_fairies", [] {
        Scenario s;
        s.hand({official(10711120), kTrainer});
        s.run({Play, 0, static_cast<std::int8_t>(kOwnHandTarget + 1)});
        CHECK(s.player().hand_count == 2);
        CHECK(s.player().hand[0] == official(90011110) && s.player().hand[1] == official(90011110));
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(10711120));
    });

    suite.add("phase2_elf.10711310.return_two_hand_cards_and_draw_two", [] {
        Scenario s;
        s.hand({official(10711310), kTrainer, official(10012110)});
        auto& p = s.player();
        const auto deck_start = p.deck_idx;
        p.deck_end = static_cast<std::uint8_t>(p.deck_idx + 2);
        p.deck[p.deck_idx] = official(10011120);
        p.deck[p.deck_idx + 1] = official(10012110);
        s.run({Play, 0, kNoTarget, 0, 0b110, true});
        CHECK(p.deck_idx == deck_start + 2 && p.deck_end == deck_start + 4 && p.hand_count == 2);
    });

    suite.add("phase2_elf.10712110.combo_three_gains_storm", [] {
        for (const auto [prior, storm] : {std::pair{1, false}, std::pair{2, true}}) {
            Scenario s;
            s.player().plays_this_turn = static_cast<std::uint16_t>(prior);
            s.hand({official(10712110)});
            s.run({Play, 0});
            CHECK(((s.player().board[0].abilities & FLAG_STORM) != 0) == storm);
        }
    });

    suite.add("phase2_elf.10712120.fanfare_and_evolve_damage_and_combo", [] {
        Scenario s;
        auto& victim = s.place(official(10011120), 1);
        victim.hp = victim.max_hp = 10;
        s.hand({official(10712120)});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board[0].hp == 6 && s.player().plays_this_turn == 2);
        s.run({Evolve, 0, 0});
        CHECK(s.player(1).board[0].hp == 2 && s.player().plays_this_turn == 3);
    });

    suite.add("phase2_elf.10712310.heal_always_and_combo_three_crest", [] {
        for (const auto [prior, crest_count] : {std::pair{1, 0}, std::pair{2, 1}}) {
            Scenario s;
            s.player().leader_hp = 10;
            s.player().plays_this_turn = static_cast<std::uint16_t>(prior);
            s.hand({official(10712310)});
            s.run({Play, 0});
            CHECK(s.player().leader_hp == 11 && s.player().crest_count == crest_count);
        }
    });

    suite.add("phase2_elf.10713310.random_damage_and_combo_crest", [] {
        Scenario s;
        auto& victim = s.place(official(10011120), 1);
        victim.hp = victim.max_hp = 5;
        s.player().plays_this_turn = 2;
        s.hand({official(10713310)});
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 3);
        CHECK(s.player().crest_count == 1);
    });

    suite.add("phase2_elf.10714110.attack_scaled_minus_hp_and_evolve_crest", [] {
        Scenario s;
        auto& victim = s.place(official(10011120), 1);
        victim.hp = victim.max_hp = 6;
        s.hand({official(10714110)});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 3);
        CHECK(s.player().plays_this_turn == 2);
        s.run({Evolve, 0});
        CHECK(s.player().crest_count == 1);
    });

    suite.add("phase2_elf.10811130.bounce_draw_and_ward", [] {
        Scenario s;
        s.player().deck_end = s.player().deck_idx;
        s.hand({official(10811130), official(10011120)});
        s.run({Play, 0, static_cast<std::int8_t>(kOwnHandTarget + 1)});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(10811130));
        CHECK((s.player().board[0].abilities & FLAG_WARD) != 0);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(10011120));
    });

    suite.add("phase2_elf.10812310.destroy_target_and_heal", [] {
        Scenario s;
        s.player().leader_hp = 10;
        s.place(official(10011120), 1);
        s.hand({official(10812310)});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
        CHECK(s.player().leader_hp == 12);
    });

    suite.add("phase2_elf.10911110.combo_three_draw_and_rush", [] {
        for (const auto [prior, drew] : {std::pair{1, false}, std::pair{2, true}}) {
            Scenario s;
            s.player().plays_this_turn = static_cast<std::uint16_t>(prior);
            s.hand({official(10911110)});
            const auto cursor = s.player().deck_idx;
            s.run({Play, 0});
            CHECK((s.player().deck_idx == cursor + (drew ? 1 : 0)));
            CHECK((s.player().board[0].abilities & FLAG_RUSH) != 0);
        }
    });

    suite.add("phase2_elf.10911120.intimidate_prevents_enemy_attack_target", [] {
        Scenario s;
        s.hand({official(10911120)});
        s.run({Play, 0});
        CHECK((s.player().board[0].abilities & kIntimidate) != 0);
        auto& enemy = s.place(official(10011120), 1);
        enemy.readied = true;
        s.state.turn_player = 1;
        CHECK(!s.legal({Attack, 0, 0}));
    });

    suite.add("phase2_elf.10911210.enemy_follower_entry_destroys_trap_and_follower", [] {
        Scenario s;
        s.hand({official(10911210)});
        s.run({Play, 0});
        s.hand({official(10011120)}, 1);
        s.state.turn_player = 1;
        s.run({Play, 0}, std::source_location::current());
        CHECK(s.player().board_count == 0 && s.player(1).board_count == 0);
        CHECK(s.player().shadows == 1 && s.player(1).shadows == 1);
    });

    suite.add("phase2_elf.10912120.fanfare_and_evolve_copy_only_enemy_hand_card", [] {
        Scenario s;
        s.hand({official(10912120)});
        s.hand({official(10011120)}, 1);
        s.run({Play, 0});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(10011120));
        s.run({Evolve, 0});
        CHECK(s.player().hand_count == 2 && s.player().hand[1] == official(10011120));
    });

    suite.add("phase2_elf.10912310.mode_boundary_and_combo_three_all_modes", [] {
        const auto spell = official(10912310);
        {
            Scenario s;
            auto& victim = s.place(official(10011120), 1);
            victim.hp = victim.max_hp = 6;
            s.hand({spell});
            s.run({Play, 0, kNoTarget, 1});
            CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 2);
            CHECK(s.player().hand_count == 0);
        }
        {
            Scenario s;
            s.player().plays_this_turn = 2;
            auto& victim = s.place(official(10011120), 1);
            victim.hp = victim.max_hp = 6;
            s.hand({spell});
            s.run({Play, 0, kNoTarget, kAllModes});
            CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 2);
            CHECK(s.player().hand_count == 2 && s.player().hand[0] == official(90011310) &&
                  s.player().hand[1] == official(90011110));
        }
    });

    suite.add("phase2_elf.11012310.summon_evidence_and_gain_crest", [] {
        Scenario s;
        s.hand({official(11012310)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(90001210));
        CHECK(s.player().crest_count == 1);
    });
}

} // namespace svwb::wb::spec
