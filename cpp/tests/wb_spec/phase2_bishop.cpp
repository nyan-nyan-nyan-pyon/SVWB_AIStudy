#include "spec.hpp"

namespace svwb::wb::spec {

void phase2_bishop(Suite& suite) {
    suite.add("phase2_bishop.10061120.ward_blocks_face_and_stays_static", [] {
        Scenario s;
        const auto fox = official(10061120);
        s.hand({fox});
        s.run({Play, 0});
        CHECK((s.player().board[0].abilities & FLAG_WARD) != 0);
        s.place(fox, 1);
        s.state.turn_player = 1;
        CHECK(!s.legal({Attack, 0, kEnemyLeader}));
        CHECK(s.legal({Attack, 0, 0}));
    });

    suite.add("phase2_bishop.10062120.amulet_act_grants_storm_only_to_self", [] {
        Scenario s;
        const auto gryphon = official(10062120);
        s.place(gryphon);
        s.place(official(10562210));
        CHECK((s.player().board[0].abilities & FLAG_WARD) != 0);
        CHECK((s.player().board[0].abilities & FLAG_STORM) == 0);
        s.run({Act, 1});
        CHECK((s.player().board[0].abilities & FLAG_STORM) != 0);
        s.run({Attack, 0, kEnemyLeader});
        CHECK(s.player(1).leader_hp == 14);

        Scenario enemy_act;
        enemy_act.place(gryphon, 1);
        enemy_act.place(official(10562210));
        enemy_act.state.turn_player = 0;
        enemy_act.run({Act, 0});
        CHECK((enemy_act.player(1).board[0].abilities & FLAG_STORM) == 0);
    });

    suite.add("phase2_bishop.10561120.enhance_draw_and_bane_gate", [] {
        const auto card = official(10561120);
        Scenario normal;
        normal.player().pp = 3;
        normal.hand({card});
        const auto cursor = normal.player().deck_idx;
        normal.run({Play, 0});
        CHECK(normal.player().deck_idx == cursor);
        CHECK((normal.player().board[0].abilities & kBane) == 0);

        Scenario enhanced;
        enhanced.player().pp = 4;
        enhanced.hand({card});
        enhanced.player().deck[enhanced.player().deck_idx] = kTrainer;
        const auto enhanced_cursor = enhanced.player().deck_idx;
        enhanced.run({Play, 0});
        CHECK(enhanced.player().deck_idx == enhanced_cursor + 1);
        CHECK((enhanced.player().board[0].abilities & kBane) != 0);
        CHECK((enhanced.player().board[0].abilities & FLAG_RUSH) != 0);
        CHECK(enhanced.player().hand_count == 1 && enhanced.player().hand[0] == kTrainer);
    });

    suite.add("phase2_bishop.10562210.acts_to_draw_exact_ward_count", [] {
        Scenario zero;
        zero.place(official(10562210));
        zero.place(kTrainer);
        zero.player().deck[zero.player().deck_idx] = kEvidence;
        zero.run({Act, 0});
        CHECK(zero.player().board_count == 1 && zero.player().board[0].card_id == kTrainer);
        CHECK(zero.player().hand_count == 0);

        Scenario count;
        count.place(official(10562210));
        count.place(official(10061120));
        count.place(official(10061120));
        count.place(kTrainer);
        count.player().deck[count.player().deck_idx] = kRapid;
        count.player().deck[count.player().deck_idx + 1] = kEvidence;
        count.run({Act, 0});
        CHECK(count.player().board_count == 3);
        CHECK(count.player().hand_count == 2 && count.player().hand[0] == kRapid &&
              count.player().hand[1] == kEvidence);
    });

    suite.add("phase2_bishop.10564110.modes_evolve_and_evolved_self_sweeps_others", [] {
        Scenario self_mode;
        const auto sofina = official(10564110);
        auto& ally = self_mode.place(kTrainer);
        const auto ally_atk = ally.atk;
        auto& enemy = self_mode.place(kTrainer, 1);
        const auto enemy_atk = enemy.atk;
        self_mode.hand({sofina});
        self_mode.run({Play, 0, kNoTarget, 1});
        CHECK(self_mode.player().board[1].evolution == Evolution::Normal);
        CHECK((self_mode.player().board[1].abilities & FLAG_WARD) != 0);
        self_mode.next_own_turn();
        CHECK(self_mode.player().board[0].atk == ally_atk - 1);
        CHECK(self_mode.player(1).board[0].atk == enemy_atk - 1);

        Scenario other_mode;
        other_mode.place(official(10061120));
        other_mode.hand({sofina});
        other_mode.run({Play, 0, kNoTarget, 2});
        CHECK(other_mode.player().board[0].evolution == Evolution::Normal);
        CHECK(other_mode.player().board[0].atk == 4 && other_mode.player().board[0].hp == 6);
        CHECK(other_mode.player().board[1].evolution == Evolution::None);
    });

    suite.add("phase2_bishop.10564120.crests_returns_two_then_draws_and_rushes", [] {
        Scenario s;
        const auto kukishiro = official(10564120);
        auto& target = s.place(kTrainer, 1);
        target.hp = target.max_hp = 5;
        const auto cursor = s.player().deck_idx;
        // Every drawn/returned card costs seven or more, so the granted Crest does not summon.
        s.hand({kukishiro, official(10764120), official(11063110), official(11004110)});
        s.player().deck[cursor] = official(11063110);
        s.player().deck[cursor + 1] = official(10764120);
        s.player().deck_end = static_cast<std::uint8_t>(cursor + 2);
        s.run({Play, 0});
        CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::Kukishiro);
        CHECK(s.player().deck_idx == cursor + 2 && s.player().hand_count == 3);
        CHECK(s.player().board[0].abilities & FLAG_RUSH);
        CHECK(s.legal({Attack, 0, 0}));
        s.run({Attack, 0, 0});
        CHECK(s.player().board[0].hp == 2 && s.player(1).board_count == 0);
    });

