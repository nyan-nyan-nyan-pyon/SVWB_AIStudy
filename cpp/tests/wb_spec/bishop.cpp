#include "spec.hpp"

namespace svwb::wb::spec {

void bishop(Suite& suite) {
    suite.add("bishop.10661210.last_word_draw_and_destroy", [] {
        Scenario s;
        const auto water = official(10661210);
        const auto enemy = s.place(kTrainer, 1);
        static_cast<void>(enemy);
        s.hand({water});
        s.player().deck[s.player().deck_idx] = kRapid;
        s.player().deck[s.player().deck_idx + 1] = kTrainer;
        s.run({Play, 0});
        CHECK(s.player().board[0].countdown == 3);
        s.player().board[0].countdown = 1;
        s.next_own_turn();
        CHECK(s.player().board_count == 0 && s.player().hand_count == 2);
        CHECK(s.player().hand[0] == kRapid && s.player().hand[1] == kTrainer);
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
    });

    suite.add("bishop.10662210.last_word_aoe_draw_and_heal", [] {
        Scenario s;
        const auto scripture = official(10662210);
        s.place(kTrainer, 1);
        auto& target = s.place(kColette, 1);
        target.hp = target.max_hp = 2;
        s.player(1).board[0].hp = 2;
        s.player(1).board[1].hp = 2;
        s.player().leader_hp = 15;
        s.player().deck[s.player().deck_idx] = kRapid;
        s.player().deck[s.player().deck_idx + 1] = kTrainer;
        s.player().deck[s.player().deck_idx + 2] = kEvidence;
        s.hand({scripture});
        s.run({Play, 0});
        s.player().board[0].countdown = 1;
        s.next_own_turn();
        CHECK(s.player().board_count == 0 && s.player().hand_count == 3);
        CHECK(s.player().hand[0] == kRapid && s.player().hand[1] == kTrainer && s.player().hand[2] == kEvidence);
        CHECK(s.player().leader_hp == 17 && s.player(1).board_count == 0);
        CHECK(s.player(1).shadows == 2);
    });

    suite.add("bishop.10762210.act_and_enhance_damage", [] {
        Scenario act;
        const auto clock = official(10762210);
        act.player().pp = 2;
        act.place(kTrainer, 1).hp = 1;
        auto& survivor = act.place(kColette, 1);
        survivor.hp = survivor.max_hp = 2;
        act.hand({clock});
        act.run({Play, 0});
        act.run({Act, 0});
        CHECK(act.player().board_count == 0 && act.player().shadows == 1);
        CHECK(act.player(1).board_count == 1);
        CHECK(act.player(1).board[0].card_id == kColette && act.player(1).board[0].hp == 1);
        CHECK(act.player(1).shadows == 1);

        Scenario enhanced;
        enhanced.place(kTrainer, 1).hp = 1;
        auto& enhanced_survivor = enhanced.place(kColette, 1);
        enhanced_survivor.hp = enhanced_survivor.max_hp = 2;
        enhanced.hand({clock});
        enhanced.run({Play, 0});
        CHECK(enhanced.player().pp == 6 && enhanced.player().board_count == 1);
        CHECK(enhanced.player().board[0].card_id == clock && enhanced.player().shadows == 0);
        CHECK(enhanced.player(1).board_count == 1 && enhanced.player(1).board[0].hp == 1);
        CHECK(enhanced.player(1).shadows == 1);
    });

    suite.add("bishop.10863210.end_turn_draw_and_evolution_heal", [] {
        for (const auto evolution : {Evolution::None, Evolution::Normal, Evolution::Super}) {
            Scenario s;
            const auto classmate = official(10863210);
            s.player().leader_hp = 15;
            auto& amulet = s.place(classmate);
            amulet.countdown = 1;
            auto& ally = s.place(kTrainer);
            ally.evolution = evolution;
            s.player().deck[s.player().deck_idx] = kRapid;
            s.player().deck[s.player().deck_idx + 1] = kTrainer;
            s.next_own_turn();
            const int expected_hp = evolution == Evolution::None ? 15 : (evolution == Evolution::Normal ? 16 : 17);
            CHECK(s.player().leader_hp == expected_hp && s.player().hand_count == 2);
            CHECK(s.player().hand[0] == kRapid && s.player().hand[1] == kTrainer);
            CHECK(s.player().board_count == 1 && s.player().board[0].card_id == kTrainer);
        }
    });

    suite.add("bishop.10563210.fanfare_target_damage", [] {
        Scenario s;
        s.place(kTrainer, 1).hp = 5;
        s.hand({official(10563210)});
        s.run({Play, 0, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(10563210));
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
    });

    suite.add("bishop.10763210.fanfare_and_act_damage", [] {
        Scenario s;
        const auto trident = official(10763210);
        auto& first_target = s.place(kTrainer, 1);
        first_target.hp = first_target.max_hp = 4;
        s.hand({trident});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
        auto& target = s.place(kColette, 1);
        target.hp = target.max_hp = 2;
        s.run({Act, 0, 0});
        CHECK(s.player().board_count == 0 && s.player().shadows == 1);
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 2);
    });

    suite.add("bishop.10061210.act_accelerates_last_word", [] {
        Scenario s;
        const auto statue = official(10061210);
        s.hand({statue});
        s.run({Play, 0});
        CHECK(s.player().board[0].countdown == 2);
        s.run({Act, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(90061130));
        CHECK(s.player().board[0].atk == 4 && s.player().board[0].hp == 4);
        CHECK(s.player().shadows == 1 && s.player().pp == 4);
    });

    suite.add("bishop.10963210.global_destroy_and_act_draw_heal", [] {
        Scenario s;
        const auto icon = official(10963210);
        s.player().leader_hp = 15;
        s.place(kTrainer);
        s.place(kColette, 1);
        s.hand({icon});
        s.player().deck[s.player().deck_idx] = kRapid;
        s.run({Play, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == icon);
        CHECK(s.player(1).board_count == 0 && s.player().shadows == 1);
        s.run({Act, 0});
        CHECK(s.player().board_count == 0 && s.player().hand_count == 1 && s.player().hand[0] == kRapid);
        CHECK(s.player().leader_hp == 16 && s.player().shadows == 2);
    });

    suite.add("bishop.10761210.amulet_replacement_draw", [] {
        Scenario s;
        const auto charm = official(10761210);
        s.hand({charm, kTrainer});
        s.player().deck[s.player().deck_idx] = kRapid;
        s.run({Play, 0, kOwnHandTarget + 1});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == charm);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] != kTrainer);
    });

