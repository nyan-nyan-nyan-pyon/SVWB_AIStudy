#include "spec.hpp"

#include <tuple>
#include <utility>

namespace svwb::wb::spec {

void elf(Suite& suite) {
    suite.add("elf.11011110.evolve_heals_at_two_original_cost_one_cards", [] {
        const auto sitter = official(11011110);
        const auto rabbit = official(10012110);
        // The fanfare's one-cost Evidence amulet is also counted, so use zero/one
        // pre-existing one-cost followers to test the printed threshold of two.
        for (const auto [count, expected_hp] : {std::pair{0, 15}, std::pair{1, 17}}) {
            Scenario s;
            s.player().leader_hp = 15;
            for (int i = 0; i < count; ++i) s.place(rabbit);
            s.hand({sitter});
            s.run({Play, 0});
            CHECK(s.player().board_count == count + 2);
            CHECK(find_card(s.player().board[count + 1].card_id)->kind == Kind::Amulet);
            s.run({Evolve, static_cast<std::int8_t>(count)});
            CHECK(s.player().leader_hp == expected_hp);
        }
    });

    suite.add("elf.11012110.entry_buff_counts_other_original_cost_one_cards", [] {
        const auto mouse = official(11012110);
        const auto rabbit = official(10012110);
        for (const auto [count, expected_atk, expected_hp] : {
                 std::tuple{1, 1, 1}, std::tuple{2, 3, 2}}) {
            Scenario s;
            for (int i = 0; i < count; ++i) s.place(rabbit);
            s.hand({mouse});
            s.run({Play, 0});
            const auto& entered = s.player().board[count];
            CHECK(entered.card_id == mouse && entered.atk == expected_atk && entered.hp == expected_hp);
            CHECK((entered.abilities & FLAG_RUSH) != 0);
        }
    });

    suite.add("elf.11012120.end_turn_adds_both_named_cards", [] {
        Scenario s;
        s.hand({official(11012120)});
        s.run({Play, 0});
        CHECK(s.player().board[0].atk == 0 && s.player().board[0].hp == 3);
        s.next_own_turn();
        CHECK(s.player().hand_count >= 2 && s.player().hand[0] == official(90011310) &&
              s.player().hand[1] == official(90011110));
    });

    suite.add("elf.11013110.evolve_summons_mouse_at_two_cost_one_cards", [] {
        const auto dog = official(11013110);
        const auto rabbit = official(10012110);
        // The one-cost Evidence amulet from the fanfare counts toward the threshold.
        for (const int count : {0, 1}) {
            Scenario s;
            for (int i = 0; i < count; ++i) s.place(rabbit);
            s.hand({dog});
            s.run({Play, 0});
            CHECK(s.player().board[count].card_id == dog && s.player().board[count].atk == 2 &&
                  s.player().board[count].hp == 3 && (s.player().board[count].abilities & FLAG_STORM));
            CHECK(find_card(s.player().board[count + 1].card_id)->kind == Kind::Amulet);
            s.run({Evolve, static_cast<std::int8_t>(count)});
            CHECK(s.player().board_count == count + (count == 1 ? 3 : 2));
            if (count == 1) CHECK(s.player().board[s.player().board_count - 1].card_id == official(11012110));
        }
    });

    suite.add("elf.11014110.super_evolve_buffs_other_summoned_followers", [] {
        Scenario s;
        const auto noishe = official(11014110);
        s.place(official(10012110));
        s.hand({noishe});
        s.run({Play, 0});
        // Noishe summons the dog; its fanfare does not run on a summon, so no Evidence is added.
        CHECK(s.player().board_count == 4);
        s.run({SuperEvolve, 1});
        CHECK(s.player().board[1].atk == 8 && s.player().board[1].hp == 7);
        const auto dog = std::find_if(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                                      [](const auto& object) { return object.card_id == official(11013110); });
        const auto mouse = std::find_if(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                                        [](const auto& object) { return object.card_id == official(11012110); });
        CHECK(dog != s.player().board.begin() + s.player().board_count && dog->atk == 4 && dog->hp == 3);
        CHECK(mouse != s.player().board.begin() + s.player().board_count && mouse->atk == 3 && mouse->hp == 1);
    });

    suite.add("elf.11011310.enhance_boundary_summons_one_or_three_evolved_fairies", [] {
        const auto rebirth = official(11011310);
        const auto fairy = official(90011110);
        for (const auto [pp, count] : {std::pair{5, 1}, std::pair{6, 3}}) {
            Scenario s;
            s.player().pp = static_cast<std::uint8_t>(pp);
            s.hand({rebirth});
            // Enhance is automatic from available PP; this card has no selectable modes.
            s.run({Play, 0});
            CHECK(s.player().board_count == count && s.player().pp == (pp == 6 ? 0 : 2));
            for (int i = 0; i < count; ++i) {
                CHECK(s.player().board[i].card_id == fairy && s.player().board[i].evolution == Evolution::Normal &&
                      s.player().board[i].atk == 3 && s.player().board[i].hp == 3);
            }
        }
    });

    suite.add("elf.10512110.combo_five_summons_and_evolves_pair", [] {
        const auto buddies = official(10512110);
        for (const auto [plays, expected_count] : {std::pair{3, 1}, std::pair{4, 2}}) {
            Scenario s;
            s.player().plays_this_turn = static_cast<std::uint16_t>(plays);
            s.hand({buddies});
            s.run({Play, 0});
            CHECK(s.player().board_count == expected_count);
            CHECK(s.player().board[0].card_id == buddies && s.player().board[0].evolution ==
                  (expected_count == 2 ? Evolution::Normal : Evolution::None));
            if (expected_count == 2) {
                CHECK(printed_card(s.player().board[1].card_id) == buddies &&
                      s.player().board[1].evolution == Evolution::Normal);
            }
        }
    });

    suite.add("elf.10912110.combo_three_grants_bane_to_generated_fairy", [] {
        const auto inspector = official(10912110);
        const auto fairy = official(90011110);
        for (const auto [plays, bane] : {std::pair{1, false}, std::pair{2, true}}) {
            Scenario s;
            s.player().plays_this_turn = static_cast<std::uint16_t>(plays);
            s.hand({inspector});
            s.run({Play, 0});
            CHECK(s.player().hand_count == 1 && s.player().hand[0] == fairy);
            CHECK(((s.player().hand_modifiers[0].abilities & kBane) != 0) == bane);
        }
    });

    suite.add("elf.10512310.combo_three_doubles_all_damage", [] {
        const auto support = official(10512310);
        for (const auto [plays, expected_leader_hp, expected_follower_hp] : {
                 std::tuple{0, 19, 4}, std::tuple{2, 18, 3}}) {
            Scenario s;
            s.player().plays_this_turn = static_cast<std::uint16_t>(plays);
            s.place(official(10012110), 1).hp = 5;
            s.place(official(10012110), 1).max_hp = 5;
            s.hand({support});
            s.run({Play, 0});
            CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == expected_follower_hp);
            CHECK(s.player(1).leader_hp == expected_leader_hp);
        }
    });

