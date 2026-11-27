#include "spec.hpp"

namespace svwb::wb::spec {

void nightmare(Suite& suite) {
    suite.add("nightmare.10653110.chosen_ally_destroy_auto_evolve_and_summon", [] {
        const auto destroyer = official(10653110);
        Scenario s;
        const auto victim = s.place(kTrainer).creation_order;
        s.hand({destroyer});
        s.run({Play, 0, kOwnBoardTarget});
        CHECK(s.player().board_count == 2);
        CHECK(s.player().board[0].creation_order != victim);
        CHECK(s.player().board[0].card_id == destroyer && s.player().board[0].evolution == Evolution::Normal);
        CHECK(s.player().board[1].card_id == official(90051120));
        CHECK(s.player().shadows == 1);

        Scenario no_choice;
        no_choice.hand({destroyer});
        no_choice.run({Play, 0});
        CHECK(no_choice.player().board_count == 1 && no_choice.player().board[0].card_id == destroyer);
        CHECK(no_choice.player().board[0].evolution == Evolution::None);
    });

    suite.add("nightmare.10752110.duplicate_effect_modes", [] {
        const auto grave_song = official(10752110);
        const auto skeleton = official(90051110);
        Scenario s;
        s.place(kTrainer, 1);
        s.hand({grave_song});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
        CHECK(s.player().board_count == 2 && s.player().board[0].card_id == grave_song &&
              s.player().board[1].card_id == skeleton);
        CHECK(s.player().shadows == 0);

        Scenario absent;
        absent.place(kTrainer, 1);
        absent.hand({grave_song});
        absent.run({Play, 0, 0});
        CHECK(absent.player().board_count == 2 && absent.player().board[0].card_id == grave_song &&
              absent.player().board[1].card_id == skeleton);
        CHECK(absent.player().shadows == 0);
    });

    suite.add("nightmare.10953110.fanfare_and_super_evolve_order", [] {
        const auto follower = official(10953110);
        Scenario s;
        s.player().leader_hp = 10;
        s.player(1).leader_hp = 10;
        s.place(kTrainer, 1).hp = 3;
        s.hand({follower});
        const auto deck_cursor = s.player().deck_idx;
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
        CHECK(s.player().leader_hp == 7 && s.player(1).leader_hp == 7);
        CHECK(s.player().deck_idx == deck_cursor);
        s.run({SuperEvolve, 0});
        CHECK(s.player().leader_hp == 6 && s.player(1).leader_hp == 6);
        CHECK(s.player().hand_count == 2 && s.player().deck_idx == deck_cursor + 2);
        CHECK(s.player().board[0].evolution == Evolution::Super);
    });

    suite.add("nightmare.10852120.follower_and_both_leader_damage", [] {
        const auto follower = official(10852120);
        Scenario s;
        s.player().leader_hp = 9;
        s.player(1).leader_hp = 8;
        s.place(kTrainer, 1).hp = 1;
        s.hand({follower});
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
        CHECK(s.player().leader_hp == 8 && s.player(1).leader_hp == 7);
        s.run({Evolve, 0});
        CHECK(s.player().leader_hp == 7 && s.player(1).leader_hp == 6);
    });

    suite.add("nightmare.11053110.ghost_banish_heals_only_own_turn", [] {
        const auto follower = official(11053110);
        const auto ghost = official(90051130);
        constexpr Effect banish_self{.operation=Operation::Banish, .target=Target::Self};
        Scenario s;
        s.player().leader_hp = 10;
        s.hand({follower});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == ghost);
        const auto ghost_order = s.player().board[1].creation_order;
        resolve_effect_steps(s.state, 0, std::array{banish_self}, ghost_order);
        CHECK(s.player().leader_hp == 12 && s.player().shadows == 0);

        Scenario enemy_turn;
        enemy_turn.player().leader_hp = 10;
        enemy_turn.place(follower);
        const auto own_ghost = enemy_turn.place(ghost).creation_order;
        enemy_turn.state.turn_player = 1;
        resolve_effect_steps(enemy_turn.state, 0, std::array{banish_self}, own_ghost);
        CHECK(enemy_turn.player().leader_hp == 10 && enemy_turn.player().shadows == 0);
    });