    suite.add("bishop.10862310.exile_health_threshold_and_enhance", [] {
        Scenario single;
        auto& eligible = single.place(kTrainer, 1);
        eligible.hp = eligible.max_hp = 3;
        auto& ineligible = single.place(kColette, 1);
        ineligible.hp = ineligible.max_hp = 4;
        single.player().pp = 2;
        single.hand({official(10862310)});
        single.run({Play, 0, 0});
        CHECK(single.player(1).board_count == 1 && single.player(1).board[0].card_id == kColette);
        CHECK(single.player(1).shadows == 0);

        Scenario enhanced;
        auto& enhanced_eligible = enhanced.place(kTrainer, 1);
        enhanced_eligible.hp = enhanced_eligible.max_hp = 3;
        auto& enhanced_ineligible = enhanced.place(kColette, 1);
        enhanced_ineligible.hp = enhanced_ineligible.max_hp = 4;
        enhanced.hand({official(10862310)});
        enhanced.player().pp = 5;
        enhanced.run({Play, 0});
        CHECK(enhanced.player(1).board_count == 1 && enhanced.player(1).board[0].card_id == kColette);
    });

    suite.add("bishop.10561310.random_return_then_draw_three", [] {
        Scenario s;
        const auto spell = official(10561310);
        s.hand({spell, kTrainer, kEvidence, kRapid});
        for (int i = 0; i < 5; ++i) s.player().deck[s.player().deck_idx + i] = kColette;
        const auto remaining = s.player().deck_remaining();
        s.run({Play, 0});
        CHECK(s.player().hand_count == 5 && s.player().deck_remaining() == remaining - 2);
    });

