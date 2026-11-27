#include "spec.hpp"

namespace svwb::wb::spec {
namespace {

// The official card catalog gives the exact text for every official ID below.
// These scenarios cover Witch's Earth Rite and spellboost rules through live card plays.

void magic_pot_draws_and_acts() {
    Scenario s;
    const auto pot = official(10031210);
    s.hand({pot});
    const auto before = s.player().hand_count;
    s.run({Play, 0});
    CHECK(s.player().hand_count == before); // Pot replaces itself with its Fanfare draw.
    CHECK(s.player().board_count == 1 && s.player().board[0].card_id == pot);
    CHECK(s.player().board[0].earth_count == 1);
    CHECK(s.legal({Act, 0}));
    s.run({Act, 0});
    CHECK(s.player().board[0].earth_count == 2 && s.player().pp == 8);
}

void magic_pot_banishes_the_merged_sigil() {
    Scenario s;
    const auto pot = official(10031210);
    const auto nocturne = official(11054110);
    s.place(nocturne);
    s.place(kEarthSigil).earth_count = 3;
    s.hand({pot});
    s.run({Play, 0});
    CHECK(s.player().board_count == 3 && s.player().shadows == 0);
    CHECK(s.player().board[1].card_id == pot && s.player().board[1].earth_count == 4);
    const auto& skeleton = s.player().board[2];
    CHECK(skeleton.card_id == official(90051110) && (skeleton.abilities & FLAG_WARD));
}

void trick_witch_modes() {
    const auto trick = official(11031120);
    {
        Scenario s;
        s.hand({trick});
        s.run({Play, 0, kNoTarget, 2});
        CHECK(s.player().board_count == 2 && s.player().board[0].card_id == trick);
        CHECK(s.player().board[1].card_id == kEarthSigil && s.player().board[1].earth_count == 2);
        CHECK(s.player().pp == 8);
    }
    {
        Scenario s;
        s.hand({official(10031210), trick});
        s.place(kTrainer, 1);
        s.run({Play, 0}); // establish one Earth Sigil
        s.player(1).board[0].hp = 3;
        s.run({Play, 0, kNoTarget, 1});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 1);
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == trick && s.player().shadows == 1);
    }
}

void gluttonous_meal_modes_and_rite() {
    const auto meal = official(10732310);
    {
        Scenario s;
        s.hand({meal});
        s.run({Play, 0, kNoTarget, 1});
        CHECK(s.player().board_count == 1 && s.player().board[0].card_id == kEarthSigil);
        CHECK(s.player().board[0].earth_count == 4);
    }
    {
        Scenario s;
        s.hand({official(10031210), meal});
        s.place(kTrainer, 1);
        s.place(kTrainer, 1);
        s.player(1).board[0].hp = 4;
        s.player(1).board[1].hp = 2;
        s.run({Play, 0});
        s.run({Act, 0}); // two sigils
        CHECK(s.player().board[0].earth_count == 2);
        s.run({Play, 0, kNoTarget, 2});
        CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 2);
        CHECK(s.player().board_count == 0);
        CHECK(s.player().shadows == 2); // The played spell and destroyed Sigil each add one shadow.
    }
}

void eruption_earth_rite_draw() {
    Scenario s;
    const auto eruption = official(10032310);
    s.hand({official(10031210), eruption});
    s.place(kTrainer);
    s.place(kTrainer, 1);
    s.player().board[0].hp = 4;
    s.player(1).board[0].hp = 4;
    s.player().leader_hp = 18;
    const auto hand_before = s.player().hand_count;
    s.run({Play, 0});
    CHECK(s.player().board[1].earth_count == 1);
    s.run({Play, 0});
    CHECK(s.player().board[0].hp == 2 && s.player(1).board[0].hp == 2);
    CHECK(s.player().leader_hp == 18 && s.player(1).leader_hp == 20);
    CHECK(s.player().hand_count == hand_before); // Pot and Eruption each replace themselves with a draw.
    CHECK(s.player().board_count == 1); // Rite consumes the Sigil.
    CHECK(s.player().board[0].card_id == kTrainer && s.player().board[0].hp == 2);
    CHECK(s.player().shadows == 2); // The played spell and destroyed Sigil each add one shadow.
}

