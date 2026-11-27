// These internal key types have no public API. Compile the actual search translation unit
// into the specification test binary so the tests exercise its real hash and equality paths.
#include "../../src/wb_search.cpp"
#include "spec.hpp"

#include <set>

namespace svwb::wb::spec {
namespace {

void distinct_key(Suite& suite, std::string label, std::function<void(PlayerState&)> change) {
    suite.add("rules.hash." + label, [change] {
        Scenario fixture;
        fixture.hand({kWingedStatue});
        fixture.place(kEarthSigil);
        fixture.crest(CrestId::Testimony, 2);
        fixture.player().faiths[0] = kFaithToken;
        fixture.player().faith_count = 1;
        GameState variant = fixture.state;
        change(variant.players[0]);
        CHECK(variant != fixture.state);
        std::unordered_set<GameState, PositionHash> keys{fixture.state, variant};
        CHECK(keys.size() == 2 && keys.contains(variant) && keys.contains(fixture.state));
        CHECK(PositionHash{}(variant) != PositionHash{}(fixture.state));
    });
}

} // namespace

void search_keys(Suite& suite) {
    distinct_key(suite, "countdown", [](PlayerState& p) { ++p.board[0].countdown; });
    distinct_key(suite, "crest_countdown", [](PlayerState& p) { ++p.crests[0].countdown; });
    distinct_key(suite, "crest_trigger_round", [](PlayerState& p) { ++p.crests[0].last_trigger_round; });
    distinct_key(suite, "faith_value", [](PlayerState& p) { ++p.faith_values[0]; });
    distinct_key(suite, "earth_count", [](PlayerState& p) { ++p.board[0].earth_count; });
    distinct_key(suite, "temporary_cost", [](PlayerState& p) { --p.hand_modifiers[0].temporary_cost_delta; });
    distinct_key(suite, "fused_ply", [](PlayerState& p) { ++p.hand_modifiers[0].fused_ply; });
    distinct_key(suite, "artifact_history", [](PlayerState& p) { ++p.artifact_types_entered; });
    distinct_key(suite, "amulet_act", [](PlayerState& p) { p.acted_amulet = true; });
    suite.add("rules.hash.legacy_card_identity", [] {
        Scenario fixture;
        fixture.place(official(11001110));
        GameState variant = fixture.state;
        // A different catalog ID is the only change. Keeping the same derived board stats is
        // valid after buffs or debuffs, and card-specific triggered abilities still differ.
        const CardId original = fixture.player().board[0].card_id;
        const auto alternate = official(90022110);
        CHECK(alternate == original + 256);
        variant.players[0].board[0].card_id = alternate;
        CHECK(variant != fixture.state);
        CHECK(legacy_key(variant) != legacy_key(fixture.state));
    });
}

void phase2_audit(Suite& suite) {
    suite.add("phase2_audit.10031310.draw_is_independent_of_leader_hp", [] {
        for (const int health : {1, 20}) {
            Scenario fixture;
            fixture.hand({official(10031310)});
            fixture.player().leader_hp = static_cast<std::int16_t>(health);
            const auto cursor = fixture.player().deck_idx;
            const auto expected = fixture.player().deck[cursor];
            fixture.run({Play, 0});
            CHECK(fixture.player().deck_idx == cursor + 1);
            CHECK(fixture.player().hand_count == 1 && fixture.player().hand[0] == expected);
        }
    });
    suite.add("phase2_audit.state_cache.competing_amulet_branches_stay_distinct", [] {
        Scenario fixture;
        fixture.hand({official(11032310)});
        fixture.place(official(90021110), 1).hp = 5;
        Scenario acted = fixture;
        acted.player().acted_amulet = true;
        std::unordered_map<GameState, bool, PositionHash> seen;
        CHECK(seen.try_emplace(fixture.state, false).second);
        CHECK(seen.try_emplace(acted.state, true).second);
        CHECK(seen.size() == 2 && !seen.at(fixture.state) && seen.at(acted.state));
        acted.run({Play, 0});
        CHECK(acted.player(1).board[0].hp == 2 && fixture.player(1).board[0].hp == 5);
        fixture.run({Play, 0});
        CHECK(fixture.player(1).board[0].hp == 5);
        CHECK(acted.player().board[0].card_id == kEvidence && fixture.player().board[0].card_id == kEvidence);
    });

    suite.add("phase2_audit.observation_seed.public_crest_and_hidden_rng", [] {
        Scenario fixture;
        fixture.crest(CrestId::Testimony, 2);
        auto hidden = fixture.state;
        ++hidden.rng_state;
        std::swap(hidden.players[0].deck[hidden.players[0].deck_idx],
                  hidden.players[0].deck[hidden.players[0].deck_idx + 1]);
        CHECK(same_observation(fixture.state, hidden, 0));
        CHECK(observation_hash_for(fixture.state, 0) == observation_hash_for(hidden, 0));
        auto public_change = fixture.state;
        ++public_change.players[0].crests[0].countdown;
        CHECK(!same_observation(fixture.state, public_change, 0));
    });

    suite.add("phase2_audit.legacy.production_pair_and_card_id_closure", [] {
        const std::filesystem::path directory(SVWB_SPEC_DECKS_PATH);
        std::vector<std::pair<std::string, Deck>> decks;
        for (const auto& file : std::filesystem::directory_iterator(directory)) {
            if (file.path().extension() == ".json") {
                decks.emplace_back(file.path().stem().string(), read_deck(file.path()));
            }
        }
        CHECK(decks.size() == 7);
        int legacy_pairs = 0;
        for (std::size_t i = 0; i < decks.size(); ++i) {
            for (std::size_t j = i; j < decks.size(); ++j) {
                if (!MatchupFeatures(decks[i].second, decks[j].second).legacy_mirror()) {
                    continue;
                }
                CHECK(decks[i].first == "bishop" && decks[j].first == "bishop");
                ++legacy_pairs;
            }
        }
        CHECK(legacy_pairs == 1);

        // The reviewed operations below create cards only from explicit operands, except the
        // listed custom handlers which evolve/remove existing objects. Any new operation in
        // this closure requires revisiting the proof.
        const std::set<Operation> reviewed{
            Operation::AddHand, Operation::Banish, Operation::Damage, Operation::Draw, Operation::ExtendCrest,
            Operation::GrantCrest, Operation::Heal, Operation::ReduceCountdown, Operation::RestorePp,
            Operation::Silence, Operation::SpendClue, Operation::Summon, Operation::SummonEvolved,
            Operation::SummonModified,
        };
        std::set<CardId> domain(kMirrorDeck.begin(), kMirrorDeck.end());
        std::vector<CardId> cards(domain.begin(), domain.end());
        std::set<CrestId> crests;
        const auto add_card_id = [&](CardId id) {
            if (id != 0 && domain.insert(id).second) cards.push_back(id);
        };
        const auto expand = [&](auto&& self, const std::array<Effect, kEffectSlots>& effects) -> void {
            for (const auto& step : effects) {
                if (step.trigger == Trigger::None) continue;
                if (step.operation == Operation::Custom) {
                    CHECK(step.handler == CardHandler::EvolveWithAlly || step.handler == CardHandler::BanishLowestAndEvolve);
                } else {
                    CHECK(reviewed.contains(step.operation));
                }
                add_card_id(step.card);
                add_card_id(step.alternate_card);
                if (step.crest != CrestId::None && crests.insert(step.crest).second) {
                    const auto* crest = find_crest(step.crest);
                    CHECK(crest);
                    self(self, crest->effects);
                }
            }
        };
        for (std::size_t i = 0; i < cards.size(); ++i) {
            const auto* card = find_card(cards[i]);
            CHECK(card && card->implemented);
            CHECK(card->faith == 0 && card->fusion == FusionMaterial::None && !card->direct_summon_cost_curve);
            add_card_id(card->crystal_form);
            add_card_id(card->accelerate_form);
            expand(expand, card->effects);
        }
        CHECK(domain.size() == 21);
        std::set<unsigned> encoded;
        for (const auto id : domain) CHECK(encoded.insert(id & 255U).second);
        CHECK(domain.contains(official(11001110)) && !domain.contains(official(90022110)));
    });

    suite.add("phase2_audit.official_faq.soul_predation_funds_gremory_last_words", [] {
        Scenario fixture;
        fixture.place(official(11054120));
        fixture.hand({official(10052310)});
        auto& player = fixture.player();
        CHECK(player.shadows == 0);
        const auto cursor = player.deck_idx;
        const std::array expected{kTrainer, kAlmiraj, kHolyFalcon};
        std::copy(expected.begin(), expected.end(), player.deck.begin() + cursor);
        fixture.run({Play, 0, kOwnBoardTarget});
        CHECK(player.board_count == 0 && player.shadows == 0);
        CHECK(player.deck_idx == cursor + 3 && player.hand_count == 3);
        CHECK(std::equal(expected.begin(), expected.end(), player.hand.begin()));

        Scenario combat;
        combat.place(official(11054120));
        auto& attacker = combat.place(official(90021110), 1);
        attacker.atk = attacker.hp = attacker.max_hp = 20;
        combat.state.turn_player = 1;
        combat.run({Attack, 0, 0});
        CHECK(combat.player().board_count == 0 && combat.player().shadows == 1);
        CHECK(combat.player().hand_count == 0);
    });

}

} // namespace svwb::wb::spec
