#include "spec.hpp"

namespace svwb::wb::spec {

void nemesis(Suite& suite) {
    suite.add("nemesis.10971110.highlander_fanfare_gate_and_rush", [] {
        const auto recruit = official(10971110);
        {
            Scenario s;
            s.player().deck_idx = 0;
            s.player().deck_end = 2;
            s.player().deck[0] = official(10071120);
            s.player().deck[1] = official(10072110);
            s.hand({recruit});
            s.place(official(10071120), 1);
            s.run({Play, 0, 0});
            CHECK(s.player(1).board_count == 0);
            CHECK(s.player().board_count == 1 && (s.player().board[0].abilities & FLAG_RUSH));
        }
        {
            Scenario s;
            s.player().deck_idx = 0;
            s.player().deck_end = 2;
            s.player().deck[0] = official(10071120);
            s.player().deck[1] = official(10071120);
            s.hand({recruit});
            s.place(official(10071120), 1);
            s.run({Play, 0});
            CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 1);
            CHECK(s.player().board_count == 1 && (s.player().board[0].abilities & FLAG_RUSH));
        }
    });

    suite.add("nemesis.10972310.discard_bane_draw_and_duplicate_deck_gate", [] {
        const auto exile = official(10972310);
        const auto cutthroat = official(10974110);
        {
            Scenario s;
            s.player().deck_idx = 0;
            s.player().deck_end = 3;
            s.player().deck[0] = cutthroat;
            s.player().deck[1] = official(10071120);
            s.player().deck[2] = official(10072110);
            s.hand({exile, official(10071120), official(10072110)});
            s.run({Play, 0, static_cast<std::int8_t>(kOwnHandTarget + 1)});
            CHECK(s.player().deck_remaining() == 0 && s.player().hand_count == 4);
            CHECK(s.player().hand[0] == official(10072110));
            CHECK(std::count(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count,
                             cutthroat) == 1);
        }
        {
            Scenario s;
            s.player().deck_idx = 0;
            s.player().deck_end = 3;
            s.player().deck[0] = cutthroat;
            s.player().deck[1] = official(10071120);
            s.player().deck[2] = official(10071120);
            s.hand({exile, official(10072110), official(10071120)});
            s.run({Play, 0, static_cast<std::int8_t>(kOwnHandTarget + 1)});
            CHECK(s.player().deck_remaining() == 2 && s.player().hand_count == 2);
            CHECK(s.player().hand[1] == cutthroat);
        }
    });