void flash_swordsman_earth_mode() {
    Scenario s;
    const auto swordsman = official(10031110);
    s.hand({official(10031210), swordsman});
    s.run({Play, 0});
    s.run({Play, 0, kNoTarget, 2});
    CHECK(s.player().board_count == 1);
    const auto& unit = s.player().board[0];
    CHECK(unit.card_id == swordsman && unit.atk == find_card(swordsman)->atk + 2);
    CHECK(unit.max_hp == find_card(swordsman)->hp + 2 && (unit.abilities & FLAG_WARD));
    CHECK(s.player().shadows == 1);
}

void spellboost_cards_advance_from_spells() {
    Scenario s;
    const auto wisdom = official(10031310);
    const auto storm = official(10131320);
    const auto sleep = official(10833310);
    s.hand({wisdom, storm, sleep});
    s.run({Play, 0});
    CHECK(s.player().hand_modifiers[0].spellboost == 1);
    CHECK(s.player().hand_modifiers[1].spellboost == 1);
    CHECK(s.player().hand_count == 3); // the Wisdom draw replaces the played spell.
}

void little_beast_fanfare_and_evolve_repeat() {
    Scenario s;
    const auto beast = official(10731120);
    s.hand({beast});
    s.place(kTrainer, 1);
    s.player(1).board[0].hp = 2;
    s.run({Play, 0, 0});
    CHECK(s.player(1).board[0].hp == 1);
    CHECK(s.player().board_count == 2 && s.player().board[0].card_id == beast);
    CHECK(s.player().board[1].card_id == kEarthSigil && s.player().board[1].earth_count == 1);
    s.player().ep = 1;
    s.run({Evolve, 0, 0});
    CHECK(s.player(1).board_count == 0);
    CHECK(s.player().board[1].earth_count == 2);
}

void truth_summon_makes_clay_golem() {
    Scenario s;
    s.hand({official(10031320)});
    s.run({Play, 0});
    CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(90031110));
    CHECK(s.player().board[0].atk == 2 && s.player().board[0].max_hp == 2);
}

void crystal_hand_has_rush() {
    Scenario s;
    const auto hand = official(10631110);
    s.hand({hand});
    s.place(kTrainer, 1);
    s.run({Play, 0});
    CHECK(s.player().board_count == 1 && s.player().board[0].card_id == hand);
    CHECK(s.player().board[0].abilities & FLAG_RUSH);
    CHECK(s.legal({Attack, 0, 0}));
}

void normal_invasion_destroys_chosen_crystal() {
    Scenario s;
    const auto crystal = official(10631110);
    s.hand({official(10632310)});
    s.place(crystal);
    const auto shadows = s.player().shadows;
    s.run({Play, 0, kOwnBoardTarget});
    CHECK(s.player().board_count == 0 && s.player().shadows == shadows + 2);
    CHECK(s.player().hand_count == 2); // Two cards drawn after consuming the spell.
}

void overreaction_adds_three_truth_summons() {
    Scenario s;
    const auto truth = official(10031320);
    s.hand({official(10931310)});
    s.run({Play, 0});
    CHECK(s.player().hand_count == 3);
    CHECK(std::all_of(s.player().hand.begin(), s.player().hand.begin() + s.player().hand_count,
                      [truth](CardId id) { return id == truth; }));
}

void riddle_melody_rewards_amulett_act() {
    Scenario s;
    const auto proof = official(90001210);
    s.hand({official(11032310)});
    s.place(proof);
    s.place(kTrainer, 1);
    s.player(1).board[0].hp = 5;
    s.run({Act, 0});
    s.run({Play, 0});
    CHECK(s.player(1).board[0].hp == 2);
    CHECK(std::any_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
                      [proof](const Permanent& p) { return p.card_id == proof; }));
}

