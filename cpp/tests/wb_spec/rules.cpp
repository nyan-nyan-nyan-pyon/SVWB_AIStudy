#include "spec.hpp"

#include <array>

namespace svwb::wb::spec {

void rules(Suite& suite) {
    suite.add("rules.board_limit_summon", [] {
        Scenario s;
        for (int i = 0; i < kBoardLimit - 1; ++i) s.place(kColette);
        const std::array summon{Effect{Trigger::None, Operation::Summon, Target::None, 3, kRabbit}};
        resolve_effect_steps(s.state, 0, summon);
        CHECK(s.player().board_count == kBoardLimit);
        CHECK(s.player().board.back().card_id == kRabbit);
        CHECK(s.player().shadows == 0);
    });

    suite.add("rules.hand_limit_overflow_draw", [] {
        Scenario s;
        s.hand({kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer});
        const auto deck_cursor = s.player().deck_idx;
        const auto next = s.player().deck[s.player().deck_idx];
        const std::array draw{Effect{Trigger::None, Operation::Draw, Target::None, 1}};
        resolve_effect_steps(s.state, 0, draw);
        CHECK(s.player().hand_count == kHandLimit);
        CHECK(s.player().deck_idx == deck_cursor + 1 && s.player().shadows == 1);
        CHECK(s.player().hand[0] == kTrainer && next != 0);
    });

    suite.add("rules.random_target_respects_eligibility", [] {
        Scenario s;
        auto& eligible = s.place(kColette, 1);
        eligible.abilities |= FLAG_WARD;
        const auto eligible_hp = eligible.hp;
        const auto& excluded = s.place(kTrainer, 1);
        const auto excluded_hp = excluded.hp;
        const std::array random_hit{
            Effect{Trigger::None, Operation::Damage, Target::RandomEnemyTwoWard, 1}};
        resolve_effect_steps(s.state, 0, random_hit);
        CHECK(s.player(1).board_count == 2);
        CHECK(s.player(1).board[0].hp == eligible_hp - 1);
        CHECK(s.player(1).board[1].hp == excluded_hp);
    });

    suite.add("rules.simultaneous_follower_destruction", [] {
        Scenario s;
        s.place(kColette, 1);
        s.place(kTrainer, 1);
        const std::array sweep{
            Effect{Trigger::None, Operation::Damage, Target::AllEnemyFollowers, 9}};
        resolve_effect_steps(s.state, 0, sweep);
        CHECK(s.player(1).board_count == 0);
        CHECK(s.player(1).shadows == 2);
        CHECK(s.player(1).destroyed_followers[kColette - kFirstCard] == 1);
        CHECK(s.player(1).destroyed_followers[kTrainer - kFirstCard] == 1);
    });

    suite.add("rules.countdown_resolution_creation_order", [] {
        Scenario s;
        s.place(kWingedStatue).countdown = 1;
        s.place(kProjectedStatue).countdown = 1;
        s.next_own_turn();
        CHECK(s.player().board_count == 2);
        CHECK(s.player().board[0].card_id == kHolyFalcon);
        CHECK(s.player().board[1].card_id == kSplendidHawk);
        CHECK(s.player().shadows == 2);
    });

    suite.add("rules.lethal_stops_later_effect_steps", [] {
        Scenario s;
        s.player(1).leader_hp = 2;
        const std::array after_lethal{
            Effect{Trigger::None, Operation::Damage, Target::EnemyLeader, 2},
            Effect{Trigger::None, Operation::Heal, Target::EnemyLeader, 10},
        };
        resolve_effect_steps(s.state, 0, after_lethal);
        CHECK(s.state.outcome == Outcome::FirstWins);
        CHECK(s.state.end_reason == EndReason::Health);
        CHECK(s.player(1).leader_hp == 0);
    });

    suite.add("rules.state_copy_is_independent_internal", [] {
        Scenario original;
        original.hand({kTrainer});
        auto copy = original.state;
        const auto unchanged = original.state;
        CHECK(apply_action(copy, {Play, 0}, original.rules));
        CHECK(original.state == unchanged);
        CHECK(copy != original.state);
    });

    suite.add("rules.board_limit_exact_capacity", [] {
        Scenario s;
        for (int i = 0; i < kBoardLimit - 1; ++i) s.place(kColette);
        s.hand({kTrainer});
        s.run({Play, 0});
        CHECK(s.player().board_count == kBoardLimit);
        CHECK(s.player().board.back().card_id == kTrainer);
    });

    suite.add("rules.board_limit_overflow_play_rejected", [] {
        Scenario s;
        for (int i = 0; i < kBoardLimit; ++i) s.place(kColette);
        s.hand({kTrainer});
        s.reject({Play, 0});
        CHECK(s.player().board_count == kBoardLimit);
        CHECK(s.player().hand_count == 1 && s.player().shadows == 0);
    });

    suite.add("rules.board_limit_summon_plus_one", [] {
        Scenario s;
        for (int i = 0; i < kBoardLimit; ++i) s.place(kColette);
        const auto before_shadows = s.player().shadows;
        const std::array summon{Effect{Trigger::None, Operation::Summon, Target::None, 1, kRabbit}};
        resolve_effect_steps(s.state, 0, summon);
        CHECK(s.player().board_count == kBoardLimit);
        CHECK(s.player().shadows == before_shadows);
    });

    suite.add("rules.hand_limit_exact_nine_draw", [] {
        Scenario s;
        s.hand({kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer});
        const auto cursor = s.player().deck_idx;
        const auto drawn = s.player().deck[cursor];
        const std::array draw{Effect{Trigger::None, Operation::Draw, Target::None, 1}};
        resolve_effect_steps(s.state, 0, draw);
        CHECK(s.player().hand_count == kHandLimit && s.player().hand[8] == drawn);
        CHECK(s.player().deck_idx == cursor + 1 && s.player().shadows == 0);
    });

    suite.add("rules.hand_limit_addhand_overflow", [] {
        Scenario s;
        s.hand({kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer});
        const std::array add{Effect{Trigger::None, Operation::AddHand, Target::None, 1, kEvidence}};
        resolve_effect_steps(s.state, 0, add);
        CHECK(s.player().hand_count == kHandLimit && s.player().shadows == 1);
        CHECK(s.player().hand[8] == kTrainer);
    });

    suite.add("rules.bounce_with_hand_room", [] {
        Scenario s;
        const auto target = s.place(kColette, 1).creation_order;
        s.hand({kTrainer}, 1);
        const std::array bounce{Effect{Trigger::None, Operation::Bounce, Target::ChosenEnemyFollower}};
        resolve_effect_steps(s.state, 0, bounce, 0, target);
        CHECK(s.player(1).board_count == 0 && s.player(1).hand_count == 2);
        CHECK(s.player(1).hand[1] == kColette && s.player(1).shadows == 0);
        CHECK(s.player(1).destroyed_followers[kColette - kFirstCard] == 0);
    });

    suite.add("rules.bounce_at_full_hand_overflows", [] {
        Scenario s;
        const auto target = s.place(kColette, 1).creation_order;
        s.hand({kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer}, 1);
        const std::array bounce{Effect{Trigger::None, Operation::Bounce, Target::ChosenEnemyFollower}};
        resolve_effect_steps(s.state, 0, bounce, 0, target);
        CHECK(s.player(1).board_count == 0 && s.player(1).hand_count == kHandLimit);
        CHECK(s.player(1).shadows == 1);
        CHECK(s.player(1).destroyed_followers[kColette - kFirstCard] == 0);
    });

    suite.add("rules.random_target_empty_eligibility", [] {
        Scenario s;
        s.place(kTrainer, 1); // No Ward follower is eligible.
        const auto rng_before = s.state.rng_state;
        const auto hp_before = s.player(1).board[0].hp;
        const std::array random_hit{
            Effect{Trigger::None, Operation::Damage, Target::RandomEnemyTwoWard, 1}};
        resolve_effect_steps(s.state, 0, random_hit);
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == hp_before);
        CHECK(s.state.rng_state == rng_before);
    });