    suite.add("nemesis.10974110.evolve_draws_bane_and_highlander_crest", [] {
        const auto cutthroat = official(10974110);
        {
            Scenario s;
            s.player().deck_idx = 0;
            s.player().deck_end = 3;
            s.player().deck[0] = cutthroat;
            s.player().deck[1] = official(10071120);
            s.player().deck[2] = official(10072110);
            s.hand({cutthroat});
            s.run({Play, 0});
            s.run({Evolve, 0});
            CHECK(s.player().hand_count == 1 && s.player().hand[0] == cutthroat);
            CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::Cutthroat);
        }
        {
            Scenario s;
            s.player().deck_idx = 0;
            s.player().deck_end = 3;
            s.player().deck[0] = cutthroat;
            s.player().deck[1] = official(10071120);
            s.player().deck[2] = official(10071120);
            s.hand({cutthroat});
            s.run({Play, 0});
            s.run({Evolve, 0});
            CHECK(s.player().hand_count == 1 && s.player().hand[0] == cutthroat);
            CHECK(s.player().crest_count == 0);
        }
    });

    suite.add("nemesis.10871130.recovers_destroyed_artifact_only", [] {
        const auto recruit = official(10871130);
        const auto artifact = official(90071130);
        {
            Scenario s;
            s.player().other_counter_count = 1;
            s.player().other_counters[0].id = artifact;
            s.player().other_counters[0].destroyed = 1;
            s.hand({recruit});
            s.run({Play, 0});
            CHECK(s.player().hand_count == 1 && s.player().hand[0] == artifact);
            CHECK(s.player().board_count == 1 && s.player().board[0].card_id == recruit);
        }
        {
            Scenario s;
            s.hand({recruit});
            s.run({Play, 0});
            CHECK(s.player().hand_count == 0 && s.player().board_count == 1);
        }
    });

    suite.add("nemesis.11071310.puppet_gets_one_random_keyword", [] {
        Scenario s;
        const auto puppet = official(90071120);
        s.hand({official(11071310)});
        s.run({Play, 0});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == puppet);
        const auto abilities = s.player().hand_modifiers[0].abilities;
        CHECK(abilities == kBane || abilities == FLAG_WARD);
        CHECK((abilities & (kBane | FLAG_WARD)) == abilities);
    });

    suite.add("nemesis.10071120.fanfare_adds_improved_puppet", [] {
        Scenario s;
        s.hand({official(10071120)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(10071120));
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90071120));
    });

    suite.add("nemesis.10072110.fanfare_adds_past_core", [] {
        Scenario s;
        s.hand({official(10072110)});
        s.run({Play, 0});
        CHECK(s.player().board[0].card_id == official(10072110));
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90071220));
    });

    suite.add("nemesis.10072210.theater_fanfare_and_countdown_puppets", [] {
        Scenario s;
        s.hand({official(10072210)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].countdown == 2);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90071110));
        s.run({EndTurn});
        CHECK(s.player().hand_count == 2);
        CHECK(std::count(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count,
                         official(90071110)) == 2);
        s.run({EndTurn});
        CHECK(s.player().hand_count == 3);
        CHECK(std::count(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count,
                         official(90071110)) == 2);
        CHECK(s.player().board[0].countdown == 1);
    });

    suite.add("nemesis.10071110.rush_and_future_core", [] {
        Scenario s;
        s.hand({official(10071110)});
        s.run({Play, 0});
        CHECK(s.player().board[0].abilities & FLAG_RUSH);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90071210));
    });

    suite.add("nemesis.10071310.destroy_and_add_both_cores", [] {
        Scenario s;
        s.hand({official(10071310)});
        s.place(official(10071120), 1);
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0);
        CHECK(s.player().hand_count == 2);
        CHECK(s.player().hand[0] == official(90071210) && s.player().hand[1] == official(90071220));
    });

    suite.add("nemesis.10572120.spell_play_auto_evolves_guardian", [] {
        Scenario s;
        s.hand({official(10572120), official(10571310)});
        s.run({Play, 0});
        CHECK(s.player().board[0].evolution == Evolution::None &&
              (s.player().board[0].abilities & FLAG_WARD));
        s.run({Play, 0});
        CHECK(s.player().board[0].evolution == Evolution::Normal);
    });

    suite.add("nemesis.10671310.large_follower_unlocks_draw", [] {
        Scenario s;
        s.player().deck_idx = 0;
        s.player().deck_end = 1;
        s.player().deck[0] = official(10071120);
        s.place(official(10071130));
        s.place(official(10071120), 1);
        s.hand({official(10671310)});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player().hand_count == 1);
        CHECK(s.player().hand[0] == official(10071120));
    });

    suite.add("nemesis.10674120.large_follower_adds_rushing_token", [] {
        Scenario s;
        s.place(official(10071130));
        s.hand({official(10674120)});
        s.run({Play, 0});
        CHECK(s.player().board[1].card_id == official(10674120));
        CHECK(s.player().board[1].abilities & FLAG_RUSH);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90074320));
    });

    suite.add("nemesis.10772110.fanfare_and_evolve_each_add_ancient", [] {
        Scenario s;
        s.hand({official(10772110)});
        s.run({Play, 0});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90071140));
        s.run({Evolve, 0});
        CHECK(s.player().hand_count == 2 && s.player().hand[1] == official(90071140));
    });

    suite.add("nemesis.10772310.field_buff_and_super_turn_hand_buff", [] {
        Scenario s;
        s.place(official(10071120));
        s.hand({official(10772310), official(10072110)});
        s.run({Play, 0});
        CHECK(s.player().board[0].atk == find_card(official(10071120))->atk + 1);
        CHECK(s.player().hand_modifiers[0].attack == 1);
    });

    suite.add("nemesis.10874120.analyze_and_choose_ally_to_evolve", [] {
        Scenario s;
        s.place(official(10071120));
        s.hand({official(10874120)});
        s.run({Play, 0});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90071130));
        s.run({Evolve, 1, kOwnBoardTarget});
        CHECK(s.player().board[0].evolution == Evolution::Normal);
    });

    suite.add("nemesis.10571110.base_or_enhanced_puppet_keywords", [] {
        for (const auto [pp, enhanced] : {std::pair{4, false}, {7, true}}) {
            Scenario s;
            s.player().pp = static_cast<std::uint8_t>(pp);
            s.hand({official(10571110)});
            s.run({Play, 0});
            CHECK(s.player().board_count == 3);
            CHECK(s.player().board[1].card_id == official(90071120));
            CHECK(s.player().board[2].card_id == official(90071110));
            CHECK(((s.player().board[1].abilities & FLAG_STORM) != 0) == enhanced);
            CHECK(((s.player().board[2].abilities & FLAG_STORM) != 0) == enhanced);
        }
    });

    suite.add("nemesis.10773110.artifact_entry_grants_rush_in_both_modes", [] {
        for (const auto [pp, count] : {std::pair{3, 2}, {5, 3}}) {
            Scenario s;
            s.player().pp = static_cast<std::uint8_t>(pp);
            s.hand({official(10773110)});
            s.run({Play, 0});
            CHECK(s.player().board_count == count);
            for (int i = 1; i < count; ++i) CHECK(s.player().board[i].abilities & FLAG_RUSH);
        }
    });

    suite.add("nemesis.10773310.artifact_type_count_scales_sweep", [] {
        Scenario s;
        s.hand({official(90071130), official(90071140), official(90071150), official(10773310)});
        s.place(official(10071120), 1);
        s.place(official(10071130), 1);
        for (int slot = 0; slot < 3; ++slot) s.run({Play, 0});
        CHECK(s.player().artifact_types_entered == 3);
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 6);
        CHECK(s.player(1).leader_hp == 19);
    });

    suite.add("nemesis.10772120.artifact_threshold_adds_two_types", [] {
        Scenario s;
        s.player().artifact_types_entered = 3;
        s.hand({official(10772120)});
        s.place(official(10071120), 1);
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0);
        CHECK(s.player().board_count == 3);
        CHECK(s.player().board[1].card_id == official(90071140));
        CHECK(s.player().board[2].card_id == official(90071150));
    });

    suite.add("nemesis.10771110.artifact_heals_and_evolve_summons_mystic", [] {
        Scenario s;
        s.player().leader_hp = 15;
        s.hand({official(90071130)});
        s.place(official(10771110));
        s.run({Play, 0});
        CHECK(s.player().leader_hp == 16 && s.player().artifact_types_entered == 1);
        s.run({Evolve, 0});
        CHECK(s.player().board_count == 3 && s.player().board[2].card_id == official(90071150));
        CHECK(s.player().leader_hp == 17);
    });

    suite.add("nemesis.10774120.artifact_trigger_and_evolve_token", [] {
        Scenario s;
        s.hand({official(90071130)});
        s.place(official(10774120));
        s.place(official(10071130), 1);
        s.run({Play, 0});
        CHECK(s.player().artifact_types_entered == 1 && s.player(1).board_count == 1);
        CHECK(s.player(1).board[0].hp == find_card(official(10071130))->hp - 3);
        s.run({Evolve, 0});
        CHECK(s.player().board_count == 3 && s.player().board[2].card_id == official(90071140));
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 3);
    });

    suite.add("nemesis.10872110.rush_destroyed_returns_ancient", [] {
        Scenario s;
        s.place(official(10872110));
        s.place(official(10071130), 1);
        s.state.turn_player = 1;
        s.run({Attack, 0, 0});
        CHECK(s.player().board_count == 0);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90071140));
    });

    suite.add("nemesis.10971120.fanfare_and_evolve_each_summon_analyze", [] {
        Scenario s;
        s.hand({official(10971120)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == official(90071130));
        s.run({Evolve, 0});
        CHECK(s.player().board_count == 3 && s.player().board[2].card_id == official(90071130));
        CHECK(s.player().artifact_types_entered == 1);
    });

    suite.add("nemesis.10972110.highlander_gate_couples_damage_and_heal", [] {
        for (const bool duplicate : {false, true}) {
            Scenario s;
            s.player().leader_hp = 15;
            s.player().deck_idx = 0;
            s.player().deck_end = 2;
            s.player().deck[0] = official(10071120);
            s.player().deck[1] = official(duplicate ? 10071120 : 10072110);
            s.hand({official(10972110)});
            s.place(official(10071130), 1);
            s.run({Play, 0, duplicate ? kNoTarget : 0});
            CHECK(s.player(1).board[0].hp == (duplicate ? 9 : 5));
            CHECK(s.player().leader_hp == (duplicate ? 15 : 19));
            CHECK(s.player().board[0].abilities & FLAG_WARD);
        }
    });

    suite.add("nemesis.10973110.highlander_grants_storm_after_destroy", [] {
        for (const bool duplicate : {false, true}) {
            Scenario s;
            s.player().deck_idx = 0;
            s.player().deck_end = 2;
            s.player().deck[0] = official(10071120);
            s.player().deck[1] = official(duplicate ? 10071120 : 10072110);
            s.hand({official(10973110)});
            s.place(official(10071120), 1);
            s.run({Play, 0, 0});
            CHECK(s.player(1).board_count == 0);
            CHECK(((s.player().board[0].abilities & FLAG_STORM) != 0) == !duplicate);
        }
    });

    suite.add("nemesis.10072120.normal_and_super_evolve_copy_counts", [] {
        {
            Scenario s;
            s.hand({official(10072120)});
            s.run({Play, 0});
            s.run({Evolve, 0});
            CHECK(s.player().board_count == 2 && s.player().board[1].card_id == official(10072120));
        }
        {
            Scenario s;
            s.hand({official(10072120)});
            s.run({Play, 0});
            s.run({SuperEvolve, 0});
            CHECK(s.player().board_count == 3);
            CHECK(s.player().board[1].card_id == official(10072120) &&
                  s.player().board[2].card_id == official(10072120));
        }
    });

    suite.add("nemesis.10571120.spell_play_triggers_three_damage_sweep", [] {
        Scenario s;
        s.player().deck_idx = 0;
        s.player().deck_end = 2;
        s.player().deck[0] = official(10031310);
        s.player().deck[1] = official(10571310);
        s.place(official(10571120));
        s.place(official(10071120), 1);
        s.place(official(10071120), 1);
        s.hand({official(10571120)});
        s.run({Play, 0});
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 0);
    });

    suite.add("nemesis.10573110.restores_pp_from_allies_and_has_rush", [] {
        Scenario s;
        s.player().pp = 6;
        s.place(official(10071120));
        s.place(official(10072110));
        s.hand({official(10573110)});
        s.run({Play, 0});
        CHECK(s.player().pp == 3 && (s.player().board[2].abilities & FLAG_RUSH));
    });

    suite.add("nemesis.10672110.summons_and_evolves_puppet_pair", [] {
        Scenario s;
        s.hand({official(10672110)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2);
        CHECK(s.player().board[0].evolution == Evolution::Normal &&
              s.player().board[1].evolution == Evolution::Normal);
        CHECK(s.player().board[0].card_id == official(10672110) &&
              s.player().board[1].card_id == official(10672110));
    });

    suite.add("nemesis.10874110.enhance_evolves_with_storm_and_removes_two_wards", [] {
        Scenario s;
        s.hand({official(10874110)});
        s.place(official(10071120), 1);
        s.place(official(10071120), 1).abilities |= FLAG_WARD;
        s.place(official(10071120), 1);
        s.player(1).board[2].abilities |= FLAG_WARD;
        s.run({Play, 0, 0});
        CHECK(s.player().board[0].evolution == Evolution::Normal &&
              (s.player().board[0].abilities & FLAG_STORM));
        CHECK(s.player(1).board_count == 1);
        CHECK(s.player(1).board[0].abilities & FLAG_WARD);
    });

    suite.add("nemesis.10971310.modes_choose_sweep_or_two_mystics", [] {
        {
            Scenario s;
            s.hand({official(10971310)});
            s.place(official(10071120), 1);
            s.run({Play, 0, kNoTarget, 1});
            CHECK(s.player(1).board_count == 0 && s.player().board_count == 0);
        }
        {
            Scenario s;
            s.hand({official(10971310)});
            s.run({Play, 0, kNoTarget, 2});
            CHECK(s.player().board_count == 2);
            CHECK(s.player().board[0].card_id == official(90071150) &&
                  s.player().board[1].card_id == official(90071150));
        }
    });

    suite.add("nemesis.10972120.puppet_entries_damage_enemy_leader", [] {
        Scenario s;
        s.hand({official(10972120)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 3);
        CHECK(s.player().board[1].card_id == official(90071120));
        CHECK(s.player().board[2].card_id == official(90071110));
        CHECK(s.player(1).leader_hp == 18);
    });

    suite.add("nemesis.11072120.launcher_summons_two_artifacts_and_rushes", [] {
        Scenario s;
        s.hand({official(11072120)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 3 && (s.player().board[0].abilities & FLAG_RUSH));
        CHECK(s.player().board[1].card_id == official(90072110));
        CHECK(s.player().board[2].card_id == official(90071140));
        CHECK(s.player().artifact_types_entered == 2);
    });

    suite.add("nemesis.10873110.destroy_and_summon_radiant_artifact", [] {
        Scenario s;
        s.hand({official(10873110)});
        s.place(official(10071120), 1);
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0);
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == official(90071160));
        CHECK(s.player().artifact_types_entered == 1);
    });

    suite.add("nemesis.10573310.transforms_selected_enemy_to_imari_goblin", [] {
        Scenario s;
        s.hand({official(10573310)});
        s.place(official(10071130), 1);
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 1);
        CHECK(s.player(1).board[0].card_id == official(90074140));
        CHECK(s.player(1).board[0].atk == find_card(official(90074140))->atk);
    });

    suite.add("nemesis.11073310.fusion_evidence_and_paid_strongest_destroy", [] {
        Scenario s;
        const auto containment = official(11073310);
        s.place(official(90001210));
        s.place(official(10071130), 1);
        s.place(official(10071120), 1);
        s.hand({containment, kEvidence});
        s.run({Fuse, 0, kNoTarget, 1});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(90001210));
        CHECK(s.player().board[0].clue_count == 2);
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 1);
        CHECK(s.player(1).board[0].card_id == official(10071120));
        CHECK(s.player().leader_hp == 20 && s.player().board_count == 0);
    });

    suite.add("nemesis.11072110.amulet_fusion_damages_enemy_followers", [] {
        Scenario s;
        s.hand({official(90001210)});
        s.place(official(11072110));
        s.place(official(10071130), 1);
        s.place(official(10071130), 1);
        s.hand({official(11073310), kEvidence});
        s.run({Fuse, 0, kNoTarget, 1});
        CHECK(s.player(1).board_count == 2);
        CHECK(s.player(1).board[0].hp == 8 && s.player(1).board[1].hp == 8);
    });

    suite.add("nemesis.11074110.fused_copy_match_destroys_name_and_halves_max_hp", [] {
        Scenario s;
        const auto target = official(10071120);
        s.player(1).leader_hp = 15;
        s.player(1).leader_max_hp = 17;
        s.place(target, 1);
        s.place(target, 1);
        s.place(official(10071130), 1);
        s.hand({official(11074110), kEvidence});
        s.run({Fuse, 0, kNoTarget, 1});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].card_id == official(10071130));
        CHECK(s.player(1).leader_max_hp == 9 && s.player(1).leader_hp == 9);
    });

    suite.add("nemesis.11074110.three_fused_plays_round_half_up", [] {
        Scenario s;
        const auto rampage = official(11074110);
        CHECK(s.player(1).leader_max_hp == 20);
        for (const int expected : {10, 5, 3}) {
            s.hand({rampage, kEvidence});
            s.run({Fuse, 0, kNoTarget, 1});
            s.run({Play, 0});
            CHECK(s.player(1).leader_max_hp == expected && s.player(1).leader_hp == expected);
            if (expected != 3) s.next_own_turn();
        }
    });
}

} // namespace svwb::wb::spec