void celestial_gift_summons_two_hands() {
    Scenario s;
    const auto hand = official(10631110);
    s.hand({official(10631310)});
    s.run({Play, 0});
    CHECK(s.player().board_count == 2);
    CHECK(s.player().board[0].card_id == hand && s.player().board[1].card_id == hand);
}

void green_fortune_repeats_damage_and_boost_on_evolve() {
    Scenario s;
    const auto card = official(10831110);
    const auto storm = official(10131320);
    s.hand({card, storm});
    s.place(kTrainer, 1);
    s.player(1).board[0].hp = 4;
    s.run({Play, 0, 0});
    CHECK(s.player(1).board[0].hp == 3 && s.player().hand_modifiers[0].spellboost == 1);
    s.player().ep = 1;
    s.run({Evolve, 0, 0});
    CHECK(s.player(1).board[0].hp == 2 && s.player().hand_modifiers[0].spellboost == 2);
}

void poppy_adds_manaria_magic_bullet() {
    Scenario s;
    const auto bullet = official(90031310);
    s.hand({official(10831120)});
    s.run({Play, 0});
    CHECK(s.player().hand_count == 1 && s.player().hand[0] == bullet);
}

void shape_wizard_earth_and_super_evolve_golems() {
    Scenario s;
    s.hand({official(10531110)});
    s.run({Play, 0});
    CHECK(s.player().board_count == 2 && s.player().board[1].card_id == kEarthSigil);
    CHECK(s.player().board[1].earth_count == 2);
    s.player().sep = 1;
    s.run({SuperEvolve, 0});
    const auto guardian = official(90031120);
    CHECK(s.player().board_count == 4);
    CHECK(s.player().board[2].card_id == guardian && s.player().board[3].card_id == guardian);
    CHECK((s.player().board[2].abilities & FLAG_WARD) && (s.player().board[3].abilities & FLAG_WARD));
}

void venture_grimoire_enhance_creates_two_copies() {
    Scenario s;
    const auto grimoire = official(10632120);
    s.hand({grimoire});
    s.player().max_pp = s.player().pp = 6;
    s.run({Play, 0}); // Enhance is paid automatically when available; no printed mode is selected.
    CHECK(s.player().board_count == 3);
    CHECK(s.player().board[0].card_id == grimoire && s.player().board[1].card_id == grimoire &&
          s.player().board[2].card_id == grimoire);
    CHECK(s.player().pp == 0);
}

void sheem_summons_two_crystals() {
    Scenario s;
    const auto crystal = official(10631110);
    s.hand({official(10634110)});
    s.run({Play, 0});
    CHECK(s.player().board_count == 3 && s.player().board[1].card_id == crystal &&
          s.player().board[2].card_id == crystal);
}

void tiko_adds_and_discounts_manaria_spells() {
    Scenario s;
    const auto bullet = official(90031310);
    s.hand({official(10833110)});
    s.run({Play, 0});
    CHECK(s.player().hand_count == 2 && s.player().hand[0] == bullet && s.player().hand[1] == bullet);
    s.player().ep = 1;
    s.run({Evolve, 0});
    CHECK(s.player().hand_modifiers[0].cost == -1 && s.player().hand_modifiers[1].cost == -1);
}

void human_love_buffs_experiment_on_board_and_in_hand() {
    Scenario s;
    const auto experiment = official(10931110);
    s.hand({official(10932310)});
    s.run({Play, 0});
    CHECK(s.player().board_count == 1 && s.player().board[0].card_id == experiment);
    CHECK(s.player().board[0].atk == find_card(experiment)->atk + 1);
    CHECK(s.player().hand_count == 1 && s.player().hand[0] == experiment);
    CHECK(s.player().hand_modifiers[0].attack == 1);
}

