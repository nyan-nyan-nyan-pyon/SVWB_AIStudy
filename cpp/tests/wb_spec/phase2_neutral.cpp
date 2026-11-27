#include "spec.hpp"

namespace svwb::wb::spec {

void phase2_neutral(Suite& suite) {
    suite.add("phase2_neutral.10001130.ward_blocks_face_and_follower_attack", [] {
        Scenario s;
        const auto goliath = official(10001130);
        s.place(goliath, 1);
        s.place(kTrainer);
        CHECK((s.player(1).board[0].abilities & FLAG_WARD) != 0);
        CHECK(!s.legal({Attack, 0, kEnemyLeader}));
        CHECK(s.legal({Attack, 0, 0}));
        s.run({Attack, 0, 0});
        CHECK(s.player(1).board_count == 1);
        CHECK(s.player(1).board[0].card_id == goliath);
        CHECK(s.player(1).board[0].hp == find_card(goliath)->hp - find_card(kTrainer)->atk);
    });

    suite.add("phase2_neutral.10002120.printed_ten_ten_has_no_keyword", [] {
        Scenario s;
        const auto mammoth = official(10002120);
        const auto* printed = find_card(mammoth);
        CHECK(printed && printed->cost == 7 && printed->atk == 10 && printed->hp == 10);
        s.hand({mammoth});
        s.run({Play, 0});
        CHECK(s.player().board_count == 1);
        CHECK(s.player().board[0].card_id == mammoth);
        CHECK(s.player().board[0].atk == 10 && s.player().board[0].hp == 10);
        CHECK((s.player().board[0].abilities & (FLAG_WARD | FLAG_STORM | FLAG_RUSH)) == 0);
        CHECK(!s.legal({Attack, 0, kEnemyLeader}));
    });

    suite.add("phase2_neutral.10502110.evolve_discards_three_then_copies_original_left_three", [] {
        Scenario s;
        const auto goddess = official(10502110);
        const auto winged_statue = kWingedStatue;
        s.place(goddess);
        s.hand({kTrainer, kRapid, kEvidence, winged_statue, kColette, kTeresa});
        s.player().hand_modifiers[3].cost = -1;
        s.run({Evolve, 0, kNoTarget, 0, 0b000111});
        CHECK(s.player().shadows == 3);
        CHECK(s.player().hand_count == 6);
        CHECK(s.player().hand[0] == winged_statue && s.player().hand[3] == winged_statue);
        CHECK(s.player().hand_modifiers[3].cost == -1);
    });

    suite.add("phase2_neutral.10602210.act_copies_opponent_deck_card_without_shadow", [] {
        Scenario s;
        const auto world = official(10602210);
        const auto rival_card = kRapid;
        s.place(world);
        s.hand({kTrainer});
        auto& rival = s.player(1);
        rival.deck_end = static_cast<std::uint8_t>(rival.deck_idx + 1);
        rival.deck[rival.deck_idx] = rival_card;
        rival.deck_modifiers[rival.deck_idx].cost = -2;
        s.run({Act, 0, kOwnHandTarget});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == rival_card);
        CHECK(s.player().hand_modifiers[0].cost == -2);
        CHECK(rival.deck_remaining() == 1);
        CHECK(s.player().shadows == 0);
    });

