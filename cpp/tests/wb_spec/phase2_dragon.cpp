#include "spec.hpp"

namespace svwb::wb::spec {

void phase2_dragon(Suite& suite) {
    suite.add("phase2_dragon.10041120.vanilla_ward", [] {
        const auto id = official(10041120);
        const auto* card = find_card(id);
        CHECK(card && card->effects[0].trigger == Trigger::None);
        CHECK(card->abilities & FLAG_WARD);
        Scenario s;
        s.hand({id});
        s.run({Play, 0});
        CHECK(s.player().board_count == 1 && (s.player().board[0].abilities & FLAG_WARD));
    });

    suite.add("phase2_dragon.10543110.super_evolve_exiles_odd_costs_and_splits_damage", [] {
        const auto id = official(10543110);
        Scenario s;
        auto& p = s.player();
        const std::array<CardId, 6> deck{official(10642310), official(10641310), official(10742310),
                                         official(10642110), official(10741120), official(10941110)};
        std::copy(deck.begin(), deck.end(), p.deck.begin());
        p.deck_idx = 0;
        p.deck_end = static_cast<std::uint8_t>(deck.size());
        auto& first = s.place(kTrainer, 1);
        first.hp = first.max_hp = 12;
        auto& second = s.place(kLilium, 1);
        second.hp = second.max_hp = 12;
        s.hand({id});
        s.run({Play, 0});
        s.run({SuperEvolve, 0});
        CHECK(p.deck_remaining() == 3); // Only printed odd costs 1, 3, 5, 7, 9 are exiled.
        CHECK(s.player(1).board_count == 2);
        CHECK(s.player(1).board[0].hp + s.player(1).board[1].hp == 21);
    });

    suite.add("phase2_dragon.10641310.discard_threshold_and_selected_follower_buff", [] {
        const auto sword = official(10641310);
        const auto discard = official(10642110);
        Scenario s;
        s.place(kTrainer);
        s.hand({discard, sword});
        s.run({Play, 0, kOwnHandTarget + 1});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == sword);
        CHECK(card_cost(*find_card(sword), s.player().hand_modifiers[0]) == 2);

        Scenario below_threshold;
        below_threshold.hand({discard, sword});
        below_threshold.player().hand_modifiers[1].cost = -1;
        below_threshold.run({Play, 0, kOwnHandTarget + 1});
        CHECK(below_threshold.player().hand_count == 0);

        Scenario buff;
        auto& target = buff.place(kTrainer);
        buff.hand({sword});
        buff.run({Play, 0, kOwnBoardTarget});
        CHECK(target.atk == find_card(kTrainer)->atk + 2 && target.hp == find_card(kTrainer)->hp + 2);
    });

    suite.add("phase2_dragon.10642110.play_discards_selected_card_and_keeps_rush_barrier", [] {
        const auto id = official(10642110);
        Scenario s;
        s.hand({id, kTrainer});
        s.run({Play, 0, kOwnHandTarget + 1});
        CHECK(s.player().hand_count == 0 && s.player().board_count == 1);
        CHECK(s.player().board[0].abilities & FLAG_RUSH);
        CHECK(s.player().board[0].abilities & kBarrier);
    });

    suite.add("phase2_dragon.10642310.red_stream_discards_then_destroys_chosen_follower", [] {
        const auto id = official(10642310);
        Scenario s;
        s.hand({id, kTrainer});
        s.place(kLilium, 1);
        s.run({Play, 0, 0, 0, 0, false, false, 1});
        CHECK(s.player().hand_count == 0);
        CHECK(s.player(1).board_count == 0);
    });

    suite.add("phase2_dragon.10644110.sagatsumatsu_discards_and_adds_two_red_streams", [] {
        const auto id = official(10644110);
        const auto red_stream = official(10642310);
        Scenario s;
        s.hand({id, kTrainer});
        s.run({Play, 0, kOwnHandTarget + 1});
        CHECK(s.player().hand_count == 2);
        CHECK(s.player().hand[0] == red_stream && s.player().hand[1] == red_stream);
        CHECK(s.player().board_count == 1);
        CHECK((s.player().board[0].abilities & (FLAG_STORM | kBane | kAura)) ==
              (FLAG_STORM | kBane | kAura));
    });

    suite.add("phase2_dragon.10741120.carry_wyvern_targets_other_ally_and_hand_follower", [] {
        const auto id = official(10741120);
        Scenario s;
        s.place(kTrainer);
        s.hand({id, kTrainer});
        s.run({Play, 0, kOwnBoardTarget});
        CHECK(s.player().board[0].atk == find_card(kTrainer)->atk + 2);
        s.run({Evolve, 1, kOwnHandTarget});
        CHECK(s.player().hand_modifiers[0].attack == 2 && s.player().hand_modifiers[0].defense == 2);
        CHECK(s.player().hand[0] == kTrainer);
    });

