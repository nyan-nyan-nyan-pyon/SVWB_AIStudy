#include "spec.hpp"

namespace svwb::wb::spec {

void phase3_necromancy(Suite& suite) {
    suite.add("phase3_necromancy.10753310.spell_shadow_not_available_to_inline_necromancy", [] {
        const auto card = official(10753310);
        for (std::uint16_t initial_shadows = 4; initial_shadows <= 7; ++initial_shadows) {
            Scenario s;
            s.player().shadows = initial_shadows;
            s.player(1).leader_hp = 20;
            auto& enemy = s.place(official(90021110), 1);
            enemy.hp = enemy.max_hp = 8;
            s.hand({card});

            s.run({Play, 0});

            CHECK(s.player(1).board_count == 1);
            CHECK(s.player(1).board[0].hp == 2);
            if (initial_shadows < 6) {
                CHECK(s.player().shadows == initial_shadows + 1);
                CHECK(s.player(1).leader_hp == 20);
            } else {
                CHECK(s.player().shadows == initial_shadows - 5);
                CHECK(s.player(1).leader_hp == 18);
            }
        }
    });

    suite.add("phase3_necromancy.10753310.enemy_death_does_not_fund_caster", [] {
        for (const int initial : {5, 6}) {
            Scenario s;
            s.player().shadows = static_cast<std::uint16_t>(initial);
            s.place(official(90021110), 1).hp = 6;
            s.hand({official(10753310)});
            s.run({Play, 0});
            CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
            CHECK(s.player().shadows == (initial == 5 ? 6 : 1));
            CHECK(s.player(1).leader_hp == (initial == 5 ? 20 : 18));
        }
    });

    suite.add("phase3_necromancy.10651310.spell_draws_before_necromancy_boundary", [] {
        for (const int initial : {3, 4}) {
            Scenario s;
            s.player().shadows = static_cast<std::uint16_t>(initial);
            s.player().leader_hp = 10;
            const auto cursor = s.player().deck_idx;
            s.hand({official(10651310)});
            s.run({Play, 0});
            CHECK(s.player().deck_idx == cursor + 2 && s.player().hand_count == 2);
            CHECK(s.player().shadows == (initial == 3 ? 4 : 1));
            CHECK(s.player().leader_hp == (initial == 3 ? 10 : 12));
        }
    });

    suite.add("phase3_necromancy.11052120.attack_spends_the_full_live_threshold", [] {
        for (const int initial : {3, 4}) {
            Scenario s;
            s.player().shadows = static_cast<std::uint16_t>(initial);
            s.place(official(11052120));
            auto& defender = s.place(official(90021110), 1);
            defender.hp = defender.max_hp = 9;
            s.run({Attack, 0, kEnemyLeader});
            CHECK(s.player().shadows == (initial == 3 ? 3 : 0));
            CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == (initial == 3 ? 9 : 5));
        }
    });
}

} // namespace svwb::wb::spec
