#include "spec.hpp"

namespace svwb::wb::spec {

void phase2_nightmare(Suite& suite) {
    suite.add("phase2_nightmare.10051110.storm_bane", [] {
        const auto card = official(10051110);
        Scenario s;
        s.hand({card});
        s.run({Play, 0});
        CHECK((s.player().board[0].abilities & FLAG_STORM) != 0);
        CHECK((s.player().board[0].abilities & kBane) != 0);
    });

    suite.add("phase2_nightmare.10551110.enhance_six", [] {
        const auto card = official(10551110);
        Scenario enhanced;
        enhanced.player().pp = 6;
        enhanced.hand({card});
        enhanced.run({Play, 0});
        CHECK(enhanced.player().board[0].max_hp == find_card(card)->hp + 6);
        CHECK((enhanced.player().board[0].abilities & FLAG_RUSH) != 0);
        CHECK((enhanced.player().board[0].abilities & (kBane | kBarrier)) == (kBane | kBarrier));
        Scenario base;
        base.player().pp = 5;
        base.hand({card});
        base.run({Play, 0});
        CHECK((base.player().board[0].abilities & (kBane | kBarrier)) == 0);
    });

    suite.add("phase2_nightmare.10552120.fanfare_evolve_draw_and_attack_block", [] {
        const auto card = official(10552120);
        Scenario s;
        s.place(kTrainer, 1);
        s.hand({card});
        const auto cursor = s.player().deck_idx;
        s.run({Play, 0, 0});
        CHECK((s.player(1).board[0].temporary_abilities & kAttackBlocked) != 0);
        CHECK(s.player().deck_idx == cursor + 1);
        s.run({Evolve, 0, 0});
        CHECK(s.player().deck_idx == cursor + 2);
        s.next_own_turn();
        CHECK((s.player(1).board[0].temporary_abilities & kAttackBlocked) == 0);
    });

    suite.add("phase2_nightmare.10553110.transform_and_skeleton_death_heal", [] {
        const auto card = official(10553110);
        const auto skeleton = official(90051110);
        Scenario s;
        s.player().leader_hp = 10;
        s.place(kTrainer);
        s.place(kTrainer, 1);
        s.hand({card});
        s.run({Play, 0});
        CHECK(s.player().board[0].card_id == skeleton && s.player().board[1].card_id == card);
        CHECK(s.player(1).board[0].card_id == skeleton);
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        resolve_effect_steps(s.state, 0, std::array{destroy}, s.player().board[0].creation_order);
        CHECK(s.player().leader_hp == 11);
    });

    suite.add("phase2_nightmare.10553310.grant_crest", [] {
        const auto card = official(10553310);
        Scenario s;
        s.hand({card});
        s.run({Play, 0});
        CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::Okuyoka);
    });