void battered_assistant_evidence_act_grows_it() {
    Scenario s;
    const auto helper = official(11031110);
    const auto proof = official(90001210);
    s.hand({helper});
    s.run({Play, 0});
    CHECK(s.player().board_count == 2 && s.player().board[0].card_id == helper &&
          s.player().board[1].card_id == proof);
    CHECK(s.player().board[0].abilities & FLAG_WARD);
    const auto atk = s.player().board[0].atk;
    const auto hp = s.player().board[0].max_hp;
    s.run({Act, 1});
    CHECK(s.player().board[0].atk == atk + 1 && s.player().board[0].max_hp == hp + 1);
}

void remirami_earth_rite_summons_guardian() {
    Scenario s;
    const auto guardian = official(90031120);
    s.place(official(90031210));
    s.hand({official(10032110)});
    s.run({Play, 0});
    CHECK(s.player().board_count == 2);
    CHECK(s.player().board[0].card_id == official(10032110) && s.player().board[1].card_id == guardian);
    CHECK(s.player().shadows == 1); // The destroyed Sigil adds one shadow.
}

void dinky_creature_earth_rite_evolves_itself() {
    Scenario s;
    s.place(official(90031210));
    s.hand({official(10731110)});
    s.run({Play, 0});
    CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(10731110));
    CHECK(s.player().board[0].evolution == Evolution::Normal && s.player().shadows == 1);
}

void workshop_move_summons_three_clay_golems() {
    Scenario s;
    const auto clay = official(90031110);
    s.hand({official(11031310)});
    s.run({Play, 0});
    CHECK(s.player().board_count == 3);
    CHECK(std::all_of(s.player().board.begin(), s.player().board.begin() + 3,
                      [clay](const Permanent& p) { return p.card_id == clay; }));
}

void magic_academy_student_heals_on_hand_entries() {
    Scenario s;
    s.hand({official(10632110)});
    s.player().leader_hp = 14;
    s.run({Play, 0});
    CHECK(s.player().leader_hp == 16);
    CHECK(s.player().board_count == 3 && s.player().board[1].card_id == official(10631110) &&
          s.player().board[2].card_id == official(10631110));
}

void ground_lightning_earth_rite_repeats_spell() {
    Scenario s;
    const auto lightning = official(10832320);
    s.hand({lightning});
    auto& sigil = s.place(official(90031210));
    sigil.earth_count = 2;
    s.place(kTrainer, 1);
    s.player(1).board[0].atk = 9;
    s.player(1).board[0].hp = 8;
    s.player(1).board[0].max_hp = 8;
    s.player(1).leader_hp = 18;
    s.run({Play, 0});
    CHECK(s.player(1).board_count == 0 && s.player(1).leader_hp == 16);
    CHECK(s.player().hand_count == 1 && s.player().hand[0] == lightning);
    CHECK(s.player().shadows == 2); // The spell and destroyed Sigil add one shadow each.
}

void crow_spell_summons_experiment_and_hits_twice() {
    Scenario s;
    const auto experiment = official(10931110);
    s.hand({official(10933310)});
    s.place(kTrainer, 1);
    s.player(1).board[0].hp = 20;
    s.player(1).board[0].max_hp = 20;
    s.run({Play, 0});
    CHECK(s.player().board_count == 1 && s.player().board[0].card_id == experiment);
    CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 6);
}

void tempestruler_summon_triggers_golem_damage() {
    Scenario s;
    const auto ruler = official(11032120);
    const auto guardian = official(90031120);
    s.place(official(90031210));
    s.hand({ruler});
    s.place(kTrainer, 1);
    s.player(1).board[0].hp = 5;
    s.run({Play, 0});
    CHECK(s.player(1).board[0].hp == 2);
    CHECK(s.player().board_count == 2 && s.player().board[0].card_id == ruler &&
          s.player().board[1].card_id == guardian);
}