    suite.add("rules.random_target_requires_follower", [] {
        Scenario s;
        s.place(kEvidence, 1); // Amulets are not random follower targets.
        const std::array random_hit{
            Effect{Trigger::None, Operation::Damage, Target::RandomEnemyFollower, 9}};
        resolve_effect_steps(s.state, 0, random_hit);
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].card_id == kEvidence);
        CHECK(s.player(1).shadows == 0);
    });

    suite.add("rules.simultaneous_lethal_default_policy", [] {
        Scenario s;
        s.player().leader_hp = s.player(1).leader_hp = 1;
        const std::array simultaneous{
            Effect{Trigger::None, Operation::Damage, Target::BothLeaders, 1}};
        resolve_effect_steps(s.state, 0, simultaneous);
        CHECK(s.player().leader_hp == 0 && s.player(1).leader_hp == 0);
        CHECK(s.state.outcome == Outcome::SecondWins);
        CHECK(s.state.end_reason == EndReason::Health);
    });

    suite.add("rules.empty_deck_draw_is_immediate_loss", [] {
        Scenario s;
        s.player().deck_idx = s.player().deck_end;
        const auto hand_before = s.player().hand_count;
        const std::array draw{Effect{Trigger::None, Operation::Draw, Target::None, 1}};
        resolve_effect_steps(s.state, 0, draw);
        CHECK(s.state.outcome == Outcome::SecondWins && s.state.end_reason == EndReason::EmptyDeck);
        CHECK(s.player().hand_count == hand_before);
    });

    suite.add("rules.countdown_not_expired_at_two", [] {
        Scenario s;
        s.place(kWingedStatue).countdown = 2;
        s.next_own_turn();
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == kWingedStatue);
        CHECK(s.player().board[0].countdown == 1 && s.player().shadows == 0);
    });

    suite.add("rules.countdown_one_expires_at_start", [] {
        Scenario s;
        s.place(kWingedStatue).countdown = 1;
        s.next_own_turn();
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == kHolyFalcon);
        CHECK(s.player().shadows == 1);
    });

    suite.add("rules.countdown_expiry_at_full_board", [] {
        Scenario s;
        for (int i = 0; i < kBoardLimit - 1; ++i) s.place(kColette);
        s.place(kWingedStatue).countdown = 1;
        s.next_own_turn();
        CHECK(s.player().board_count == kBoardLimit);
        CHECK(s.player().board.back().card_id == kHolyFalcon);
        CHECK(s.player().shadows == 1);
    });

    suite.add("rules.crest_duplicate_is_not_added", [] {
        Scenario s;
        s.crest(CrestId::Testimony, 2);
        const auto next_order = s.state.next_creation_order;
        const std::array grant{Effect{Trigger::None, Operation::GrantCrest, Target::None, 0, 0, 0, 0, CrestId::Testimony}};
        resolve_effect_steps(s.state, 0, grant);
        CHECK(s.player().crest_count == 1 && s.player().crests[0].countdown == 2);
        CHECK(s.state.next_creation_order == next_order);
    });

    suite.add("rules.crest_capacity_plus_one", [] {
        Scenario s;
        const std::array ids{CrestId::Testimony, CrestId::Rapid, CrestId::Rituel,
                             CrestId::Haylem, CrestId::Erralde};
        for (const auto id : ids) s.crest(id, 3);
        const auto next_order = s.state.next_creation_order;
        const std::array grant{Effect{Trigger::None, Operation::GrantCrest, Target::None, 0, 0, 0, 0, CrestId::Kukishiro}};
        resolve_effect_steps(s.state, 0, grant);
        CHECK(s.player().crest_count == kCrestLimit);
        CHECK(s.state.next_creation_order == next_order);
    });

    suite.add("rules.expiring_crest_full_hand", [] {
        Scenario s;
        s.hand({kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer, kTrainer});
        s.crest(CrestId::Testimony, 1);
        s.player().leader_hp = 17;
        s.next_own_turn();
        CHECK(s.player().leader_hp == 18);
        CHECK(s.player().hand_count == kHandLimit && s.player().crest_count == 0);
        CHECK(s.player().shadows >= 2); // Testimony's generated card and the regular turn draw overflow.
    });

    suite.add("rules.state_identity_tracks_crest_internal", [] {
        Scenario s;
        auto changed = s.state;
        changed.players[0].crest_count = 1;
        changed.players[0].crests[0] = {900, CrestId::Rapid, 2, 0, 0};
        CHECK(changed != s.state);
    });

    suite.add("rules.state_identity_tracks_faith_internal", [] {
        Scenario s;
        auto changed = s.state;
        changed.players[0].faith_count = 1;
        changed.players[0].faiths[0] = kFaithToken;
        changed.players[0].faith_values[0] = 4;
        CHECK(changed != s.state);
    });

    suite.add("rules.state_identity_tracks_turn_flags_internal", [] {
        Scenario s;
        auto changed = s.state;
        changed.players[0].attacked_leader_last_turn = true;
        CHECK(changed != s.state);
        changed = s.state;
        changed.players[0].extra_pp_available = true;
        CHECK(changed != s.state);
    });
}

} // namespace svwb::wb::spec
