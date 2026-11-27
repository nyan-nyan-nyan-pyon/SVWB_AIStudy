#include "spec.hpp"

namespace svwb::wb::spec {

void phase2_nemesis(Suite& suite) {
    suite.add("phase2_nemesis.10071130.bane_ward_combat", [] {
        Scenario s;
        const auto veteran = official(10071130);
        auto& durable_attacker = s.place(kTrainer);
        durable_attacker.atk = 1;
        durable_attacker.hp = durable_attacker.max_hp = 20;
        s.place(kTrainer);
        s.place(veteran, 1);
        s.place(kTrainer, 1);
        CHECK(s.legal({Attack, 0, 0}));
        CHECK(!s.legal({Attack, 0, 1}));
        s.run({Attack, 0, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == kTrainer);
        CHECK(s.player(1).board_count == 2 && s.player(1).board[0].hp == find_card(veteran)->hp - 1);
    });

    suite.add("phase2_nemesis.10572310.discard_then_recover_destroyed", [] {
        Scenario s;
        const auto spell = official(10572310);
        const auto a = official(90071130);
        const auto b = official(90071140);
        s.player().other_counter_count = 2;
        s.player().other_counters[0].id = a;
        s.player().other_counters[0].destroyed = 1;
        s.player().other_counters[1].id = b;
        s.player().other_counters[1].destroyed = 1;
        s.hand({spell, official(10071120), official(10072110)});
        s.run({Play, 0, static_cast<std::int8_t>(kOwnHandTarget + 1)});
        CHECK(s.player().hand_count == 3);
        CHECK(std::count(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count, a) == 1);
        CHECK(std::count(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count, b) == 1);
    });

    suite.add("phase2_nemesis.10574110.sloth_evolve_end_turn", [] {
        Scenario s;
        const auto sloth = official(10574110);
        s.hand({sloth});
        s.run({Play, 0});
        CHECK(s.player().board[0].abilities & kAmbush);
        s.reject({Attack, 0, kEnemyLeader});
        s.run({Evolve, 0});
        s.run({EndTurn});
        CHECK(s.player().board_count == 0);
        CHECK(s.player(1).crest_count == 1 && s.player(1).crests[0].id == CrestId::Sloth);
        CHECK(s.player(1).crests[0].countdown == find_crest(CrestId::Sloth)->countdown - 1);

        Scenario start_trigger;
        start_trigger.player().leader_hp = 10;
        start_trigger.hand({sloth, kTrainer});
        start_trigger.place(kTrainer);
        start_trigger.run({Play, 0});
        start_trigger.next_own_turn();
        const bool cost_branch = start_trigger.player().hand_modifiers[0].temporary_cost_delta == -1;
        const bool buff_branch = start_trigger.player().board[0].atk == find_card(kTrainer)->atk + 2 &&
                                 start_trigger.player().board[0].hp == find_card(kTrainer)->hp + 2;
        const bool heal_branch = start_trigger.player().leader_hp == 13;
        CHECK(static_cast<int>(cost_branch) + static_cast<int>(buff_branch) + static_cast<int>(heal_branch) == 1);
    });

    suite.add("phase2_nemesis.10574120.discard_draw_spell", [] {
        Scenario s;
        const auto imari = official(10574120);
        const auto drawn_spell = official(10571310);
        s.player().deck_idx = 0;
        s.player().deck_end = 1;
        s.player().deck[0] = drawn_spell;
        s.hand({imari, official(10071120)});
        s.run({Play, 0, static_cast<std::int8_t>(kOwnHandTarget + 1)});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == drawn_spell);

        Scenario evolved;
        evolved.player().deck_idx = 0;
        evolved.player().deck_end = 2;
        evolved.player().deck[0] = official(10571310);
        evolved.player().deck[1] = official(10771310);
        evolved.place(imari);
        evolved.run({SuperEvolve, 0});
        CHECK(evolved.player().hand_count == 2);
        CHECK(evolved.player().hand[0] != evolved.player().hand[1]);
        Scenario trigger;
        trigger.place(imari);
        trigger.run({Evolve, 0});
        trigger.hand({official(10571310)});
        trigger.run({Play, 0});
        CHECK(std::count_if(trigger.player().board.begin(), trigger.player().board.begin() + trigger.player().board_count,
                            [](const Permanent& p) { return p.card_id == official(90074140); }) == 1);
    });

    suite.add("phase2_nemesis.10671120.fanfare_artifact_bane_ward", [] {
        Scenario s;
        s.hand({official(10671120)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == official(90073110));
        CHECK((s.player().board[1].abilities & (kBane | FLAG_WARD)) == (kBane | FLAG_WARD));
    });

    suite.add("phase2_nemesis.10672120.banish_small_and_ambush", [] {
        Scenario s;
        s.hand({official(10672120)});
        s.place(official(10012110), 1);
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0);
        CHECK(s.player().board[0].abilities & kAmbush);
        s.reject({Attack, 0, kEnemyLeader});
    });

    suite.add("phase2_nemesis.10672310.summons_three_health_modified_artifacts", [] {
        Scenario s;
        s.hand({official(10672310)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 3);
        CHECK(s.player().board[0].card_id == official(10673110));
        CHECK(s.player().board[1].card_id == official(10671110));
        CHECK(s.player().board[2].card_id == official(10672110));
        CHECK(s.player().board[0].max_hp == find_card(official(10673110))->hp + 1);
        CHECK(s.player().board[1].max_hp == find_card(official(10671110))->hp + 1);
        CHECK(s.player().board[2].max_hp == find_card(official(10672110))->hp + 1);
    });

    suite.add("phase2_nemesis.10673310.large_follower_entry_discount_and_auto_evolve", [] {
        Scenario s;
        const auto large = official(10071130);
        s.hand({large, official(10673310)});
        s.place(official(10071120), 1);
        s.run({Play, 0});
        CHECK(s.player().hand_count == 1 && s.player().hand_modifiers[0].temporary_cost_delta == -1);
        s.run({Play, 0});
        CHECK(s.player().board[0].evolution == Evolution::Normal);
        CHECK(s.player(1).board_count == 0);
    });

    suite.add("phase2_nemesis.10674110.fanfare_summons_golem_and_automatically_evolves_large", [] {
        Scenario s;
        const auto kamishira = official(10674110);
        const auto large = official(10071130);
        s.hand({kamishira, large});
        s.run({Play, 0});
        CHECK(s.player().board_count == 3);
        CHECK(s.player().board[1].card_id == official(10671110));
        CHECK(s.player().board[2].card_id == official(10672110));
        s.player().pp = 10;
        s.run({Play, 0});
        const auto large_on_board = std::find_if(s.player().board.begin(),
            s.player().board.begin() + s.player().board_count,
            [&](const Permanent& p) { return p.card_id == large; });
        CHECK(large_on_board != s.player().board.begin() + s.player().board_count);
        CHECK(large_on_board->evolution == Evolution::Normal);
        s.run({SuperEvolve, 0});
        CHECK(s.player(1).leader_hp == 16);
    });

    suite.add("phase2_nemesis.10771120.artifact_threshold_summon_count", [] {
        for (const auto [types, expected] : {std::pair{2, 2}, {3, 3}}) {
            Scenario s;
            s.player().artifact_types_entered = static_cast<std::uint16_t>(types);
            s.hand({official(10771120)});
            s.run({Play, 0});
            CHECK(s.player().board_count == expected);
            CHECK(s.player().board[0].card_id == official(10771120));
            CHECK(s.player().board[0].abilities & FLAG_RUSH);
            for (int i = 1; i < expected; ++i) CHECK(s.player().board[i].card_id == official(10771120));
        }
    });

    suite.add("phase2_nemesis.10771310.mode_selects_artifact", [] {
        Scenario s;
        s.player().artifact_types_entered = 3;
        s.hand({official(10771310)});
        s.run({Play, 0, kNoTarget, kAllModes});
        CHECK(s.player().hand_count == 2);
        CHECK(std::count(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count,
                         official(90071130)) == 1);
        CHECK(std::count(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count,
                         official(90071140)) == 1);
    });

    suite.add("phase2_nemesis.10774110.artifact_type_sweep_and_storm_ward", [] {
        Scenario s;
        s.player().artifact_types_entered = 2;
        s.hand({official(10774110)});
        s.place(official(10071130), 1);
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == find_card(official(10071130))->hp - 2);
        CHECK((s.player().board[0].abilities & (FLAG_STORM | FLAG_WARD)) == (FLAG_STORM | FLAG_WARD));
    });

    suite.add("phase2_nemesis.10871110.last_words_rebirth_does_not_recur", [] {
        Scenario s;
        const auto kratos = official(10871110);
        constexpr Effect destroy_self{.operation=Operation::Destroy, .target=Target::Self};
        const auto first_order = s.place(kratos).creation_order;
        resolve_effect_steps(s.state, 0, std::array{destroy_self}, first_order);
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == kratos);
        CHECK(s.player().board[0].abilities & FLAG_WARD);
        const auto reborn_order = s.player().board[0].creation_order;
        CHECK(s.player().board[0].modifiers.effect_flags & kLoseLastWords);
        resolve_effect_steps(s.state, 0, std::array{destroy_self}, reborn_order);
        CHECK(s.player().board_count == 0);
        const auto counter = std::find_if(s.player().other_counters.begin(),
            s.player().other_counters.begin() + s.player().other_counter_count,
            [&](const CardCounters& entry) { return entry.id == kratos; });
        CHECK(counter != s.player().other_counters.begin() + s.player().other_counter_count &&
              counter->destroyed == 2);
    });

