#include "spec.hpp"

namespace svwb::wb::spec {

void phase3_decks(Suite& suite) {
    suite.add("phase3_decks.10503210.matching_original_cost_ticks_and_resolves_last_words", [] {
        Scenario s;
        const auto amulet = official(10503210);
        s.hand({amulet, official(10031310)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].countdown == 5);
        s.player().board[0].countdown = 1;
        s.place(official(10021110), 1).modifiers.cost = 2; // Another original-cost-one card.
        s.run({Play, 0});
        CHECK(s.player().board_count == 0);
        CHECK(s.player().hand_count == 3);  // One from the spell and two from Last Words.
    });

    suite.add("phase3_decks.10503210.nonmatching_original_cost_does_not_tick", [] {
        Scenario s;
        const auto amulet = official(10503210);
        s.place(amulet).countdown = 3;
        s.hand({official(10031310)});
        s.run({Play, 0});
        CHECK(s.player().board[0].card_id == amulet && s.player().board[0].countdown == 3);
    });

    suite.add("phase3_decks.10854120.necromancy_twenty_discounts_nightmare_hand_only", [] {
        Scenario s;
        const auto ceres = official(10854120);
        const auto outsider = official(10503210);
        s.player().shadows = 20;
        s.hand({ceres, ceres, outsider});
        s.run({Play, 0});
        CHECK(card_cost(*find_card(ceres), s.player().hand_modifiers[0]) == find_card(ceres)->cost - 2);
        CHECK(card_cost(*find_card(outsider), s.player().hand_modifiers[1]) == find_card(outsider)->cost);
    });

    suite.add("phase3_decks.10854120.nineteen_shadows_does_not_apply_necromancy", [] {
        Scenario s;
        const auto ceres = official(10854120);
        s.player().shadows = 19;
        s.hand({ceres, ceres});
        s.run({Play, 0});
        CHECK(card_cost(*find_card(ceres), s.player().hand_modifiers[0]) == find_card(ceres)->cost);
    });

    suite.add("phase3_decks.10854120.turn_end_heals_own_leader", [] {
        Scenario s;
        s.player().leader_hp = 10;
        s.place(official(10854120));
        s.run({EndTurn});
        CHECK(s.player().leader_hp == 14);
    });

    suite.add("phase3_decks.10824110.rally_twenty_reached_by_play_deals_four", [] {
        Scenario s;
        const auto card = official(10824110);
        s.player().followers_entered = 18;
        s.player(1).leader_hp = 20;
        s.hand({card});
        s.run({Play, 0});
        CHECK(s.player().followers_entered == 20 && s.player().board_count == 2);
        CHECK(s.player(1).leader_hp == 16);
    });

    suite.add("phase3_decks.10824110.rally_below_twenty_skips_leader_damage", [] {
        Scenario s;
        s.player().followers_entered = 17;
        s.player(1).leader_hp = 20;
        s.hand({official(10824110)});
        s.run({Play, 0});
        CHECK(s.player().followers_entered == 19 && s.player().board_count == 2);
        CHECK(s.player(1).leader_hp == 20);
    });

    suite.add("phase3_decks.10824110.evolve_adds_desperado_shot", [] {
        Scenario s;
        const auto spell = official(90024330);
        s.player().ep = 1;
        s.hand({official(10824110)});
        s.run({Play, 0});
        s.run({Evolve, 0});
        CHECK(std::find(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count, spell) !=
              s.player().hand.begin() + s.player().hand_count);
    });

    suite.add("phase3_decks.11061110.last_words_adds_two_evidence", [] {
        Scenario s;
        const auto card = official(11061110);
        const auto evidence = official(90001210);
        auto& craftsman = s.place(card);
        craftsman.hp = 1;
        const auto order = craftsman.creation_order;
        constexpr Effect destroy_self{.operation=Operation::Destroy, .target=Target::Self};
        resolve_effect_steps(s.state, 0, std::array{destroy_self}, order);
        CHECK(s.player().board_count == 0);
        CHECK(std::count(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count, evidence) == 2);
    });

    suite.add("phase3_decks.11054110.fanfare_summons_bane_ghost", [] {
        Scenario s;
        const auto ghost = official(90051130);
        s.hand({official(11054110)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == ghost);
        CHECK(s.player().board[1].abilities & kBane);
    });

    suite.add("phase3_decks.11054110.own_turn_banish_summons_modified_ward_skeleton", [] {
        Scenario s;
        s.place(official(11054110));
        const auto proof = s.place(official(90001210)).creation_order;
        constexpr Effect banish_self{.operation=Operation::Banish, .target=Target::Self};
        resolve_effect_steps(s.state, 0, std::array{banish_self}, proof);
        const auto skeleton = official(90051110);
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == skeleton);
        CHECK(s.player().board[1].atk == find_card(skeleton)->atk + 2);
        CHECK(s.player().board[1].max_hp == find_card(skeleton)->hp + 4);
        CHECK(s.player().board[1].abilities & FLAG_WARD);
    });

    suite.add("phase3_decks.11054110.enemy_turn_banish_does_not_summon_skeleton", [] {
        Scenario s;
        s.place(official(11054110));
        const auto proof = s.place(official(90001210)).creation_order;
        s.state.turn_player = 1;
        constexpr Effect banish_self{.operation=Operation::Banish, .target=Target::Self};
        resolve_effect_steps(s.state, 0, std::array{banish_self}, proof);
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(11054110));
    });

    suite.add("phase3_decks.10943310.no_prior_face_attack_uses_three_damage", [] {
        Scenario s;
        s.player().attacked_leader_last_turn = false;
        for (int i = 0; i < 3; ++i) {
            auto& enemy = s.place(kTrainer, 1);
            enemy.hp = enemy.max_hp = 10;
        }
        s.hand({official(10943310)});
        s.run({Play, 0});
        int total_damage = 0;
        for (int i = 0; i < s.player(1).board_count; ++i)
            total_damage += s.player(1).board[i].max_hp - s.player(1).board[i].hp;
        CHECK(total_damage == 3);
    });

    suite.add("phase3_decks.11071110.last_words_adds_two_evidence", [] {
        Scenario s;
        const auto evidence = official(90001210);
        const auto order = s.place(official(11071110)).creation_order;
        constexpr Effect destroy_self{.operation=Operation::Destroy, .target=Target::Self};
        resolve_effect_steps(s.state, 0, std::array{destroy_self}, order);
        CHECK(s.player().board_count == 0);
        CHECK(std::count(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count, evidence) == 2);
    });

    suite.add("phase3_decks.11034110.fanfare_adds_evidence", [] {
        Scenario s;
        const auto evidence = official(90001210);
        s.hand({official(11034110)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == evidence);
    });

    suite.add("phase3_decks.10513310.combo_sets_draw_count", [] {
        Scenario s;
        s.player().plays_this_turn = 2;
        s.hand({official(10513310)});
        s.run({Play, 0});
        CHECK(s.player().hand_count == 3);
    });
}

} // namespace svwb::wb::spec