    suite.add("phase2_nightmare.10554110.reanimate_and_super_crest", [] {
        const auto card = official(10554110);
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        Scenario s;
        const auto a = s.place(kTrainer).creation_order;
        const auto b = s.place(kLilium).creation_order;
        resolve_effect_steps(s.state, 0, std::array{destroy}, a);
        resolve_effect_steps(s.state, 0, std::array{destroy}, b);
        s.hand({card});
        s.run({Play, 0});
        CHECK(s.player().board_count == 3);
        CHECK(s.player().board[1].card_id == kTrainer && s.player().board[2].card_id == kLilium);
        s.run({SuperEvolve, 0});
        CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::Miltio);
    });

    suite.add("phase2_nightmare.10554120.shuffle_draw_twice_and_cost_multiplicity", [] {
        const auto card = official(10554120);
        Scenario s;
        s.player(1).leader_hp = 10;
        s.hand({card, kRabbit, kRabbit, kRabbit, kRabbit});
        const auto cursor = s.player().deck_idx;
        for (std::size_t i = cursor; i < s.player().deck_end; ++i) s.player().deck[i] = kRabbit;
        s.run({Play, 0});
        CHECK(s.player().deck_idx == cursor + 8);
        CHECK(s.player().hand_count == 4);
        CHECK(s.player(1).leader_hp == 2);
    });

    suite.add("phase2_nightmare.10652110.enhance_reanimate_nine_boundary", [] {
        const auto card = official(10652110);
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        Scenario paid;
        const auto corpse = paid.place(official(10553110)).creation_order;
        resolve_effect_steps(paid.state, 0, std::array{destroy}, corpse);
        paid.player().pp = 8;
        paid.hand({card});
        paid.run({Play, 0});
        CHECK(paid.player().board_count == 2 && paid.player().board[1].card_id == official(10553110));
        Scenario below;
        below.player().pp = 7;
        below.hand({card});
        below.run({Play, 0});
        CHECK(below.player().board_count == 1);
    });

    suite.add("phase2_nightmare.10652120.enhance_storm_and_evolve_damage", [] {
        const auto card = official(10652120);
        Scenario s;
        s.player().pp = 7;
        s.place(kTrainer, 1).hp = 5;
        s.hand({card});
        s.run({Play, 0});
        CHECK((s.player().board[0].abilities & FLAG_STORM) != 0);
        s.run({Evolve, 0, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 1);
    });

    suite.add("phase2_nightmare.10652310.banish_and_copy", [] {
        const auto card = official(10652310);
        Scenario s;
        const auto victim = s.place(kTrainer, 1);
        const auto id = victim.card_id;
        const auto atk = victim.atk;
        const auto hp = victim.hp;
        s.hand({card});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0);
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == id);
        CHECK(s.player().board[0].atk == atk && s.player().board[0].hp == hp);
    });

    suite.add("phase2_nightmare.10654110.aura_clash_and_super_attack_limit", [] {
        const auto card = official(10654110);
        Scenario s;
        auto& almes = s.place(card);
        almes.readied = true;
        CHECK((almes.abilities & kAura) != 0);
        s.place(kTrainer, 1).readied = true;
        s.run({Attack, 0, 0});
        CHECK(s.player().board_count == 1 && s.player(1).board_count == 0);
        Scenario super;
        super.place(card);
        super.run({SuperEvolve, 0});
        CHECK(super.player().board[0].attack_limit == 3);
    });

    suite.add("phase2_nightmare.10654120.necromancy_auto_evolve_and_add_card", [] {
        const auto card = official(10654120);
        Scenario paid;
        paid.player().shadows = 4;
        paid.hand({card});
        paid.run({Play, 0});
        CHECK(paid.player().board[0].evolution == Evolution::Normal);
        CHECK(paid.player().hand_count == 1 && paid.player().hand[0] == official(90054330));
        Scenario unpaid;
        unpaid.player().shadows = 3;
        unpaid.hand({card});
        unpaid.run({Play, 0});
        CHECK(unpaid.player().board[0].evolution == Evolution::None && unpaid.player().shadows == 3);
    });

    suite.add("phase2_nightmare.10751310.buff_two_and_draw", [] {
        const auto card = official(10751310);
        Scenario s;
        s.place(kTrainer);
        s.place(kLilium);
        s.hand({card});
        const auto cursor = s.player().deck_idx;
        s.run({Play, 0, kNoTarget, 0, 0b11});
        CHECK(s.player().board[0].max_hp == find_card(kTrainer)->hp + 1 &&
              s.player().board[1].max_hp == find_card(kLilium)->hp + 1);
        CHECK(s.player().deck_idx == cursor + 1);
    });

    suite.add("phase2_nightmare.10752120.destroy_then_reanimate", [] {
        const auto card = official(10752120);
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        Scenario s;
        const auto corpse = s.place(kLilium).creation_order;
        resolve_effect_steps(s.state, 0, std::array{destroy}, corpse);
        s.place(kTrainer, 1);
        s.hand({card});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player().board_count == 2);
        CHECK(s.player().board[1].card_id == kLilium);
    });

    suite.add("phase2_nightmare.10752310.summon_and_evolve_three_tokens", [] {
        const auto card = official(10752310);
        Scenario s;
        s.hand({card});
        s.run({Play, 0});
        CHECK(s.player().board_count == 3);
        CHECK(s.player().board[0].evolution != Evolution::None &&
              s.player().board[1].evolution != Evolution::None &&
              s.player().board[2].evolution != Evolution::None);
    });

    suite.add("phase2_nightmare.10754110.random_deck_recruits_and_super_buff", [] {
        const auto card = official(10754110);
        Scenario s;
        s.hand({card});
        for (std::size_t i = s.player().deck_idx; i < s.player().deck_end; ++i) {
            s.player().deck[i] = (i % 2 == 0) ? official(10551110) : official(10951120);
        }
        s.run({Play, 0});
        CHECK(s.player().board_count == 3);
        s.run({SuperEvolve, 0});
        for (std::size_t i = 1; i < s.player().board_count; ++i) {
            CHECK(s.player().board[i].atk >= find_card(s.player().board[i].card_id)->atk + 2);
            CHECK((s.player().board[i].abilities & FLAG_RUSH) != 0);
        }
    });

    suite.add("phase2_nightmare.10851110.storm_ignores_ward", [] {
        const auto card = official(10851110);
        Scenario s;
        auto& ward = s.place(official(10001130), 1);
        ward.atk = 0;
        s.hand({card});
        s.run({Play, 0});
        s.run({Attack, 0, kEnemyLeader});
        CHECK(s.player(1).leader_hp < 20);
    });

    suite.add("phase2_nightmare.10853310.return_one_draw_two", [] {
        const auto card = official(10853310);
        Scenario s;
        s.hand({card, kRabbit});
        const auto cursor = s.player().deck_idx;
        s.run({Play, 0, static_cast<std::int8_t>(kOwnHandTarget + 1)});
        CHECK(s.player().deck_idx == cursor + 2 && s.player().hand_count == 2);
        CHECK(s.player().shadows == 1);
        CHECK(std::count(s.player().deck.begin() + s.player().deck_idx,
                         s.player().deck.begin() + s.player().deck_end, kRabbit) >= 1);
    });

    suite.add("phase2_nightmare.10854110.fanfare_modes_and_evolve_modes", [] {
        const auto card = official(10854110);
        Scenario s;
        s.player().leader_hp = 10;
        s.player(1).leader_hp = 10;
        s.hand({card});
        s.run({Play, 0, kNoTarget, 1});
        CHECK(s.player().leader_hp == 14 && s.player(1).leader_hp == 6);
        const auto cursor = s.player().deck_idx;
        s.run({Evolve, 0, kNoTarget, 1});
        CHECK(s.player().deck_idx == cursor + 2);
    });

    suite.add("phase2_nightmare.10951120.last_words_modified_copy_loses_last_words", [] {
        const auto card = official(10951120);
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        Scenario s;
        const auto order = s.place(card).creation_order;
        resolve_effect_steps(s.state, 0, std::array{destroy}, order);
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == card);
        CHECK(s.player().board[0].atk == find_card(card)->atk + 1);
        CHECK((s.player().board[0].abilities & FLAG_RUSH) != 0);
        CHECK((s.player().board[0].modifiers.effect_flags & kLoseLastWords) != 0);
    });

    suite.add("phase2_nightmare.10951310.summon_both_tokens", [] {
        const auto card = official(10951310);
        Scenario s;
        s.hand({card});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2);
        CHECK(s.player().board[0].card_id == official(90051130));
        CHECK(s.player().board[1].card_id == official(90051140));
    });

    suite.add("phase2_nightmare.10952110.ward_last_words_destroy_and_heal", [] {
        const auto card = official(10952110);
        Scenario s;
        s.player().leader_hp = 10;
        s.place(kTrainer, 1);
        const auto order = s.place(card).creation_order;
        CHECK((s.player().board[0].abilities & FLAG_WARD) != 0);
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        resolve_effect_steps(s.state, 0, std::array{destroy}, order);
        CHECK(s.player().leader_hp == 12 && s.player(1).board_count == 0);
    });

    suite.add("phase2_nightmare.10952120.chosen_destroy_and_leader_damage", [] {
        const auto card = official(10952120);
        Scenario s;
        s.player(1).leader_hp = 10;
        s.place(kTrainer, 1);
        s.hand({card});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).leader_hp == 8);
        Scenario no_target;
        no_target.hand({card});
        no_target.run({Play, 0});
        CHECK(no_target.player(1).leader_hp == 18);
    });

    suite.add("phase2_nightmare.10953310.grant_rebirth_last_words", [] {
        const auto card = official(10953310);
        const auto ally = official(10002120);
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        Scenario s;
        const auto order = s.place(ally).creation_order;
        s.hand({card});
        s.run({Play, 0, static_cast<std::int8_t>(kOwnBoardTarget)});
        resolve_effect_steps(s.state, 0, std::array{destroy}, order);
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == ally);
        CHECK(s.player().shadows == 2);
        Scenario no_ally;
        no_ally.hand({card});
        no_ally.reject({Play, 0});
    });

    suite.add("phase2_nightmare.10954110.reanimate_three_damage_and_crest", [] {
        const auto card = official(10954110);
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        Scenario s;
        for (int i = 0; i < 3; ++i) {
            const auto order = s.place(kLilium).creation_order;
            resolve_effect_steps(s.state, 0, std::array{destroy}, order);
        }
        s.place(kTrainer, 1).hp = 2;
        s.hand({card});
        s.run({Play, 0});
        CHECK(s.player().board_count == 4 && s.player().board[1].card_id == kLilium);
        CHECK(s.player(1).board_count == 0);
        s.run({SuperEvolve, 0});
        CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::IstenDead);
    });

    suite.add("phase2_nightmare.10954120.hand_discount_and_mode_choice", [] {
        const auto card = official(10954120);
        Scenario s;
        s.state.turn_player = 0;
        s.player().leader_hp = 12;
        s.hand({card});
        s.run({EndTurn});
        CHECK(s.player().hand_modifiers[0].cost == -1);
        s.run({EndTurn});
        CHECK(s.player().hand_count == 2 && s.player().hand[0] == card);
        CHECK(s.player().hand_modifiers[0].cost == -1);
        s.run({Play, 0, kNoTarget, 1});
        CHECK((s.player().board[0].abilities & FLAG_STORM) != 0);
        CHECK(s.player().leader_hp == 10);
        Scenario above_threshold;
        above_threshold.state.turn_player = 0;
        above_threshold.player().leader_hp = 13;
        above_threshold.hand({card});
        above_threshold.run({EndTurn});
        CHECK(above_threshold.player().hand_modifiers[0].cost == 0);
        above_threshold.run({EndTurn});
    });

    suite.add("phase2_nightmare.11051120.last_words_self_damage", [] {
        const auto card = official(11051120);
        Scenario s;
        s.player().leader_hp = 10;
        const auto order = s.place(card).creation_order;
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        resolve_effect_steps(s.state, 0, std::array{destroy}, order);
        CHECK(s.player().leader_hp == 8 && s.player().shadows == 1);
    });

    suite.add("phase2_nightmare.11052120.rush_necromancy_attack_sweep", [] {
        const auto card = official(11052120);
        Scenario paid;
        paid.player().shadows = 4;
        auto& first_enemy = paid.place(kTrainer, 1);
        first_enemy.hp = 3;
        auto& second_enemy = paid.place(kLilium, 1);
        second_enemy.hp = 3;
        paid.hand({card});
        paid.run({Play, 0});
        CHECK((paid.player().board[0].abilities & FLAG_RUSH) != 0);
        paid.run({Attack, 0, 0});
        CHECK(paid.player(1).board_count == 0 && paid.player().shadows == 0);
        Scenario unpaid;
        unpaid.player().shadows = 3;
        auto& durable_enemy = unpaid.place(kTrainer, 1);
        durable_enemy.hp = durable_enemy.max_hp = 20;
        unpaid.hand({card});
        unpaid.run({Play, 0});
        unpaid.run({Attack, 0, 0});
        CHECK(unpaid.player().shadows == 3 && unpaid.player(1).board[0].hp == 14);
    });

    suite.add("phase2_nightmare.11052310.summon_and_add_evidence", [] {
        const auto card = official(11052310);
        const auto evidence = official(90001210);
        Scenario s;
        s.hand({card});
        s.run({Play, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == evidence);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == evidence);
    });

    suite.add("phase2_nightmare.11053310.scale_damage_and_banish_evidence", [] {
        const auto card = official(11053310);
        const auto evidence = official(90001210);
        Scenario s;
        auto& target = s.place(kTrainer, 1);
        target.hp = target.max_hp = 2;
        s.place(evidence).clue_count = 2;
        s.hand({card});
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
        CHECK(s.player().board_count == 0 && s.player().shadows == 1);
    });

    suite.add("phase2_nightmare.11054120.ward_necromancy_last_words_and_evolve_crest", [] {
        const auto card = official(11054120);
        Scenario s;
        s.player().shadows = 2;
        s.player().leader_hp = 10;
        const auto order = s.place(card).creation_order;
        CHECK((s.player().board[0].abilities & FLAG_WARD) != 0);
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::Self};
        const auto cursor = s.player().deck_idx;
        resolve_effect_steps(s.state, 0, std::array{destroy}, order);
        CHECK(s.player().shadows == 1 && s.player().deck_idx == cursor + 1);
        Scenario crest;
        const auto copy = official(11054120);
        crest.place(copy);
        crest.run({Evolve, 0});
        CHECK(crest.player().crest_count == 1 && crest.player().crests[0].id == CrestId::Gremory);
    });
}

} // namespace svwb::wb::spec