void governor_rite_evolves_each_guardian_on_entry() {
    Scenario s;
    const auto guardian = official(90031120);
    s.place(official(90031210)).earth_count = 2;
    s.hand({official(10533110)});
    s.run({Play, 0});
    CHECK(s.player().board_count == 3 && s.player().board[0].card_id == official(10533110));
    CHECK(s.player().board[1].card_id == guardian && s.player().board[2].card_id == guardian);
    CHECK(s.player().board[1].evolution == Evolution::Normal &&
          s.player().board[2].evolution == Evolution::Normal);
    CHECK(s.player().shadows == 1);
}

void ginger_grants_rush_to_later_allies_and_boosts_hand() {
    Scenario s;
    const auto ginger = official(10834120);
    const auto guardian = official(90031120);
    const auto storm = official(10131320);
    s.hand({ginger, storm});
    s.run({Play, 0});
    CHECK(s.player().board_count == 3 && s.player().board[0].card_id == ginger);
    CHECK(s.player().board[1].card_id == guardian && s.player().board[2].card_id == guardian);
    CHECK((s.player().board[1].abilities & FLAG_RUSH) && (s.player().board[2].abilities & FLAG_RUSH));
    CHECK(s.player().hand_modifiers[0].spellboost == 2);
}

void chimera_damage_budget_matches_earth_sigils() {
    Scenario s;
    s.hand({official(11034120)});
    auto& sigil = s.place(official(90031210));
    sigil.earth_count = 3;
    s.place(kTrainer, 1);
    s.player(1).board[0].hp = 10;
    s.player(1).board[0].max_hp = 10;
    s.run({Play, 0});
    CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 7);
    CHECK(s.player(1).leader_hp == 20);
    CHECK(s.player().board_count == 2 && s.player().board[1].card_id == official(11034120));
}

void chimera_hits_leader_on_empty_board() {
    Scenario s;
    s.hand({official(11034120)});
    s.place(kEarthSigil).earth_count = 7;
    s.run({Play, 0});
    CHECK(s.player(1).leader_hp == 13);
}

void chimera_sends_only_excess_to_leader() {
    Scenario s;
    s.hand({official(11034120)});
    s.place(kEarthSigil).earth_count = 9;
    s.place(kTrainer, 1).hp = 2;
    s.place(kTrainer, 1).hp = 3;
    s.run({Play, 0});
    CHECK(s.player(1).board_count == 0);
    CHECK(s.player(1).leader_hp == 16);
}

void chimera_allocates_to_older_followers_first() {
    Scenario s;
    s.hand({official(11034120)});
    s.place(kEarthSigil).earth_count = 3;
    s.place(kTrainer, 1).hp = 2;
    s.place(kEvidence, 1);
    auto& newer = s.place(kTrainer, 1);
    const auto newer_order = newer.creation_order;
    newer.hp = 3;
    s.run({Play, 0});
    CHECK(s.player(1).board_count == 2 && s.player(1).board[0].card_id == kEvidence);
    CHECK(s.player(1).board[1].creation_order == newer_order && s.player(1).board[1].hp == 2);
    CHECK(s.player(1).leader_hp == 20);
}

void chimera_without_sigils_deals_no_damage() {
    Scenario s;
    s.hand({official(11034120)});
    s.place(kTrainer, 1).hp = 2;
    s.run({Play, 0});
    CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 2);
    CHECK(s.player(1).leader_hp == 20);
}

void follower_only_split_damage_stays_off_leader() {
    for (const bool enemy_has_follower : {false, true}) {
        Scenario s;
        s.hand({official(10514120)});
        if (enemy_has_follower) s.place(kTrainer, 1).hp = 2;
        s.run({Play, 0, kNoTarget, 3});
        CHECK(s.player(1).board_count == 0 && s.player(1).leader_hp == 20);
    }
}