    suite.add("nightmare.10753110.dead_entries_grant_storm_and_super_evolve_targets", [] {
        const auto follower = official(10753110);
        const auto shadow = official(90051140);
        const auto skeleton = official(90051110);
        Scenario s;
        s.place(kTrainer, 1);
        s.player(1).leader_hp = 10;
        s.hand({follower});
        s.run({Play, 0});
        CHECK(s.player().board_count == 3);
        CHECK(s.player().board[1].card_id == shadow && s.player().board[2].card_id == skeleton);
        CHECK((s.player().board[1].abilities & FLAG_STORM) && (s.player().board[2].abilities & FLAG_STORM));
        const auto first_zombie = s.player().board[1].creation_order;
        s.run({SuperEvolve, 0, kOwnBoardTarget + 1});
        CHECK(s.player().board_count == 3 && s.player().board[1].card_id == skeleton);
        CHECK(s.player().board[2].card_id == shadow && s.player().board[2].creation_order != first_zombie);
        CHECK(s.player().board[2].abilities & FLAG_STORM);
        CHECK(s.player(1).board_count == 0);
        CHECK(s.player().board[0].evolution == Evolution::Super);
        constexpr Effect destroy_zombie{.operation = Operation::Destroy, .target = Target::ChosenOwnFollower};
        resolve_effect_steps(s.state, 0, std::span(&destroy_zombie, 1), 0,
                             s.player().board[2].creation_order);
        CHECK(s.player().board_count == 2);
    });

    suite.add("nightmare.10754120.necromancy_threshold_and_dead_entry_trigger", [] {
        const auto necromancer = official(10754120);
        const auto shadow = official(90051140);
        Scenario paid;
        paid.player().shadows = 10;
        paid.player(1).leader_hp = 10;
        paid.hand({necromancer});
        paid.run({Play, 0});
        CHECK(paid.player().shadows == 0 && paid.player().board_count == 4);
        CHECK(paid.player().board[1].card_id == shadow && paid.player().board[2].card_id == shadow &&
              paid.player().board[3].card_id == shadow);
        CHECK((paid.player().board[1].abilities & (FLAG_RUSH | FLAG_WARD)) == (FLAG_RUSH | FLAG_WARD));
        CHECK(paid.player(1).leader_hp == 7);

        Scenario insufficient;
        insufficient.player().shadows = 9;
        insufficient.hand({necromancer});
        insufficient.run({Play, 0});
        CHECK(insufficient.player().shadows == 9 && insufficient.player().board_count == 1);
    });

    suite.add("nightmare.10051130.necromancy_four_grants_storm_only_when_paid", [] {
        const auto card = official(10051130);
        Scenario paid;
        paid.player().shadows = 4;
        paid.hand({card});
        paid.run({Play, 0});
        CHECK(paid.player().shadows == 0 && (paid.player().board[0].abilities & FLAG_STORM));

        Scenario under_threshold;
        under_threshold.player().shadows = 3;
        under_threshold.hand({card});
        under_threshold.run({Play, 0});
        CHECK(under_threshold.player().shadows == 3 &&
              !(under_threshold.player().board[0].abilities & FLAG_STORM));
    });

    suite.add("nightmare.10051310.mode_draw_follower_or_reanimate", [] {
        const auto card = official(10051310);
        Scenario draw;
        draw.hand({card});
        draw.run({Play, 0, kNoTarget, 1});
        CHECK(draw.player().hand_count == 1 && find_card(draw.player().hand[0])->kind == Kind::Follower);

        Scenario reanimate;
        const auto skeleton = official(90051110);
        const auto one = reanimate.place(skeleton).creation_order;
        const auto two = reanimate.place(skeleton).creation_order;
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        resolve_effect_steps(reanimate.state, 0, std::array{destroy}, one);
        resolve_effect_steps(reanimate.state, 0, std::array{destroy}, two);
        reanimate.hand({card});
        reanimate.run({Play, 0, kNoTarget, 2});
        CHECK(reanimate.player().board_count == 1 && reanimate.player().board[0].card_id == skeleton &&
              (reanimate.player().board[0].modifiers.effect_flags & kDeadTrait));
        CHECK(reanimate.player().shadows == 3); // Two destroyed Skeletons and the spell itself.
    });