    suite.add("bishop.10661310.draw_two_amulets", [] {
        Scenario s;
        const auto spell = official(10661310);
        s.hand({spell});
        std::fill(s.player().deck.begin() + s.player().deck_idx, s.player().deck.begin() + s.player().deck_end,
                  kTrainer);
        s.player().deck[s.player().deck_idx] = official(10062210);
        s.player().deck[s.player().deck_idx + 1] = official(10661210);
        s.run({Play, 0});
        CHECK(s.player().hand_count == 2 && s.player().hand[0] == official(10062210) &&
              s.player().hand[1] == official(10661210));
    });

    suite.add("bishop.11061310.training_buff_and_barrier", [] {
        Scenario s;
        auto& buff_target = s.place(kTrainer);
        const auto old_atk = buff_target.atk;
        const auto old_hp = buff_target.hp;
        s.hand({official(11061310)});
        s.run({Play, 0, kOwnBoardTarget});
        CHECK(s.player().board[0].atk == old_atk + 2 && s.player().board[0].hp == old_hp + 2);
        CHECK(s.player().board[0].abilities & kBarrier);
    });

    suite.add("bishop.11063310.testimony_modes", [] {
        Scenario damage;
        damage.place(kTrainer, 1).hp = 4;
        damage.hand({official(11063310)});
        damage.run({Play, 0, kNoTarget, 1});
        CHECK(damage.player(1).board_count == 0 && damage.player(1).shadows == 1);
        CHECK(damage.player().crest_count == 1);

        Scenario summon;
        summon.hand({official(11063310)});
        summon.run({Play, 0, kNoTarget, 2});
        CHECK(summon.player().board_count == 1 && summon.player().board[0].card_id == official(90061140));
    });

    suite.add("bishop.11062310.rabbit_brood_and_enhance", [] {
        Scenario normal;
        normal.player().pp = 5;
        normal.hand({official(11062310)});
        normal.run({Play, 0});
        CHECK(normal.player().board_count == 2);
        CHECK(normal.player().board[0].card_id == official(90061140));
        CHECK(normal.player().board[1].card_id == official(90061140));

        Scenario enhanced;
        enhanced.player().pp = 8;
        enhanced.hand({official(11062310)});
        enhanced.run({Play, 0});
        CHECK(enhanced.player().board_count == 4);
        CHECK(std::all_of(enhanced.player().board.begin(), enhanced.player().board.begin() + 4,
                          [](const auto& object) { return object.card_id == official(90061140); }));
    });

    suite.add("bishop.10962310.promise_modes_and_large_card_all", [] {
        Scenario hit;
        hit.place(kTrainer, 1).hp = 3;
        hit.player().leader_hp = 15;
        hit.hand({official(10962310)});
        hit.run({Play, 0, kNoTarget, 1});
        CHECK(hit.player(1).board_count == 0 && hit.player().leader_hp == 15);

        Scenario heal;
        heal.player().leader_hp = 15;
        heal.hand({official(10962310)});
        heal.run({Play, 0, kNoTarget, 2});
        CHECK(heal.player().leader_hp == 17);

        Scenario both;
        both.place(official(10964110));
        both.place(kTrainer, 1).hp = 3;
        both.player().leader_hp = 15;
        both.hand({official(10962310)});
        both.run({Play, 0, kNoTarget, kAllModes});
        CHECK(both.player(1).board_count == 0 && both.player().leader_hp == 17);
    });

    suite.add("bishop.10062110.priest_evolution_exile_scope", [] {
        Scenario normal;
        normal.place(official(10062110));
        normal.place(kTrainer, 1);
        auto& normal_target = normal.place(kColette, 1);
        normal_target.hp = 3;
        normal.run({Evolve, 0, 0});
        CHECK(normal.player(1).board_count == 1 && normal.player(1).board[0].card_id == kColette);

        Scenario super_evolved;
        super_evolved.place(official(10062110));
        super_evolved.place(kTrainer, 1);
        auto& super_target = super_evolved.place(kColette, 1);
        super_target.hp = 3;
        super_evolved.run({SuperEvolve, 0});
        CHECK(super_evolved.player(1).board_count == 0);
        CHECK(super_evolved.player(1).shadows == 0);
    });