void kitty_trick_randomly_resolves_two_distinct_rite_effects() {
    Scenario s;
    s.hand({official(10532310)});
    auto& sigil = s.place(official(90031210));
    sigil.earth_count = 2;
    s.player().leader_hp = 10;
    s.run({Play, 0});
    const bool summoned = std::any_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
        [](const Permanent& p) { return p.card_id == official(90031110); });
    const bool healed = s.player().leader_hp == 12;
    const bool earth_added = std::any_of(s.player().board.begin(), s.player().board.begin() + s.player().board_count,
        [](const Permanent& p) { return p.card_id == kEarthSigil && p.earth_count == 3; });
    CHECK(static_cast<int>(summoned) + static_cast<int>(healed) + static_cast<int>(earth_added) == 2);
    CHECK(s.player().shadows == 2); // Spell play and the consumed Sigil each add one shadow.
}

void ecstasy_fusion_super_evolve_grants_drain() {
    Scenario s;
    const auto ecstasy = official(10933110);
    const auto experiment = official(10931110);
    s.hand({ecstasy, kTrainer});
    s.run({Fuse, 0, kNoTarget, 1});
    CHECK(s.player().hand_count == 1 && s.player().hand_modifiers[0].fused_materials == 1);
    s.player().sep = 1;
    s.run({Play, 0});
    CHECK(s.player().board_count == 2 && s.player().board[1].card_id == experiment);
    s.run({SuperEvolve, 0, kOwnBoardTarget + 1});
    CHECK(s.player().board[1].abilities & kDrain);
}

void sephie_fusion_pays_two_pp_then_fanfare_summons_two() {
    Scenario s;
    const auto sephie = official(10934110);
    const auto experiment = official(10931110);
    s.hand({sephie, kTrainer, kLilium});
    s.player().sep = 1;
    s.run({Fuse, 0, kNoTarget, 3});
    CHECK(s.player().pp == 8 && s.player().board_count == 1 && s.player().board[0].card_id == experiment);
    s.run({Play, 0});
    CHECK(s.player().pp == 1 && s.player().board_count == 4);
    CHECK(s.player().board[1].card_id == sephie && s.player().board[2].card_id == experiment &&
          s.player().board[3].card_id == experiment);
    s.run({SuperEvolve, 1});
    CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::Sephie);
}

void sweet_entity_rite_mode_and_evolve_repeat() {
    Scenario s;
    const auto sweet = official(10733110);
    s.place(official(90031210));
    s.hand({sweet});
    s.place(kTrainer, 1);
    s.player(1).board[0].hp = 5;
    s.run({Play, 0, kNoTarget, 1});
    CHECK(s.player(1).board[0].hp == 2 && s.player().shadows == 1);
    s.place(official(90031210));
    s.player().ep = 1;
    s.run({Evolve, 0, kNoTarget, 1});
    CHECK(s.player(1).board_count == 0 && s.player(1).shadows == 1);
    CHECK(s.player().board_count == 1 && s.player().shadows == 2);
}

void sweet_entity_draw_mode_runs_with_rite() {
    Scenario s;
    s.place(official(90031210));
    s.hand({official(10733110)});
    s.run({Play, 0, kNoTarget, 2});
    CHECK(s.player().board_count == 1 && s.player().board[0].card_id == official(10733110));
    CHECK(s.player().hand_count == 2 && s.player().shadows == 1);
}

void lincle_grants_crest_and_rush() {
    Scenario s;
    const auto lincle = official(10534110);
    s.hand({lincle});
    s.run({Play, 0});
    CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::Lincle);
    CHECK(s.player().board_count == 1 && (s.player().board[0].abilities & FLAG_RUSH));
}

void insomnia_witch_evolution_destroys_its_crest() {
    Scenario s;
    s.hand({official(10532110)});
    s.run({Play, 0});
    CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::Insomnia);
    s.player().ep = 1;
    s.run({Evolve, 0});
    CHECK(s.player().crest_count == 0);
}

