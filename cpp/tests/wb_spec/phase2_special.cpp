#include "spec.hpp"

#include <array>

namespace svwb::wb::spec {

void phase2_special(Suite& suite) {
    using enum CrestId;

    suite.add("phase2_special.crest.shared_slots_faith_occupies_last_slot", [] {
        Scenario s;
        s.player().faith_count = 4;
        s.player().faiths[0] = official(10614122);
        s.player().faiths[1] = official(10624122);
        s.player().faiths[2] = official(10634122);
        s.player().faiths[3] = official(10664122);
        s.crest(Testimony, 2);
        s.hand({official(10622310)});
        s.player().plays_this_turn = 2;
        s.run({Play, 0});
        CHECK(s.player().faith_count == 4 && s.player().crest_count == 1 && s.player().crests[0].id == Testimony);
    });

    suite.add("phase2_special.crest.expiry_does_not_remove_faith", [] {
        Scenario s;
        s.player().faiths[0] = official(10664122);
        s.player().faith_count = 1;
        s.crest(Rapid, 1);
        s.next_own_turn();
        CHECK(s.player().crest_count == 0 && s.player().faith_count == 1 &&
              s.player().faiths[0] == official(10664122));
    });

    suite.add("phase2_special.crest.identical_faiths_from_deck_are_unique", [] {
        auto deck = kMirrorDeck;
        deck[0] = official(10614120);
        deck[1] = official(10614120);
        const auto game = new_game(deck, deck, 819);
        for (const auto& player : game.players) {
            CHECK(player.faith_count == 1 && player.faiths[0] == official(10614122));
        }
    });

    suite.add("phase2_special.crest.full_area_does_not_replace_without_rule", [] {
        Scenario s;
        constexpr std::array ids{Testimony, Rapid, Rituel, Haylem, Erralde};
        for (const auto id : ids) s.crest(id, 4);
        s.hand({official(10622310)});
        s.player().plays_this_turn = 2;
        s.run({Play, 0});
        CHECK(s.player().crest_count == kCrestLimit && s.player().crests[0].id == Testimony &&
              s.player().crests[kCrestLimit - 1].id == Erralde);
    });

    suite.add("phase2_special.crest.full_area_replaces_oldest_only_when_configured", [] {
        Scenario s;
        s.rules.replace_oldest_crest_when_full = true;
        constexpr std::array ids{Testimony, Rapid, Rituel, Haylem, Erralde};
        for (int i = 0; i < kCrestLimit; ++i) s.crest(ids[i], i + 1);
        s.hand({official(10622310)});
        s.player().plays_this_turn = 2;
        s.run({Play, 0});
        CHECK(s.player().crest_count == kCrestLimit && s.player().crests[0].id == Rapid &&
              s.player().crests[kCrestLimit - 1].id == March &&
              s.player().crests[kCrestLimit - 1].countdown == 4);
    });

    suite.add("phase2_special.crest.new_crest_does_not_tick_on_creation_turn", [] {
        Scenario s;
        s.hand({official(10622310)});
        s.player().plays_this_turn = 2;
        s.run({Play, 0});
        CHECK(s.player().crest_count == 1 && s.player().crests[0].id == March &&
              s.player().crests[0].countdown == 4);
    });

    suite.add("phase2_special.crest.start_and_end_clocks_do_not_double_tick", [] {
        Scenario s;
        s.rules.crest_clock = CrestClock::Start;
        s.crest(Testimony, 3);
        s.next_own_turn();
        CHECK(s.player().crest_count == 1 && s.player().crests[0].countdown == 2);
    });

    suite.add("phase2_special.crest.owner_countdown_does_not_tick_on_opponent_turn", [] {
        Scenario s;
        s.crest(Testimony, 2);
        s.run({EndTurn});
        CHECK(s.player().crests[0].countdown == 2);
        s.run({EndTurn});
        CHECK(s.player().crests[0].countdown == 1);
    });

    suite.add("phase2_special.token.tiger_act_rejects_without_pp_and_preserves_state", [] {
        Scenario s;
        s.place(official(10961210));
        s.player().pp = 0;
        s.reject({Act, 0});
    });

    suite.add("phase2_special.token.tiger_act_consumes_source_and_fresh_copy_works_next_turn", [] {
        Scenario s;
        s.place(official(10961210));
        s.place(official(10961210));
        s.run({Act, 0});
        CHECK(s.player().board_count == 2 && s.player().board[0].card_id == official(10961210) &&
              s.player().board[1].card_id == official(90061120) && s.player().shadows == 1);
        s.next_own_turn();
        CHECK(s.player().board[0].countdown == 2);
        s.run({Act, 0});
        CHECK(s.player().board_count == 2 && s.player().board[0].card_id == official(90061120) &&
              s.player().board[1].card_id == official(90061120) && s.player().shadows == 2);
    });

    suite.add("phase2_special.token.tiger_act_obeys_board_capacity", [] {
        Scenario s;
        s.place(official(10961210));
        while (s.player().board_count < kBoardLimit) s.place(kTrainer);
        s.run({Act, 0});
        CHECK(s.player().board_count == kBoardLimit && s.player().pp == 7 && s.player().shadows == 1 &&
              std::any_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                          [](const Permanent& object) { return object.card_id == official(90061120); }));
    });

