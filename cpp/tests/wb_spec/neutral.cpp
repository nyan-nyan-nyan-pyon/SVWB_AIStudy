#include "spec.hpp"

namespace svwb::wb::spec {

void neutral(Suite& suite) {
    suite.add("neutral.10501110.cost_one_board_fanfare_buff", [] {
        const auto oddity = official(10501110);
        Scenario qualifies;
        qualifies.place(official(10503210)); // Original cost 1, different card.
        qualifies.hand({oddity});
        qualifies.run({Play, 0});
        CHECK(qualifies.player().board[1].card_id == oddity);
        CHECK(qualifies.player().board[1].atk == find_card(oddity)->atk + 1);
        CHECK(qualifies.player().board[1].hp == find_card(oddity)->hp + 1);

        Scenario no_match;
        no_match.place(kColette);
        no_match.hand({oddity});
        no_match.run({Play, 0});
        CHECK(no_match.player().board[1].atk == find_card(oddity)->atk);
        CHECK(no_match.player().board[1].hp == find_card(oddity)->hp);
    });

    suite.add("neutral.10001120.last_words_and_evolve_draw", [] {
        const auto bell = official(10001120);
        Scenario destroyed;
        auto& p = destroyed.player();
        const auto drawn = p.deck[p.deck_idx];
        const auto entity = destroyed.place(bell).creation_order;
        resolve_effect_steps(destroyed.state, 0,
            std::array{Effect{.operation=Operation::Destroy, .target=Target::Self}}, entity);
        CHECK(p.board_count == 0 && p.hand_count == 1 && p.hand[0] == drawn);

        Scenario evolved;
        auto& q = evolved.player();
        const auto evolve_draw = q.deck[q.deck_idx];
        evolved.place(bell);
        evolved.run({Evolve, 0});
        CHECK(q.hand_count == 1 && q.hand[0] == evolve_draw);
        CHECK(q.board[0].evolution == Evolution::Normal);
    });

    suite.add("neutral.10001210.act_destroys_self_and_removes_ward", [] {
        Scenario s;
        const auto loupe = official(10001210);
        s.place(loupe);
        s.place(kTrainer, 1).abilities |= FLAG_WARD;
        s.run({Act, 0, 0});
        CHECK(s.player().board_count == 0 && s.player().shadows == 1);
        CHECK((s.player(1).board[0].abilities & FLAG_WARD) == 0);
    });

    suite.add("neutral.10701110.evolve_draws_neutral_not_next_card", [] {
        Scenario s;
        auto& p = s.player();
        const auto kids = official(10701110);
        const auto neutral_draw = official(10001130);
        const auto nonneutral = kColette;
        const auto start = p.deck_idx;
        p.deck[start] = nonneutral;
        p.deck[start + 1] = neutral_draw;
        p.deck_end = static_cast<std::uint8_t>(start + 2);
        s.place(kids);
        s.run({Evolve, 0});
        CHECK(p.hand_count == 1 && p.hand[0] == neutral_draw);
        CHECK(p.deck_remaining() == 1);
        CHECK(std::count(p.deck.begin() + p.deck_idx, p.deck.begin() + p.deck_end, nonneutral) == 1);
        CHECK(std::count(p.deck.begin() + p.deck_idx, p.deck.begin() + p.deck_end, neutral_draw) == 0);
    });

    suite.add("neutral.10803110.fanfare_recovers_destroyed_follower_name", [] {
        Scenario s;
        auto& p = s.player();
        const auto recovered = official(10001130);
        p.other_counters[0] = {recovered, 1, {}, 0};
        p.other_counter_count = 1;
        s.hand({official(10803110)});
        s.run({Play, 0});
        CHECK(p.hand_count == 1 && p.hand[0] == recovered);
        CHECK(p.other_counters[0].destroyed == 1);
    });

    suite.add("neutral.10803310.mode_three_buffs_all_allies", [] {
        Scenario s;
        s.hand({official(10803310)});
        s.place(kTrainer);
        s.place(kColette);
        s.place(kHolyFalcon, 1);
        s.run({Play, 0, kNoTarget, 3});
        CHECK(s.player().board_count == 2);
        CHECK(s.player().board[0].atk == find_card(kTrainer)->atk + 1);
        CHECK(s.player().board[1].atk == find_card(kColette)->atk + 1);
        CHECK(s.player(1).board[0].atk == find_card(kHolyFalcon)->atk);
    });

    suite.add("neutral.10002110.normal_and_super_evolve_heal_amounts", [] {
        const auto henriette = official(10002110);
        Scenario normal;
        normal.player().leader_hp = 14;
        normal.place(henriette);
        normal.run({Evolve, 0});
        CHECK(normal.player().leader_hp == 16);

        Scenario super;
        super.player().leader_hp = 14;
        super.place(henriette);
        super.run({SuperEvolve, 0});
        CHECK(super.player().leader_hp == 18);
    });

    suite.add("neutral.10002210.guild_follower_search_then_act_grants_rush", [] {
        Scenario s;
        auto& p = s.player();
        const auto guild = official(10002210);
        const auto follower = official(10001130);
        const auto nonfollower = official(10001210);
        const auto start = p.deck_idx;
        p.deck[start] = nonfollower;
        p.deck[start + 1] = follower;
        p.deck_end = static_cast<std::uint8_t>(start + 2);
        s.hand({guild});
        s.place(kTrainer);
        s.run({Play, 0});
        CHECK(p.hand_count == 1 && p.hand[0] == follower);
        CHECK(p.deck_remaining() == 1);
        CHECK(std::count(p.deck.begin() + p.deck_idx, p.deck.begin() + p.deck_end, nonfollower) == 1);
        CHECK(std::count(p.deck.begin() + p.deck_idx, p.deck.begin() + p.deck_end, follower) == 0);
        s.run({Act, 1, kOwnBoardTarget});
        CHECK(p.board_count == 1 && p.shadows == 1);
        CHECK(p.board[0].abilities & FLAG_RUSH);
    });

    suite.add("neutral.10703110.discard_then_sweep_enemy_followers", [] {
        Scenario s;
        const auto discarded = kRapid;
        s.hand({official(10703110), discarded});
        s.place(kTrainer, 1).hp = 2;
        s.place(kColette, 1).hp = 2;
        s.run({Play, 0, kOwnHandTarget + 1});
        CHECK(s.player().hand_count == 0 && s.player().shadows == 1);
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 2);
    });

