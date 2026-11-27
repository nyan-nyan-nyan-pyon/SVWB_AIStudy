#include "spec.hpp"

namespace svwb::wb::spec {

void phase2_royal(Suite& suite) {
    suite.add("phase2_royal.10021130.storm_attack_window", [] {
        Scenario s;
        s.hand({official(10021130)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 1 && (s.player().board[0].abilities & FLAG_STORM));
        s.run({Attack, 0, kEnemyLeader});
        CHECK(s.player(1).leader_hp < 20);
    });

    suite.add("phase2_royal.10021310.return_selected_hand_and_draw_royal_followers", [] {
        Scenario s;
        for (int i = s.player().deck_idx; i < s.player().deck_end; ++i) s.player().deck[i] = kTrainer;
        s.hand({official(10021310), kTrainer, official(90021110)});
        s.player().deck[s.player().deck_idx] = official(10921120);
        s.player().deck[s.player().deck_idx + 1] = official(10021130);
        s.run({Play, 0, static_cast<std::int8_t>(kOwnHandTarget + 1)});
        CHECK(s.player().hand_count == 3);
        CHECK(s.player().hand[0] == official(90021110));
        for (int i = 1; i < 3; ++i)
            CHECK(s.player().hand[i] == official(10921120) || s.player().hand[i] == official(10021130));
    });

    suite.add("phase2_royal.10022120.super_evolve_draws_and_grants_storm_to_all_named_copies", [] {
        Scenario s;
        for (int i = s.player().deck_idx; i < s.player().deck_end; ++i) s.player().deck[i] = kTrainer;
        s.hand({official(10022120)});
        s.player().deck[s.player().deck_idx] = official(10022120);
        s.player().deck[s.player().deck_idx + 1] = official(10022120);
        s.player().deck[s.player().deck_idx + 2] = official(10021130);
        s.run({Play, 0});
        s.run({SuperEvolve, 0});
        CHECK(s.player().hand_count == 2);
        CHECK(s.player().hand[0] == official(10022120) && s.player().hand[1] == official(10022120));
        CHECK((s.player().hand_modifiers[0].abilities & FLAG_STORM) &&
              (s.player().hand_modifiers[1].abilities & FLAG_STORM));
    });

    suite.add("phase2_royal.10521110.discard_selected_card_heals_three_or_six_for_spell", [] {
        Scenario follower;
        follower.player().leader_hp = 10;
        follower.hand({official(10521110), official(10021130)});
        follower.run({Play, 0, static_cast<std::int8_t>(kOwnHandTarget + 1)});
        CHECK(follower.player().leader_hp == 13 && follower.player().hand_count == 0);

        Scenario spell;
        spell.player().leader_hp = 10;
        spell.hand({official(10521110), official(10021310)});
        spell.run({Play, 0, static_cast<std::int8_t>(kOwnHandTarget + 1)});
        CHECK(spell.player().leader_hp == 16 && spell.player().hand_count == 0);
    });

    suite.add("phase2_royal.10521310.discard_spell_then_two_random_three_damage_hits", [] {
        Scenario s;
        s.hand({official(10521310), official(10021310)});
        auto& only_target = s.place(official(10621110), 1);
        only_target.hp = only_target.max_hp = 6;
        s.run({Play, 0, static_cast<std::int8_t>(kOwnHandTarget + 1)});
        CHECK(s.player().hand_count == 0);
        CHECK(s.player(1).board_count == 0);
    });

    suite.add("phase2_royal.10522110.fanfare_draw_and_heal", [] {
        Scenario s;
        s.player().leader_hp = 10;
        s.player().deck[s.player().deck_idx] = official(10021130);
        s.hand({official(10522110)});
        s.run({Play, 0});
        CHECK(s.player().leader_hp == 13 && s.player().hand_count == 1);
        CHECK(s.player().hand[0] == official(10021130));
        CHECK(s.player().board[0].abilities & FLAG_RUSH);

        Scenario drawn;
        drawn.player().deck[drawn.player().deck_idx] = official(10522110);
        constexpr Effect draw_one{.operation=Operation::Draw, .amount=1};
        resolve_effect_steps(drawn.state, 0, std::array{draw_one});
        CHECK(drawn.player().hand_count == 1);
        CHECK(card_cost(*find_card(drawn.player().hand[0]), drawn.player().hand_modifiers[0]) == 3);
    });

    suite.add("phase2_royal.10523310.enhance_adds_four_gold_coins", [] {
        Scenario basic;
        basic.player().pp = 4;
        basic.hand({official(10523310)});
        basic.run({Play, 0});
        CHECK(basic.player().hand_count == 2);
        CHECK(basic.player().hand[0] == official(90021350) && basic.player().hand[1] == official(90021350));

        Scenario enhanced;
        enhanced.player().pp = 5;
        enhanced.hand({official(10523310)});
        enhanced.run({Play, 0});
        CHECK(enhanced.player().hand_count == 4);
        for (int i = 0; i < 4; ++i) CHECK(enhanced.player().hand[i] == official(90021350));
    });

    suite.add("phase2_royal.10524110.end_turn_unevolved_sweeps_enemy_followers", [] {
        Scenario s;
        s.hand({official(10524110)});
        auto& a = s.place(official(10621110), 1);
        a.hp = a.max_hp = 7;
        auto& b = s.place(official(10621110), 1);
        b.hp = b.max_hp = 6;
        s.run({Play, 0});
        s.run({EndTurn});
        CHECK(s.player(1).board_count == 0);
    });

    suite.add("phase2_royal.10621120.returns_chosen_hand_then_draws_two_royal_followers", [] {
        Scenario s;
        for (int i = s.player().deck_idx; i < s.player().deck_end; ++i) s.player().deck[i] = kTrainer;
        s.hand({official(10621120), kTrainer, official(90021110)});
        s.player().deck[s.player().deck_idx] = official(10921120);
        s.player().deck[s.player().deck_idx + 1] = official(10021130);
        s.run({Play, 0, static_cast<std::int8_t>(kOwnHandTarget + 1)});
        CHECK(s.player().hand_count == 3);
        CHECK(s.player().hand[0] == official(90021110));
        for (int i = 1; i < 3; ++i)
            CHECK(s.player().hand[i] == official(10921120) || s.player().hand[i] == official(10021130));
    });

    suite.add("phase2_royal.10622310.grants_march_crest_and_enhance_extends_countdown", [] {
        Scenario base;
        base.player().pp = 2;
        base.hand({official(10622310)});
        base.run({Play, 0});
        CHECK(base.player().crest_count == 1 && base.player().crests[0].id == CrestId::March);
        CHECK(base.player().crests[0].countdown == find_crest(CrestId::March)->countdown);

        Scenario enhanced;
        enhanced.player().pp = 3;
        enhanced.crest(CrestId::March, 3);
        enhanced.hand({official(10622310)});
        enhanced.run({Play, 0});
        CHECK(enhanced.player().crest_count == 1 && enhanced.player().crests[0].countdown == 5);
    });

    suite.add("phase2_royal.10623110.destroy_selected_enemy_and_enhance_refunds_pp_and_adds_follower", [] {
        Scenario s;
        s.player().pp = 6;
        s.hand({official(10623110)});
        s.place(official(10621110), 1);
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0);
        CHECK(s.player().pp == 3 && s.player().hand_count == 1);
        CHECK(s.player().hand[0] == official(10621110));
    });

    suite.add("phase2_royal.10624120.fanfare_adds_shin_depth_and_evolve_upgrades_faith", [] {
        Scenario s;
        s.hand({official(10624120)});
        s.player().faiths[0] = official(10624122);
        s.player().faith_count = 1;
        s.player().faith_values[0] = 5;
        s.run({Play, 0});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90024320));
        const auto faith_before = s.player().faith_values[0];
        s.run({Evolve, 0});
        CHECK(s.player().faith_values[0] == faith_before - 5);
        CHECK(s.player().faith_upgrades[0] == 1);
    });

    suite.add("phase2_royal.10721110.fanfare_deals_three_and_hand_cost_reduces_after_follower_enters", [] {
        Scenario hit;
        hit.hand({official(10721110)});
        auto& enemy = hit.place(official(10621110), 1);
        enemy.hp = enemy.max_hp = 5;
        hit.run({Play, 0, 0});
        CHECK(hit.player(1).board[0].hp == 2);

        Scenario discount;
        discount.hand({official(10721110)});
        discount.place(official(10621110));
        discount.run({SuperEvolve, 0});
        CHECK(card_cost(*find_card(discount.player().hand[0]), discount.player().hand_modifiers[0]) == 1);
    });

    suite.add("phase2_royal.10823110.super_turn_auto_evolves_and_follower_attack_repeats_damage", [] {
        Scenario s;
        s.hand({official(10823110)});
        auto& target = s.place(official(10621110), 1);
        target.hp = target.max_hp = 10;
        s.run({Play, 0});
        CHECK(s.player().board[0].evolution == Evolution::Normal);
        target.hp = target.max_hp = s.player().board[0].atk + 10;
        s.run({Attack, 0, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 1);
    });

    suite.add("phase2_royal.10921120.rush_ward_follower_can_attack_after_play", [] {
        Scenario s;
        s.hand({official(10921120)});
        s.place(official(10621110), 1);
        s.run({Play, 0});
        CHECK((s.player().board[0].abilities & FLAG_RUSH) && (s.player().board[0].abilities & FLAG_WARD));
        s.run({Attack, 0, 0});
        CHECK(s.player().board[0].attacked_this_turn);
    });

    suite.add("phase2_royal.10924120.aura_bane_storm_ward_and_three_attacks_per_turn", [] {
        Scenario s;
        s.hand({official(10924120)});
        s.run({Play, 0});
        const auto flags = s.player().board[0].abilities;
        CHECK((flags & FLAG_STORM) && (flags & FLAG_WARD) && (flags & kBane) && (flags & kAura));
        s.run({Attack, 0, kEnemyLeader});
        s.run({Attack, 0, kEnemyLeader});
        s.run({Attack, 0, kEnemyLeader});
        CHECK(s.player().board[0].attacked_thrice);
        s.reject({Attack, 0, kEnemyLeader});
    });

    suite.add("phase2_royal.11021110.enhance_copies_self_and_last_words_summons_evidence", [] {
        Scenario s;
        s.player().pp = 4;
        s.hand({official(11021110)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == official(11021110));
        CHECK(s.player().board[0].atk == s.player().board[1].atk &&
              s.player().board[0].hp == s.player().board[1].hp);
        constexpr Effect destroy_own{.operation=Operation::Destroy, .target=Target::AllOwnFollowers};
        resolve_effect_steps(s.state, 0, std::array{destroy_own});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == kEvidence &&
              s.player().board[0].clue_count == 2);
    });

    suite.add("phase2_royal.11023110.fanfare_copies_newspaper_and_evolve_grants_bane", [] {
        Scenario s;
        s.hand({official(11023110)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 3);
        CHECK(s.player().board[1].card_id == official(11021110) && s.player().board[2].card_id == official(11021110));
        CHECK(s.player().board[1].atk == s.player().board[0].atk && s.player().board[1].hp == s.player().board[0].hp);
        CHECK(s.player().board[2].atk == s.player().board[0].atk && s.player().board[2].hp == s.player().board[0].hp);
        s.run({Evolve, 0});
        CHECK((s.player().board[1].abilities & kBane) && (s.player().board[2].abilities & kBane));
    });

    suite.add("phase2_royal.11024110.fanfare_copies_self_and_assigns_ward_storm_by_attack_health", [] {
        Scenario s;
        s.hand({official(11024110)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == official(11024110));
        const auto* printed = find_card(official(11024110));
        CHECK(printed);
        if (printed->atk > printed->hp) {
            CHECK((s.player().board[0].abilities & FLAG_STORM) && (s.player().board[1].abilities & FLAG_WARD));
        } else if (printed->atk < printed->hp) {
            CHECK((s.player().board[0].abilities & kDrain) && (s.player().board[1].abilities & kBane));
        } else {
            CHECK(!(s.player().board[0].abilities & (FLAG_STORM | kDrain)) &&
                  !(s.player().board[1].abilities & (FLAG_WARD | kBane)));
        }
    });
}

} // namespace svwb::wb::spec
