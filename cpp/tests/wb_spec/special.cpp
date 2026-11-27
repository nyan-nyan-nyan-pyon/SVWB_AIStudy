#include "spec.hpp"

#include <array>
#include <tuple>
#include <utility>

namespace svwb::wb::spec {

void special(Suite& suite) {
    using enum CrestId;

    // Form costs and effects are documented against the listed printed source cards.
    struct SummoningForm {
        std::uint32_t source;
        const char* case_name;
        int pp;
        int copies;
    };
    constexpr std::array summoning_forms{
        SummoningForm{10671110, "special.10671112.accelerate_summons_one", 2, 1},
        SummoningForm{10672110, "special.10672112.accelerate_summons_two", 3, 2},
        SummoningForm{10673110, "special.10673112.accelerate_summons_one", 1, 1},
    };
    for (const auto form : summoning_forms) {
        suite.add(form.case_name, [form] {
            Scenario s;
            const auto base = official(form.source);
            s.hand({base});
            s.player().pp = static_cast<std::uint8_t>(form.pp);
            s.run({Play, 0});
            CHECK(s.player().board_count == form.copies && s.player().pp == 0);
            CHECK(s.player().shadows == 1);
            for (int slot = 0; slot < s.player().board_count; ++slot) {
                CHECK(s.player().board[slot].card_id == base);
                CHECK(s.player().board[slot].evolution == Evolution::None);
            }
        });
    }

    suite.add("special.10901112.accelerate_damages_only_the_enemy_follower", [] {
        Scenario s;
        s.hand({official(10901110)});
        s.player().pp = 1;
        s.place(kTrainer, 1);
        s.run({Play, 0});
        CHECK(s.player().board_count == 0 && s.player().pp == 0 && s.player().shadows == 1);
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 1);
        CHECK(s.player(1).leader_hp == 20);
    });