    suite.add("bishop.10061130.wing_warrior_buffs_on_play_and_evolve", [] {
        Scenario s;
        s.place(kTrainer);
        s.hand({official(10061130)});
        s.run({Play, 0, kOwnBoardTarget});
        CHECK(s.player().board[0].atk == 4 && s.player().board[0].hp == 4);
        s.next_own_turn();
        s.run({Evolve, 1, kOwnBoardTarget});
        CHECK(s.player().board[0].atk == 5 && s.player().board[0].hp == 5);
    });

    suite.add("bishop.10561110.foresight_priest_damage_and_evolve", [] {
        Scenario s;
        s.place(kTrainer, 1).hp = 2;
        s.hand({official(10561110)});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0 && (s.player().board[0].abilities & FLAG_WARD));
        auto& second = s.place(kColette, 1);
        second.hp = second.max_hp = 2;
        s.next_own_turn();
        s.run({Evolve, 0, 0});
        CHECK(s.player(1).board_count == 0 && (s.player().board[0].abilities & FLAG_WARD));
    });

    suite.add("bishop.10762110.holy_planner_three_amulet_gate", [] {
        Scenario below;
        below.place(official(10062210));
        below.place(official(10661210));
        below.place(kTrainer, 1).hp = 5;
        below.hand({official(10762110)});
        below.run({Play, 0});
        CHECK(below.player(1).board_count == 1 && below.player(1).board[0].hp == 5);

        Scenario met;
        met.place(official(10062210));
        met.place(official(10661210));
        met.place(official(10662210));
        met.place(kTrainer, 1).hp = 5;
        met.hand({official(10762110)});
        met.run({Play, 0, 0});
        CHECK(met.player(1).board_count == 0 && met.player(1).shadows == 1);
    });

    suite.add("bishop.11062110.trainer_token_and_evolution_pp", [] {
        Scenario s;
        s.hand({official(11062110)});
        s.run({Play, 0});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == official(90061140));
        s.player().pp = 4;
        s.run({Evolve, 0});
        CHECK(s.player().pp == 5);
    });

    suite.add("bishop.10761110.profit_supporter_amulet_threshold", [] {
        Scenario below;
        below.place(official(10062210));
        below.place(official(10661210));
        below.hand({official(10761110)});
        below.run({Play, 0});
        CHECK(below.player().board_count == 3 && below.player().board[2].card_id == official(10761110));
        CHECK(below.player().board[2].abilities & FLAG_RUSH);

        Scenario met;
        met.place(official(10062210));
        met.place(official(10661210));
        met.place(official(10662210));
        met.hand({official(10761110)});
        met.run({Play, 0});
        CHECK(met.player().board_count == 5 && met.player().board[3].card_id == official(10761110) &&
              met.player().board[4].card_id == official(10761110));
    });

    suite.add("bishop.11062120.winged_performer_summon_and_evolve_buff", [] {
        Scenario s;
        s.place(kTrainer);
        s.hand({official(11062120)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 3 && s.player().board[2].card_id == official(90061110));
        s.run({Evolve, 1});
        CHECK(s.player().board[2].atk == 3 && s.player().board[2].hp == 3);
    });

    suite.add("bishop.10562120.mouse_priestess_draw_damage_and_evolve", [] {
        Scenario s;
        s.place(kTrainer, 1).hp = 1;
        auto& extra_target = s.place(kColette, 1);
        extra_target.hp = extra_target.max_hp = 1;
        s.player().deck[s.player().deck_idx] = kRapid;
        s.hand({official(10562120)});
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 0 && s.player().hand_count == 1);
        s.place(kTrainer, 1).hp = 1;
        s.run({Evolve, 0});
        CHECK(s.player(1).board_count == 0);
    });

    suite.add("bishop.10763110.guardian_amulet_threshold_evolution_barrier", [] {
        Scenario below;
        below.place(official(10062210));
        below.place(official(10661210));
        below.hand({official(10763110)});
        below.run({Play, 0});
        CHECK(below.player().board[2].evolution == Evolution::None);

        Scenario met;
        met.place(official(10062210));
        met.place(official(10661210));
        met.place(official(10662210));
        met.hand({official(10763110)});
        met.run({Play, 0});
        CHECK(met.player().board[3].evolution == Evolution::Normal &&
              (met.player().board[3].abilities & kBarrier) && (met.player().board[3].abilities & FLAG_WARD));
    });

    suite.add("bishop.10863110.colette_evolves_from_ally_and_splashes", [] {
        Scenario s;
        s.place(kTrainer).evolution = Evolution::Normal;
        s.place(kTrainer, 1).hp = 1;
        auto& extra_target = s.place(kColette, 1);
        extra_target.hp = extra_target.max_hp = 1;
        s.hand({official(10863110)});
        s.run({Play, 0});
        CHECK(s.player().board[1].evolution == Evolution::Normal &&
              (s.player().board[1].abilities & FLAG_WARD));
        CHECK(s.player(1).board_count == 0);
    });

    suite.add("bishop.10861130.grant_repeat_destroy_on_evolve", [] {
        Scenario s;
        s.place(kTrainer, 1);
        s.hand({official(10861130)});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
        s.place(kColette, 1);
        s.next_own_turn();
        s.run({Evolve, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 2);
    });

    suite.add("bishop.10864120.rituel_modes_and_crest", [] {
        Scenario damage;
        damage.place(kTrainer, 1).hp = 1;
        auto& second = damage.place(kColette, 1);
        second.hp = second.max_hp = 1;
        damage.hand({official(10864120)});
        damage.run({Play, 0, kNoTarget, 1});
        CHECK(damage.player(1).board_count == 0 && damage.player(1).shadows == 2);

        Scenario crest;
        crest.hand({official(10864120)});
        crest.run({Play, 0, kNoTarget, 2});
        crest.run({Evolve, 0});
        CHECK(crest.player().crest_count == 1);
    });

    suite.add("bishop.10961120.peryton_two_rush_copies", [] {
        Scenario s;
        s.hand({official(10961120)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 3);
        CHECK(s.player().board[1].card_id == official(10961120) && s.player().board[2].card_id == official(10961120));
        CHECK((s.player().board[1].abilities & FLAG_RUSH) && (s.player().board[2].abilities & FLAG_RUSH));
    });

    suite.add("bishop.11064110.rapid_hat_exile_and_rabbit", [] {
        Scenario s;
        s.place(kTrainer, 1);
        s.hand({official(11064110)});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 0);
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == official(90061140));
        s.crest(CrestId::Rapid, 0);
        s.run({SuperEvolve, 0});
        CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::Rapid &&
              s.player().crests[0].countdown == 1);
    });

    suite.add("bishop.10762120.transmission_bird_exile_heal_and_storm", [] {
        Scenario below;
        below.place(kTrainer, 1);
        below.player().leader_hp = 15;
        below.hand({official(10762120)});
        below.run({Play, 0, 0});
        CHECK(below.player(1).board_count == 0 && below.player(1).shadows == 0 && below.player().leader_hp == 15);
        CHECK(below.player().board[0].abilities & FLAG_STORM);

        Scenario met;
        met.place(official(10062210));
        met.place(official(10661210));
        met.place(official(10662210));
        met.place(kTrainer, 1);
        met.player().leader_hp = 15;
        met.hand({official(10762120)});
        met.run({Play, 0, 0});
        CHECK(met.player(1).board_count == 0 && met.player().leader_hp == 18);
    });

    suite.add("bishop.10862110.edith_rebirth_and_super_evolve_destroy", [] {
        Scenario s;
        s.place(official(10862110));
        auto& defender = s.place(kTrainer, 1);
        defender.atk = 9;
        defender.hp = 20;
        s.run({Attack, 0, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(10862110));
        CHECK(s.player().board[0].abilities & FLAG_WARD);

        Scenario super_evolved;
        super_evolved.place(official(10862110));
        super_evolved.place(kTrainer, 1);
        super_evolved.run({SuperEvolve, 0, 0});
        CHECK(super_evolved.player(1).board_count == 0 && super_evolved.player(1).shadows == 1);
    });

    suite.add("bishop.10963110.oath_leader_heal_destroys_and_super_heals", [] {
        Scenario play;
        play.place(kTrainer, 1);
        play.place(kEvidence, 1);
        play.player().leader_hp = 15;
        play.hand({official(10963110)});
        play.run({Play, 0});
        CHECK(play.player().leader_hp == 17 && play.player(1).board_count == 1);
        CHECK(play.player(1).shadows == 1 && (play.player().board[0].abilities & FLAG_WARD));

        Scenario super_evolved;
        super_evolved.place(official(10963110));
        super_evolved.place(kTrainer, 1);
        super_evolved.player().leader_hp = 15;
        super_evolved.run({SuperEvolve, 0});
        CHECK(super_evolved.player().leader_hp == 17 && super_evolved.player(1).board_count == 0);
    });

    suite.add("bishop.10861120.teresa_sister_buff_and_storm", [] {
        Scenario s;
        s.hand({official(10861120)});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == kHealingSister);
        CHECK(s.player().board[1].atk == 5 && s.player().board[1].hp == 3);
        CHECK(s.player().board[1].abilities & FLAG_STORM);
    });

    suite.add("bishop.11063110.corrector_exile_rabbits_and_super_draw_heal", [] {
        Scenario s;
        s.place(kTrainer, 1);
        s.hand({official(11063110)});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 0);
        CHECK(s.player().board_count == 3 && s.player().board[1].card_id == official(90061140) &&
              s.player().board[2].card_id == official(90061140));
        s.player().leader_hp = 15;
        s.player().deck[s.player().deck_idx] = kRapid;
        s.player().deck[s.player().deck_idx + 1] = kEvidence;
        s.run({SuperEvolve, 0});
        CHECK(s.player().leader_hp == 17 && s.player().hand_count == 2);
        CHECK(s.player().hand[0] == kRapid && s.player().hand[1] == kEvidence);
    });

    suite.add("bishop.11061120.training_monk_double_attack_growth", [] {
        Scenario s;
        s.hand({official(11061120)});
        s.player().pp = 7;
        s.run({Play, 0});
        CHECK(s.player().board[0].abilities & FLAG_RUSH);
        s.place(kTrainer, 1);
        s.place(official(10061130), 1);
        s.place(official(90021110), 1);
        CHECK(s.legal({Attack, 0, 0}));
        s.run({Attack, 0, 0});
        CHECK(s.player().board[0].atk == 6 && s.player().board[0].hp == 6);
        s.run({Attack, 0, 0});
        CHECK(s.player().board[0].atk == 8 && s.player().board[0].hp == 8);
        CHECK(s.player(1).board_count == 1);
        s.reject({Attack, 0, 0});
    });

    suite.add("bishop.90064210.moonshadow_ring_three_amulet_sweep", [] {
        Scenario s;
        s.place(official(10062210));
        s.place(official(10661210));
        s.place(official(90064210));
        s.place(kTrainer, 1).hp = 3;
        s.player(1).leader_hp = 20;
        s.next_own_turn();
        CHECK(s.player(1).board_count == 0 && s.player(1).leader_hp == 17);
        CHECK(s.player().board_count == 2);
    });

    suite.add("bishop.90064320.abyss_repeats_after_amulet_destroy", [] {
        Scenario s;
        const auto abyss = official(90064320);
        s.place(official(10762210));
        s.player(1).leader_hp = 20;
        s.hand({abyss});
        s.run({Play, 0, kOwnBoardTarget});
        CHECK(s.player().board_count == 0 && s.player().hand_count == 1 && s.player().hand[0] == abyss);
        CHECK(s.player(1).leader_hp == 18);
        CHECK(s.player().shadows == 2);
    });
}

} // namespace svwb::wb::spec

