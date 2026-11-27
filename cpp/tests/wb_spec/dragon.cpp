#include "spec.hpp"

namespace svwb::wb::spec {

void dragon(Suite& suite) {
    suite.add("dragon.10042310.oracle_draw_only_at_ten", [] {
        const auto oracle = official(10042310);
        Scenario reaches_ten;
        auto& p = reaches_ten.player();
        p.max_pp = p.pp = 9;
        const auto expected_draw = p.deck[p.deck_idx];
        reaches_ten.hand({oracle});
        reaches_ten.run({Play, 0});
        CHECK(p.max_pp == 10 && p.pp == 6);
        CHECK(p.hand_count == 1 && p.hand[0] == expected_draw);

        Scenario below_ten;
        auto& q = below_ten.player();
        q.max_pp = q.pp = 8;
        const auto held = official(10644120);
        below_ten.hand({oracle, held});
        const auto no_draw = q.deck[q.deck_idx];
        below_ten.run({Play, 0});
        CHECK(q.max_pp == 9 && q.pp == 5);
        CHECK(q.hand_count == 1 && q.hand[0] == held && q.deck[q.deck_idx] == no_draw);
    });

    suite.add("dragon.11041310.divine_rain_overflow_branch", [] {
        const auto rain = official(11041310);
        Scenario ordinary;
        auto& p = ordinary.player();
        p.max_pp = p.pp = 6;
        p.leader_hp = 12;
        const auto draw = p.deck[p.deck_idx];
        ordinary.hand({rain});
        ordinary.run({Play, 0});
        CHECK(p.max_pp == 6 && p.pp == 3 && p.leader_hp == 13);
        CHECK(p.hand_count == 1 && p.hand[0] == draw);

        Scenario overflow;
        auto& q = overflow.player();
        q.max_pp = q.pp = 7;
        q.leader_hp = 12;
        const auto overflow_draw = q.deck[q.deck_idx];
        overflow.hand({rain});
        overflow.run({Play, 0});
        CHECK(q.max_pp == 8 && q.pp == 4 && q.leader_hp == 13);
        CHECK(q.hand_count == 1 && q.hand[0] == overflow_draw);
    });

    suite.add("dragon.10042110.dragon_warrior_super_evolve_clears_all", [] {
        const auto warrior = official(10042110);
        Scenario normal;
        normal.hand({warrior});
        normal.place(kTrainer, 1);
        auto& survivor = normal.place(official(10042120), 1);
        survivor.hp = survivor.max_hp = 9;
        normal.run({Play, 0});
        normal.run({Evolve, 0, 0});
        CHECK(normal.player(1).board_count == 1);
        CHECK(normal.player(1).board[0].card_id == official(10042120));
        CHECK(normal.player(1).board[0].hp == 9);

        Scenario super;
        super.hand({warrior});
        super.place(kTrainer, 1);
        super.place(kLilium, 1);
        super.run({Play, 0});
        super.run({SuperEvolve, 0});
        CHECK(super.player(1).board_count == 0);
        CHECK(super.player(1).shadows == 2);
        CHECK(super.player().board[0].evolution == Evolution::Super);
    });

    suite.add("dragon.10041310.dragonnewt_crush_awaken_threshold", [] {
        const auto crush = official(10041310);
        Scenario asleep;
        asleep.player().max_pp = 6;
        auto& asleep_target = asleep.place(kTrainer, 1);
        asleep_target.hp = asleep_target.max_hp = 5;
        asleep.hand({crush});
        asleep.run({Play, 0, 0});
        CHECK(asleep.player(1).board_count == 1 && asleep.player(1).board[0].hp == 3);

        Scenario awake;
        awake.player().max_pp = 7;
        auto& awake_target = awake.place(kTrainer, 1);
        awake_target.hp = awake_target.max_hp = 5;
        awake.hand({crush});
        awake.run({Play, 0, 0});
        CHECK(awake.player(1).board_count == 1 && awake.player(1).board[0].hp == 1);
    });

    suite.add("dragon.10843310.foxfire_recycles_and_draws_when_awake", [] {
        const auto foxfire = official(10843310);
        Scenario asleep;
        asleep.player().max_pp = 6;
        const int asleep_deck = asleep.player().deck_remaining();
        auto& asleep_target = asleep.place(kTrainer, 1);
        asleep_target.hp = asleep_target.max_hp = 3;
        asleep.hand({foxfire});
        asleep.run({Play, 0, 0});
        CHECK(asleep.player(1).board_count == 1 && asleep.player(1).board[0].hp == 2);
        CHECK(asleep.player().deck_remaining() == asleep_deck + 1);

        Scenario awake;
        awake.player().max_pp = 7;
        awake.player().leader_hp = 20;
        const int awake_deck = awake.player().deck_remaining();
        awake.hand({foxfire});
        awake.run({Play, 0, kEnemyLeader});
        CHECK(awake.player(1).leader_hp == 19 && awake.player().hand_count == 1);
        CHECK(awake.player().deck_remaining() == awake_deck);
    });

    suite.add("dragon.10543310.tideflower_repeats_random_damage_and_awakens", [] {
        const auto tideflower = official(10543310);
        Scenario ordinary;
        ordinary.player().max_pp = 6;
        auto& ordinary_target = ordinary.place(kTrainer, 1);
        ordinary_target.hp = ordinary_target.max_hp = 5;
        ordinary.hand({tideflower});
        ordinary.run({Play, 0});
        CHECK(ordinary.player(1).board_count == 1 && ordinary.player(1).board[0].hp == 1);
        CHECK(ordinary.player(1).leader_hp == 20);

        Scenario awake;
        awake.player().max_pp = 7;
        auto& awake_target = awake.place(kTrainer, 1);
        awake_target.hp = awake_target.max_hp = 5;
        awake.hand({tideflower});
        awake.run({Play, 0});
        CHECK(awake.player(1).board_count == 1 && awake.player(1).board[0].hp == 1);
        CHECK(awake.player(1).leader_hp == 18);
    });

    suite.add("dragon.10641120.matured_sea_fish_enhance_summons_pair", [] {
        const auto fish = official(10641120);
        Scenario s;
        s.hand({fish});
        s.player().pp = 6;
        s.run({Play, 0});
        CHECK(s.player().pp == 0 && s.player().board_count == 3);
        CHECK(s.player().board[0].card_id == fish && s.player().board[1].card_id == fish &&
              s.player().board[2].card_id == fish);
    });

    suite.add("dragon.10741110.dragonian_herald_enhance_pair_has_rush", [] {
        const auto herald = official(10741110);
        Scenario s;
        s.hand({herald});
        s.player().pp = 4;
        s.run({Play, 0});
        CHECK(s.player().pp == 0 && s.player().board_count == 3);
        CHECK(s.player().board[0].card_id == herald && s.player().board[1].card_id == herald &&
              s.player().board[2].card_id == herald);
        CHECK((s.player().board[0].abilities & FLAG_RUSH) && (s.player().board[1].abilities & FLAG_RUSH) &&
              (s.player().board[2].abilities & FLAG_RUSH));
    });

    suite.add("dragon.10742120.server_enters_evolved_at_max_pp", [] {
        const auto server = official(10742120);
        Scenario s;
        s.player().max_pp = 10;
        s.player().leader_hp = 10;
        s.hand({server});
        s.run({Play, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].evolution == Evolution::Normal);
        s.run({EndTurn});
        CHECK(s.player().leader_hp == 12);

        Scenario not_max;
        not_max.player().max_pp = 9;
        not_max.hand({server});
        not_max.run({Play, 0});
        CHECK(not_max.player().board[0].evolution == Evolution::None);
    });

    suite.add("dragon.10841130.spirit_generates_orca_and_evolution_crest", [] {
        const auto spirit = official(10841130);
        const auto orca = official(90041130);
        Scenario s;
        s.hand({spirit});
        s.run({Play, 0});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == orca);
        s.run({Evolve, 0});
        CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::Wadatsumi);
    });

    suite.add("dragon.10842120.kimika_fanfare_discards_draws_and_heals", [] {
        const auto kimika = official(10842120);
        Scenario s;
        s.player().leader_hp = 10;
        const auto draw = s.player().deck[s.player().deck_idx];
        s.hand({kimika, kTrainer});
        s.run({Play, 0, kOwnHandTarget + 1});
        CHECK(s.player().leader_hp == 11 && s.player().shadows == 1);
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == draw);
    });

    suite.add("dragon.10942110.assault_gets_conditional_attack_buff", [] {
        const auto assault = official(10942110);
        Scenario s;
        s.player().attacked_leader_last_turn = true;
        s.hand({assault});
        s.run({Play, 0});
        CHECK(s.player().board[0].abilities & FLAG_STORM);
        const int expected_damage = s.player().board[0].atk + 1;
        s.run({Attack, 0, kEnemyLeader});
        CHECK(s.player(1).leader_hp == 20 - expected_damage);
    });

    suite.add("dragon.10943310.ungia_doubles_allocation_after_face_attack", [] {
        const auto ungia = official(10943310);
        Scenario base;
        auto& base_first = base.place(kTrainer, 1);
        base_first.hp = base_first.max_hp = 8;
        auto& base_second = base.place(kTrainer, 1);
        base_second.hp = base_second.max_hp = 8;
        base.hand({ungia});
        base.run({Play, 0});
        CHECK(base.player(1).board[0].hp + base.player(1).board[1].hp == 13);

        Scenario doubled;
        doubled.player().attacked_leader_last_turn = true;
        auto& doubled_first = doubled.place(kTrainer, 1);
        doubled_first.hp = doubled_first.max_hp = 8;
        auto& doubled_second = doubled.place(kTrainer, 1);
        doubled_second.hp = doubled_second.max_hp = 8;
        doubled.hand({ungia});
        doubled.run({Play, 0});
        CHECK(doubled.player(1).board_count == 2);
        CHECK(doubled.player(1).board[0].hp + doubled.player(1).board[1].hp == 10);
    });

    suite.add("dragon.10541310.slicing_damage_uses_drawn_follower_cost", [] {
        const auto slicing = official(10541310);
        const auto cost_five = official(10042120);
        Scenario s;
        auto& p = s.player();
        std::fill(p.deck.begin(), p.deck.end(), cost_five);
        p.deck_idx = 0;
        p.deck_end = kDeckSize;
        auto& target = s.place(kTrainer, 1);
        target.hp = target.max_hp = 7;
        s.hand({slicing});
        s.run({Play, 0});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == cost_five);
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 2);
    });

    suite.add("dragon.10542310.prominence_roar_scales_with_board_size", [] {
        const auto roar = official(10542310);
        Scenario s;
        auto& ally = s.place(kTrainer);
        ally.hp = ally.max_hp = 6;
        auto& target = s.place(kTrainer, 1);
        target.hp = target.max_hp = 6;
        s.hand({roar});
        s.run({Play, 0});
        CHECK(s.player().board[0].hp == 4 && s.player(1).board[0].hp == 4);
    });

    suite.add("dragon.10042120.howling_tamer_enhance_summons_winged_dragon", [] {
        const auto tamer = official(10042120);
        const auto winged = official(90041120);
        Scenario s;
        s.player().pp = 7;
        s.hand({tamer});
        s.run({Play, 0});
        CHECK(s.player().pp == 0 && s.player().board_count == 2);
        CHECK(s.player().board[1].card_id == winged);
    });

    suite.add("dragon.10842110.lolo_duplicates_and_adds_orca_at_turn_end", [] {
        const auto lolo = official(10842110);
        const auto orca = official(90041130);
        Scenario s;
        s.hand({lolo});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2);
        CHECK(s.player().board[0].card_id == lolo && s.player().board[1].card_id == lolo);
        CHECK((s.player().board[0].abilities & FLAG_RUSH) && (s.player().board[1].abilities & FLAG_RUSH));
        s.run({EndTurn});
        CHECK(std::count(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count, orca) == 2);
    });

    suite.add("dragon.10943110.dust_rogue_repeats_damage_and_evolve_summons", [] {
        const auto rogue = official(10943110);
        const auto assault = official(10942110);
        Scenario s;
        s.player().attacked_leader_last_turn = true;
        auto& target = s.place(kTrainer, 1);
        target.hp = target.max_hp = 8;
        s.hand({rogue});
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
        s.run({Evolve, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == assault);
    });

    suite.add("dragon.10643110.barrier_fighter_discards_for_random_damage", [] {
        const auto fighter = official(10643110);
        Scenario s;
        auto& target = s.place(kTrainer, 1);
        target.hp = target.max_hp = 8;
        s.hand({fighter, kLilium});
        s.run({Play, 0, kOwnHandTarget + 1});
        CHECK(s.player().shadows == 1 && s.player().hand_count == 0);
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 2);
        CHECK((s.player().board[0].abilities & (FLAG_WARD | kBarrier)) == (FLAG_WARD | kBarrier));
    });

    suite.add("dragon.10842310.last_makeup_hits_follower_and_leader", [] {
        const auto makeup = official(10842310);
        Scenario s;
        auto& target = s.place(kTrainer, 1);
        target.hp = target.max_hp = 6;
        s.hand({makeup});
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 2);
        CHECK(s.player(1).leader_hp == 16);
    });

    suite.add("dragon.11044120.gilva_damage_is_enemy_missing_health", [] {
        const auto gilva = official(11044120);
        Scenario s;
        s.player(1).leader_hp = 15;
        auto& target = s.place(kTrainer, 1);
        target.hp = target.max_hp = 6;
        s.hand({gilva});
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 1);
    });

    suite.add("dragon.10742110.guardian_destroys_selected_follower_and_has_ward", [] {
        const auto guardian = official(10742110);
        Scenario s;
        s.place(kTrainer, 1);
        s.hand({guardian});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
        CHECK(s.player().board_count == 1 && (s.player().board[0].abilities & FLAG_WARD));
    });

    suite.add("dragon.10743310.black_flame_torrent_clears_and_hits_leader", [] {
        const auto torrent = official(10743310);
        Scenario s;
        auto& target = s.place(kTrainer, 1);
        target.hp = target.max_hp = 5;
        s.hand({torrent});
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
        CHECK(s.player(1).leader_hp == 17);
    });

    suite.add("dragon.10542110.hammer_dragoon_hits_and_summons", [] {
        const auto hammer = official(10542110);
        const auto winged = official(90041120);
        Scenario s;
        auto& target = s.place(kTrainer, 1);
        target.hp = target.max_hp = 7;
        s.hand({hammer});
        s.run({Play, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == winged);
    });

    suite.add("dragon.10642120.spike_dragon_evolves_at_turn_end_and_blasts", [] {
        const auto spike = official(10642120);
        Scenario s;
        s.player(1).leader_hp = 20;
        auto& target = s.place(kTrainer, 1);
        target.hp = target.max_hp = 5;
        s.hand({spike});
        s.run({Play, 0});
        s.run({EndTurn});
        CHECK(s.player().board[0].evolution == Evolution::Normal);
        CHECK(s.player(1).board[0].hp == 2 && s.player(1).leader_hp == 17);
    });

    suite.add("dragon.10942120.battler_restores_pp_and_evolve_buffs_ally", [] {
        const auto battler = official(10942120);
        Scenario s;
        s.player().leader_hp = 10;
        s.place(kTrainer);
        s.hand({battler});
        s.player().pp = 8;
        s.run({Play, 0});
        CHECK(s.player().leader_hp == 13 && s.player().pp == 3);
        s.run({Evolve, 1, kOwnBoardTarget});
        CHECK(s.player().board[0].atk == find_card(kTrainer)->atk + 3);
        CHECK(s.player().board[0].hp == find_card(kTrainer)->hp + 3);
    });

    suite.add("dragon.10744110.burned_knight_super_evolve_grants_crest", [] {
        const auto knight = official(10744110);
        Scenario s;
        auto& target = s.place(kTrainer, 1);
        target.hp = target.max_hp = 9;
        s.hand({knight});
        s.run({Play, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
        s.run({SuperEvolve, 0});
        CHECK(s.player(1).crest_count == 1 && s.player(1).crests[0].id == CrestId::BurnedKnight);
    });

    suite.add("dragon.10544110.irantza_end_turn_changes_after_evolution", [] {
        const auto irantza = official(10544110);
        Scenario base;
        base.player().leader_hp = 10;
        auto& base_target = base.place(kTrainer, 1);
        base_target.hp = base_target.max_hp = 8;
        base.hand({irantza});
        base.run({Play, 0});
        base.run({EndTurn});
        CHECK(base.player().leader_hp == 18 && base.player(1).board_count == 0 &&
              base.player(1).shadows == 1);

        Scenario evolved;
        evolved.player(1).leader_hp = 20;
        auto& evolved_target = evolved.place(kTrainer, 1);
        evolved_target.hp = evolved_target.max_hp = 8;
        evolved.hand({irantza});
        evolved.run({Play, 0});
        evolved.run({Evolve, 0});
        evolved.run({EndTurn});
        CHECK(evolved.player(1).leader_hp == 12);
    });

    suite.add("dragon.10744120.elder_dragon_summons_and_grants_crest", [] {
        const auto elder = official(10744120);
        const auto winged = official(90041120);
        Scenario s;
        s.hand({elder});
        s.run({Play, 0});
        CHECK(s.player().board_count == 2 && s.player().board[1].card_id == winged);
        CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::ElderDragon);
        const auto before = s.player().crests[0].countdown;
        s.run({SuperEvolve, 0});
        CHECK(s.player().crests[0].countdown == before + 2);
    });

    suite.add("dragon.10844120.lumiore_discards_two_then_super_draws_three", [] {
        const auto lumiore = official(10844120);
        Scenario s;
        auto& enemy = s.place(kTrainer, 1);
        enemy.hp = enemy.max_hp = 4;
        const int initial_deck_idx = s.player().deck_idx;
        const std::array expected_draws{s.player().deck[s.player().deck_idx],
                                        s.player().deck[s.player().deck_idx + 1],
                                        s.player().deck[s.player().deck_idx + 2]};
        s.hand({lumiore, kTrainer, kLilium, kRapid});
        s.run({Play, 0, kNoTarget, 0, 0b110, true});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == kRapid);
        CHECK(s.player(1).board_count == 0 && s.player(1).leader_hp == 16);
        s.run({SuperEvolve, 0});
        CHECK(s.player().hand_count == 4 && s.player().deck_idx == initial_deck_idx + 3);
        for (int i = 0; i < 3; ++i) CHECK(s.player().hand[i + 1] == expected_draws[i]);
    });

    suite.add("dragon.10944110.antemaria_ignores_ward_with_storm_condition", [] {
        const auto antemaria = official(10944110);
        Scenario s;
        s.player().attacked_leader_last_turn = true;
        s.place(kTrainer, 1).abilities |= FLAG_WARD;
        s.hand({antemaria});
        s.run({Play, 0});
        CHECK(s.player().board[0].abilities & FLAG_STORM);
        CHECK(s.legal({Attack, 0, kEnemyLeader}));
        const int damage = s.player().board[0].atk;
        s.run({Attack, 0, kEnemyLeader});
        CHECK(s.player(1).leader_hp == 20 - damage);
    });

    suite.add("dragon.10944120.nomagudara_modes_repeat_on_evolution", [] {
        const auto nomagudara = official(10944120);
        Scenario draw_heal;
        auto& p = draw_heal.player();
        p.leader_hp = 10;
        const auto first_draw = p.deck[p.deck_idx];
        const auto second_draw = p.deck[p.deck_idx + 1];
        draw_heal.hand({nomagudara});
        draw_heal.run({Play, 0, kNoTarget, 1});
        CHECK(p.leader_hp == 13 && p.hand_count == 1 && p.hand[0] == first_draw);
        draw_heal.run({Evolve, 0, kNoTarget, 1});
        CHECK(p.leader_hp == 16 && p.hand_count == 2 && p.hand[1] == second_draw);

        Scenario debuff;
        auto& target = debuff.place(kTrainer, 1);
        target.hp = target.max_hp = 9;
        debuff.hand({nomagudara});
        debuff.run({Play, 0, kNoTarget, 2});
        CHECK(debuff.player(1).board[0].hp == 5 && debuff.player(1).board[0].max_hp == 5);
        debuff.run({Evolve, 0, kNoTarget, 2});
        CHECK(debuff.player(1).board[0].hp == 1 && debuff.player(1).board[0].max_hp == 1);
    });

    suite.add("dragon.11044110.light_off_discards_evidence_for_each_damage_repeat", [] {
        const auto light_off = official(11044110);
        Scenario s;
        auto& p = s.player();
        const auto five_cost = official(10042120);
        std::fill(p.deck.begin(), p.deck.end(), five_cost);
        p.deck_idx = 0;
        p.deck_end = 4;
        p.deck[0] = kTrainer;
        p.deck[1] = five_cost;
        p.deck[2] = kEvidence;
        p.deck[3] = five_cost;
        auto& enemy = s.place(kTrainer, 1);
        enemy.hp = enemy.max_hp = 20;
        s.hand({light_off, kEvidence, kEvidence, kTrainer});
        s.run({Play, 0});
        CHECK(p.hand_count == 1 && p.hand[0] == kTrainer);
        CHECK(p.deck_remaining() == 2 && p.deck[0] == five_cost && p.deck[1] == five_cost);
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 4);
    });

    suite.add("dragon.11042110.carrier_destroys_two_selected_followers_and_adds_evidence", [] {
        const auto carrier = official(11042110);
        Scenario s;
        s.place(kTrainer, 1);
        s.place(kLilium, 1);
        s.hand({carrier});
        s.run({Play, 0, kNoTarget, 0, 0b11, false, true});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 2);
        CHECK(s.player().hand_count == 2 && s.player().hand[0] == kEvidence &&
              s.player().hand[1] == kEvidence);
    });

    suite.add("dragon.10843110.jeada_gains_barrier_and_second_follower_attack", [] {
        const auto jeada = official(10843110);
        Scenario s;
        auto& first = s.place(kTrainer, 1);
        first.atk = 3;
        auto& second = s.place(kLilium, 1);
        second.atk = 3;
        s.hand({jeada});
        s.run({Play, 0});
        CHECK(s.legal({Attack, 0, 0}));
        s.run({Attack, 0, 0});
        CHECK(s.player().board_count == 1 && s.player().board[0].hp == 2);
        CHECK(s.legal({Attack, 0, 0}));
        s.run({Attack, 0, 0});
        CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 2);
        CHECK(s.player().board_count == 1 && s.player().board[0].hp == 2);
    });

    suite.add("dragon.10841120.treasure_keeper_last_words_draw_three", [] {
        const auto keeper = official(10841120);
        Scenario s;
        auto& p = s.player();
        const std::array expected{s.player().deck[s.player().deck_idx],
                                  s.player().deck[s.player().deck_idx + 1],
                                  s.player().deck[s.player().deck_idx + 2]};
        const auto source = s.place(keeper).creation_order;
        constexpr std::array destroy{Effect{.operation=Operation::Destroy, .target=Target::Self}};
        resolve_effect_steps(s.state, 0, destroy, source);
        CHECK(p.board_count == 0 && p.hand_count == 3);
        for (int i = 0; i < 3; ++i) CHECK(p.hand[i] == expected[i]);
    });

    suite.add("dragon.10942310.jaws_discards_two_then_damages_follower_and_leader", [] {
        const auto jaws = official(10942310);
        Scenario s;
        auto& enemy = s.place(kTrainer, 1);
        enemy.hp = enemy.max_hp = 5;
        s.hand({jaws, kTrainer, kLilium, kRapid});
        s.run({Play, 0, kNoTarget, 0, 0b110, true});
        CHECK(s.player().hand_count == 1 && s.player().hand[0] == kRapid);
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 2);
        CHECK(s.player(1).leader_hp == 17);
    });

    suite.add("dragon.10743110.dragonnewt_leader_modes_destroy_strongest", [] {
        const auto leader = official(10743110);
        Scenario evolve_mode;
        evolve_mode.place(kTrainer, 1);
        auto& strongest = evolve_mode.place(kLilium, 1);
        strongest.atk = 5;
        evolve_mode.hand({leader});
        evolve_mode.run({Play, 0, kNoTarget, 1});
        CHECK(evolve_mode.player(1).board_count == 1 && evolve_mode.player(1).board[0].card_id == kTrainer);
        CHECK(evolve_mode.player().board[0].evolution == Evolution::Normal);

        Scenario stealth_mode;
        stealth_mode.place(kTrainer, 1);
        auto& highest = stealth_mode.place(kLilium, 1);
        highest.atk = 5;
        stealth_mode.hand({leader});
        stealth_mode.run({Play, 0, kNoTarget, 2});
        CHECK(stealth_mode.player(1).board_count == 1 && stealth_mode.player(1).board[0].card_id == kTrainer);
        CHECK(stealth_mode.player().board[0].abilities & kAmbush);
        CHECK(stealth_mode.player().board[0].evolution == Evolution::None);
    });

    suite.add("dragon.11043110.strongman_discards_evidence_then_evolves_for_three", [] {
        const auto strongman = official(11043110);
        Scenario s;
        auto& p = s.player();
        p.max_pp = p.pp = 9;
        auto& enemy = s.place(kTrainer, 1);
        enemy.hp = enemy.max_hp = 5;
        s.hand({strongman, kEvidence, kTrainer});
        s.run({Play, 0});
        CHECK(p.max_pp == 10 && p.hand_count == 1 && p.hand[0] == kTrainer);
        s.run({Evolve, 0, 0});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 2);
    });

    suite.add("dragon.10643310.executioner_blade_damage_and_discard_chain", [] {
        const auto blade = official(10643310);
        const auto red_stream = official(10642310);
        Scenario damage;
        auto& enemy = damage.place(kTrainer, 1);
        enemy.hp = enemy.max_hp = 8;
        damage.hand({blade});
        damage.run({Play, 0});
        CHECK(damage.player(1).board_count == 1 && damage.player(1).board[0].hp == 1);

        Scenario discard_seven;
        discard_seven.place(kTrainer, 1);
        discard_seven.hand({red_stream, blade});
        discard_seven.run({Play, 0, 0, 0, 0, false, false, 1});
        CHECK(discard_seven.player().hand_count == 1 && discard_seven.player().hand[0] == blade);
        CHECK(card_cost(*find_card(blade), discard_seven.player().hand_modifiers[0]) == 5);

        Scenario discard_five;
        discard_five.place(kTrainer, 1);
        discard_five.hand({red_stream, blade});
        discard_five.player().hand_modifiers[1].cost = -2;
        discard_five.run({Play, 0, 0, 0, 0, false, false, 1});
        CHECK(discard_five.player().hand_count == 1 && discard_five.player().hand[0] == blade);
        CHECK(card_cost(*find_card(blade), discard_five.player().hand_modifiers[0]) == 3);
    });
}

} // namespace svwb::wb::spec