    suite.add("neutral.10702110.super_evolve_summons_two_copies", [] {
        Scenario s;
        const auto reporter = official(10702110);
        s.place(reporter);
        s.run({SuperEvolve, 0});
        CHECK(s.player().board_count == 3);
        CHECK(s.player().board[0].card_id == reporter && s.player().board[0].evolution == Evolution::Super);
        CHECK(s.player().board[1].card_id == reporter && s.player().board[2].card_id == reporter);
        CHECK(s.player().board[1].creation_order < s.player().board[2].creation_order);
    });

    suite.add("neutral.11002110.fanfare_and_evolve_repeat_damage_and_token", [] {
        const auto angel = official(11002110);
        Scenario s;
        s.hand({angel});
        s.place(kTrainer, 1).hp = 5;
        s.run({Play, 0, 0});
        CHECK(s.player(1).board[0].hp == 3 && s.player().hand_count == 1);
        const auto token = s.player().hand[0];
        s.run({Evolve, 0, 0});
        CHECK(s.player(1).board[0].hp == 1 && s.player().hand_count == 2 && s.player().hand[1] == token);
    });

    suite.add("neutral.10802110.fanfare_damage_and_evolve_storm", [] {
        Scenario s;
        const auto alphid = official(10802110);
        s.hand({alphid});
        s.place(kTrainer, 1).hp = 5;
        s.run({Play, 0, 0});
        CHECK(s.player(1).board[0].hp == 1);
        s.run({Evolve, 0});
        CHECK(s.player().board[0].abilities & FLAG_STORM);
        CHECK(s.legal({Attack, 0, kEnemyLeader}));
    });