    suite.add("phase2_dragon.10741310.pp_ramp_transforms_named_hand_and_deck_cards", [] {
        const auto id = official(10741310);
        const auto sleep = official(10742310);
        Scenario s;
        auto& p = s.player();
        p.max_pp = p.pp = 5;
        p.deck[0] = id;
        p.deck[1] = kTrainer;
        p.deck_idx = 0;
        p.deck_end = 2;
        s.hand({id, id});
        s.run({Play, 0});
        CHECK(p.max_pp == 6 && p.pp == 1);
        CHECK(p.hand_count == 1 && p.hand[0] == sleep);
        CHECK(p.deck[0] == sleep && p.deck[1] == kTrainer);
    });

    suite.add("phase2_dragon.10742310.heal_always_draws_only_in_overflow", [] {
        const auto id = official(10742310);
        Scenario ordinary;
        ordinary.player().max_pp = 6;
        ordinary.player().leader_hp = 10;
        ordinary.hand({id});
        const auto deck_before = ordinary.player().deck_remaining();
        ordinary.run({Play, 0});
        CHECK(ordinary.player().leader_hp == 13);
        CHECK(ordinary.player().deck_remaining() == deck_before);

        Scenario overflow;
        overflow.player().max_pp = 7;
        overflow.player().leader_hp = 10;
        overflow.hand({id});
        overflow.run({Play, 0});
        CHECK(overflow.player().leader_hp == 13);
        CHECK(overflow.player().hand_count == 1);
    });

    suite.add("phase2_dragon.10841110.gido_auto_evolves_at_ten_health_boundary", [] {
        const auto id = official(10841110);
        Scenario at_threshold;
        at_threshold.player().leader_hp = 10;
        at_threshold.hand({id});
        at_threshold.run({Play, 0});
        CHECK(at_threshold.player().board[0].evolution == Evolution::Normal);
        CHECK((at_threshold.player().board[0].abilities & (FLAG_WARD | kBane)) == (FLAG_WARD | kBane));

        Scenario above;
        above.player().leader_hp = 11;
        above.hand({id});
        above.run({Play, 0});
        CHECK(above.player().board[0].evolution == Evolution::None);
    });