    suite.add("phase2_neutral.10703210.countdown_sequence_and_act_extension", [] {
        Scenario s;
        const auto city = official(10703210);
        s.hand({city, kTrainer, kRapid});
        s.place(kTrainer, 1);
        s.player().leader_hp = 15;
        s.run({Play, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].countdown == 1);
        s.run({Act, 0, kOwnHandTarget});
        CHECK(s.player().board[0].countdown == 2);
        CHECK(s.player().shadows == 1 && s.player().hand_count == 1);
        s.next_own_turn();
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].card_id == kTrainer &&
              s.player(1).board[0].hp == find_card(kTrainer)->hp - 2);
        CHECK(s.player().board[0].sequence == 1);
        s.run({Act, 0, kOwnHandTarget});
        s.next_own_turn();
        CHECK(s.player().leader_hp == 17 && s.player().board[0].sequence == 2);
        s.run({EndTurn});
        CHECK(s.player(1).leader_hp == 18 && s.player().board_count == 0 && s.player().shadows == 3);
    });

    suite.add("phase2_neutral.10802310.copies_enemy_hand_discounts_and_draws", [] {
        Scenario s;
        const auto spell = official(10802310);
        s.hand({spell});
        s.hand({kRapid}, 1);
        s.player(1).hand_modifiers[0].cost = -2;
        const auto draw = s.player().deck[s.player().deck_idx];
        s.run({Play, 0});
        CHECK(s.player().hand_count == 2);
        CHECK(s.player().hand[0] == kRapid && s.player().hand_modifiers[0].cost == -3);
        CHECK(s.player().hand[1] == draw);
        CHECK(s.player(1).hand_count == 1 && s.player(1).hand[0] == kRapid);
        CHECK(s.player(1).hand_modifiers[0].cost == -2);
    });

    suite.add("phase2_neutral.10804120.fanfare_restores_sep_and_enters_with_ward", [] {
        Scenario s;
        const auto olivier = official(10804120);
        s.hand({olivier});
        s.player().sep = 0;
        s.run({Play, 0});
        CHECK(s.player().sep == 2);
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == olivier);
        CHECK((s.player().board[0].abilities & FLAG_WARD) != 0);
        CHECK(!s.legal({Attack, 0, kEnemyLeader}));
    });

    suite.add("phase2_neutral.10902110.buff_selects_two_other_followers", [] {
        Scenario s;
        const auto angel = official(10902110);
        s.hand({angel});
        s.place(kTrainer);
        s.place(kColette);
        s.place(kLilium);
        s.run({Play, 0, kNoTarget, 0, 0b101});
        CHECK(s.player().board_count == 4);
        CHECK(s.player().board[0].atk == find_card(kTrainer)->atk + 3);
        CHECK(s.player().board[0].hp == find_card(kTrainer)->hp + 3);
        CHECK(s.player().board[1].atk == find_card(kColette)->atk);
        CHECK(s.player().board[1].hp == find_card(kColette)->hp);
        CHECK(s.player().board[2].atk == find_card(kLilium)->atk + 3);
        CHECK(s.player().board[2].hp == find_card(kLilium)->hp + 3);
        CHECK(s.player().board[3].card_id == angel);
    });

    suite.add("phase2_neutral.10804110.mode_three_clears_both_players_crests_only", [] {
        Scenario s;
        const auto bahamut = official(10804110);
        s.hand({bahamut});
        s.place(kWingedStatue);
        s.place(kTrainer);
        s.place(kColette, 1);
        s.crest(CrestId::Testimony, 2, 0);
        s.crest(CrestId::Rapid, 3, 1);
        s.run({Play, 0, kNoTarget, 3});
        CHECK(s.player().crest_count == 0 && s.player(1).crest_count == 0);
        CHECK(s.player().pp == 1);
        CHECK(s.player().board_count == 3 && s.player(1).board_count == 1);
        CHECK(s.player().board[0].card_id == kWingedStatue &&
              s.player().board[1].card_id == kTrainer && s.player().board[2].card_id == bahamut);
        CHECK(s.player(1).board[0].card_id == kColette);
        CHECK(s.player().board[0].hp == find_card(kWingedStatue)->hp &&
              s.player().board[1].hp == find_card(kTrainer)->hp &&
              s.player(1).board[0].hp == find_card(kColette)->hp);
    });

    suite.add("phase2_neutral.10804110.mode_one_and_two_only_banish_their_zones", [] {
        const auto bahamut = official(10804110);
        {
            Scenario followers;
            followers.hand({bahamut});
            followers.place(kTrainer);
            followers.place(kColette, 1);
            followers.place(kWingedStatue);
            followers.crest(CrestId::Testimony, 2);
            followers.run({Play, 0, kNoTarget, 1});
            CHECK(followers.player().pp == 1);
            CHECK(followers.player().board_count == 2 && followers.player().board[0].card_id == kWingedStatue &&
                  followers.player().board[1].card_id == bahamut);
            CHECK(followers.player(1).board_count == 0 && followers.player().crest_count == 1);
            CHECK(followers.player().shadows == 0 && followers.player(1).shadows == 0);
        }
        {
            Scenario amulets;
            amulets.hand({bahamut});
            amulets.place(kWingedStatue);
            amulets.place(kProjectedStatue, 1);
            amulets.place(kTrainer);
            amulets.crest(CrestId::Testimony, 2);
            amulets.run({Play, 0, kNoTarget, 2});
            CHECK(amulets.player().pp == 1);
            CHECK(amulets.player().board_count == 2 && amulets.player().board[0].card_id == kTrainer &&
                  amulets.player().board[1].card_id == bahamut);
            CHECK(amulets.player(1).board_count == 0 && amulets.player().crest_count == 1);
            CHECK(amulets.player().shadows == 0 && amulets.player(1).shadows == 0);
        }
    });
}

} // namespace svwb::wb::spec