void noin_interrogation_adds_proof_and_crest() {
    Scenario s;
    const auto proof = official(90001210);
    s.hand({official(11033310)});
    s.run({Play, 0});
    CHECK(s.player().hand_count == 1 && s.player().hand[0] == proof);
    CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::NoinInterrogation);
}

void lala_anthem_rite_grants_crest_and_aura() {
    Scenario s;
    const auto lala = official(10734110);
    s.place(official(90031210));
    s.hand({lala});
    s.run({Play, 0});
    CHECK(s.player().crest_count == 1 && s.player().crests[0].id == CrestId::Lala);
    CHECK(s.player().board_count == 1 && s.player().board[0].card_id == lala);
    CHECK(s.player().board[0].abilities & kAura);
    CHECK(s.player().shadows == 1);
}

void kaldigensla_discount_tracks_each_crystal_hand() {
    Scenario s;
    const auto kaldigensla = official(10634120);
    s.hand({kaldigensla, official(10631310)});
    s.run({Play, 1});
    CHECK(s.player().board_count == 2);
    CHECK(s.player().hand_modifiers[0].cost == -2);
    CHECK(card_cost(*find_card(kaldigensla), s.player().hand_modifiers[0]) == 8);
}

void lovely_masterpiece_aoe_and_ward() {
    Scenario s;
    const auto masterpiece = official(10734120);
    s.hand({masterpiece});
    s.place(kTrainer, 1);
    s.player(1).board[0].hp = 8;
    s.run({Play, 0});
    CHECK(s.player(1).board_count == 1 && s.player(1).board[0].hp == 2);
    CHECK(s.player().board[0].card_id == masterpiece && (s.player().board[0].abilities & FLAG_WARD));
}

void sammy_spellboost_reduces_cost_before_play() {
    Scenario s;
    const auto sammy = official(10832110);
    s.hand({official(10031310), sammy});
    s.run({Play, 0});
    CHECK(s.player().hand_modifiers[0].spellboost == 1);
    s.player().pp = 3;
    s.reject({Play, 0});
    s.player().pp = 4;
    s.run({Play, 0});
    CHECK(s.player().board_count == 1 && s.player().board[0].card_id == sammy);
    CHECK(s.player().pp == 0);
}

} // namespace