    suite.add("phase2_bishop.10662110.rush_bane_trade_and_crystal_form", [] {
        Scenario s;
        const auto paste = official(10662110);
        auto& target = s.place(kTrainer, 1);
        target.hp = target.max_hp = 3;
        s.hand({paste});
        s.run({Play, 0});
        CHECK((s.player().board[0].abilities & FLAG_RUSH) != 0);
        CHECK((s.player().board[0].abilities & kBane) != 0);
        s.run({Attack, 0, 0});
        CHECK(s.player().board_count == 0 && s.player(1).board_count == 0);
        CHECK(find_card(paste)->crystal_form == official(10662112));
    });

    suite.add("phase2_bishop.10663210.fanfare_refunds_only_for_amulet_target", [] {
        const auto book = official(10663210);
        Scenario amulet;
        amulet.place(official(10562210));
        amulet.hand({book});
        amulet.run({Play, 0, static_cast<std::int8_t>(kOwnBoardTarget)});
        CHECK(amulet.player().pp == 8 && amulet.player().board_count == 1);
        CHECK(amulet.player().board[0].card_id == book && amulet.player().shadows == 1);

        Scenario follower;
        follower.place(kTrainer);
        follower.hand({book});
        follower.run({Play, 0, static_cast<std::int8_t>(kOwnBoardTarget)});
        CHECK(follower.player().pp == 6 && follower.player().board_count == 1);
        CHECK(follower.player().board[0].card_id == book);
    });

    suite.add("phase2_bishop.10664110.super_evolve_destroy_amulet_sweeps_enemy", [] {
        Scenario reanimate;
        reanimate.player().other_counters[0] = {official(10062210), 2, {}, 0};
        reanimate.player().other_counters[1] = {official(10661210), 1, {}, 0};
        reanimate.player().other_counter_count = 2;
        reanimate.hand({official(10664110)});
        reanimate.run({Play, 0});
        CHECK(reanimate.player().board_count == 3);
        const auto summoned_first = reanimate.player().board[1].card_id;
        const auto summoned_second = reanimate.player().board[2].card_id;
        CHECK((summoned_first == official(10062210) && summoned_second == official(10661210)) ||
              (summoned_first == official(10661210) && summoned_second == official(10062210)));

        Scenario amulet_target;
        amulet_target.place(official(10664110));
        amulet_target.place(official(10562210));
        auto& first = amulet_target.place(kTrainer, 1);
        first.hp = first.max_hp = 3;
        auto& second = amulet_target.place(kTrainer, 1);
        second.hp = second.max_hp = 4;
        amulet_target.run({SuperEvolve, 0, static_cast<std::int8_t>(kOwnBoardTarget + 1)});
        CHECK(amulet_target.player().board_count == 1);
        CHECK(amulet_target.player(1).board_count == 1 && amulet_target.player(1).board[0].hp == 1);
        CHECK(amulet_target.player().shadows == 1);

        Scenario follower_target;
        follower_target.place(official(10664110));
        follower_target.place(kTrainer);
        auto& victim = follower_target.place(kTrainer, 1);
        victim.hp = victim.max_hp = 4;
        follower_target.run({SuperEvolve, 0, static_cast<std::int8_t>(kOwnBoardTarget + 1)});
        CHECK(follower_target.player(1).board_count == 1 && follower_target.player(1).board[0].hp == 4);
    });