    suite.add("phase2_nemesis.10871120.super_evolve_grants_ambush", [] {
        Scenario s;
        const auto riona = official(10871120);
        s.place(official(10071120));
        s.hand({riona});
        s.run({Play, 0});
        CHECK(s.player().board[1].abilities & FLAG_WARD);
        s.run({SuperEvolve, 1, kOwnBoardTarget});
        CHECK(s.player().board[0].abilities & kAmbush);
    });

    suite.add("phase2_nemesis.10872310.draw_two_and_super_turn_heal", [] {
        Scenario s;
        s.player().leader_hp = 15;
        s.player().deck_idx = 0;
        s.player().deck_end = 2;
        s.player().deck[0] = official(10071120);
        s.player().deck[1] = official(10072110);
        s.hand({official(10872310)});
        s.run({Play, 0});
        CHECK(s.player().hand_count == 2);
        CHECK(s.player().leader_hp == 17);
    });

    suite.add("phase2_nemesis.10873310.damage_and_artifact_threshold_ep", [] {
        Scenario s;
        s.player().artifact_types_entered = 3;
        s.player().ep = 0;
        s.hand({official(10873310)});
        s.place(official(10071130), 1);
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == find_card(official(10071130))->hp - 6);
        CHECK(s.player().ep == 1);
    });

    suite.add("phase2_nemesis.10973310.highlander_destroys_all", [] {
        Scenario s;
        s.player().deck_idx = 0;
        s.player().deck_end = 2;
        s.player().deck[0] = official(10071120);
        s.player().deck[1] = official(10072110);
        s.hand({official(10973310)});
        s.place(official(10071120), 1);
        s.place(official(10072110), 1);
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 0);
    });

    suite.add("phase2_nemesis.11071120.storm_aura", [] {
        Scenario s;
        const auto visitor = official(11071120);
        s.hand({visitor});
        s.run({Play, 0});
        CHECK((s.player().board[0].abilities & (FLAG_STORM | kAura)) == (FLAG_STORM | kAura));
        CHECK(s.legal({Attack, 0, kEnemyLeader}));
    });

    suite.add("phase2_nemesis.11072310.fuse_summons_evidence_and_doubles_sweep", [] {
        Scenario s;
        const auto arm = official(11072310);
        s.hand({arm, kEvidence});
        s.place(official(10071130), 1);
        s.run({Fuse, 0, kNoTarget, 1});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(90001210));
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == find_card(official(10071130))->hp - 8);
        Scenario unfused;
        unfused.hand({arm});
        unfused.place(official(10071130), 1);
        unfused.run({Play, 0});
        CHECK(unfused.player(1).board[0].hp == find_card(official(10071130))->hp - 4);
    });

    suite.add("phase2_nemesis.11073110.fanfare_evidence_and_fusion_trigger", [] {
        Scenario s;
        const auto gentleman = official(11073110);
        const auto arm = official(11072310);
        s.player().leader_hp = 15;
        s.hand({gentleman});
        s.run({Play, 0});
        CHECK(s.player().board[0].abilities & FLAG_WARD);
        CHECK(s.player().hand_count == 2 && s.player().hand[0] == official(90001210) &&
              s.player().hand[1] == official(90001210));
        s.place(official(10071120), 1);
        s.hand({arm, kEvidence});
        s.run({Fuse, 0, kNoTarget, 1});
        CHECK(s.player(1).board_count == 0 && s.player().leader_hp == 17);
    });

    suite.add("phase2_nemesis.11074120.fusion_rush_last_words_crest_super_storm", [] {
        Scenario s;
        const auto stranger = official(11074120);
        const auto arm = official(11072310);
        s.place(stranger);
        s.hand({arm, kEvidence});
        s.run({Fuse, 0, kNoTarget, 1});
        CHECK(s.player().board[0].abilities & FLAG_RUSH);
        s.run({SuperEvolve, 0});
        CHECK(s.player().board[0].abilities & FLAG_STORM);
        s.place(official(10071130), 1);
        s.state.turn_player = 1;
        s.run({Attack, 0, 0});
        CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::Stranger);
    });
}

} // namespace svwb::wb::spec