    suite.add("special.10952112.crystallize_four_turns_summons_original", [] {
        Scenario s;
        s.hand({official(10952110)});
        s.player().pp = 2;
        s.run({Play, 0});
        for (int remaining = 4; remaining > 0; --remaining) {
            CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(10952112));
            CHECK(s.player().board[0].countdown == remaining);
            s.next_own_turn();
        }
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(10952110));
        CHECK(s.player().board[0].evolution == Evolution::None && s.player().shadows == 1);
    });

    suite.add("special.Crystallize.four_forms_and_last_words", [] {
        constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 4> forms{{
            {10962120,10962122}, {10661110,10661112}, {10662110,10662112}, {10663110,10663112},
        }};
        for (const auto [base_code, form_code] : forms) {
            Scenario s;
            const auto base = official(base_code);
            const auto form = official(form_code);
            s.hand({base});
            s.player().pp = static_cast<std::uint8_t>(find_card(form)->cost);
            s.run({Play, 0});
            CHECK(s.player().board_count == 1 && s.player().board[0].card_id == form &&
                  s.player().board[0].countdown == 3 && printed_card(form) == base);
            CHECK(!is_deck_card(form));
            s.next_own_turn();
            CHECK(s.player().board[0].countdown == 2);
            s.next_own_turn();
            CHECK(s.player().board[0].countdown == 1);
            s.next_own_turn();
            CHECK(s.player().board_count == 1 && s.player().board[0].card_id == base &&
                  s.player().board[0].evolution == Evolution::None && s.player().shadows == 1);
        }
    });

    suite.add("special.TokenCatalog.special_form_tokens_are_not_deck_cards", [] {
        // Crystallize last words return the base follower; the generated card is a token-like
        // non-deck entity even though the printed base card itself is deck eligible.
        constexpr std::array<std::uint32_t, 9> token_ids{
            90061120, 90061110, 90061130, 90061140, 90041110, 90041120, 90041130, 90044330, 90051110,
        };
        for (const auto code : token_ids) {
            const auto id = official(code);
            CHECK(!is_deck_card(id));
            CHECK(find_card(id)->kind != Kind::Spell || find_card(id)->implemented);
        }
    });

    suite.add("special.TokenEffect.Tiger_act_summons_rush_token", [] {
        Scenario s;
        s.hand({official(10961210)});
        s.run({Play, 0});
        s.run({Act, 0});
        const auto& token = s.player().board[0];
        CHECK(token.card_id == official(90061120) && token.atk == 4 && token.hp == 4 && (token.abilities & FLAG_RUSH));
        CHECK(!is_deck_card(token.card_id));
    });

    suite.add("special.Crystallize.MiraculousAlmiraj_act_reduces_countdown", [] {
        Scenario s;
        s.hand({official(10962120)});
        s.player().pp = 1;
        s.run({Play, 0});
        s.next_own_turn();
        CHECK(s.player().board[0].countdown == 2);
        s.run({Act, 0});
        CHECK(s.player().board[0].countdown == 1);
        s.next_own_turn();
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(10962120) && s.player().shadows == 1);
    });

    suite.add("special.CrestCountdown.multiple_crests_tick_and_expire_independently", [] {
        Scenario s;
        s.player().leader_hp = 17;
        for (int i = 0; i < 4; ++i) s.place(kHolyFalcon);
        s.place(kWingedStatue).countdown = 1;
        s.crest(Testimony, 2);
        s.crest(Rapid, 1);
        const auto top = s.player().deck[s.player().deck_idx];
        s.next_own_turn();
        CHECK(s.player().leader_hp == 18 && s.player().board_count == 5 && s.player().shadows == 1);
        CHECK(s.player().board[4].card_id == kRabbit && s.player().board[4].evolution == Evolution::Normal);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == top);
        CHECK(s.player().crest_count == 1 && s.player().crests[0].id == Testimony && s.player().crests[0].countdown == 1);
    });

    suite.add("special.CrestCountdown.expiry_last_word_respects_board_capacity", [] {
        Scenario s;
        for (int i = 0; i < kBoardLimit; ++i) s.place(kTrainer);
        s.crest(Rituel, 1);
        s.rules.crest_clock = CrestClock::End;
        s.run({EndTurn});
        CHECK(s.player().crest_count == 0 && s.player().board_count == kBoardLimit && s.player().shadows == 0);
    });

    suite.add("special.CrestLifecycle.remove_crests_but_preserve_faith", [] {
        Scenario s;
        s.crest(Rituel, 1);
        s.crest(Rapid, 2);
        s.player().faiths[0] = official(10664122);
        s.player().faith_count = 1;
        s.hand({official(10804110)});
        s.run({Play, 0, kNoTarget, 3});
        CHECK(s.player().crest_count == 0 && s.player(1).crest_count == 0);
        CHECK(s.player().board_count == 1 && s.player().shadows == 0 && s.player().faith_count == 1);
    });

    suite.add("special.CrestCountdown.end_clock_expiry_and_shadow_rule", [] {
        Scenario s;
        s.rules.crest_clock = CrestClock::End;
        s.rules.expired_crest_adds_shadow = true;
        s.crest(Rituel, 1);
        s.run({EndTurn});
        CHECK(s.player().crest_count == 0 && s.player().shadows == 1);
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == kRituel &&
              s.player().board[0].evolution == Evolution::Normal);
    });

    suite.add("special.10744112.BurnedKnight_turn_start_and_heal_once_per_turn", [] {
        Scenario s;
        s.hand({official(10744110)});
        s.run({Play, 0});
        s.run({SuperEvolve, 0});
        CHECK(s.player(1).crests[0].id == BurnedKnight);
        s.run({EndTurn});
        CHECK(s.player(1).leader_hp == 18);
        constexpr Effect heal{.operation=Operation::Heal, .target=Target::OwnLeader, .amount=1};
        resolve_effect_steps(s.state, 1, std::array{heal});
        CHECK(s.player(1).leader_hp == 18);
        resolve_effect_steps(s.state, 1, std::array{heal});
        CHECK(s.player(1).leader_hp == 19);
    });

    suite.add("special.10744122.ElderDragon_super_evolve_extends_countdown", [] {
        Scenario s;
        s.hand({official(10744120)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().crests[0].countdown == 2);
        s.run({SuperEvolve, 0});
        CHECK(s.player().crests[0].countdown == 4);
        s.run({EndTurn});
        CHECK(s.player().board_count == 3 && s.player().board[2].card_id == official(90041120));
    });

    suite.add("special.11004112.Rezeal_last_words_rebirth_without_inherited_crest_flag", [] {
        Scenario s;
        s.crest(Rezeal, 0);
        s.hand({official(10021130)});
        s.run({Play, 0});
        CHECK(s.player().board[0].modifiers.effect_flags & kRebirthLastWords);
        s.reject({Attack, 0, kEnemyLeader});
        const auto before = s.player().board[0].creation_order;
        constexpr Effect destroy{.operation=Operation::Destroy, .target=Target::AllEnemyFollowers};
        resolve_effect_steps(s.state, 1, std::array{destroy});
        CHECK(s.player().board_count == 1 && s.player().board[0].creation_order != before &&
              !(s.player().board[0].modifiers.effect_flags & kRebirthLastWords));
    });

    suite.add("special.10964112.Erralde_turn_end_heals_and_damages", [] {
        Scenario s;
        s.player().leader_hp = 15;
        s.place(official(10964110));
        s.run({Evolve, 0});
        CHECK(s.player().crests[0].id == Erralde);
        s.next_own_turn();
        CHECK(s.player().leader_hp == 16 && s.player(1).leader_hp == 19 && s.player().crests[0].countdown == 0);
    });

    suite.add("special.10564122.Kukishiro_routes_draws_by_cost_parity", [] {
        Scenario s;
        s.crest(Kukishiro, 0);
        auto& p = s.player();
        p.deck[p.deck_idx] = kWingedStatue;
        p.deck[p.deck_idx + 1] = kLilium;
        p.deck[p.deck_idx + 2] = kWingedStatue;
        s.next_own_turn();
        CHECK(p.board_count == 1 && s.player(1).board_count == 0);
        s.next_own_turn();
        CHECK(p.board_count == 1 && s.player(1).board_count == 1);
        s.hand({kExias, kExias, kExias, kExias, kExias, kExias, kExias, kExias, kExias});
        s.next_own_turn();
        CHECK(p.board_count == 1 && s.player(1).board_count == 1 && p.shadows == 1 && p.hand_count == 9);
    });

    suite.add("special.10864112.Sisters_super_evolve_grants_cost_discount_and_double_strike", [] {
        Scenario s;
        const auto card = official(10864110);
        s.place(card);
        s.hand({official(10862120)});
        s.place(kTrainer, 1);
        s.run({SuperEvolve, 0});
        CHECK(s.player().crests[0].id == Sisters && s.player().hand_modifiers[0].cost == -3);
        s.run({Attack, 0, 0});
        CHECK(s.player().board[0].abilities & kDoubleStrike);
        s.run({Attack, 0, kEnemyLeader});
        s.reject({Attack, 0, kEnemyLeader});
        s.next_own_turn();
        CHECK(!(s.player().board[0].abilities & kDoubleStrike) && s.player().crests[0].id == Sisters);
    });

    suite.add("special.10704112.Haylem_marks_attacker_then_summons_evolved_copy", [] {
        Scenario s;
        const auto honey = official(10704110);
        s.place(honey).atk = 0;
        s.place(kTrainer, 1);
        s.run({Attack, 0, 0});
        CHECK(s.player(1).board[0].banish_at_end);
        s.run({EndTurn});
        s.reject({Attack, 0, kEnemyLeader});
        s.run({EndTurn});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 0);
        s.place(kProjectedStatue, 1);
        s.place(kTrainer, 1).atk = 20;
        s.state.turn_player = 1;
        s.run({Attack, 1, 0});
        CHECK(s.player().crests[0].id == Haylem);
        s.run({EndTurn});
        CHECK(s.player().crests[0].countdown == 1);
        s.next_own_turn();
        CHECK(s.player().crest_count == 0 && s.player().board[0].card_id == honey &&
              s.player().board[0].evolution == Evolution::Normal);
    });

    suite.add("special.10712312_10713312.Shrink_and_Expand_exchange_on_expiry", [] {
        Scenario shrink;
        shrink.hand({official(10712310)});
        shrink.player().plays_this_turn = 2;
        shrink.run({Play, 0});
        CHECK(shrink.player().crests[0].id == Shrink && shrink.player().crests[0].countdown == 1);
        shrink.next_own_turn();
        const auto expand_spell = official(10713310);
        CHECK(shrink.player().crest_count == 0 &&
              std::find(shrink.player().hand.begin(), shrink.player().hand.begin() + shrink.player().hand_count,
                        expand_spell) != shrink.player().hand.begin() + shrink.player().hand_count);
        Scenario expand;
        expand.hand({official(10713310)});
        expand.player().plays_this_turn = 2;
        expand.run({Play, 0});
        expand.next_own_turn();
        const auto shrink_spell = official(10712310);
        CHECK(expand.player().crest_count == 0 &&
              std::find(expand.player().hand.begin(), expand.player().hand.begin() + expand.player().hand_count,
                        shrink_spell) != expand.player().hand.begin() + expand.player().hand_count);
    });

    suite.add("special.10714112_10714122.Dustdays_and_DeerKing_combo_three_turn_end", [] {
        Scenario s;
        s.player().deck[s.player().deck_idx] = official(10012110);
        s.crest(Dustdays, 3);
        s.crest(DeerKing, 3);
        s.player().plays_this_turn = 3;
        s.run({EndTurn});
        CHECK(s.player().deck_modifiers[s.player().deck_idx].attack == 1 &&
              s.player().deck_modifiers[s.player().deck_idx].defense == 1);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90011310));
    });

    suite.add("special.11012312.Beasts_turn_start_summons_Mouse", [] {
        Scenario s;
        s.crest(Beasts, 2);
        s.next_own_turn();
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(11012110));
    });

    suite.add("special.10724112.Gildaria_damages_enemy_on_follower_entry", [] {
        Scenario s;
        s.crest(Gildaria, 0);
        s.hand({official(10012110)});
        s.run({Play, 0});
        CHECK(s.player(1).leader_hp == 19);
    });

    suite.add("special.11024122_10524122.Almerize_and_Unkei_ordered_outputs", [] {
        Scenario s;
        s.place(official(11024120));
        s.place(official(10524120));
        s.run({SuperEvolve, 0});
        CHECK(s.player().crests[0].id == Almerize && s.player().crests[0].countdown == 4);
        s.next_own_turn();
        CHECK(s.player().hand[0] == official(90021310) && s.player().hand_modifiers[0].cost == -1);
        s.run({SuperEvolve, 1});
        CHECK(s.player().crests[1].id == Unkei);
        s.next_own_turn();
        CHECK(s.player().hand[2] == official(90021320) && s.player().hand_modifiers[2].cost == -1 &&
              s.player().hand[3] == official(90021350));
    });

    suite.add("special.10622312.March_enhance_summons_soldier", [] {
        Scenario s;
        s.hand({official(10622310), official(10621110)});
        s.player().pp = 3;
        s.run({Play, 0});
        CHECK(s.player().crests[0].id == March && s.player().crests[0].countdown == 4);
        s.next_own_turn();
        const int before = s.player().board_count;
        s.run({Play, 0});
        CHECK(s.player().board_count == before + 2 && s.player().board[before + 1].card_id == official(10621110));
    });

    suite.add("special.10544122_10841132.Yube_and_Wadatsumi_marine_attack_and_entry", [] {
        Scenario s;
        const auto orca = official(90041130);
        s.crest(Yube, 0);
        s.crest(Wadatsumi, 0);
        s.hand({orca, orca});
        s.run({Play, 0});
        s.run({Play, 0});
        CHECK(s.player().board[0].atk == 3 && s.player().board[1].hp == 3);
        s.place(kCorrector, 1).atk = 0;
        s.run({Attack, 0, 0});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == orca && s.player().board[0].atk == 4);
        s.run({EndTurn});
        CHECK(s.player().board[0].atk == 3);
    });

    suite.add("special.10844112.Reflection_expiry_returns_discounted_source", [] {
        Scenario s;
        const auto duo = official(10844110);
        for (int i = 0; i < 3; ++i) {
            s.hand({duo});
            s.player().pp = 10;
            s.run({Play, 0});
        }
        const auto target = s.player().board[0].creation_order;
        constexpr Effect destroy{.operation = Operation::Destroy, .target = Target::Self};
        resolve_effect_steps(s.state, 0, std::array{destroy}, target);
        CHECK(s.player().crest_count == 1 && s.player().crests[0].id == Reflection &&
              s.player().crests[0].countdown == 2);
        s.next_own_turn();
        s.next_own_turn();
        const auto returned = official(10844110);
        const auto found = std::find(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count, returned);
        CHECK(found != s.player().hand.begin() + s.player().hand_count &&
              s.player().hand_modifiers[std::distance(s.player().hand.begin(), found)].cost == -2);
    });

    suite.add("special.10554112.Miltio_suppresses_fanfare_and_evolves_played_follower", [] {
        Scenario s;
        const auto soldier = official(10621110);
        s.crest(Miltio, 0);
        s.hand({soldier, official(10642110), kTrainer});
        s.player().pp = 4;
        s.run({Play, 0});
        CHECK(s.player().pp == 2 && s.player().board[0].evolution == Evolution::Normal);
        s.player().pp = 10;
        s.run({Play, 0});
        CHECK(s.player().board[1].evolution == Evolution::Normal && s.player().hand_count == 1 &&
              s.player().hand[0] == kTrainer);
    });

    suite.add("special.11054122.Gremory_adds_shadows_at_turn_end", [] {
        Scenario s;
        s.crest(Gremory, 0);
        s.run({EndTurn});
        CHECK(s.player().shadows == 2);
    });

    suite.add("special.10532112.Insomnia_expiry_damages_all_followers", [] {
        Scenario s;
        s.crest(Insomnia, 2);
        auto& own = s.place(kTrainer);
        auto& enemy = s.place(kTrainer, 1);
        own.hp = own.max_hp = 5;
        enemy.hp = enemy.max_hp = 5;
        s.next_own_turn();
        s.next_own_turn();
        CHECK(s.player().crest_count == 0 && s.player().board_count == 1 && s.player().board[0].hp == 2 &&
              s.player(1).board_count == 1 && s.player(1).board[0].hp == 2);
    });

    suite.add("special.10553312.Okuyoka_draws_then_summons_ward_skeleton_at_matching_cost_threshold", [] {
        Scenario s;
        s.hand({official(10553310), kTrainer, kTrainer, kTrainer, kTrainer});
        s.run({Play, 0});
        s.run({EndTurn});
        CHECK(s.player().board[0].card_id == official(90051110) && s.player().hand_count == 5 &&
              (s.player().board[0].abilities & FLAG_WARD));
    });

    suite.add("special.10954112.IstenDead_marks_reanimated_followers_and_suppresses_enemy_field", [] {
        Scenario s;
        constexpr Effect destroy{.operation = Operation::Destroy, .target = Target::Self};
        const auto first = s.place(kExias).creation_order;
        const auto second = s.place(kLilium).creation_order;
        resolve_effect_steps(s.state, 0, std::array{destroy}, first);
        resolve_effect_steps(s.state, 0, std::array{destroy}, second);
        s.hand({official(10954110)});
        s.run({Play, 0});
        s.run({SuperEvolve, 0});
        CHECK(s.player().crests[0].id == IstenDead);
        s.place(kLilium);
        s.place(kTrainer, 1);
        s.run({EndTurn});
        CHECK(s.player(1).board_count == 0);
    });

    suite.add("special.10534112.Lincle_entry_reduces_max_hp_and_super_evolve_replenishes_deck", [] {
        Scenario s;
        const auto card = official(10534110);
        s.hand({card, card, card});
        s.run({Play, 0});
        s.run({Play, 0});
        s.run({Play, 0});
        CHECK(s.player(1).leader_max_hp == 16 && s.player(1).leader_hp == 16);
        const auto before = s.player().deck_remaining();
        s.run({SuperEvolve, 0});
        CHECK(s.player().deck_remaining() == before + 10);
    });

    suite.add("special.10934112.Sephie_gives_fused_experiment_Rush_once_per_turn", [] {
        Scenario s;
        s.hand({official(10934110), kEvidence, kTrainer});
        s.player().pp = 2;
        s.crest(Sephie, 0);
        s.run({Fuse, 0, kNoTarget, 1});
        CHECK(s.player().pp == 0 && s.player().board_count == 1 &&
              s.player().board[0].card_id == official(10931110) && (s.player().board[0].abilities & FLAG_STORM));
        s.player().pp = 10;
        s.run({Play, 0});
        CHECK(s.player().board_count == 4 && !(s.player().board[2].abilities & FLAG_STORM) &&
              !(s.player().board[3].abilities & FLAG_STORM));
    });

    suite.add("special.11033312_11034112.Noin_acts_damage_heal_and_restore_EP", [] {
        Scenario s;
        s.crest(NoinInterrogation, 0);
        s.crest(Noin, 0);
        s.place(kEarthSigil).earth_count = 1;
        auto& target = s.place(kCorrector, 1);
        target.hp = target.max_hp = 5;
        s.player().leader_hp = 12;
        s.player().ep = 1;
        s.run({Act, 0});
        CHECK(s.player(1).board[0].hp == 2 && s.player().pp == 10);
        s.next_own_turn();
        s.run({Act, 0});
        CHECK(s.player().leader_hp == 14);
        s.next_own_turn();
        s.run({Act, 0});
        CHECK(s.player().ep == 2 && s.player().crest_count == 1 && s.player().crests[0].id == Noin);
    });

    suite.add("special.11033312.ep_recovery_at_cap_still_expires", [] {
        Scenario s;
        s.crest(NoinInterrogation, 0);
        s.player().crests[0].sequence = 2;
        s.player().ep = 2;
        s.place(kEarthSigil).earth_count = 1;
        s.run({Act, 0});
        CHECK(s.player().ep == 2 && s.player().crest_count == 0);
    });

    suite.add("special.10634112_10833112.Shim_and_Tico_buff_attack_and_damage_spells", [] {
        Scenario s;
        const auto hand = official(10631110);
        s.crest(Shim, 0);
        s.crest(Tico, 0);
        s.place(hand);
        s.hand({official(90031310)});
        s.run({Attack, 0, kEnemyLeader});
        s.run({Play, 0});
        CHECK(s.player().board[0].atk == find_card(hand)->atk + 1 &&
              s.player(1).leader_hp == 20 - s.player().board[0].atk - 1);
    });

    suite.add("special.10734112.Lala_timing_modes_summon_evolved_follower", [] {
        for (const auto timing : {LalaSummonTiming::OpponentTurnEnd, LalaSummonTiming::OwnerTurnStart,
                                  LalaSummonTiming::Disabled}) {
            Scenario s;
            s.rules.lala_summon_timing = timing;
            s.crest(Lala, 1);
            s.run({EndTurn});
            CHECK(s.player().board_count == 0);
            s.run({EndTurn});
            CHECK(s.player().crest_count == 0);
            if (timing == LalaSummonTiming::Disabled) CHECK(s.player().board_count == 0);
            else CHECK(s.player().board[0].card_id == official(10734110) &&
                       s.player().board[0].evolution == Evolution::Normal &&
                       s.player().board[0].readied == (timing == LalaSummonTiming::OpponentTurnEnd));
        }
    });

    suite.add("special.10574112.Sloth_runs_each_temporary_penalty", [] {
        Scenario s;
        s.crest(Sloth, 3);
        s.place(kCorrector);
        s.hand({kTrainer});
        int temporary_cost_effects = 0;
        for (int turn = 0; turn < 3; ++turn) {
            s.next_own_turn();
            if (s.player().hand_modifiers[0].temporary_cost_delta == 1) ++temporary_cost_effects;
        }
        CHECK(temporary_cost_effects == 1 && s.player().crest_count == 0 &&
              s.player().leader_hp == 17 && s.player().board[0].hp == 3);
    });

    suite.add("special.10974112_11074122.Cutthroat_auto_evolves_once_and_Stranger_ticks_on_fusion", [] {
        Scenario s;
        s.crest(Cutthroat, 0);
        s.crest(Stranger, 2);
        s.hand({kTrainer, kLilium, official(11073310), kEvidence, official(11072310), kEvidence});
        s.run({Play, 0});
        s.run({Play, 0});
        CHECK(s.player().board[0].evolution == Evolution::Normal && s.player().board[1].evolution == Evolution::None);
        s.run({Fuse, 0, kNoTarget, 1});
        CHECK(s.player().crests[1].countdown == 1);
        s.run({Fuse, 1, kNoTarget, 2});
        CHECK(s.player().crest_count == 1 && s.player().board[3].card_id == official(11074120));
    });

    suite.add("special.FaithCatalog.four_official_faiths_are_created_from_source_cards", [] {
        constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 4> source_and_faith{{
            {10614120,10614122}, {10624120,10624122}, {10634120,10634122}, {10664120,10664122},
        }};
        for (const auto [source_id, faith_id] : source_and_faith) {
            auto deck = kMirrorDeck;
            deck[0] = official(source_id);
            const auto state = new_game(deck, deck, 91);
            for (const auto& side : state.players) {
                CHECK(side.faith_count == 1 && side.faiths[0] == official(faith_id) &&
                      side.faith_values[0] == 0 && side.crest_count == 0);
            }
            const auto faith = official(faith_id);
            CHECK(find_card(faith)->kind == Kind::Faith && !is_deck_card(faith));
        }
    });

    suite.add("special.10614122.Sasanido_spends_faith_and_enables_evolution_damage", [] {
        Scenario s;
        const auto faith = official(10614122);
        s.player().faiths[0] = faith;
        s.player().faith_count = 1;
        s.player().faith_values[0] = 10;
        s.place(kTrainer);
        s.hand({official(10614120)});
        s.run({Play, 0});
        CHECK(s.player().faith_values[0] == 0 && s.player().faith_upgrades[0] == 1 &&
              s.player().hand_count == 1 && s.player().hand[0] == official(90014330));
        s.run({Play, 0, static_cast<std::int8_t>(kOwnBoardTarget)});
        CHECK(s.player().faith_values[0] == 1 && s.player().board[0].evolution == Evolution::Normal &&
              s.player(1).leader_hp == 19);
    });

    suite.add("special.10624122.Idmeta_counts_enhance_and_upgrades_its_board_buff", [] {
        Scenario s;
        const auto faith = official(10624122);
        s.player().faiths[0] = faith;
        s.player().faith_count = 1;
        s.player().faith_values[0] = 5;
        s.place(kTrainer);
        s.hand({official(10624120), official(10522310)});
        s.run({Play, 0});
        CHECK(s.player().hand[1] == official(90024320));
        s.run({Evolve, 1});
        CHECK(s.player().faith_values[0] == 0 && s.player().faith_upgrades[0] == 1);
        s.hand({official(10522310)});
        s.player().pp = 4;
        const int before_attack = s.player().board[0].atk;
        const int before_health = s.player().board[0].hp;
        s.run({Play, 0});
        CHECK(s.player().faith_values[0] == 1 && s.player().board[0].atk == before_attack + 1 &&
              s.player().board[0].hp == before_health + 1);
    });

    suite.add("special.10634122.Kargidensla_tracks_each_crystal_hand_and_adds_Abyss", [] {
        Scenario s;
        const auto faith = official(10634122);
        const auto gem = official(10631110);
        s.player().faiths[0] = faith;
        s.player().faith_count = 1;
        s.hand({official(10634120)});
        s.player().pp = 10;
        s.run({Play, 0});
        CHECK(s.player().faith_values[0] == 2 && s.player().board_count == 3);
        CHECK(s.player().board[1].card_id == gem && (s.player().board[1].abilities & FLAG_RUSH) &&
              s.player().board[2].card_id == gem && (s.player().board[2].abilities & FLAG_RUSH));
        s.run({Evolve, 0});
        CHECK(s.player().faith_values[0] == 2 && s.player().hand[0] == official(90034330));
    });

    suite.add("special.10664122.Liantous_counts_destroyed_amulet_and_spends_ten", [] {
        Scenario s;
        const auto faith = official(10664122);
        const auto liantous = official(10664120);
        s.player().faiths[0] = faith;
        s.player().faith_count = 1;
        s.player().faith_values[0] = 9;
        s.place(kWingedStatue);
        s.place(kTrainer, 1);
        s.place(kLilium, 1);
        s.hand({liantous});
        s.player().pp = 10;
        s.run({Play, 0, kNoTarget, 0, kBothBoardsSelection | 1 | (3 << kBoardLimit)});
        CHECK(s.player().board_count == 2 && s.player(1).board_count == 0 && s.player().faith_values[0] == 10);
        s.next_own_turn();
        CHECK(s.player().faith_values[0] == 0 && s.player().hand[0] == official(90064320));
    });

    suite.add("special.90034330.ArcaneAbyss_matches_official_equal_probability_qa", [] {
        // Use consecutive seeds independent of the resolver's PRNG. The Q&A specifies
        // 1/27 for all attack, 3/27 for all points in any one bin, and 6/27 for one in each.
        constexpr int samples = 270;
        int all_in_one_portion = 0;
        int all_attack = 0;
        int one_each = 0;
        for (int seed = 0; seed < samples; ++seed) {
            Scenario s;
            s.player().faiths[0] = official(10634122);
            s.player().faith_count = 1;
            s.player().faith_values[0] = 3;
            s.player().leader_hp = 10;
            s.hand({official(90034330)});
            s.state.rng_state = static_cast<std::uint64_t>(seed);
            s.run({Play, 0});
            const auto gem = official(10631110);
            CHECK(s.player().board_count == 1 && s.player().board[0].card_id == gem);
            const int attack = s.player().board[0].atk - find_card(gem)->atk;
            const int heal = s.player().leader_hp - 10;
            const int damage = 20 - s.player(1).leader_hp;
            CHECK(attack == s.player().board[0].hp - find_card(gem)->hp);
            CHECK(attack >= 0 && heal >= 0 && damage >= 0 && attack + heal + damage == 3);
            if ((attack == 3 && heal == 0 && damage == 0) ||
                (attack == 0 && heal == 3 && damage == 0) ||
                (attack == 0 && heal == 0 && damage == 3)) ++all_in_one_portion;
            if (attack == 3 && heal == 0 && damage == 0) ++all_attack;
            if (attack == 1 && heal == 1 && damage == 1) ++one_each;
        }
        // Wide binomial acceptance intervals reject grossly incorrect weights without
        // coupling this specification test to particular pseudorandom sequences.
        CHECK(all_attack >= 1 && all_attack <= 23);
        CHECK(all_in_one_portion >= 10 && all_in_one_portion <= 51);
        CHECK(one_each >= 34 && one_each <= 87);
    });
}

} // namespace svwb::wb::spec