    suite.add("elf.10513310.draw_count_matches_combo_after_play", [] {
        Scenario s;
        s.player().plays_this_turn = 2;
        s.hand({official(10513310)});
        const auto cursor = s.player().deck_idx;
        s.run({Play, 0});
        CHECK(s.player().plays_this_turn == 3 && s.player().deck_idx == cursor + 3 && s.player().hand_count == 3);
    });

    suite.add("elf.10913110.combo_three_fanfare_draws_two", [] {
        const auto captain = official(10913110);
        for (const auto [plays, expected_draw] : {std::pair{1, 0}, std::pair{2, 2}}) {
            Scenario s;
            s.player().plays_this_turn = static_cast<std::uint16_t>(plays);
            s.hand({captain});
            const auto cursor = s.player().deck_idx;
            s.run({Play, 0});
            CHECK(s.player().deck_idx == cursor + expected_draw && s.player().hand_count == expected_draw);
        }
    });

    suite.add("elf.10913310.end_turn_discount_requires_combo_three", [] {
        const auto delayed_spell = official(10913310);
        for (const auto [plays, expected_delta] : {std::pair{2, 0}, std::pair{3, -1}}) {
            Scenario s;
            s.player().plays_this_turn = static_cast<std::uint16_t>(plays);
            s.hand({delayed_spell});
            s.run({EndTurn});
            CHECK(s.player().hand_modifiers[0].cost == expected_delta);
        }
    });