    suite.add("phase2_bishop.10664120.fanfare_three_targets_and_faith_threshold", [] {
        Scenario s;
        const auto book = official(10664120);
        s.player().faiths[0] = official(10664122);
        s.player().faith_count = 1;
        s.player().faith_values[0] = 9;
        s.place(official(10562210));
        s.place(official(10061120));
        s.place(kTrainer, 1);
        s.hand({book});
        s.run({Play, 0, kNoTarget, 0, kBothBoardsSelection | 3 | (1 << kBoardLimit)});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == book);
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
        CHECK(s.player().faith_values[0] == 10);
        s.run({EndTurn});
        CHECK(s.player().faith_values[0] == 0);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90064320));
    });

    suite.add("phase2_bishop.10761120.draws_only_amulets_and_evolve_damage_is_hand_count", [] {
        Scenario s;
        const auto card = official(10761120);
        s.hand({card});
        s.player().deck_end = static_cast<std::uint8_t>(s.player().deck_idx + 3);
        s.player().deck[s.player().deck_idx] = kTrainer;
        s.player().deck[s.player().deck_idx + 1] = official(10562210);
        s.player().deck[s.player().deck_idx + 2] = official(10663210);
        s.run({Play, 0});
        CHECK(s.player().hand_count == 2);
        CHECK((s.player().hand[0] == official(10562210) && s.player().hand[1] == official(10663210)) ||
              (s.player().hand[0] == official(10663210) && s.player().hand[1] == official(10562210)));
        auto& low = s.place(kTrainer, 1);
        low.hp = low.max_hp = 2;
        auto& high = s.place(kTrainer, 1);
        high.hp = high.max_hp = 3;
        s.next_own_turn();
        s.run({Evolve, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 1);
    });

    suite.add("phase2_bishop.10764120.banish_and_heal_three_amulet_threshold", [] {
        for (const auto [amulet_count, expected_hp] : {std::pair{2, 15}, std::pair{3, 18}}) {
            Scenario s;
            s.player().leader_hp = 15;
            for (int i = 0; i < amulet_count; ++i) s.place(official(10562210));
            s.place(kTrainer, 1);
            s.hand({official(10764120)});
            s.run({Play, 0, 0});
            CHECK(s.player().leader_hp == expected_hp);
            CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 0);
        }
    });

    suite.add("phase2_bishop.10862120.discount_only_on_super_evolution", [] {
        Scenario normal;
        const auto researcher = official(10862120);
        normal.hand({researcher, kTrainer});
        normal.run({Play, 1});
        normal.run({Evolve, 0});
        CHECK(card_cost(*find_card(researcher), normal.player().hand_modifiers[0]) == 6);

        Scenario super_evolved;
        super_evolved.hand({researcher, kTrainer});
        super_evolved.run({Play, 1});
        super_evolved.run({SuperEvolve, 0});
        CHECK(card_cost(*find_card(researcher), super_evolved.player().hand_modifiers[0]) == 3);
        super_evolved.player().pp = 3;
        super_evolved.run({Play, 0});
        CHECK(super_evolved.player().board_count == 2);
        CHECK((super_evolved.player().board[1].abilities & FLAG_RUSH) != 0);
        CHECK((super_evolved.player().board[1].abilities & kBane) != 0);
    });

    suite.add("phase2_bishop.10864110.summons_and_super_evolves_only_cost_two_or_less", [] {
        Scenario eligible;
        eligible.hand({official(10864110)});
        eligible.player().deck_end = static_cast<std::uint8_t>(eligible.player().deck_idx + 2);
        eligible.player().deck[eligible.player().deck_idx] = official(10061120);
        eligible.player().deck[eligible.player().deck_idx + 1] = official(10561120);
        eligible.run({Play, 0});
        CHECK(eligible.player().board_count == 2);
        CHECK(eligible.player().board[1].card_id == official(10061120));
        CHECK(eligible.player().board[1].evolution == Evolution::Super);
        eligible.run({SuperEvolve, 0});
        CHECK(eligible.player().crest_count == 1 && eligible.player().crests[0].id == CrestId::Sisters);

        Scenario none;
        none.hand({official(10864110)});
        none.player().deck_end = static_cast<std::uint8_t>(none.player().deck_idx + 1);
        none.player().deck[none.player().deck_idx] = official(10964110);
        none.run({Play, 0});
        CHECK(none.player().board_count == 1);
    });

    suite.add("phase2_bishop.10961110.ward_and_own_turn_leader_heal_auto_evolves", [] {
        Scenario own_turn;
        own_turn.player().leader_hp = 15;
        own_turn.place(official(10961110));
        own_turn.place(official(10562210));
        own_turn.place(official(10562210));
        own_turn.place(official(10562210));
        own_turn.hand({official(10764120)});
        own_turn.place(kTrainer, 1);
        own_turn.run({Play, 0, 0});
        CHECK(own_turn.player().board[0].evolution == Evolution::Normal);
        CHECK(own_turn.player().board[0].abilities & FLAG_WARD);

        Scenario opponent_turn;
        opponent_turn.player(1).leader_hp = 15;
        opponent_turn.place(official(10961110));
        opponent_turn.place(official(10562210));
        opponent_turn.place(official(10562210));
        opponent_turn.place(official(10562210));
        opponent_turn.hand({official(10764120)}, 1);
        opponent_turn.place(kTrainer);
        opponent_turn.state.turn_player = 1;
        opponent_turn.run({Play, 0, 0});
        CHECK(opponent_turn.player().board[0].evolution == Evolution::None);
    });

    suite.add("phase2_bishop.10964110.fanfare_destroys_and_evolution_grants_crest", [] {
        Scenario s;
        const auto erralde = official(10964110);
        s.place(kTrainer, 1);
        s.hand({erralde});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
        s.run({Evolve, 0});
        CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::Erralde);
    });

    suite.add("phase2_bishop.10964120.amulet_destruction_sequence_and_evolve_sweep", [] {
        Scenario s;
        s.player().leader_hp = 15;
        s.place(official(10964120));
        auto& first = s.place(kTrainer, 1);
        first.hp = first.max_hp = 3;
        auto& second = s.place(kTrainer, 1);
        second.hp = second.max_hp = 3;
        s.place(official(10562210));
        s.run({Act, 1});
        CHECK(s.player(1).board_count == 0 && s.player().leader_hp == 15);
        s.place(official(10562210));
        s.run({Act, 1});
        CHECK(s.player().leader_hp == 17);
        s.place(official(10562210));
        s.run({Act, 1});
        CHECK(s.player().board_count == 2);
        CHECK(s.player().board[1].card_id == official(90061110));

        Scenario evolve;
        evolve.place(official(10964120));
        evolve.place(official(10562210));
        evolve.place(official(10562210));
        evolve.run({Evolve, 0});
        CHECK(evolve.player().board_count == 1 && evolve.player().board[0].card_id == official(10964120));
        CHECK(evolve.player().shadows == 2);
    });

    suite.add("phase2_bishop.11064120.intimidate_aura_banish_copy_scope", [] {
        Scenario s;
        const auto idol = official(11064120);
        s.place(idol);
        CHECK((s.player().board[0].abilities & kIntimidate) != 0);
        CHECK((s.player().board[0].abilities & kAura) != 0);
        CHECK((s.player().board[0].abilities & kAttackBlocked) != 0);
        CHECK(!s.legal({Attack, 0, kEnemyLeader}));
        s.place(kTrainer, 1);
        s.state.turn_player = 1;
        CHECK(!s.legal({Attack, 0, 0}));
        s.state.turn_player = 0;
        s.run({SuperEvolve, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 0);
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == kTrainer);

        Scenario opponent_banish;
        opponent_banish.place(idol, 1);
        opponent_banish.place(kTrainer, 1);
        opponent_banish.place(official(10562210));
        opponent_banish.place(official(10562210));
        opponent_banish.place(official(10562210));
        opponent_banish.hand({official(10764120)});
        opponent_banish.run({Play, 0, 1});
        CHECK(opponent_banish.player(1).board_count == 1 &&
              opponent_banish.player(1).board[0].card_id == idol);
        CHECK(opponent_banish.player().board_count == 4);
        CHECK(std::none_of(opponent_banish.player().board.begin(),
                           opponent_banish.player().board.begin() + opponent_banish.player().board_count,
                           [idol](const auto& permanent) { return permanent.card_id == idol; }));
    });
}

} // namespace svwb::wb::spec
