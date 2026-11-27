#include "spec.hpp"

namespace svwb::wb::spec {

void phase3_world(Suite& suite) {
    suite.add("phase3_world.10503310.base_draws_two_and_destroys_unique_strongest", [] {
        Scenario s;
        const auto world = official(10503310);
        auto& owner = s.player();
        owner.pp = 5;
        owner.deck_end = static_cast<std::uint8_t>(owner.deck_idx + 3);
        owner.deck[owner.deck_idx] = official(10001130);
        owner.deck[owner.deck_idx + 1] = official(10001120);
        owner.deck[owner.deck_idx + 2] = kRapid;
        s.hand({world});
        s.place(kTrainer, 1).atk = 7;
        s.place(kColette, 1).atk = 2;

        s.run({Play, 0});

        CHECK(owner.hand_count == 2);
        CHECK(owner.hand[0] == official(10001130) && owner.hand[1] == official(10001120));
        CHECK(owner.deck_remaining() == 1);
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].card_id == kColette);
        CHECK(s.player(1).shadows == 1);
        CHECK(s.player(1).leader_hp == 20);
    });

    suite.add("phase3_world.10503310.enhance_draws_two_then_damages_all_enemy_followers_and_leader", [] {
        Scenario s;
        const auto world = official(10503310);
        auto& owner = s.player();
        owner.deck_end = static_cast<std::uint8_t>(owner.deck_idx + 3);
        owner.deck[owner.deck_idx] = official(10001130);
        owner.deck[owner.deck_idx + 1] = official(10001120);
        owner.deck[owner.deck_idx + 2] = kRapid;
        s.hand({world});
        s.place(kTrainer, 1);
        s.place(kColette, 1);
        s.player(1).leader_hp = 10;

        s.run({Play, 0});

        CHECK(owner.hand_count == 2);
        CHECK(owner.hand[0] == official(10001130) && owner.hand[1] == official(10001120));
        CHECK(owner.deck_remaining() == 1);
        CHECK(s.player(1).leader_hp == 6);
        CHECK(s.player(1).board_count == 0);
        CHECK(s.player(1).shadows == 2);
    });

    suite.add("phase3_world.10503310.draws_two_with_zero_through_three_cards_remaining", [] {
        const auto world = official(10503310);
        for (const std::uint8_t pp : {std::uint8_t{5}, std::uint8_t{10}}) {
            for (int remaining = 0; remaining <= 3; ++remaining) {
                Scenario s;
                auto& owner = s.player();
                owner.pp = pp;
                owner.deck_end = static_cast<std::uint8_t>(owner.deck_idx + remaining);
                if (remaining > 0) owner.deck[owner.deck_idx] = official(10001130);
                if (remaining > 1) owner.deck[owner.deck_idx + 1] = official(10001120);
                if (remaining > 2) owner.deck[owner.deck_idx + 2] = kRapid;
                s.place(kTrainer, 1);
                s.hand({world});

                s.run({Play, 0});

                const auto expected_draws = std::min(remaining, 2);
                const bool deck_out = remaining < 2;
                CHECK(owner.hand_count == expected_draws);
                if (expected_draws > 0) CHECK(owner.hand[0] == official(10001130));
                if (expected_draws > 1) CHECK(owner.hand[1] == official(10001120));
                CHECK(owner.deck_remaining() == remaining - expected_draws);
                CHECK(s.state.outcome == (deck_out ? Outcome::SecondWins : Outcome::Ongoing));
                CHECK(s.state.end_reason == (deck_out ? EndReason::EmptyDeck : EndReason::None));
                CHECK(s.player(1).board_count == (deck_out ? 1 : 0));
                CHECK(s.player(1).leader_hp == (!deck_out && pp == 10 ? 16 : 20));
            }
        }
    });

    suite.add("phase3_world.10503310.enhance_deck_out_precedes_lethal_damage", [] {
        Scenario s;
        auto& owner = s.player();
        owner.deck_end = static_cast<std::uint8_t>(owner.deck_idx + 1);
        owner.deck[owner.deck_idx] = official(10001130);
        s.player(1).leader_hp = 4;
        s.hand({official(10503310)});

        s.run({Play, 0});

        CHECK(s.state.outcome == Outcome::SecondWins);
        CHECK(s.state.end_reason == EndReason::EmptyDeck);
        CHECK(owner.hand_count == 1 && owner.hand[0] == official(10001130));
        CHECK(owner.deck_remaining() == 0);
        CHECK(s.player(1).leader_hp == 4);
    });

    suite.add("phase3_world.10503310.spell_cost_and_controller_legality", [] {
        const auto world = official(10503310);
        {
            Scenario too_expensive;
            too_expensive.player().pp = 4;
            too_expensive.hand({world});
            too_expensive.reject({Play, 0});
        }
        {
            Scenario opponent_turn;
            opponent_turn.hand({world});
            opponent_turn.state.turn_player = 1;
            opponent_turn.reject({Play, 0});
        }
        {
            Scenario base;
            base.player().pp = 5;
            base.hand({world});
            CHECK(base.legal({Play, 0}));
        }
        {
            Scenario enhance;
            enhance.player().pp = 10;
            enhance.hand({world});
            CHECK(enhance.legal({Play, 0}));
        }
    });
}

} // namespace svwb::wb::spec