    suite.add("elf.11013310.one_cost_board_threshold_grants_mouse_storm", [] {
        const auto interrogation = official(11013310);
        const auto one_cost = official(10012110);
        const auto mouse = official(11012110);
        for (const auto [count, storm] : {std::pair{1, false}, std::pair{2, true}}) {
            Scenario s;
            for (int i = 0; i < count; ++i) s.place(one_cost);
            s.hand({interrogation});
            s.run({Play, 0});
            CHECK(s.player().hand_count == 1 && s.player().hand[0] == mouse);
            CHECK(((s.player().hand_modifiers[0].abilities & FLAG_STORM) != 0) == storm);
        }
    });

    suite.add("elf.10713110.end_turn_draw_starts_at_combo_three", [] {
        const auto archer = official(10713110);
        for (const auto [prior_plays, expected_draw] : {std::pair{1, 0}, std::pair{2, 1}}) {
            Scenario s;
            s.player().plays_this_turn = static_cast<std::uint16_t>(prior_plays);
            s.place(official(10012110), 1).hp = 5;
            s.place(official(10012110), 1).max_hp = 5;
            s.hand({archer});
            s.run({Play, 0, 0});
            const auto cursor = s.player().deck_idx;
            s.run({EndTurn});
            CHECK(s.player().deck_idx == cursor + expected_draw);
        }
    });

    suite.add("elf.10513110.super_evolve_grants_bane_only_to_fairies", [] {
        const auto boatman = official(10513110);
        const auto fairy = official(90011110);
        Scenario s;
        s.place(official(10012110));
        s.hand({boatman});
        s.run({Play, 0});
        s.run({SuperEvolve, 1});
        int fairies = 0;
        for (int i = 0; i < s.player().board_count; ++i) {
            const auto& object = s.player().board[i];
            if (object.card_id == fairy) {
                ++fairies;
                CHECK((object.abilities & kBane) != 0);
            }
        }
        CHECK(fairies == 3 && (s.player().board[0].abilities & kBane) == 0);
    });

    suite.add("elf.10511120.fairy_entry_trigger_damages_enemy_leader", [] {
        const auto artisan = official(10511120);
        const auto fairy = official(90011110);
        Scenario s;
        s.hand({artisan, fairy, kTrainer});
        s.run({Play, 0});
        CHECK(s.player(1).leader_hp == 19);
        s.run({Play, 0});
        CHECK(s.player(1).leader_hp == 18);
        s.run({Play, 0});
        CHECK(s.player(1).leader_hp == 18);
    });

