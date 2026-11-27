#include "spec.hpp"

#include <utility>

namespace svwb::wb::spec {

void royal(Suite& suite) {
    suite.add("royal.10522310.base_and_enhance_summon_count", [] {
        const auto knight = official(90021110);
        Scenario base;
        base.player().pp = 2;
        base.hand({official(10522310)});
        base.run({Play, 0});
        CHECK(base.player().board_count == 2 && base.player().board[0].card_id == knight &&
              base.player().board[1].card_id == knight && base.player().pp == 0);

        Scenario enhanced;
        enhanced.player().pp = 4;
        enhanced.hand({official(10522310)});
        enhanced.run({Play, 0});
        CHECK(enhanced.player().board_count == 4 && enhanced.player().pp == 0);
    });

    suite.add("royal.10921310.enhance_summon_five_steel_knights", [] {
        Scenario s;
        s.hand({official(10921310)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 5 && s.player().pp == 4);
        for (int i = 0; i < 5; ++i) {
            CHECK(s.player().board[i].card_id == official(90021120));
            CHECK(s.player().board[i].abilities & FLAG_WARD);
        }
    });

    suite.add("royal.10621310.enhance_buff_on_summoned_troops", [] {
        Scenario s;
        s.hand({official(10621310)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 3 && s.player().pp == 3);
        for (int i = 0; i < 3; ++i)
            CHECK(s.player().board[i].card_id == official(10621110) && s.player().board[i].atk == 4 &&
                  s.player().board[i].hp == 4);
    });

    suite.add("royal.10721120.fanfare_adds_both_distinct_outputs", [] {
        Scenario s;
        s.hand({official(10721120)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == official(90021110));
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90021120));
    });

    suite.add("royal.10922110.fanfare_flag_and_golden_cup", [] {
        Scenario s;
        s.hand({official(10922110)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == official(90021210));
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90021320));
    });

    suite.add("royal.10621110.enhance_stats_and_rush", [] {
        Scenario s;
        s.hand({official(10621110)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].atk == 3 && s.player().board[0].hp == 3);
        CHECK(s.player().board[0].abilities & FLAG_RUSH);
        CHECK(s.player().pp == 7);
    });

    suite.add("royal.10622110.enhance_guard_stats_and_evolve_damage", [] {
        Scenario s;
        s.hand({official(10622110)});
        s.place(official(10621110), 1);
        s.run({Play, 0});
        CHECK(s.player().board[0].atk == 5 && s.player().board[0].hp == 5 &&
              (s.player().board[0].abilities & FLAG_WARD));
        s.run({Evolve, 0, 0});
        CHECK(s.player(1).board_count == 0);
    });

    suite.add("royal.10821130.fanfare_rush_and_evolve_ward_tokens", [] {
        Scenario s;
        s.hand({official(10821130)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == official(90021120));
        CHECK(s.player().board[1].abilities & FLAG_RUSH);
        s.run({Evolve, 0});
        CHECK(s.player().board_count == 3 && s.player().board[2].card_id == official(90021120));
        CHECK(s.player().board[2].abilities & FLAG_WARD);
    });

    suite.add("royal.10723110.mode_grants_rush_or_ward_to_summoned_followers", [] {
        Scenario rush;
        rush.hand({official(10723110)});
        rush.run({Play, 0, kNoTarget, 1});
        CHECK(rush.player().board_count == 4 && !(rush.player().board[0].abilities & FLAG_RUSH));
        for (int i = 1; i < 4; ++i) CHECK(rush.player().board[i].abilities & FLAG_RUSH);

        Scenario guard;
        guard.hand({official(10723110)});
        guard.run({Play, 0, kNoTarget, 2});
        CHECK(guard.player().board_count == 4 && !(guard.player().board[0].abilities & FLAG_WARD));
        for (int i = 1; i < 4; ++i) CHECK(guard.player().board[i].abilities & FLAG_WARD);
    });

    suite.add("royal.10922120.enhance_grants_stats_and_rush", [] {
        Scenario s;
        s.hand({official(10922120)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 3 && s.player().pp == 3);
        CHECK(s.player().board[0].atk == 7 && (s.player().board[0].abilities & FLAG_RUSH));
        for (int i = 1; i < 3; ++i)
            CHECK(s.player().board[i].card_id == official(90021120) && s.player().board[i].atk == 5 &&
                  (s.player().board[i].abilities & FLAG_RUSH));
    });

    suite.add("royal.10724110.rally20_crest_evolve_and_followup_rush", [] {
        Scenario s;
        s.player().followers_entered = 19;
        s.hand({official(10724110)});
        s.run({Play, 0});
        CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::Gildaria);
        CHECK(s.player().board[0].evolution == Evolution::Normal && s.player().board_count == 3);
        s.hand({official(10022110)});
        s.run({Play, 0});
        CHECK(s.player().board[3].card_id == official(10022110));
        CHECK(s.player().board[3].abilities & FLAG_RUSH);
    });

    suite.add("royal.10824120.fanfare_and_super_evolve_summon_four_knights", [] {
        Scenario s;
        s.hand({official(10824120)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 4);
        for (int i = 1; i < 4; ++i)
            CHECK(s.player().board[i].card_id == official(90021110));
        s.run({SuperEvolve, 0});
        CHECK(s.player().board_count == 5 && s.player().board[4].card_id == official(90021110));
    });

    suite.add("royal.10624110.enhance_threshold_modes_and_pp", [] {
        for (const auto [pp, expected] : {std::pair{6, 2}, {7, 3}, {8, 4}}) {
            Scenario s;
            s.player().pp = static_cast<std::uint8_t>(pp);
            s.hand({official(10624110)});
            s.run({Play, 0});
            CHECK(s.player().board_count == expected);
            CHECK(s.player().pp == 0);
        }
    });

    suite.add("royal.10822120.fanfare_six_damage_and_intimidate", [] {
        Scenario s;
        s.hand({official(10822120)});
        s.place(official(10621110), 1);
        s.place(official(10621110), 1);
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 0);
        CHECK(s.player().board[0].abilities & kIntimidate);
    });

    suite.add("royal.10821110.fanfare_and_super_evolve_repeat_sweep", [] {
        Scenario s;
        s.hand({official(10821110)});
        s.place(official(10621110), 1);
        s.place(official(10621110), 1);
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == official(90021130));
        CHECK(s.player(1).board_count == 0);
        s.place(official(10022110), 1);
        s.run({SuperEvolve, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].card_id == official(90021110));
    });

    suite.add("royal.10723310.rally_aoe_scales_with_own_board", [] {
        Scenario s;
        s.player().followers_entered = 10;
        s.hand({official(10723310)});
        s.place(kTrainer);
        s.place(kTrainer);
        s.place(kTrainer);
        s.place(official(10621110), 1);
        s.place(official(10621110), 1);
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0);
    });

