#include "spec.hpp"

namespace svwb::wb::spec {

void phase2_witch(Suite& suite) {
    suite.add("phase2_witch.10032120.spellboost_discount", [] {
        Scenario s;
        const auto card = official(10032120);
        s.hand({card, official(10031310)});
        s.run({Play, 1});
        CHECK(s.player().hand[0] == card && s.player().hand_modifiers[0].spellboost == 1);
        CHECK(card_cost(*find_card(card), s.player().hand_modifiers[0]) == find_card(card)->cost - 1);
    });
    suite.add("phase2_witch.10131320.spellboost_scales_damage", [] {
        Scenario s;
        const auto card = official(10131320);
        s.hand({card});
        s.player().hand_modifiers[0].spellboost = 3;
        auto& enemy = s.place(kTrainer, 1);
        enemy.hp = enemy.max_hp = 6;
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 1);
    });
    suite.add("phase2_witch.10531120.random_damage_and_three_boosts", [] {
        Scenario s;
        const auto storm = official(10131320);
        const auto card = official(10531120);
        s.hand({card, storm, storm});
        for (int i = 0; i < 4; ++i) { auto& e = s.place(kTrainer, 1); e.hp = e.max_hp = 6; }
        s.run({Play, 0});
        int total_damage = 0;
        for (int i = 0; i < s.player(1).board_count; ++i)
            total_damage += s.player(1).board[i].max_hp - s.player(1).board[i].hp;
        CHECK(total_damage == 9);
        CHECK(s.player().hand_modifiers[0].spellboost == 3 && s.player().hand_modifiers[1].spellboost == 3);
    });
    suite.add("phase2_witch.10531310.discard_then_draw_two", [] {
        Scenario s;
        const auto card = official(10531310);
        s.hand({card, kTrainer, official(10131320)});
        s.run({Play, 0, static_cast<std::int8_t>(kOwnHandTarget + 1)});
        CHECK(s.player().hand_count == 3);
        CHECK(std::find(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count, kTrainer) ==
              s.player().hand.begin() + s.player().hand_count);
    });
    suite.add("phase2_witch.10532120.fanfare_draw_and_lastwords_boost", [] {
        Scenario s;
        const auto card = official(10532120);
        const auto boost = official(10131320);
        s.hand({card, boost, official(10031310)});
        s.run({Play, 2});
        CHECK(card_cost(*find_card(card), s.player().hand_modifiers[0]) == find_card(card)->cost - 1);
        const auto initial = s.player().hand_count;
        s.run({Play, 0});
        CHECK(s.player().hand_count == initial && s.player().hand[0] == boost);
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == card);
        const auto last_words = official(10032310);
        s.hand({last_words, boost});
        s.player().board[0].hp = 1;
        s.run({Play, 0});
        CHECK(s.player().board_count == 0 &&
              std::any_of(s.player().hand_modifiers.begin(),
                          s.player().hand_modifiers.begin() + s.player().hand_count,
                          [](const CardModifiers& modifier) { return modifier.spellboost == 2; }));
    });
    suite.add("phase2_witch.10533310.transforms_board_from_deck", [] {
        Scenario s;
        const auto target = official(10533310);
        s.hand({target});
        for (auto& id : s.player().deck) id = official(10931110);
        auto& ally = s.place(kTrainer);
        const auto order = ally.creation_order;
        s.run({Play, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].creation_order == order);
        CHECK(s.player().board[0].card_id == official(10931110));
    });
    suite.add("phase2_witch.10534120.fanfare_damage_and_evolve_transform", [] {
        Scenario s;
        const auto card = official(10534120);
        const auto hawk = official(90061130);
        s.hand({card});
        s.place(kTrainer, 1);
        s.player(1).board[0].hp = s.player(1).board[0].max_hp = 11;
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 1);
        s.place(kTrainer);
        s.player().ep = 1;
        s.run({Evolve, 0, static_cast<std::int8_t>(kOwnBoardTarget + 1)});
        CHECK(s.player().board[1].card_id == hawk);
    });
    suite.add("phase2_witch.10633310.modes_and_enhance_all", [] {
        const auto crystal = official(10631110);
        {
            Scenario s;
            s.hand({official(10633310)});
            s.player().pp = s.player().max_pp = static_cast<std::uint8_t>(find_card(official(10633310))->cost);
            s.run({Play, 0, kNoTarget, 1});
            CHECK(s.player().board_count == 1 && s.player().board[0].card_id == crystal);
            CHECK(s.player().board[0].atk == find_card(crystal)->atk + 1);
            CHECK(s.player().board[0].abilities & FLAG_STORM);
        }
        {
            Scenario s;
            s.hand({official(10633310)});
            s.player().pp = s.player().max_pp = 6;
            s.run({Play, 0, kNoTarget, kAllModes}); // This modal Enhance choice resolves all modes.
            CHECK(s.player().board_count == 3);
            CHECK(s.player().board[1].card_id == crystal && s.player().board[2].card_id == crystal);
            CHECK(s.player().board[1].atk == find_card(crystal)->atk + 1 &&
                  s.player().board[2].atk == find_card(crystal)->atk + 1);
        }
    });
    suite.add("phase2_witch.10731310.earth_rite_reduces_cost_and_draws", [] {
        Scenario s;
        const auto card = official(10731310);
        s.hand({card, official(10031110)});
        s.place(official(90031210));
        s.run({Play, 1, kNoTarget, 2}); // Flash Swordsman pays an Earth Rite.
        CHECK(s.player().hand_modifiers[0].cost == -1);
        const auto before = s.player().hand_count;
        s.run({Play, 0});
        CHECK(s.player().hand_count == before + 1);
        CHECK(std::any_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                          [](const Permanent& p) { return p.card_id == kEarthSigil && p.earth_count == 1; }));
    });
    suite.add("phase2_witch.10732110.rush_lastwords_and_super_evolve", [] {
        Scenario s;
        const auto monster = official(10732110);
        s.hand({monster});
        s.place(official(90031210)).earth_count = 2;
        s.player().sep = 1;
        s.run({Play, 0});
        const auto new_monster = [&] {
            for (int i = 0; i < s.player().board_count; ++i)
                if (s.player().board[i].card_id == monster) return i;
            return -1;
        };
        CHECK(new_monster() >= 0 && (s.player().board[new_monster()].abilities & FLAG_RUSH));
        s.run({SuperEvolve, static_cast<std::int8_t>(new_monster())});
        CHECK(s.player().board_count == 3); // The two-Earth rite is paid and summons two copies.
        auto& attacker = s.place(official(90021110), 1);
        attacker.atk = 8;
        attacker.hp = attacker.max_hp = 20;
        s.run({EndTurn}); // The Super Evolution damage protection ends when the opponent's turn starts.
        s.run({Attack, 0, static_cast<std::int8_t>(new_monster())});
        CHECK(std::any_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                          [](const Permanent& p) { return p.card_id == kEarthSigil && p.earth_count == 2; }));
    });
    suite.add("phase2_witch.10732120.fanfare_adds_earth_and_ward", [] {
        Scenario s;
        const auto card = official(10732120);
        s.hand({card});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && (s.player().board[0].abilities & FLAG_WARD));
        CHECK(s.player().board[1].card_id == kEarthSigil && s.player().board[1].earth_count == 2);
    });
    suite.add("phase2_witch.10733310.earth_discount_destroy_and_add_earth", [] {
        Scenario s;
        const auto card = official(10733310);
        s.hand({card, official(10031110)});
        s.place(official(90031210));
        s.run({Play, 1, kNoTarget, 2});
        CHECK(s.player().hand_modifiers[0].cost == -1);
        s.place(kTrainer, 1);
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0);
        CHECK(std::any_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                          [](const Permanent& p) { return p.card_id == kEarthSigil && p.earth_count == 2; }));
    });
    suite.add("phase2_witch.10831310.spellboost_scales_damage", [] {
        Scenario s;
        const auto card = official(10831310);
        s.hand({card});
        s.player().hand_modifiers[0].spellboost = 2;
        auto& enemy = s.place(kTrainer, 1);
        enemy.hp = enemy.max_hp = 5;
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 1);
    });
    suite.add("phase2_witch.10832310.heal_and_spellboost_hand", [] {
        Scenario s;
        const auto card = official(10832310);
        s.hand({card, official(10131320)});
        s.player().leader_hp = 15;
        s.run({Play, 0});
        CHECK(s.player().leader_hp == 17);
        CHECK(s.player().hand_count == 1 && s.player().hand_modifiers[0].spellboost == 2);
    });
    suite.add("phase2_witch.10833310.threshold_draw_heal_and_pp", [] {
        Scenario s;
        const auto card = official(10833310);
        s.hand({card});
        s.player().hand_modifiers[0].spellboost = 5;
        s.player().leader_hp = 15;
        s.player().pp = 5;
        s.player().max_pp = 10;
        const auto before = s.player().hand_count;
        s.run({Play, 0});
        CHECK(s.player().leader_hp == 17 && s.player().pp == 4);
        CHECK(s.player().hand_count == before + 1);
    });
    suite.add("phase2_witch.10834110.spellboost_threshold_tokens", [] {
        Scenario s;
        const auto card = official(10834110);
        s.hand({card});
        s.player().hand_modifiers[0].spellboost = 20;
        s.run({Play, 0});
        CHECK(s.player().hand_count == 2);
        CHECK(std::find(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count,
                        official(90034340)) != s.player().hand.begin() + s.player().hand_count);
        CHECK(std::find(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count,
                        official(90034350)) != s.player().hand.begin() + s.player().hand_count);
    });
    suite.add("phase2_witch.10931110.enter_count_buffs_sixth_and_rush", [] {
        Scenario s;
        const auto card = official(10931110);
        s.hand({card, card, card, card, card, card, official(10032310)});
        s.player().pp = s.player().max_pp = 10;
        for (int i = 0; i < 6; ++i) s.player().hand_modifiers[i].cost = -1;
        for (int i = 0; i < 5; ++i) s.run({Play, 0});
        CHECK(s.player().board_count == kBoardLimit);
        s.run({Play, 1}); // Eruption frees board space after the five recorded entries.
        CHECK(s.player().board_count == 0);
        s.run({Play, 0});
        const auto& sixth = s.player().board[0];
        CHECK(sixth.card_id == card && sixth.atk == find_card(card)->atk + 3);
        CHECK(sixth.max_hp == find_card(card)->hp + 3 && (sixth.abilities & FLAG_RUSH));
    });
    suite.add("phase2_witch.10931120.fanfare_damage_and_evolve_boost", [] {
        Scenario s;
        const auto card = official(10931120);
        const auto boosted = official(10131320);
        s.hand({card, boosted});
        s.place(kTrainer, 1);
        s.player(1).board[0].hp = s.player(1).board[0].max_hp = 8;
        s.player(1).leader_hp = 15;
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 1);
        CHECK(s.player(1).leader_hp == 11);
        s.player().ep = 1;
        s.run({Evolve, 0, kOwnHandTarget}); // The played source left the boosted spell at hand slot 0.
        CHECK(s.player().hand_modifiers[0].spellboost == 4);
    });
    suite.add("phase2_witch.10932110.fanfare_enhance_and_evolve_bane", [] {
        const auto experiment = official(10931110);
        {
            Scenario s;
            s.hand({official(10932110)});
            const auto base_cost = static_cast<std::uint8_t>(find_card(official(10932110))->cost);
            s.player().pp = s.player().max_pp = base_cost;
            s.run({Play, 0});
            CHECK(s.player().board_count == 3);
            CHECK(s.player().board[1].card_id == experiment && s.player().board[2].card_id == experiment);
        }
        {
            Scenario s;
            s.hand({official(10932110)});
            s.player().pp = s.player().max_pp = 8;
            s.run({Play, 0});
            CHECK(s.player().board_count == 4);
            CHECK(s.player().board[1].card_id == experiment && s.player().board[3].card_id == experiment);
            CHECK((s.player().board[1].abilities & FLAG_WARD) && (s.player().board[3].abilities & FLAG_WARD));
        }
    });
    suite.add("phase2_witch.10932120.shuffle_hand_and_draw_same_count", [] {
        Scenario s;
        s.hand({official(10932120), kTrainer, official(10131320)});
        s.run({Play, 0});
        CHECK(s.player().hand_count == 2);
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(10932120));
    });
    suite.add("phase2_witch.10934120.vanilla_keywords_and_spellboost_discount", [] {
        Scenario s;
        const auto card = official(10934120);
        s.hand({card, official(10031310)});
        const auto printed = find_card(card)->abilities;
        s.run({Play, 1});
        CHECK(card_cost(*find_card(card), s.player().hand_modifiers[0]) == find_card(card)->cost - 1);
        s.player().pp = 10; // Isolate the discounted 9-cost play after the 2-cost spell.
        s.run({Play, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == card);
        CHECK((s.player().board[0].abilities & printed) == printed);
    });
    suite.add("phase2_witch.11032110.fanfare_proof_and_act_trigger", [] {
        Scenario s;
        const auto card = official(11032110);
        const auto proof = official(90001210);
        s.hand({card, card});
        s.player().leader_hp = 16;
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == proof);
        s.run({Act, 1});
        CHECK(s.player().leader_hp == 17);
        const auto card_it = std::find(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count, card);
        CHECK(card_it != s.player().hand.begin() + s.player().hand_count);
        const auto card_slot = static_cast<int>(card_it - s.player().hand.begin());
        CHECK(card_cost(*find_card(card), s.player().hand_modifiers[card_slot]) == find_card(card)->cost - 1);
    });
}

} // namespace svwb::wb::spec