    suite.add("phase2_dragon.10844110.reflection_entry_counts_and_last_words_crest", [] {
        const auto id = official(10844110);
        Scenario s;
        s.hand({id, id, id});
        s.player().hand_modifiers[1].cost = -3;
        s.player().hand_modifiers[2].cost = -3;
        s.run({Play, 0});
        s.run({Play, 0});
        s.run({Play, 0});
        CHECK(s.player().board_count == 3);
        CHECK(s.player().board[2].atk == find_card(id)->atk + 4);
        CHECK(s.player().board[2].max_hp == find_card(id)->hp + 4);
        CHECK(s.player().board[2].evolution == Evolution::Normal);
        CHECK(s.player().board[2].abilities & FLAG_WARD);
        const auto source = s.player().board[2].creation_order;
        const std::array<Effect, 1> destroy_self{{Effect{.operation=Operation::Destroy, .target=Target::Self}}};
        resolve_effect_steps(s.state, 0, destroy_self, source);
        CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::Reflection);
    });

    suite.add("phase2_dragon.10941110.stealth_thief_storm_gate_and_last_words_copy", [] {
        const auto id = official(10941110);
        Scenario no_face;
        no_face.hand({id});
        no_face.run({Play, 0});
        CHECK(!(no_face.player().board[0].abilities & FLAG_STORM));

        Scenario face;
        face.player().attacked_leader_last_turn = true;
        face.hand({id});
        face.run({Play, 0});
        CHECK(face.player().board[0].abilities & FLAG_STORM);
        face.place(kTrainer, 1);
        face.player(1).board[0].atk = 20;
        face.run({Attack, 0, 0});
        CHECK(face.player().hand_count == 1 && face.player().hand[0] == id);
    });

    suite.add("phase2_dragon.10941120.cave_dragon_auto_evolves_only_in_overflow", [] {
        const auto id = official(10941120);
        Scenario dormant;
        dormant.player().max_pp = 6;
        dormant.hand({id});
        dormant.run({Play, 0});
        CHECK(dormant.player().board[0].evolution == Evolution::None);

        Scenario overflow;
        overflow.player().max_pp = 7;
        overflow.hand({id});
        overflow.run({Play, 0});
        CHECK(overflow.player().board[0].evolution == Evolution::Normal);
        CHECK(overflow.player().board[0].abilities & kAmbush);
    });

    suite.add("phase2_dragon.10941310.enhance_summons_whelp_and_deals_random_three", [] {
        const auto id = official(10941310);
        const auto whelp = official(90041110);
        Scenario base;
        base.place(kTrainer, 1);
        base.hand({id});
        base.player().pp = 2;
        base.run({Play, 0});
        CHECK(base.player().board_count == 1 && base.player().board[0].card_id == whelp);
        CHECK(base.player(1).board[0].hp == find_card(kTrainer)->hp);

        Scenario enhanced;
        auto& target = enhanced.place(kTrainer, 1);
        target.hp = target.max_hp = 6;
        enhanced.hand({id});
        enhanced.player().pp = 3;
        enhanced.run({Play, 0});
        CHECK(enhanced.player().board_count == 1 && enhanced.player().board[0].card_id == whelp);
        CHECK(enhanced.player(1).board[0].hp == 3);
    });

    suite.add("phase2_dragon.11041120.ice_lizard_enhance_chains_three_hits_and_buffs_self", [] {
        const auto id = official(11041120);
        Scenario base;
        auto& target = base.place(kTrainer, 1);
        target.hp = target.max_hp = 8;
        base.hand({id});
        base.player().pp = 6;
        base.run({Play, 0});
        CHECK(base.player(1).board[0].hp == 5);

        Scenario enhanced;
        auto& enhanced_target = enhanced.place(kTrainer, 1);
        enhanced_target.hp = enhanced_target.max_hp = 12;
        enhanced.hand({id});
        enhanced.player().pp = 7;
        enhanced.run({Play, 0});
        CHECK(enhanced.player().board[0].atk == find_card(id)->atk + 5);
        CHECK(enhanced.player().board[0].max_hp == find_card(id)->hp + 5);
        CHECK(enhanced.player(1).board[0].hp == 3);
    });

    suite.add("phase2_dragon.11042120.scout_dragonnewt_attack_auto_evolves_other_ally", [] {
        const auto id = official(11042120);
        Scenario s;
        s.hand({id});
        s.run({Play, 0});
        CHECK(s.player().board[0].abilities & kAmbush);
        s.place(kTrainer);
        s.next_own_turn();
        s.run({Attack, 0, kEnemyLeader});
        CHECK(s.player().board[1].evolution == Evolution::Normal);
        CHECK(s.player().board[0].evolution == Evolution::None);
    });

    suite.add("phase2_dragon.11042310.evidence_discard_and_draw_uses_printed_cost", [] {
        const auto id = official(11042310);
        const auto cost_five = official(10042120);
        const auto cost_two = official(10041110);
        Scenario s;
        auto& target = s.place(kTrainer, 1);
        target.hp = target.max_hp = 8;
        s.player().deck[0] = cost_two;
        s.player().deck_modifiers[0].cost = 5; // Effective cost 7; printed cost remains 2.
        s.player().deck[1] = cost_five;
        s.player().deck_modifiers[1].cost = -4; // Effective cost 1; printed cost remains 5.
        s.player().deck_idx = 0;
        s.player().deck_end = 2;
        s.hand({id, kEvidence});
        s.run({Play, 0});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == cost_five);
        CHECK(s.player().hand_modifiers[0].cost == -4);
        CHECK(s.player().deck_remaining() == 1 && s.player().deck[1] == cost_two);
        CHECK(s.player(1).board[0].hp == 4);
    });

    suite.add("phase2_dragon.11042310.no_evidence_does_not_draw", [] {
        const auto id = official(11042310);
        const auto cost_five = official(10042120);
        Scenario no_evidence;
        auto& second_target = no_evidence.place(kTrainer, 1);
        second_target.hp = second_target.max_hp = 8;
        std::fill(no_evidence.player().deck.begin(), no_evidence.player().deck.end(), cost_five);
        no_evidence.player().deck_idx = 0;
        no_evidence.player().deck_end = kDeckSize;
        no_evidence.hand({id});
        no_evidence.run({Play, 0});
        CHECK(no_evidence.player().hand_count == 0);
        CHECK(no_evidence.player().deck_remaining() == kDeckSize);
        CHECK(no_evidence.player(1).board[0].hp == 4);
    });

    suite.add("phase2_dragon.11042310.no_eligible_printed_cost_keeps_deck_and_does_not_draw", [] {
        const auto id = official(11042310);
        const auto cost_two = official(10041110);
        Scenario no_eligible;
        auto& target = no_eligible.place(kTrainer, 1);
        target.hp = target.max_hp = 8;
        std::fill(no_eligible.player().deck.begin(), no_eligible.player().deck.end(), cost_two);
        no_eligible.player().deck_idx = 0;
        no_eligible.player().deck_end = kDeckSize;
        no_eligible.hand({id, kEvidence});
        no_eligible.run({Play, 0});
        CHECK(no_eligible.player().hand_count == 0);
        CHECK(no_eligible.player().deck_remaining() == kDeckSize);
        CHECK(no_eligible.player(1).board[0].hp == 4);
    });

    suite.add("phase2_dragon.11043310.mode_one_two_zero_followers_buff_summoned_mouse", [] {
        const auto id = official(11043310);
        const auto token = official(11012110);
        Scenario empty;
        empty.hand({id});
        empty.run({Play, 0, kNoTarget, static_cast<std::uint8_t>(kModeMask | 0b0011)});
        CHECK(empty.player().board_count == 1 && empty.player().board[0].card_id == token);
        CHECK(empty.player().board[0].atk == find_card(token)->atk + 1);
    });

    suite.add("phase2_dragon.11043310.mode_one_two_one_follower_buff_both_followers", [] {
        const auto id = official(11043310);
        const auto token = official(11012110);
        Scenario one;
        auto& existing = one.place(kTrainer);
        one.hand({id});
        one.run({Play, 0, kNoTarget, static_cast<std::uint8_t>(kModeMask | 0b0011)});
        CHECK(one.player().board_count == 2 && one.player().board[1].card_id == token);
        CHECK(existing.atk == find_card(kTrainer)->atk + 1);
        CHECK(one.player().board[1].atk == find_card(token)->atk + 1);
    });

    suite.add("phase2_dragon.11043310.mode_one_two_two_followers_skip_evidence_amulet", [] {
        const auto id = official(11043310);
        const auto token = official(11012110);
        Scenario mixed;
        auto& left = mixed.place(kTrainer);
        auto& evidence = mixed.place(kEvidence);
        auto& right = mixed.place(kTrainer);
        mixed.hand({id});
        mixed.run({Play, 0, kNoTarget, static_cast<std::uint8_t>(kModeMask | 0b0011)});
        CHECK(mixed.player().board_count == 4 && mixed.player().board[3].card_id == token);
        CHECK(left.atk == find_card(kTrainer)->atk + 1);
        CHECK(evidence.atk == find_card(kEvidence)->atk);
        CHECK(right.atk == find_card(kTrainer)->atk + 1);
        CHECK(mixed.player().board[3].atk == find_card(token)->atk);
    });

    suite.add("phase2_dragon.11043310.mode_one_two_three_followers_buff_only_leftmost_two", [] {
        const auto id = official(11043310);
        const auto token = official(11012110);
        Scenario three;
        auto& first = three.place(kTrainer);
        auto& second = three.place(kTrainer);
        auto& third = three.place(kTrainer);
        three.hand({id});
        three.run({Play, 0, kNoTarget, static_cast<std::uint8_t>(kModeMask | 0b0011)});
        CHECK(three.player().board_count == 4 && three.player().board[3].card_id == token);
        CHECK(first.atk == find_card(kTrainer)->atk + 1);
        CHECK(second.atk == find_card(kTrainer)->atk + 1);
        CHECK(third.atk == find_card(kTrainer)->atk);
        CHECK(three.player().board[3].atk == find_card(token)->atk);
    });

    suite.add("phase2_dragon.11043310.mode_one_two_full_board_skips_mouse_and_buffs_leftmost_two", [] {
        const auto id = official(11043310);
        Scenario full;
        for (int i = 0; i < kBoardLimit; ++i) full.place(kTrainer);
        full.hand({id});
        full.run({Play, 0, kNoTarget, static_cast<std::uint8_t>(kModeMask | 0b0011)});
        CHECK(full.player().board_count == kBoardLimit);
        CHECK(full.player().board[0].atk == find_card(kTrainer)->atk + 1);
        CHECK(full.player().board[1].atk == find_card(kTrainer)->atk + 1);
        for (int i = 2; i < kBoardLimit; ++i) CHECK(full.player().board[i].atk == find_card(kTrainer)->atk);
    });

    suite.add("phase2_dragon.11043310.mode_two_without_followers_skips_amulet", [] {
        Scenario s;
        s.place(kEvidence);
        s.hand({official(11043310)});
        s.run({Play, 0, kNoTarget, static_cast<std::uint8_t>(kModeMask | 0b0110)});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == kEvidence);
        CHECK(s.player().board[0].atk == 0);
        CHECK(s.player().hand_count == 3);
        CHECK(std::all_of(s.player().hand.begin(), s.player().hand.begin() + 3,
                          [](CardId id) { return id == kEvidence; }));
    });

    suite.add("phase2_dragon.11043310.mode_three_four_add_tokens_and_damage", [] {
        const auto id = official(11043310);
        Scenario damage;
        auto& enemy = damage.place(kTrainer, 1);
        enemy.hp = enemy.max_hp = 8;
        damage.hand({id});
        damage.run({Play, 0, kNoTarget, static_cast<std::uint8_t>(kModeMask | 0b1100)});
        CHECK(damage.player(1).board[0].hp == 4);
        CHECK(damage.player().hand_count == 3);
    });
}

} // namespace svwb::wb::spec