    suite.add("phase2_special.token.tiger_rush_token_cannot_attack_leader_with_ward", [] {
        Scenario s;
        s.place(official(10961210));
        s.run({Act, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(90061120));
        s.place(kTrainer, 1).abilities |= FLAG_WARD;
        s.reject({Attack, 0, kEnemyLeader});
        s.run({Attack, 0, 0});
        CHECK(s.player(1).board_count == 0);
    });

    suite.add("phase2_special.countdown.form_cost_one_below_is_atomic_rejection", [] {
        Scenario s;
        s.hand({official(10962120)});
        s.player().pp = 0;
        s.reject({Play, 0});
        CHECK(s.player().hand_count == 1 && s.player().board_count == 0);
    });

    suite.add("phase2_special.countdown.act_at_one_resolves_last_words_once", [] {
        Scenario s;
        s.hand({official(10962120)});
        s.player().pp = 1;
        s.run({Play, 0});
        s.player().board[0].countdown = 1;
        s.player().pp = 1;
        s.run({Act, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(10962120) &&
              s.player().board[0].countdown == 0 && s.player().shadows == 1);
        s.reject({Act, 0});
    });

    suite.add("phase2_special.countdown.act_cost_failure_does_not_reduce_countdown", [] {
        Scenario s;
        s.hand({official(10962120)});
        s.player().pp = 1;
        s.run({Play, 0});
        const auto countdown = s.player().board[0].countdown;
        s.player().pp = 0;
        s.reject({Act, 0});
        CHECK(s.player().board[0].countdown == countdown);
    });

    suite.add("phase2_special.countdown.countdown_three_requires_three_owner_turns", [] {
        Scenario s;
        s.hand({official(10661110)});
        s.player().pp = 2;
        s.run({Play, 0});
        for (int remaining = 2; remaining >= 1; --remaining) {
            s.next_own_turn();
            CHECK(s.player().board_count == 1 && s.player().board[0].countdown == remaining);
        }
        s.next_own_turn();
        CHECK(s.player().board[0].card_id == official(10661110) && s.player().shadows == 1);
    });

    suite.add("phase2_special.countdown.opponent_turn_does_not_reduce_form_countdown", [] {
        Scenario s;
        s.hand({official(10661110)});
        s.player().pp = 2;
        s.run({Play, 0});
        s.run({EndTurn});
        CHECK(s.player().board[0].countdown == 3);
    });

    suite.add("phase2_special.countdown.full_board_expiry_keeps_form_and_no_duplicate_shadow", [] {
        Scenario s;
        s.hand({official(10661110)});
        s.player().pp = 2;
        s.run({Play, 0});
        while (s.player().board_count < kBoardLimit) s.place(kTrainer);
        s.next_own_turn();
        s.next_own_turn();
        s.next_own_turn();
        CHECK(s.player().board_count == kBoardLimit &&
              std::any_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                          [](const Permanent& object) { return object.card_id == official(10661110); }) &&
              s.player().shadows == 1);
    });

    suite.add("phase2_special.faith.sasanido_below_threshold_does_not_spend_faith", [] {
        Scenario s;
        s.player().faiths[0] = official(10614122);
        s.player().faith_count = 1;
        s.player().faith_values[0] = 8;
        s.player().ep = 1;
        s.place(kTrainer);
        s.run({Evolve, 0});
        s.hand({official(10614120)});
        s.run({Play, 0});
        CHECK(s.player().faith_values[0] == 9);
        CHECK(s.player().hand_count == 0);
    });

    suite.add("phase2_special.faith.sasanido_exact_threshold_spends_once_and_increments_evolution", [] {
        Scenario s;
        s.player().faiths[0] = official(10614122);
        s.player().faith_count = 1;
        s.player().faith_values[0] = 9;
        s.player().ep = 1;
        s.place(kTrainer);
        s.place(kTrainer);
        s.run({Evolve, 0});
        CHECK(s.player().faith_values[0] == 10);
        s.hand({official(10614120)});
        s.run({Play, 0});
        CHECK(s.player().faith_values[0] == 0 && s.player().faith_upgrades[0] == 1);
        s.run({Play, 0, static_cast<std::int8_t>(kOwnBoardTarget + 1)});
        CHECK(s.player().faith_values[0] == 1 && s.player().board[1].evolution == Evolution::Normal);
    });

    suite.add("phase2_special.faith.values_saturate_without_wrapping", [] {
        Scenario s;
        s.player().faiths[0] = official(10634122);
        s.player().faith_count = 1;
        s.player().faith_values[0] = 65535;
        s.hand({official(10012110)});
        s.run({Play, 0});
        CHECK(s.player().faith_values[0] == 65535);
    });

    suite.add("phase2_special.faith.evolution_progress_does_not_count_enemy_evolution", [] {
        Scenario s;
        s.player().faiths[0] = official(10614122);
        s.player().faith_count = 1;
        s.place(kTrainer, 1);
        s.player(1).ep = 1;
        s.state.turn_player = 1;
        s.run({Evolve, 0});
        CHECK(s.player().faith_values[0] == 0);
    });

    suite.add("phase2_special.faith.amulet_destruction_progress_is_owner_scoped", [] {
        Scenario s;
        s.player().faiths[0] = official(10664122);
        s.player().faith_count = 1;
        s.player(1).faiths[0] = official(10664122);
        s.player(1).faith_count = 1;
        s.place(kWingedStatue, 1);
        constexpr Effect destroy{.operation = Operation::Destroy, .target = Target::AllAmulets};
        resolve_effect_steps(s.state, 0, std::array{destroy});
        CHECK(s.player().faith_values[0] == 0 && s.player(1).faith_values[0] == 1 &&
              s.player(1).board_count == 1 && s.player(1).board[0].card_id == kHolyFalcon);
    });

    suite.add("phase2_special.faith.unrelated_turn_does_not_advance_faith", [] {
        Scenario s;
        s.player().faiths[0] = official(10614122);
        s.player().faith_count = 1;
        s.player().faith_values[0] = 3;
        const auto before = s.player();
        s.run({EndTurn});
        CHECK(s.player().faith_count == 1 && s.player().faiths[0] == official(10614122) &&
              s.player().faith_values[0] == before.faith_values[0]);
    });
}

} // namespace svwb::wb::spec