    suite.add("royal.10721310.rally_lock_after_selected_damage", [] {
        Scenario s;
        s.player().followers_entered = 10;
        s.hand({official(10721310)});
        auto& target = s.place(official(10022110), 1);
        target.hp = target.max_hp = 5;
        s.run({Play, 0, 0});
        CHECK(s.player(1).board[0].hp == 2);
        CHECK(s.player(1).board[0].abilities & kAttackBlocked);
    });

    suite.add("royal.10821120.evolve_applies_enemy_turn_attack_lock", [] {
        Scenario s;
        s.hand({official(10821120)});
        s.place(official(10022110), 1);
        s.run({Play, 0});
        CHECK(s.player().board[0].abilities & kAmbush);
        s.run({Evolve, 0, 0});
        CHECK(s.player(1).board[0].abilities & kAttackBlocked);
    });

    suite.add("royal.11022110.damage_equals_current_attack_and_adds_evidence", [] {
        Scenario s;
        s.hand({official(11022110)});
        s.place(official(10022110), 1);
        s.run({Play, 0, 0});
        CHECK(s.player(1).board[0].hp == 1);
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == kEvidence);
    });

    suite.add("royal.11022310.doubles_selected_damage_after_evidence_act", [] {
        Scenario s;
        s.hand({official(11022310)});
        s.place(kEvidence).clue_count = 2;
        s.place(official(11021110), 1);
        s.run({Act, 0});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].card_id == kEvidence);
    });

    suite.add("royal.10924110.flag_countdown_reduction", [] {
        Scenario s;
        s.hand({official(10924110)});
        auto& old_flag = s.place(official(90021210));
        old_flag.countdown = 7;
        s.run({Play, 0});
        CHECK(old_flag.countdown == 2 && (s.player().board[1].abilities & FLAG_STORM));
    });

    suite.add("royal.10021110.storm_follower_enters_ready", [] {
        Scenario s;
        s.hand({official(10021110)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 1 && (s.player().board[0].abilities & FLAG_STORM));
        CHECK(s.legal({Attack, 0, kEnemyLeader}));
    });

    suite.add("royal.10623310.mode_draw_damage_and_enhance_both", [] {
        Scenario draw;
        draw.player().pp = 1;
        draw.hand({official(10623310)});
        draw.player().deck[draw.player().deck_idx] = kTrainer;
        draw.run({Play, 0, kNoTarget, 1});
        CHECK(draw.player().hand_count == 1 && draw.player().hand[0] == kTrainer);

        Scenario damage;
        damage.player().pp = 1;
        damage.hand({official(10623310)});
        auto& enemy = damage.place(official(10621110), 1);
        enemy.hp = enemy.max_hp = 5;
        damage.run({Play, 0, kNoTarget, 2});
        CHECK(damage.player(1).board_count == 1 && damage.player(1).board[0].hp == 2);

        Scenario enhanced;
        enhanced.player().pp = 3;
        enhanced.hand({official(10623310)});
        enhanced.player().deck[enhanced.player().deck_idx] = kTrainer;
        enhanced.place(official(10621110), 1);
        enhanced.run({Play, 0, kNoTarget, 5});
        CHECK(enhanced.player().hand_count == 1 && enhanced.player().hand[0] == kTrainer);
        CHECK(enhanced.player(1).board_count == 0 && enhanced.player().pp == 0);
    });

    suite.add("royal.10722310.rally_doubles_rush_steel_knights_in_hand", [] {
        Scenario s;
        s.player().followers_entered = 10;
        s.hand({official(10722310)});
        s.run({Play, 0});
        CHECK(s.player().hand_count == 2);
        for (int i = 0; i < 2; ++i)
            CHECK(s.player().hand[i] == official(90021120) &&
                  (s.player().hand_modifiers[i].abilities & FLAG_RUSH));
    });

    suite.add("royal.11023310.evidence_act_adds_newspaper", [] {
        Scenario s;
        s.hand({official(11023310)});
        s.player().deck[s.player().deck_idx] = kTrainer;
        s.place(kEvidence).clue_count = 2;
        s.run({Act, 0});
        s.run({Play, 0});
        CHECK(s.player().acted_evidence && s.player().hand_count == 1 && s.player().hand[0] == kTrainer);
        CHECK(std::any_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                          [](const auto& object) { return object.card_id == official(11021110); }));
        const auto evidence = std::find_if(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                                           [](const auto& object) { return object.card_id == kEvidence; });
        CHECK(std::count_if(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                            [](const auto& object) { return object.card_id == kEvidence; }) == 1);
        CHECK(evidence != s.player().board.begin() + s.player().board_count && evidence->clue_count == 2);
        CHECK(s.player().shadows == 1); // The spell, with no shadow for the banished Evidence.
    });

    suite.add("royal.11023310.unacted_evidence_merges_without_newspaper", [] {
        Scenario s;
        s.hand({official(11023310)});
        s.place(kEvidence).clue_count = 2;
        s.run({Play, 0});
        CHECK(!s.player().acted_evidence && s.player().board_count == 1);
        CHECK(s.player().board[0].card_id == kEvidence && s.player().board[0].clue_count == 3);
        CHECK(s.player().shadows == 1);
    });

    suite.add("royal.10722110.last_words_and_evolve_damage", [] {
        Scenario last_words;
        last_words.hand({official(10722110)});
        last_words.run({Play, 0});
        constexpr Effect destroy_own{.operation=Operation::Destroy, .target=Target::AllOwnFollowers};
        resolve_effect_steps(last_words.state, 0, std::array{destroy_own});
        CHECK(last_words.player().board_count == 1 &&
              last_words.player().board[0].card_id == official(90021110));

        Scenario evolve;
        evolve.hand({official(10722110)});
        evolve.place(official(10621110), 1);
        evolve.run({Play, 0});
        evolve.run({Evolve, 0, 0});
        CHECK(evolve.player(1).board_count == 0);
    });

    suite.add("royal.10823310.board_count_threshold_selects_one_or_both_modes", [] {
        Scenario one;
        one.hand({official(10823310)});
        one.player().leader_hp = 10;
        one.place(kTrainer);
        one.place(official(10621110), 1);
        one.run({Play, 0, kNoTarget, 1});
        CHECK(one.player(1).board_count == 0 && one.player().leader_hp == 10);

        Scenario both;
        both.hand({official(10823310)});
        both.player().leader_hp = 10;
        both.place(kTrainer);
        both.place(kTrainer);
        both.place(official(10621110), 1);
        both.run({Play, 0, kNoTarget, kAllModes});
        CHECK(both.player(1).board_count == 0 && both.player().leader_hp == 12);
    });

    suite.add("royal.10921110.fanfare_flag_and_evolve_gold_shoe", [] {
        Scenario s;
        s.hand({official(10921110)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == official(90021210));
        s.run({Evolve, 0});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90021330));
    });

    suite.add("royal.10922310.cost_three_spell_creates_discounted_copy", [] {
        Scenario s;
        s.hand({official(10922310)});
        auto& enemy = s.place(official(10621110), 1);
        enemy.hp = enemy.max_hp = 8;
        s.run({Play, 0, 0});
        CHECK(s.player(1).board[0].hp == 3);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(10922310));
        CHECK(s.player().hand_modifiers[0].cost == -2);
    });

    suite.add("royal.10923310.enhance_two_flags_and_four_damage", [] {
        Scenario s;
        s.hand({official(10923310)});
        s.place(official(10621110), 1);
        s.place(official(10621110), 1);
        s.run({Play, 0});
        CHECK(s.player().pp == 4 && s.player().board_count == 2);
        CHECK(s.player().board[0].card_id == official(90021210) &&
              s.player().board[1].card_id == official(90021210));
        CHECK(s.player(1).board_count == 0);
    });

    suite.add("royal.10923110.fanfare_and_super_evolve_rewards", [] {
        Scenario s;
        s.hand({official(10923110)});
        s.place(official(10621110), 1);
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0);
        CHECK(std::any_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                          [](const auto& object) { return object.card_id == official(90021210); }));
        s.run({SuperEvolve, 0});
        CHECK(s.player().hand_count == 2 && s.player().hand[0] == official(90021310) &&
              s.player().hand[1] == official(90021340));
    });

    suite.add("royal.10722120.draw_two_summon_two_and_evolve_damage", [] {
        Scenario s;
        s.hand({official(10722120)});
        s.player().deck[s.player().deck_idx] = kTrainer;
        s.player().deck[s.player().deck_idx + 1] = kLilium;
        s.place(official(10621110), 1);
        s.run({Play, 0});
        CHECK(s.player().board_count == 3 && s.player().hand_count == 2);
        CHECK(s.player().hand[0] == kTrainer && s.player().hand[1] == kLilium);
        CHECK(s.player().board[1].card_id == official(90021110) &&
              s.player().board[2].card_id == official(90021110));
        s.run({Evolve, 0, 0});
        CHECK(s.player(1).board_count == 0);
    });

    suite.add("royal.10822310.highest_attack_follower_receives_minus_ten", [] {
        Scenario s;
        s.player().pp = 4;
        s.hand({official(10822310)});
        s.place(official(10022110), 1);
        s.place(official(10621110), 1);
        s.player(1).board[0].atk = 1;
        s.player(1).board[0].hp = s.player(1).board[0].max_hp = 8;
        s.player(1).board[1].atk = 2;
        s.player(1).board[1].hp = s.player(1).board[1].max_hp = 8;
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].card_id == official(10022110));
    });

    suite.add("royal.10524120.banishes_selected_follower_and_adds_gold_coin", [] {
        Scenario s;
        s.hand({official(10524120)});
        s.place(official(10022110), 1);
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 0);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90021350));
    });

    suite.add("royal.10724120.fanfare_buffs_other_royal_followers", [] {
        Scenario s;
        s.place(official(10022110));
        s.hand({official(10724120)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 4);
        CHECK(s.player().board[0].atk == 2 && s.player().board[0].hp == 5 &&
              (s.player().board[0].abilities & FLAG_WARD));
        CHECK(s.player().board[2].card_id == official(90021120) && s.player().board[2].atk == 3 &&
              s.player().board[2].hp == 5 && (s.player().board[2].abilities & FLAG_WARD));
        CHECK(s.player().board[3].card_id == official(90021120) && s.player().board[3].atk == 3 &&
              s.player().board[3].hp == 5 && (s.player().board[3].abilities & FLAG_WARD));
    });

    suite.add("royal.11021310.summons_evolved_barrier_steel_knights", [] {
        Scenario s;
        s.hand({official(11021310)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2);
        for (int i = 0; i < 2; ++i)
            CHECK(s.player().board[i].card_id == official(90021120) &&
                  s.player().board[i].evolution == Evolution::Normal &&
                  (s.player().board[i].abilities & kBarrier));
    });

    suite.add("royal.11022120.amulet_condition_banish_and_ward", [] {
        Scenario s;
        s.hand({official(11022120)});
        s.place(kEvidence);
        s.place(official(10022110), 1);
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0 && (s.player().board[1].abilities & FLAG_WARD));
    });

    suite.add("royal.10622120.fanfare_destroys_all_one_hp_followers", [] {
        Scenario s;
        s.hand({official(10622120)});
        s.place(official(10621110), 1);
        s.place(official(10621110), 1);
        s.player(1).board[0].hp = 1;
        s.player(1).board[1].hp = 2;
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 2);
        CHECK(s.player().board[0].abilities & FLAG_STORM);
    });

    suite.add("royal.10022210.crown_buffs_followers_on_entry", [] {
        Scenario s;
        s.hand({official(10022210), official(10022110)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].countdown == 4);
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == official(10022110));
        CHECK(s.player().board[1].atk == 2 && s.player().board[1].hp == 3);
    });

    suite.add("royal.10523110.cannot_attack_end_turn_summons_steel_knight", [] {
        Scenario s;
        s.hand({official(10523110)});
        s.run({Play, 0});
        s.reject({Attack, 0, kEnemyLeader});
        s.run({EndTurn});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == official(90021120));
    });

    suite.add("royal.10522120.end_turn_sweeps_with_two_hand_spells", [] {
        Scenario s;
        s.hand({official(10522120), official(10522310), official(10623310)});
        s.place(official(10621110), 1);
        s.run({Play, 0});
        CHECK(s.player().hand_count == 2);
        s.run({EndTurn});
        CHECK(s.player(1).board_count == 0);
    });
}

} // namespace svwb::wb::spec