    suite.add("neutral.10001110.enhance_four_applies_stat_bonus", [] {
        Scenario s;
        const auto fighter = official(10001110);
        s.hand({fighter});
        s.player().pp = 4;
        s.run({Play, 0});
        CHECK(s.player().pp == 0);
        CHECK(s.player().board[0].atk == find_card(fighter)->atk + 3);
        CHECK(s.player().board[0].hp == find_card(fighter)->hp + 3);
    });

    suite.add("neutral.10601110.last_words_damage_enemy_leader", [] {
        Scenario s;
        const auto speaker = official(10601110);
        const auto entity = s.place(speaker).creation_order;
        s.player(1).leader_hp = 12;
        resolve_effect_steps(s.state, 0,
            std::array{Effect{.operation=Operation::Destroy, .target=Target::Self}}, entity);
        CHECK(s.player().board_count == 0 && s.player().shadows == 1);
        CHECK(s.player(1).leader_hp == 11);
    });

    suite.add("neutral.10704120.turn_end_draw_only_on_owners_turn", [] {
        Scenario s;
        auto& p = s.player();
        s.place(official(10704120));
        const auto expected = p.deck[p.deck_idx];
        s.run({EndTurn});
        CHECK(s.state.turn_player == 1 && p.hand_count == 1 && p.hand[0] == expected);
        const auto owner_deck_idx = p.deck_idx;
        const auto normal_draw = p.deck[p.deck_idx];
        s.run({EndTurn});
        CHECK(s.state.turn_player == 0 && p.deck_idx == owner_deck_idx + 1);
        CHECK(p.hand_count == 2 && p.hand[1] == normal_draw);
    });

    suite.add("neutral.10502120.evolve_destroys_board_only_when_top_hand_cost_sum_is_higher", [] {
        const auto general = official(10502120);
        Scenario wins_comparison;
        wins_comparison.hand({official(10002120), official(10002120), official(10002120)});
        wins_comparison.hand({official(10802110), official(10802110), official(10802110)}, 1);
        wins_comparison.place(general);
        wins_comparison.place(kTrainer, 1);
        wins_comparison.place(kColette, 1);
        wins_comparison.run({Evolve, 0});
        CHECK(wins_comparison.player(1).board_count == 0);
        CHECK(wins_comparison.player(1).shadows == 2);

        Scenario loses_comparison;
        loses_comparison.hand({});
        loses_comparison.hand({official(10002120)}, 1);
        loses_comparison.place(general);
        loses_comparison.place(kTrainer, 1);
        loses_comparison.run({Evolve, 0});
        CHECK(loses_comparison.player(1).board_count == 1);
        CHECK(loses_comparison.player(1).shadows == 0);
    });