void witch(Suite& suite) {
    suite.add("witch.10031210.fanfare_and_act_add_earth", magic_pot_draws_and_acts);
    suite.add("witch.10031210.merged_sigil_triggers_banish", magic_pot_banishes_the_merged_sigil);
    suite.add("witch.11031120.both_modes_and_rite_targeting", trick_witch_modes);
    suite.add("witch.10732310.both_modes_and_area_damage", gluttonous_meal_modes_and_rite);
    suite.add("witch.10032310.area_damage_and_conditional_rite_draw", eruption_earth_rite_draw);
    suite.add("witch.10031110.earth_rite_buff_and_ward", flash_swordsman_earth_mode);
    suite.add("witch.10031310.spell_play_boosts_witch_hand", spellboost_cards_advance_from_spells);
    suite.add("witch.10731120.fanfare_and_evolve_repeat", little_beast_fanfare_and_evolve_repeat);
    suite.add("witch.10031320.clay_golem_token", truth_summon_makes_clay_golem);
    suite.add("witch.10631110.crystal_rush", crystal_hand_has_rush);
    suite.add("witch.10632310.destroy_crystal_draw_two", normal_invasion_destroys_chosen_crystal);
    suite.add("witch.10931310.add_three_truth_summons", overreaction_adds_three_truth_summons);
    suite.add("witch.11032310.acted_amulet_bonus", riddle_melody_rewards_amulett_act);
    suite.add("witch.10631310.summon_two_crystals", celestial_gift_summons_two_hands);
    suite.add("witch.10831110.fanfare_and_evolve_effects", green_fortune_repeats_damage_and_boost_on_evolve);
    suite.add("witch.10831120.manaria_magic_bullet", poppy_adds_manaria_magic_bullet);
    suite.add("witch.10531110.super_evolve_two_guardians", shape_wizard_earth_and_super_evolve_golems);
    suite.add("witch.10632120.enhance_summon_two_copies", venture_grimoire_enhance_creates_two_copies);
    suite.add("witch.10634110.fanfare_two_crystals", sheem_summons_two_crystals);
    suite.add("witch.10833110.evolve_manaria_spell_cost", tiko_adds_and_discounts_manaria_spells);
    suite.add("witch.10932310.buff_board_and_hand_experiment", human_love_buffs_experiment_on_board_and_in_hand);
    suite.add("witch.11031110.evidence_act_buff", battered_assistant_evidence_act_grows_it);
    suite.add("witch.10032110.earth_rite_guardian", remirami_earth_rite_summons_guardian);
    suite.add("witch.10731110.earth_rite_auto_evolve", dinky_creature_earth_rite_evolves_itself);
    suite.add("witch.11031310.summon_three_clay_golems", workshop_move_summons_three_clay_golems);
    suite.add("witch.10632110.hand_entry_heal_trigger", magic_academy_student_heals_on_hand_entries);
    suite.add("witch.10832320.earth_rite_duplicate_and_face_damage", ground_lightning_earth_rite_repeats_spell);
    suite.add("witch.10933310.two_random_seven_damage_steps", crow_spell_summons_experiment_and_hits_twice);
    suite.add("witch.11032120.golem_entry_damage_trigger", tempestruler_summon_triggers_golem_damage);
    suite.add("witch.10533110.earth_rite_each_golem_entry", governor_rite_evolves_each_guardian_on_entry);
    suite.add("witch.10834120.ally_entry_rush_and_spellboost", ginger_grants_rush_to_later_allies_and_boosts_hand);
    suite.add("witch.11034120.sigils_define_damage_budget", chimera_damage_budget_matches_earth_sigils);
    suite.add("witch.11034120.empty_board_damage_hits_leader", chimera_hits_leader_on_empty_board);
    suite.add("witch.11034120.excess_damage_hits_leader", chimera_sends_only_excess_to_leader);
    suite.add("witch.11034120.older_followers_take_damage_first", chimera_allocates_to_older_followers_first);
    suite.add("witch.11034120.no_sigils_deals_no_damage", chimera_without_sigils_deals_no_damage);
    suite.add("witch.split_damage.miroku_never_hits_leader", follower_only_split_damage_stays_off_leader);
    suite.add("witch.10532310.random_two_rite_effects", kitty_trick_randomly_resolves_two_distinct_rite_effects);
    suite.add("witch.10933110.fused_super_evolve_drain", ecstasy_fusion_super_evolve_grants_drain);
    suite.add("witch.10934110.fusion_pp_and_super_crest", sephie_fusion_pays_two_pp_then_fanfare_summons_two);
    suite.add("witch.10733110.rite_mode_and_evolve_repeat", sweet_entity_rite_mode_and_evolve_repeat);
    suite.add("witch.10733110.rite_draw_mode", sweet_entity_draw_mode_runs_with_rite);
    suite.add("witch.10534110.lincle_crest_and_rush", lincle_grants_crest_and_rush);
    suite.add("witch.10532110.insomnia_crest_removed_on_evolve", insomnia_witch_evolution_destroys_its_crest);
    suite.add("witch.11033310.proof_and_crest", noin_interrogation_adds_proof_and_crest);
    suite.add("witch.10734110.lala_rite_crest_and_aura", lala_anthem_rite_grants_crest_and_aura);
    suite.add("witch.10634120.crystal_entry_cost_discount", kaldigensla_discount_tracks_each_crystal_hand);
    suite.add("witch.10734120.aoe_and_ward", lovely_masterpiece_aoe_and_ward);
    suite.add("witch.10832110.spellboost_cost_discount", sammy_spellboost_reduces_cost_before_play);
}

} // namespace svwb::wb::spec