    suite.add("nightmare.10052310.destroy_chosen_ally_then_draw_two", [] {
        const auto card = official(10052310);
        Scenario s;
        const auto victim = s.place(kTrainer).creation_order;
        s.hand({card});
        const auto cursor = s.player().deck_idx;
        s.run({Play, 0, kOwnBoardTarget});
        CHECK(s.player().board_count == 0);
        CHECK(s.player().shadows == 2 && s.player().deck_idx == cursor + 2 && s.player().hand_count == 2);
        CHECK(std::none_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                           [victim](const auto& object) { return object.creation_order == victim; }));
    });

    suite.add("nightmare.10551120.reanimate_on_play_and_evolve", [] {
        const auto card = official(10551120);
        const auto skeleton = official(90051110);
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        Scenario s;
        std::array<EntityId, 4> dead{};
        for (auto& order : dead) order = s.place(skeleton).creation_order;
        for (const auto order : dead) resolve_effect_steps(s.state, 0, std::array{destroy}, order);
        s.hand({card});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == skeleton &&
              (s.player().board[1].modifiers.effect_flags & kDeadTrait));
        s.run({Evolve, 0});
        CHECK(s.player().board_count == 3 && s.player().board[0].evolution == Evolution::Normal);
        CHECK(s.player().board[2].card_id == skeleton &&
              (s.player().board[2].modifiers.effect_flags & kDeadTrait));
    });

    suite.add("nightmare.10551310.damage_target_and_shuffle_copy_into_deck", [] {
        const auto card = official(10551310);
        Scenario s;
        s.place(kTrainer, 1).hp = 5;
        s.hand({card});
        const auto before = std::count(s.player().deck.begin() + s.player().deck_idx,
                                       s.player().deck.begin() + s.player().deck_end, card);
        s.run({Play, 0, 0});
        const auto after = std::count(s.player().deck.begin() + s.player().deck_idx,
                                      s.player().deck.begin() + s.player().deck_end, card);
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
        CHECK(after == before + 1);
    });

    suite.add("nightmare.10552310.damage_only_lowest_health_leader", [] {
        const auto card = official(10552310);
        Scenario s;
        s.player().leader_hp = 15;
        s.player(1).leader_hp = 10;
        s.hand({card});
        s.run({Play, 0});
        CHECK(s.player().leader_hp == 15 && s.player(1).leader_hp == 7);

        Scenario tied;
        tied.player().leader_hp = tied.player(1).leader_hp = 10;
        tied.hand({card});
        tied.run({Play, 0});
        CHECK(tied.player().leader_hp == 7 && tied.player(1).leader_hp == 7);
    });

    suite.add("nightmare.10651310.draw_two_then_necromancy_heal", [] {
        const auto card = official(10651310);
        Scenario paid;
        paid.player().leader_hp = 10;
        paid.player().shadows = 4;
        paid.hand({card});
        const auto cursor = paid.player().deck_idx;
        paid.run({Play, 0});
        CHECK(paid.player().hand_count == 2 && paid.player().deck_idx == cursor + 2);
        CHECK(paid.player().leader_hp == 12 && paid.player().shadows == 1);

        Scenario unpaid;
        unpaid.player().leader_hp = 10;
        unpaid.player().shadows = 2;
        unpaid.hand({card});
        unpaid.run({Play, 0});
        CHECK(unpaid.player().leader_hp == 10 && unpaid.player().shadows == 3);
    });

    suite.add("nightmare.10653310.mode_restore_ep_or_damage_enemy_board", [] {
        const auto card = official(10653310);
        Scenario ep;
        ep.player().ep = 1;
        ep.hand({card});
        ep.run({Play, 0, kNoTarget, 1});
        CHECK(ep.player().ep == 2);

        Scenario clear;
        clear.place(kTrainer, 1).hp = 2;
        clear.place(kLilium, 1).hp = 2;
        clear.hand({card});
        clear.run({Play, 0, kNoTarget, 2});
        CHECK(clear.player(1).board_count == 0 && clear.player(1).shadows == 2);
    });

    suite.add("nightmare.10753310.split_six_and_paid_leader_damage", [] {
        const auto card = official(10753310);
        Scenario paid;
        paid.player().shadows = 6;
        paid.player(1).leader_hp = 10;
        paid.place(kTrainer, 1).hp = 2;
        paid.place(kLilium, 1).hp = 4;
        paid.hand({card});
        paid.run({Play, 0});
        CHECK(paid.player(1).board_count == 0 && paid.player(1).shadows == 2);
        CHECK(paid.player().shadows == 1 && paid.player(1).leader_hp == 8);

        Scenario unpaid;
        unpaid.player().shadows = 4;
        unpaid.player(1).leader_hp = 10;
        unpaid.hand({card});
        unpaid.run({Play, 0});
        CHECK(unpaid.player().shadows == 5 && unpaid.player(1).leader_hp == 10);
    });

    suite.add("nightmare.10852310.fourth_mode_adds_four_shadows", [] {
        const auto card = official(10852310);
        Scenario s;
        s.player().shadows = 2;
        s.hand({card});
        s.run({Play, 0, kNoTarget, kModeMask | 0b1001});
        CHECK(s.player().shadows == 7 && s.player(1).leader_hp == 19);
    });

    suite.add("nightmare.10853110.play_debuff_and_evolve_hand_buff", [] {
        const auto card = official(10853110);
        Scenario s;
        s.place(kCorrector, 1);
        s.hand({card, kRabbit});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board[0].hp == 2);
        s.run({Evolve, 0, kOwnHandTarget});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == kRabbit);
        CHECK(s.player().hand_modifiers[0].attack == 3);
    });

    suite.add("nightmare.10951110.fanfare_damages_both_leaders", [] {
        const auto card = official(10951110);
        Scenario s;
        s.player().leader_hp = 13;
        s.player(1).leader_hp = 11;
        s.hand({card});
        s.run({Play, 0});
        CHECK(s.player().leader_hp == 11 && s.player(1).leader_hp == 9);
    });

    suite.add("nightmare.10952310.base_and_enhanced_chain_counts", [] {
        const auto card = official(10952310);
        Scenario base;
        base.player().leader_hp = base.player(1).leader_hp = 10;
        auto& base_target = base.place(official(10754120), 1);
        base_target.hp = base_target.max_hp = 100;
        base.hand({card});
        base.player().pp = 2;
        base.run({Play, 0});
        CHECK(base.player().leader_hp == 9 && base.player(1).leader_hp == 9);
        CHECK(base.player(1).board[0].hp == 97);

        Scenario enhanced;
        enhanced.player().leader_hp = enhanced.player(1).leader_hp = 10;
        auto& enhanced_target = enhanced.place(official(10754120), 1);
        enhanced_target.hp = enhanced_target.max_hp = 100;
        enhanced.hand({card});
        enhanced.player().pp = 4;
        enhanced.run({Play, 0});
        CHECK(enhanced.player().leader_hp == 8 && enhanced.player(1).leader_hp == 8);
        CHECK(enhanced.player(1).board[0].hp == 94);
    });

    suite.add("nightmare.10052110.last_words_add_skeleton_to_hand", [] {
        const auto card = official(10052110);
        const auto skeleton = official(90051120);
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        Scenario s;
        const auto order = s.place(card).creation_order;
        resolve_effect_steps(s.state, 0, std::array{destroy}, order);
        CHECK(s.player().board_count == 0 && s.player().shadows == 1);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == skeleton);
    });

    suite.add("nightmare.10851120.attack_triggers_both_leaders_and_death_adds_skeleton", [] {
        const auto card = official(10851120);
        const auto skeleton = official(90051120);
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        Scenario s;
        s.player().leader_hp = s.player(1).leader_hp = 10;
        auto& attacker = s.place(card);
        attacker.readied = true;
        s.run({Attack, 0, kEnemyLeader});
        CHECK(s.player().leader_hp == 9 && s.player(1).leader_hp == 8);
        resolve_effect_steps(s.state, 0, std::array{destroy}, s.player().board[0].creation_order);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == skeleton);
    });

    suite.add("nightmare.10751120.evolve_target_and_last_words_skeleton", [] {
        const auto card = official(10751120);
        const auto skeleton = official(90051110);
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        Scenario s;
        s.place(kTrainer, 1).hp = 4;
        const auto order = s.place(card).creation_order;
        s.run({Evolve, 0, 0});
        CHECK(s.player(1).board[0].hp == 1);
        resolve_effect_steps(s.state, 0, std::array{destroy}, order);
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == skeleton);
    });

    suite.add("nightmare.10851130.hp_ahead_controls_two_follower_summon", [] {
        const auto card = official(10851130);
        const auto token = official(90051120);
        Scenario ahead;
        ahead.player().leader_hp = 15;
        ahead.player(1).leader_hp = 14;
        ahead.hand({card});
        ahead.run({Play, 0});
        CHECK(ahead.player().board_count == 3 && ahead.player().board[1].card_id == token &&
              ahead.player().board[2].card_id == token);

        Scenario behind;
        behind.player().leader_hp = 14;
        behind.player(1).leader_hp = 15;
        behind.hand({card});
        behind.run({Play, 0});
        CHECK(behind.player().board_count == 1);
    });

    suite.add("nightmare.11051110.last_words_summon_and_add_evidence", [] {
        const auto card = official(11051110);
        const auto evidence = official(90001210);
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        Scenario s;
        const auto order = s.place(card).creation_order;
        resolve_effect_steps(s.state, 0, std::array{destroy}, order);
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == evidence);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == evidence && s.player().shadows == 1);
    });

    suite.add("nightmare.10651110.last_words_draw_then_damage_own_leader", [] {
        const auto card = official(10651110);
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        Scenario s;
        s.player().leader_hp = 10;
        const auto cursor = s.player().deck_idx;
        const auto order = s.place(card).creation_order;
        resolve_effect_steps(s.state, 0, std::array{destroy}, order);
        CHECK(s.player().hand_count == 1 && s.player().deck_idx == cursor + 1);
        CHECK(s.player().leader_hp == 9 && s.player().shadows == 1);
    });

    suite.add("nightmare.10051120.fanfare_self_damage", [] {
        const auto card = official(10051120);
        Scenario s;
        s.player().leader_hp = 10;
        s.hand({card});
        s.run({Play, 0});
        CHECK(s.player().leader_hp == 9 && s.player().board[0].card_id == card);
    });
}

} // namespace svwb::wb::spec
