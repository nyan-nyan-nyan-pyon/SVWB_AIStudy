#include "spec.hpp"

namespace svwb::wb::spec {

void phase3_points(Suite& suite) {
    suite.add("phase3_points.10801120.ep_recovery_respects_two_point_cap_and_repeats", [] {
        const auto reina = official(10801120);
        for (std::uint8_t initial_ep = 0; initial_ep <= 2; ++initial_ep) {
            Scenario s;
            s.player().ep = initial_ep;
            s.hand({reina});
            s.run({Play, 0});
            CHECK(s.player().ep == std::min<std::uint8_t>(2, initial_ep + 1));
        }

        Scenario repeated;
        repeated.player().ep = 0;
        repeated.hand({reina, reina});
        repeated.run({Play, 0});
        CHECK(repeated.player().ep == 1);
        repeated.run({Play, 0});
        CHECK(repeated.player().ep == 2);

        Scenario already_full;
        already_full.player().ep = 2;
        already_full.hand({reina, reina});
        already_full.run({Play, 0});
        already_full.run({Play, 0});
        CHECK(already_full.player().ep == 2);
    });

    suite.add("phase3_points.10804120.sep_recovery_respects_two_point_cap", [] {
        const auto card = official(10804120);
        for (std::uint8_t initial_sep = 0; initial_sep <= 2; ++initial_sep) {
            Scenario s;
            s.player().ep = 1;
            s.player().sep = initial_sep;
            s.hand({card});
            s.run({Play, 0});
            CHECK(s.player().sep == 2);
            CHECK(s.player().ep == 1);
        }
        Scenario repeated;
        repeated.player().sep = 0;
        repeated.hand({card, card});
        repeated.run({Play, 0});
        CHECK(repeated.player().sep == 2);
        repeated.next_own_turn();
        const auto second = std::ranges::find(repeated.player().hand.begin(),
            repeated.player().hand.begin() + repeated.player().hand_count, card);
        CHECK(second != repeated.player().hand.begin() + repeated.player().hand_count);
        repeated.run({Play, static_cast<std::int8_t>(second - repeated.player().hand.begin())});
        CHECK(repeated.player().sep == 2);
    });

    suite.add("phase3_points.10653310.mode_one_ep_recovery_respects_cap", [] {
        const auto card = official(10653310);
        for (std::uint8_t initial_ep = 1; initial_ep <= 2; ++initial_ep) {
            Scenario s;
            s.player().ep = initial_ep;
            s.hand({card});
            s.run({Play, 0, kNoTarget, 1});
            CHECK(s.player().ep == 2);
        }
    });

    suite.add("phase3_points.10873310.artifact_gated_ep_recovery_respects_cap", [] {
        const auto card = official(10873310);
        for (std::uint8_t initial_ep = 1; initial_ep <= 2; ++initial_ep) {
            Scenario s;
            s.player().ep = initial_ep;
            s.player().artifact_types_entered = 3;
            s.hand({card});
            s.place(kTrainer, 1);
            s.run({Play, 0, 0});
            CHECK(s.player().ep == 2);
        }
        Scenario below;
        below.player().ep = 1;
        below.player().artifact_types_entered = 2;
        below.hand({card});
        below.place(kTrainer, 1);
        below.run({Play, 0, 0});
        CHECK(below.player().ep == 1 && below.player(1).board_count == 0);
    });

    suite.add("phase3_points.10854110.mode_two_ep_recovery_respects_cap", [] {
        const auto card = official(10854110);
        for (std::uint8_t initial_ep = 1; initial_ep <= 2; ++initial_ep) {
            Scenario s;
            s.player().ep = initial_ep;
            s.hand({card});
            s.run({Play, 0, kNoTarget, 2});
            CHECK(s.player().ep == 2);
        }
    });

    suite.add("phase3_points.manual_evolve_spends_ep_and_super_evolve_spends_sep", [] {
        const auto reina = official(10801120);
        {
            Scenario normal;
            normal.player().ep = 2;
            normal.player().sep = 2;
            normal.hand({reina});
            normal.run({Play, 0});
            normal.run({Evolve, 0});
            CHECK(normal.player().ep == 1 && normal.player().sep == 2);
            CHECK(normal.player().board[0].evolution == Evolution::Normal);
        }
        {
            Scenario super;
            super.player().ep = 2;
            super.player().sep = 2;
            super.hand({reina});
            super.run({Play, 0});
            super.run({SuperEvolve, 0});
            CHECK(super.player().ep == 2 && super.player().sep == 1);
            CHECK(super.player().board[0].evolution == Evolution::Super);
        }
    });

    suite.add("phase3_points.90014330.auto_evolution_spends_no_evolution_point", [] {
        Scenario s;
        s.player().ep = 0;
        s.player().sep = 0;
        s.place(official(90021110));
        s.reject({Evolve, 0});
        s.reject({SuperEvolve, 0});
        s.hand({official(90014330)});
        s.run({Play, 0, kOwnBoardTarget});
        CHECK(s.player().board[0].evolution == Evolution::Normal);
        CHECK(s.player().ep == 0 && s.player().sep == 0);
    });
}

} // namespace svwb::wb::spec