    suite.add("neutral.10701310.banish_targets_enemy_permanent_without_shadow", [] {
        Scenario s;
        s.hand({official(10701310)});
        s.place(official(10001210), 1);
        s.place(kTrainer, 1);
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 1);
        CHECK(s.player(1).board[0].card_id == kTrainer);
        CHECK(s.player(1).shadows == 0);
    });

    suite.add("neutral.10801120.fanfare_ep_restore_from_zero_and_one", [] {
        const auto reina = official(10801120);
        Scenario from_zero;
        from_zero.player().ep = 0;
        from_zero.hand({reina});
        from_zero.run({Play, 0});
        CHECK(from_zero.player().ep == 1);

        Scenario recover;
        recover.player().ep = 1;
        recover.hand({reina});
        recover.run({Play, 0});
        CHECK(recover.player().ep == 2);
    });

    suite.add("neutral.10901310.reincarnation_returns_highest_cost_destroyed_follower_then_draws", [] {
        Scenario s;
        auto& p = s.player();
        const auto low = official(10802110);
        const auto high = official(10002120);
        p.other_counters[0] = {low, 1, {}, 0};
        p.other_counters[1] = {high, 1, {}, 0};
        p.other_counter_count = 2;
        p.deck = {};
        p.deck_idx = 0;
        p.deck_end = 0;
        s.hand({official(10901310)});
        s.run({Play, 0});
        CHECK(p.hand_count == 1 && p.hand[0] == high);
        CHECK(p.deck_remaining() == 0);
    });

    suite.add("neutral.10903110.watchman_damage_scales_with_neutral_hand_count", [] {
        Scenario s;
        auto& p = s.player();
        const auto watchman = official(10903110);
        s.hand({watchman, official(10001130), kColette});
        s.place(kTrainer, 1).hp = 7;
        s.place(kColette, 1).hp = 7;
        s.run({Play, 0});
        CHECK(p.hand_count == 3);
        CHECK(s.player(1).board_count == 2);
        CHECK(s.player(1).board[0].hp == 5 && s.player(1).board[1].hp == 5);
        const auto pp_before_evolve = p.pp;
        s.run({Evolve, 0});
        CHECK(p.pp == pp_before_evolve + 1);
    });

    suite.add("neutral.10603110.random_keywords_grants_exactly_three_distinct_allowed_abilities", [] {
        Scenario s;
        const auto beast = official(10603110);
        s.hand({beast});
        s.run({Play, 0});
        constexpr std::uint16_t allowed = FLAG_STORM | kBane | kIntimidate | kDrain | kAura | kBarrier;
        const auto gained = static_cast<std::uint16_t>(s.player().board[0].abilities & allowed);
        CHECK((s.player().board[0].abilities & static_cast<std::uint16_t>(~allowed)) == 0);
        const int count = static_cast<int>((gained & FLAG_STORM) != 0) + static_cast<int>((gained & kBane) != 0) +
            static_cast<int>((gained & kIntimidate) != 0) + static_cast<int>((gained & kDrain) != 0) +
            static_cast<int>((gained & kAura) != 0) + static_cast<int>((gained & kBarrier) != 0);
        CHECK(count == 3);
    });

    suite.add("neutral.11002210.countdown_banishes_enemy_follower_without_shadow_and_adds_evidence", [] {
        Scenario s;
        const auto crime_scene = official(11002210);
        s.place(crime_scene);
        s.place(kTrainer, 1);
        s.next_own_turn();
        CHECK(s.player(1).board_count == 1);
        CHECK(s.player(1).board[0].card_id == official(90001210));
        CHECK(s.player(1).shadows == 0);
        CHECK(s.player().board[0].countdown == 2);
    });

    suite.add("neutral.10503310.base_spell_draws_two_and_destroys_strongest_only", [] {
        Scenario s;
        auto& p = s.player();
        const auto world = official(10503310);
        p.pp = 5;
        p.deck[p.deck_idx] = official(10001130);
        p.deck[p.deck_idx + 1] = official(10001120);
        s.hand({world});
        s.place(kTrainer, 1).atk = 7;
        s.place(kColette, 1).atk = 2;
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].card_id == kColette);
        CHECK(s.player(1).shadows == 1);
        check(p.hand_count == 2, "expected two drawn cards, observed " + std::to_string(p.hand_count));
        CHECK(p.hand[0] == official(10001130) && p.hand[1] == official(10001120));
    });

    suite.add("neutral.10503310.enhance_deck_out_precedes_lethal_damage", [] {
        Scenario s;
        auto& p = s.player();
        p.deck_end = static_cast<std::uint8_t>(p.deck_idx + 1);
        p.pp = 10;
        s.player(1).leader_hp = 4;
        s.hand({official(10503310)});
        s.run({Play, 0});
        check(s.state.outcome == Outcome::SecondWins && s.state.end_reason == EndReason::EmptyDeck,
              "expected deck-out before Enhance damage, observed outcome=" +
                  std::to_string(static_cast<int>(s.state.outcome)) +
                  " end_reason=" + std::to_string(static_cast<int>(s.state.end_reason)));
        CHECK(s.player(1).leader_hp == 4);
    });

    suite.add("neutral.10504110.getenou_mode_two_discards_hand_and_discount_drawn_cards", [] {
        Scenario s;
        auto& p = s.player();
        const auto getenou = official(10504110);
        const auto first = official(10001130);
        const auto second = official(10001120);
        p.deck[p.deck_idx] = first;
        p.deck[p.deck_idx + 1] = second;
        s.hand({getenou, kColette});
        s.player().pp = 8;
        s.run({Play, 0, kNoTarget, 2});
        CHECK(p.hand_count == 2 && p.hand[0] == first && p.hand[1] == second);
        CHECK(p.hand_modifiers[0].cost == -8 && p.hand_modifiers[1].cost == -8);
        CHECK(p.shadows == 1);
    });

    suite.add("neutral.10601120.chosen_fanfare_target_destroyed_and_unselected_survives", [] {
        Scenario s;
        const auto jailer = official(10601120);
        s.hand({jailer});
        s.place(kTrainer, 1);
        s.place(kColette, 1);
        s.run({Play, 0, 1});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].card_id == kTrainer);
        CHECK(s.player(1).shadows == 1);
        CHECK(s.player().board[0].abilities & kBane);
    });

    suite.add("neutral.10603210.turn_end_splash_excludes_assailants_on_both_sides", [] {
        Scenario s;
        const auto omega = official(10604110); // Has the Assailant trait.
        s.place(official(10603210));
        s.place(kColette).hp = 5;
        s.place(omega);
        s.place(kHolyFalcon, 1).hp = 5;
        s.place(omega, 1);
        s.run({EndTurn});
        CHECK(s.player().board[1].hp == 3 && s.player().board[2].hp == find_card(omega)->hp);
        CHECK(s.player(1).board[0].hp == 3 && s.player(1).board[1].hp == find_card(omega)->hp);
    });

    suite.add("neutral.10903210.end_turn_condition_requires_every_cost_one_through_eight", [] {
        Scenario s;
        const auto prison = official(10903210);
        s.place(prison);
        s.player().played_cost_mask = 0x0fe; // Costs 1 through 7, missing 8.
        s.run({EndTurn});
        CHECK(std::any_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
            [prison](const auto& object) { return object.card_id == prison; }));
    });

    suite.add("neutral.10801110.evolve_bonus_applies_five_attack_and_five_health", [] {
        Scenario s;
        const auto hansa = official(10801110);
        s.place(hansa);
        s.run({Evolve, 0});
        CHECK(s.player().board[0].atk == find_card(hansa)->atk + 2 + 5);
        CHECK(s.player().board[0].hp == find_card(hansa)->hp + 2 + 5);
    });

    suite.add("neutral.10901110.fanfare_hits_chosen_then_other_follower", [] {
        Scenario s;
        const auto jailer = official(10901110);
        s.hand({jailer});
        s.place(kTrainer, 1).hp = 8;
        s.place(kColette, 1).hp = 8;
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 2);
        CHECK(s.player(1).board[0].hp == 2 && s.player(1).board[1].hp == 6);
        CHECK(s.player().board[0].abilities & FLAG_WARD);
    });

    suite.add("neutral.11003110.fanfare_damages_all_enemy_followers_and_adds_two_evidence", [] {
        Scenario s;
        const auto curios = official(11003110);
        const auto evidence = official(90001210);
        s.hand({curios});
        s.place(kTrainer, 1).hp = 2;
        s.place(kColette, 1).hp = 2;
        s.place(kHolyFalcon, 1).hp = 5;
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].card_id == kHolyFalcon);
        CHECK(s.player(1).board[0].hp == 3 && s.player(1).shadows == 2);
        CHECK(s.player().hand_count == 2);
        CHECK(s.player().hand[0] == evidence && s.player().hand[1] == evidence);
        CHECK(s.player().board[0].abilities & FLAG_WARD);
    });

    suite.add("neutral.10604110.super_evolve_resolves_two_random_fanfare_branches", [] {
        Scenario s;
        const auto omega = official(10604110);
        s.state.rng_state = 0x4f4d454741ULL;
        s.rules.fanfare_repeat_limit = 0;
        s.player().pp = 5;
        s.place(omega);
        s.place(kColette, 1);
        s.run({SuperEvolve, 0});
        CHECK(s.player().board[0].evolution == Evolution::Super);
        CHECK(s.player().board[0].atk >= find_card(omega)->atk + 3);
        CHECK(s.player().pp == 5 || s.player().pp == 7);
        CHECK(s.player(1).leader_hp == 18 || s.player(1).leader_hp == 20);
        CHECK(s.player(1).board_count == 0 || s.player(1).board_count == 1);
        CHECK(s.player().board[0].atk > find_card(omega)->atk + 3 || s.player().pp == 7 ||
              s.player(1).leader_hp == 18 || s.player(1).board_count == 0);
    });

    suite.add("neutral.10704110.face_attack_does_not_mark_clash_opponent", [] {
        Scenario s;
        const auto honey = official(10704110);
        s.place(honey);
        const auto enemy_hp = s.player(1).leader_hp;
        s.run({Attack, 0, kEnemyLeader});
        CHECK(s.player(1).leader_hp == enemy_hp - find_card(honey)->atk);
        CHECK((s.player().board[0].abilities & kBarrier) == 0);
        CHECK(s.player(1).board_count == 0);
    });

    suite.add("neutral.11003210.act_draws_and_spends_one_evidence", [] {
        Scenario s;
        auto& p = s.player();
        const auto evidence = official(90001210);
        const auto ideal_world = official(11003210);
        s.hand({evidence, ideal_world});
        s.run({Play, 0});
        s.run({Play, 0});
        CHECK(p.board_count == 1 && p.board[0].clue_count == 2);
        const auto drawn = p.deck[p.deck_idx];
        s.run({Act, 0});
        CHECK(p.hand_count == 1 && p.hand[0] == drawn);
        CHECK(p.board_count == 1 && p.board[0].card_id == ideal_world && p.board[0].clue_count == 1);
    });

    suite.add("neutral.evidence.full_board_rejects_both_hand_plays", [] {
        Scenario s;
        s.place(kEvidence).clue_count = 2;
        for (int i = 1; i < kBoardLimit; ++i) s.place(kTrainer);
        s.hand({official(90001210), official(11003210)});
        s.reject({Play, 0});
        s.reject({Play, 1});
    });

    suite.add("neutral.evidence.full_board_summon_keeps_existing_evidence_without_banish_trigger", [] {
        Scenario s;
        const auto nocturne = official(11054110);
        const auto ideal_world = official(11003210);
        s.place(nocturne);
        auto& evidence = s.place(ideal_world);
        evidence.clue_count = 2;
        const auto evidence_order = evidence.creation_order;
        s.place(kTrainer);
        s.place(kTrainer);
        const auto last = s.place(kTrainer).creation_order;

        // Free a slot before resolving queued triggers so a stray Nocturne summon is observable.
        const std::array steps{
            Effect{.operation = Operation::Summon, .amount = 1, .card = kEvidence},
            Effect{.operation = Operation::Destroy, .target = Target::ChosenOwnFollower},
        };
        resolve_effect_steps(s.state, 0, steps, 0, last, s.rules);
        CHECK(s.player().board_count == kBoardLimit - 1 && s.player().shadows == 1);
        CHECK(s.player().board[0].card_id == nocturne);
        CHECK(s.player().board[1].card_id == ideal_world && s.player().board[1].clue_count == 2 &&
              s.player().board[1].creation_order == evidence_order);
    });
}

} // namespace svwb::wb::spec