    suite.add("elf.10814120.enhanced_buff_triggers_one_eve_per_turn", [] {
        const auto tia = official(10814120);
        const auto eve = official(90014110);
        Scenario s;
        s.player().pp = 4;
        s.place(official(10012110));
        s.place(official(10012110));
        s.hand({tia});
        // The Enhance payment is selected automatically; the card has no selectable modes.
        s.run({Play, 0});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == eve);
        CHECK(s.player().board[0].atk == 2 && s.player().board[1].atk == 2 && s.player().board[2].atk == 3);
    });

    suite.add("elf.10813110.barrier_applies_to_existing_source_and_summon", [] {
        Scenario s;
        s.place(official(10012110));
        s.hand({official(10813110)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 3);
        for (int i = 0; i < s.player().board_count; ++i) CHECK((s.player().board[i].abilities & kBarrier) != 0);
    });

    suite.add("elf.10813310.deck_summons_two_distinct_low_cost_followers_then_buffs", [] {
        const auto mirror = official(10813310);
        const auto tamer = official(10011110);
        const auto adventurer = official(10012110);
        Scenario s;
        auto& p = s.player();
        p.deck_end = static_cast<std::uint8_t>(p.deck_idx + 2);
        p.deck[p.deck_idx] = tamer;
        p.deck[p.deck_idx + 1] = adventurer;
        s.place(adventurer);
        s.hand({mirror});
        s.run({Play, 0});
        CHECK(p.board_count == 3 && p.deck_idx == p.deck_end);
        for (int i = 0; i < p.board_count; ++i) CHECK(p.board[i].atk >= 2 && p.board[i].hp >= 2);
        CHECK(p.board[1].card_id != p.board[2].card_id);
    });

    suite.add("elf.10914110.super_evolve_grants_storm", [] {
        Scenario s;
        s.hand({official(10914110)});
        s.place(official(10012110), 1);
        s.place(official(10012110), 1);
        s.run({Play, 0, 0});
        s.run({SuperEvolve, 0});
        CHECK(s.player().board[0].evolution == Evolution::Super &&
              (s.player().board[0].abilities & FLAG_STORM) != 0);
    });

    suite.add("elf.10012120.super_evolve_bounces_chosen_follower", [] {
        Scenario s;
        s.hand({official(10012120)});
        const auto target = official(10012110);
        s.place(target, 1);
        s.run({Play, 0});
        s.run({SuperEvolve, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).hand_count == 1 &&
              s.player(1).hand[0] == target);
        CHECK(s.player(1).shadows == 0 && s.player().board[0].evolution == Evolution::Super);
    });

    suite.add("elf.10811110.fanfare_destruction_scales_with_board_difference", [] {
        const auto balance = official(10811110);
        for (const auto [enemy_count, expected_destroyed] : {std::pair{1, 0}, std::pair{2, 1}}) {
            Scenario s;
            for (int i = 0; i < enemy_count; ++i) {
                auto& object = s.place(official(10012110), 1);
                object.hp = object.max_hp = 5;
            }
            s.hand({balance});
            s.run({Play, 0});
            CHECK(s.player(1).board_count == enemy_count - expected_destroyed);
            CHECK(s.player(1).shadows == expected_destroyed && s.player().shadows == 0);
        }
    });

    suite.add("elf.10814110.fanfare_destroys_enemy_and_buffs_other_follower", [] {
        Scenario s;
        const auto setus = official(10814110);
        s.place(official(10012110));
        s.place(official(10012110), 1);
        s.hand({setus});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1 && s.player().shadows == 0);
        CHECK(s.player().board[0].atk == 2 && s.player().board[0].hp == 2);
        CHECK(s.player().board[1].card_id == setus &&
              (s.player().board[1].abilities & (FLAG_STORM | FLAG_WARD)) == (FLAG_STORM | FLAG_WARD));
    });

    suite.add("elf.11011120.fanfare_and_evolve_each_summon_two_rushers", [] {
        Scenario s;
        const auto wolf = official(11011120);
        s.hand({wolf});
        s.run({Play, 0});
        CHECK(s.player().board_count == 3);
        s.run({Evolve, 0});
        CHECK(s.player().board_count == 5);
        CHECK(s.player().board[0].card_id == wolf && s.player().board[0].evolution == Evolution::Normal);
        for (int i = 1; i < s.player().board_count; ++i) {
            CHECK(printed_card(s.player().board[i].card_id) == wolf &&
                  (s.player().board[i].abilities & FLAG_RUSH) != 0);
        }
    });

    suite.add("elf.10614110.super_evolve_destroys_follower_and_fanfare_adds_green_fairies", [] {
        Scenario s;
        const auto altenia = official(10614110);
        const auto green_fairy = official(90011120);
        s.place(kTrainer, 1);
        s.place(kLilium, 1);
        s.hand({altenia});
        s.run({Play, 0});
        CHECK(s.player().board_count == 4);
        s.run({SuperEvolve, 0, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).shadows == 1);
        CHECK(std::count_if(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                            [green_fairy](const auto& object) { return object.card_id == green_fairy; }) == 3);
    });

    suite.add("elf.10711110.damage_cap_reduces_four_damage_to_three", [] {
        Scenario s;
        const auto bear = official(10711110);
        const auto attacker = official(10712120);
        s.hand({bear});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2);
        auto& rival = s.place(attacker, 1);
        rival.atk = 4;
        rival.hp = rival.max_hp = 4;
        s.state.turn_player = 1;
        s.run({Attack, 0, 0});
        CHECK(s.player().board[0].hp == 3 && s.player(1).board_count == 0);
    });

    suite.add("elf.10914120.hand_cost_drops_per_play_until_turn_end", [] {
        const auto hien = official(10914120);
        Scenario s;
        s.hand({hien, kTrainer, kTrainer});
        s.run({Play, 1});
        CHECK(card_cost(*find_card(hien), s.player().hand_modifiers[0]) == 8);
        s.run({Play, 1});
        CHECK(card_cost(*find_card(hien), s.player().hand_modifiers[0]) == 7);
        s.run({EndTurn});
        CHECK(card_cost(*find_card(hien), s.player().hand_modifiers[0]) == 9);
    });

    suite.add("elf.11014120.fanfare_draws_twice_and_summons_only_drawn_elf_followers", [] {
        const auto anomaly = official(11014120);
        const auto elf_follower = official(10012110);
        const auto neutral_follower = kTrainer;
        Scenario s;
        auto& p = s.player();
        p.deck_end = static_cast<std::uint8_t>(p.deck_idx + 2);
        p.deck[p.deck_idx] = elf_follower;
        p.deck[p.deck_idx + 1] = neutral_follower;
        s.hand({anomaly});
        const auto cursor = p.deck_idx;
        s.run({Play, 0});
        CHECK(p.deck_idx == cursor + 2 && p.board_count == 2);
        CHECK(p.board[0].card_id == anomaly && p.board[1].card_id == elf_follower);
        CHECK(p.hand_count == 1 && p.hand[0] == neutral_follower);
    });

    suite.add("elf.10714120.end_turn_split_damage_uses_current_attack", [] {
        Scenario s;
        const auto king = official(10714120);
        auto& victim = s.place(official(10012110), 1);
        victim.hp = victim.max_hp = 7;
        s.hand({king});
        s.run({Play, 0});
        CHECK(s.player().hand_count == 2 && s.player().hand[0] == official(90011310) &&
              s.player().hand[1] == official(90011310));
        s.run({EndTurn});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 2);
    });

    suite.add("elf.10011130.combo_three_fanfare_evolves_only_at_threshold", [] {
        const auto treant = official(10011130);
        for (const auto [prior_plays, evolution] : {std::pair{1, Evolution::None},
                                                    std::pair{2, Evolution::Normal}}) {
            Scenario s;
            s.player().plays_this_turn = static_cast<std::uint16_t>(prior_plays);
            s.hand({treant});
            s.run({Play, 0});
            CHECK(s.player().board[0].evolution == evolution);
        }
    });

    suite.add("elf.10612120.storm_follower_can_attack_leader_on_entry", [] {
        Scenario s;
        const auto wolf = official(10612120);
        s.hand({wolf});
        s.run({Play, 0});
        CHECK(s.legal({Attack, 0, kEnemyLeader}));
        s.run({Attack, 0, kEnemyLeader});
        CHECK(s.player(1).leader_hp == 15);
    });
}

} // namespace svwb::wb::spec

