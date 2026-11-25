#include "wb_cards.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace svwb::wb {
namespace {

using enum Trigger;

constexpr int kExtraPpRefillTurn = 6;
constexpr EntityId kLeaderTargetFlag = EntityId{1} << 30;

struct Ref {
    int seat = 0;
    EntityId order = 0;

    bool operator==(const Ref&) const = default;
};

bool has(const Permanent& object, std::uint16_t abilities) {
    return (object.abilities & abilities) != 0;
}

const CardDef& definition(CardId id) {
    const auto* card = find_card(id);
    assert(card != nullptr);
    return *card;
}

bool follower(const Permanent& object) {
    return definition(object.card_id).kind == Kind::Follower;
}

void set_evolved_form(Permanent& object, Evolution evolution) {
    object.evolution = evolution;
    object.abilities |= FLAG_RUSH;
    const int bonus = evolution == Evolution::Super ? 3 : 2;
    object.atk = static_cast<std::int16_t>(object.atk + bonus);
    object.hp = static_cast<std::int16_t>(object.hp + bonus);
    object.max_hp = static_cast<std::int16_t>(object.max_hp + bonus);
}

bool legacy_counter(CardId card) {
    return card >= kFirstCard && card <= kLastForm;
}

CardCounters& ensure_counter(PlayerState& player, CardId id) {
    auto end = player.other_counters.begin() + player.other_counter_count;
    auto item = std::find_if(player.other_counters.begin(), end, [id](const auto& entry) { return entry.id == id; });
    if (item == end) {
        if (player.other_counter_count == kAdditionalCardCounters) {
            throw std::logic_error("Too many distinct card counters in one match");
        }
        *item = {id, 0, {}};
        ++player.other_counter_count;
    }
    return *item;
}

NamedActions& counts(PlayerState& player, CardId card) {
    const auto id = printed_card(card);
    return legacy_counter(id) ? player.actions[id - kFirstCard] : ensure_counter(player, id).actions;
}

NamedActions used_actions(const PlayerState& player, CardId card) {
    const auto id = printed_card(card);
    if (legacy_counter(id)) return player.actions[id - kFirstCard];
    for (int i = 0; i < player.other_counter_count; ++i) {
        if (player.other_counters[i].id == id) return player.other_counters[i].actions;
    }
    return {};
}

int ply(const GameState& state) {
    return 2 * (state.turn_number - 1) + state.turn_player + 1;
}

Permanent* find(GameState& state, Ref ref) {
    if (ref.order == 0) {
        return nullptr;
    }
    auto& player = state.players[ref.seat];
    const auto end = player.board.begin() + player.board_count;
    const auto position = std::find_if(player.board.begin(), end, [&](const auto& object) {
        return object.creation_order == ref.order;
    });
    return position == end ? nullptr : &*position;
}

Crest* find(GameState& state, int seat, EntityId order) {
    auto& player = state.players[seat];
    const auto end = player.crests.begin() + player.crest_count;
    const auto position = std::find_if(player.crests.begin(), end, [&](const auto& crest) {
        return crest.creation_order == order;
    });
    return position == end ? nullptr : &*position;
}

template<class T, std::size_t N>
void erase(std::array<T, N>& array, std::uint8_t& count, int index) {
    std::move(array.begin() + index + 1, array.begin() + count, array.begin() + index);
    array[--count] = {};
}

int clue_slot(const PlayerState& player) {
    for (int i = 0; i < player.board_count; ++i) {
        if (player.board[i].clue_count != 0) {
            return i;
        }
    }
    return -1;
}

int earth_slot(const PlayerState& player) {
    for (int i = 0; i < player.board_count; ++i) {
        if (player.board[i].earth_count != 0) return i;
    }
    return -1;
}

bool can_enter(const PlayerState& player, const CardDef& card, const RuleConfig& rules) {
    return card.kind == Kind::Spell || (card.kind != Kind::Faith &&
        (player.board_count < kBoardLimit || (card.earth && earth_slot(player) >= 0) ||
         (card.clue && rules.merge_clue_on_full_board && clue_slot(player) >= 0)));
}

std::uint64_t random_word(GameState& state) {
    auto word = (state.rng_state += 0x9e3779b97f4a7c15ULL);
    word = (word ^ (word >> 30)) * 0xbf58476d1ce4e5b9ULL;
    word = (word ^ (word >> 27)) * 0x94d049bb133111ebULL;
    return word ^ (word >> 31);
}

int random_index(GameState& state, int count) {
    assert(count > 0);
    const auto bound = static_cast<std::uint64_t>(count);
    const auto threshold = (std::uint64_t{0} - bound) % bound;
    std::uint64_t word;
    do {
        word = random_word(state);
    } while (word < threshold);
    return static_cast<int>(word % bound);
}

void shuffle(GameState& state, PlayerState& player, int first) {
    for (int i = player.deck_end - 1; i > first; --i) {
        const int other = first + random_index(state, i - first + 1);
        std::swap(player.deck[i], player.deck[other]);
        std::swap(player.deck_modifiers[i], player.deck_modifiers[other]);
    }
}

void spend(PlayerState& player, int amount) {
    const int regular = std::min(amount, static_cast<int>(player.pp));
    player.pp = static_cast<std::uint8_t>(player.pp - regular);
    player.bonus_pp = static_cast<std::uint8_t>(player.bonus_pp - (amount - regular));
}

void add_card(PlayerState& player, CardId id, CardModifiers modifiers = {}) {
    if (player.hand_count == kHandLimit) {
        ++player.shadows;
    } else {
        player.hand[player.hand_count] = id;
        player.hand_modifiers[player.hand_count++] = modifiers;
    }
}

void remember_generated(PlayerState& player, CardId id) {
    if (player.public_generated_count < player.public_generated.size()) {
        player.public_generated[player.public_generated_count++] = id;
    }
}

void add_public_card(PlayerState& player, CardId id, CardModifiers modifiers = {}) {
    if (player.hand_count < kHandLimit) remember_generated(player, id);
    add_card(player, id, modifiers);
}

void reveal_played_card(PlayerState& player, CardId id) {
    const auto end = player.public_generated.begin() + player.public_generated_count;
    const auto generated = std::find(player.public_generated.begin(), end, id);
    if (generated != end) {
        erase(player.public_generated, player.public_generated_count,
              static_cast<int>(generated - player.public_generated.begin()));
        return;
    }
    for (std::size_t i = 0; i < player.public_deck.size(); ++i) {
        const auto bit = std::uint64_t{1} << i;
        if (player.public_deck[i] == id && !(player.public_spent & bit)) {
            player.public_spent |= bit;
            return;
        }
    }
}

void erase_hand(PlayerState& player, int index) {
    std::move(player.hand_modifiers.begin() + index + 1, player.hand_modifiers.begin() + player.hand_count,
              player.hand_modifiers.begin() + index);
    player.hand_modifiers[player.hand_count - 1] = {};
    erase(player.hand, player.hand_count, index);
}

void return_to_deck(GameState& state, PlayerState& player, CardId id, CardModifiers modifiers) {
    if (player.deck_end == kDeckCapacity) {
        if (player.deck_idx == 0) throw std::logic_error("Deck capacity exceeded");
        const auto remaining = player.deck_remaining();
        std::move(player.deck.begin() + player.deck_idx, player.deck.begin() + player.deck_end, player.deck.begin());
        std::move(player.deck_modifiers.begin() + player.deck_idx, player.deck_modifiers.begin() + player.deck_end,
                  player.deck_modifiers.begin());
        player.deck_end = static_cast<std::uint8_t>(remaining);
        player.deck_idx = 0;
    }
    const auto slot = player.deck_idx + random_index(state, player.deck_remaining() + 1);
    std::move_backward(player.deck.begin() + slot, player.deck.begin() + player.deck_end, player.deck.begin() + player.deck_end + 1);
    std::move_backward(player.deck_modifiers.begin() + slot, player.deck_modifiers.begin() + player.deck_end,
                       player.deck_modifiers.begin() + player.deck_end + 1);
    player.deck[slot] = id;
    player.deck_modifiers[slot] = modifiers;
    ++player.deck_end;
}

void lose(GameState& state, int seat, EndReason reason) {
    state.outcome = seat == 0 ? Outcome::SecondWins : Outcome::FirstWins;
    state.end_reason = reason;
}

void draw(GameState& state, int seat, int amount) {
    auto& player = state.players[seat];
    for (int i = 0; i < amount; ++i) {
        if (player.deck_idx == player.deck_end) {
            lose(state, seat, EndReason::EmptyDeck);
            return;
        }
        const auto slot = player.deck_idx++;
        add_card(player, player.deck[slot], player.deck_modifiers[slot]);
    }
}

struct PlayCost {
    const CardDef& form;
    int cost;
    bool enhanced;
};

PlayCost play_cost(const CardDef& card, int pp, CardModifiers modifiers, bool suppressed = false) {
    if (!(suppressed && card.kind == Kind::Follower)) {
        if (const int enhanced = enhance_payment(card, pp)) return {card, enhanced, true};
    }
    const auto price = card_cost(card, modifiers);
    const auto alternate = card.crystal_form ? card.crystal_form : card.accelerate_form;
    if (pp < price && alternate != 0) {
        const auto& form = definition(alternate);
        return {form, form.cost, false};
    }
    return {card, price, false};
}

int play_mode(const PlayCost& payment, int choice) {
    return payment.enhanced && !payment.form.enhanced_all_modes ? kEnhancedMode : choice;
}

EntityId chosen_entity(const GameState& state, Action action, bool consumes_source = false) {
    if (action.dst == kEnemyLeader) return kLeaderTargetFlag;
    if (action.dst < 0) return 0;
    const int seat = state.turn_player;
    if (action.dst >= kOwnHandTarget) {
        const int slot = action.dst - kOwnHandTarget;
        return kHandTargetFlag | static_cast<EntityId>(slot - (consumes_source && slot > action.src));
    }
    if (action.dst >= kOwnBoardTarget) {
        return state.players[seat].board[action.dst - kOwnBoardTarget].creation_order;
    }
    return state.players[1 - seat].board[action.dst].creation_order;
}

bool takes_mode(const Effect& effect, int mode) {
    if (mode & kModeMask) {
        return effect.mode == 0 || (effect.mode >= 1 && effect.mode <= 4 && (mode & (1 << (effect.mode - 1))));
    }
    return effect.mode == 0 || effect.mode == mode ||
        (mode == kAllModes && effect.mode < kEnhancedMode) || (mode == 0 && effect.mode == kBaseModeOnly);
}

int amulets(const PlayerState& player) {
    return static_cast<int>(std::count_if(player.board.begin(), player.board.begin() + player.board_count,
        [](const Permanent& object) { return !follower(object); }));
}

bool board_cost(const PlayerState& player, int cost, bool minimum, EntityId except = 0, EntityId other = 0) {
    return std::any_of(player.board.begin(), player.board.begin() + player.board_count, [&](const Permanent& object) {
        const int price = definition(object.card_id).cost;
        return object.creation_order != except && object.creation_order != other && (minimum ? price >= cost : price == cost);
    });
}

bool has_trait(const CardDef& card, std::string_view trait) {
    auto traits = card.traits;
    while (!traits.empty()) {
        const auto separator = traits.find('\n');
        if (traits.substr(0, separator) == trait) return true;
        if (separator == std::string_view::npos) break;
        traits.remove_prefix(separator + 1);
    }
    return false;
}

constexpr std::string_view kFairyTrait = "妖精";
constexpr std::string_view kRoyalClass = "ロイヤル";
constexpr std::string_view kNightmareClass = "ナイトメア";
constexpr std::string_view kNemesisClass = "ネメシス";
constexpr std::string_view kMarineTrait = "海洋";
constexpr std::string_view kDeadName = "死者";
constexpr std::string_view kGolemTrait = "ゴーレム";
constexpr std::string_view kArtifactTrait = "アーティファクト";
constexpr std::string_view kPuppetTrait = "人形";
constexpr std::string_view kManariaTrait = "マナリア";

bool is_large_follower(const CardDef& card) {
    return card.kind == Kind::Follower && card.cost >= 5;
}

bool has_deck_duplicates(const PlayerState& player, const RuleConfig& rules) {
    for (int i = player.deck_idx; i < player.deck_end; ++i) {
        for (int j = i + 1; j < player.deck_end; ++j) {
            if (player.deck[i] == player.deck[j] || (rules.highlander_counts_card_names &&
                definition(player.deck[i]).name == definition(player.deck[j]).name)) return true;
        }
    }
    return false;
}

void modify_hand_cost(CardModifiers& modifiers, int delta, bool temporary) {
    auto& cost = temporary ? modifiers.temporary_cost_delta : modifiers.cost;
    cost = static_cast<std::int8_t>(std::clamp(cost + delta, -127, 127));
}

void boost_card(const CardDef& card, CardModifiers& modifiers, int count) {
    if (card.spellboost) modifiers.spellboost = static_cast<std::uint8_t>(std::min(255, modifiers.spellboost + count));
}

bool has_last_words(const Permanent& object) {
    if (object.silenced) return false;
    if (object.modifiers.effect_flags & kRebirthLastWords) return true;
    if (object.modifiers.effect_flags & kLoseLastWords) return false;
    const auto& effects = definition(object.card_id).effects;
    return std::any_of(effects.begin(), effects.end(), [](const auto& step) { return step.trigger == LastWords; });
}

struct ChoiceTarget {
    Target target = Target::None;
    CardId card = 0;
};

ChoiceTarget chosen_target(const CardDef& card, const PlayerState& player, Trigger trigger, int mode,
                           const RuleConfig& rules, bool super = false, bool fused = false) {
    bool cost_available = true;
    for (const auto& effect : card.effects) {
        if (effect.trigger != trigger || !takes_mode(effect, mode) ||
            (super && effect.condition == Condition::NormalEvolution) ||
            (effect.condition == Condition::OwnAmulets && amulets(player) < effect.threshold) ||
            (effect.condition == Condition::Combo && player.plays_this_turn + (trigger == OnPlay) < effect.threshold) ||
            (effect.condition == Condition::BelowCombo && player.plays_this_turn + (trigger == OnPlay) >= effect.threshold)) continue;
        if (effect.operation == Operation::EarthRite) {
            const int slot = earth_slot(player);
            cost_available = slot >= 0 && player.board[slot].earth_count >= effect.amount;
        }
        if ((effect.condition == Condition::CostPaid && !cost_available) ||
            (effect.condition == Condition::Fused && !fused) ||
            (effect.condition == Condition::NoDeckDuplicates && has_deck_duplicates(player, rules)) ||
            (effect.condition == Condition::HasDeckDuplicates && !has_deck_duplicates(player, rules))) continue;
        switch (effect.target) {
        case Target::ChosenEnemyFollower: case Target::ChosenEnemyPermanent: case Target::ChosenOwnFollower:
        case Target::ChosenOwnPermanent: case Target::ChosenOwnHand: case Target::ChosenSmallEnemyFollower:
        case Target::ChosenOtherOwnFollower: case Target::ChosenOtherPermanent:
        case Target::ChosenOwnHandThree: case Target::ChosenOtherOwnTwoFollowers:
        case Target::ChosenOwnHandTwo: case Target::ChosenOwnSpell: case Target::ChosenUnevolvedOwnFollower:
        case Target::ChosenEnemyOrLeader:
        case Target::ChosenEnemyTwoFollowers: case Target::ChosenHandAndEnemy: case Target::ChosenOwnHandFollower:
        case Target::ChosenOwnNamedFollower: case Target::ChosenOwnGolem: case Target::ChosenOtherUnevolvedOwnFollower:
        case Target::ChosenOwnLargeFollower: case Target::ChosenBoostedHand: case Target::ChosenAffordableArtifactHand:
        case Target::ChosenOtherThreePermanents: case Target::ChosenOtherFollower:
            return {effect.target, effect.card};
        default: break;
        }
    }
    return {};
}

bool can_attack_target(const Permanent& object) {
    return follower(object) && !has(object, kAmbush | kIntimidate);
}

enum class Origin : std::uint8_t {
    Board, Spell, Removed, Crest, ExpiredCrest, SuperDamage, Banished, DelayedBanish, Faith, Rebirth, DirectSummon,
};

struct Ability {
    Trigger trigger;
    Origin origin;
    Ref source;
    CardId card = 0;
    CrestId crest = CrestId::None;
    EntityId chosen = 0;
    int mode = 0;
    int spellboost = 0;
    int fused_materials = 0;
    std::uint16_t selection = 0;
    EntityId event_order = 0;
    int event_cost = 0;
    Permanent event_object{};
    CardId event_card = 0;
    int effective_cost = 0;
    int enhance_cost = 0;
    int discarded_slot = -1;
};

bool fusion_matches(const CardDef& host, const CardDef& material) {
    switch (host.fusion) {
    case FusionMaterial::None: return false;
    case FusionMaterial::Any: return true;
    case FusionMaterial::Follower: return material.kind == Kind::Follower;
    case FusionMaterial::Spell: return material.kind == Kind::Spell;
    case FusionMaterial::Amulet: return material.kind == Kind::Amulet;
    case FusionMaterial::SameClass: return material.class_name == host.class_name;
    case FusionMaterial::Trait: {
        return has_trait(material, host.fusion_trait);
    }
    case FusionMaterial::AmuletTrait:
        return material.kind == Kind::Amulet && has_trait(material, host.fusion_trait);
    case FusionMaterial::NamedPair:
        return material.id == host.fusion_materials[0] || material.id == host.fusion_materials[1];
    }
    return false;
}

bool condition_met(GameState& state, const Effect& effect, const Ability& ability, bool paid, const RuleConfig& rules) {
    const int owner = ability.source.seat;
    const auto& player = state.players[owner];
    const auto* source = find(state, ability.source);
    const auto evolved = [&](Evolution form) {
        return std::any_of(player.board.begin(), player.board.begin() + player.board_count,
            [&](const Permanent& object) { return object.evolution == form; });
    };
    switch (effect.condition) {
    case Condition::Always: return true;
    case Condition::Combo: return player.plays_this_turn >= effect.threshold;
    case Condition::Rally: return player.followers_entered >= effect.threshold;
    case Condition::Overflow: return player.overflow();
    case Condition::CostPaid: return paid;
    case Condition::AtMaxPp: return player.max_pp == kMaxPp;
    case Condition::Fused: return ability.fused_materials > 0;
    case Condition::NotFused: return ability.fused_materials == 0;
    case Condition::OwnTurn: return state.turn_player == owner;
    case Condition::OwnAmulets: return amulets(player) >= effect.threshold;
    case Condition::BoardCostAtLeast: return board_cost(player, effect.threshold, true);
    case Condition::OtherBoardCost: return board_cost(player, effect.threshold, false, ability.source.order) ||
        board_cost(state.players[1 - owner], effect.threshold, false);
    case Condition::FollowerAttack: return ability.chosen != 0;
    case Condition::SelfEvolved: return source && source->evolution != Evolution::None;
    case Condition::NormalEvolution: return source && source->evolution == Evolution::Normal;
    case Condition::OwnSuper: return evolved(Evolution::Super);
    case Condition::OwnEvolvedWithoutSuper: return evolved(Evolution::Normal) && !evolved(Evolution::Super);
    case Condition::PlayedOneThroughEight: return (player.played_cost_mask & 0x1fe) == 0x1fe;
    case Condition::BelowCombo: return player.plays_this_turn < effect.threshold;
    case Condition::BelowRally: return player.followers_entered < effect.threshold;
    case Condition::OwnCostOneCards:
    case Condition::OtherOwnCostOneCards:
    case Condition::BelowOwnCostOneCards: {
        const auto count = std::count_if(player.board.begin(), player.board.begin() + player.board_count, [&](const auto& object) {
            return definition(object.card_id).cost == 1 &&
                (effect.condition != Condition::OtherOwnCostOneCards || object.creation_order != ability.source.order);
        });
        return effect.condition == Condition::BelowOwnCostOneCards ? count < effect.threshold : count >= effect.threshold;
    }
    case Condition::EventFairy: return ability.event_card && has_trait(definition(ability.event_card), kFairyTrait);
    case Condition::EventSpell: return ability.event_card && definition(ability.event_card).kind == Kind::Spell;
    case Condition::EvidenceActed: return player.acted_evidence;
    case Condition::EvidenceUnused: return !player.acted_evidence;
    case Condition::HandSpells:
        return std::count_if(player.hand.begin(), player.hand.begin() + player.hand_count,
            [](CardId id) { return definition(id).kind == Kind::Spell; }) >= effect.threshold;
    case Condition::SuperTurn: return state.turn_number >= 7 - owner;
    case Condition::EnhanceAtLeast: return ability.enhance_cost >= effect.threshold;
    case Condition::EffectiveCost: return ability.effective_cost == effect.threshold;
    case Condition::NotOverflow: return !player.overflow();
    case Condition::PreviousFaceAttack: return player.attacked_leader_last_turn;
    case Condition::NoPreviousFaceAttack: return !player.attacked_leader_last_turn;
    case Condition::SelfUnevolved: return source && source->evolution == Evolution::None;
    case Condition::SelfSuper: return source && source->evolution == Evolution::Super;
    case Condition::OwnHpAtMost: return player.leader_hp <= effect.threshold;
    case Condition::OwnHpAhead: return player.leader_hp > state.players[1 - owner].leader_hp;
    case Condition::EventMarine: return ability.event_card && has_trait(definition(ability.event_card), kMarineTrait);
    case Condition::EventDead: case Condition::OwnTurnDeadEntry:
        return (effect.condition == Condition::EventDead || owner == state.turn_player) &&
             ability.event_card && ((ability.event_object.modifiers.effect_flags & kDeadTrait) ||
             has_trait(definition(ability.event_card), kDeadName));
    case Condition::EventNightmare: return ability.event_card && definition(ability.event_card).class_name == kNightmareClass;
    case Condition::EventNamed: return ability.event_card == effect.card;
    case Condition::HasChosen: return ability.chosen != 0;
    case Condition::BoostAtLeast: return ability.spellboost >= effect.threshold;
    case Condition::ActedAmulet: return player.acted_amulet;
    case Condition::EnteredSame: {
        const auto end = player.other_counters.begin() + player.other_counter_count;
        const auto counter = std::find_if(player.other_counters.begin(), end,
            [&](const auto& entry) { return entry.id == ability.card; });
        return counter != end && counter->entered > effect.threshold;
    }
    case Condition::EventGolem: return ability.event_card && has_trait(definition(ability.event_card), kGolemTrait);
    case Condition::EventArtifact: return ability.event_card && has_trait(definition(ability.event_card), kArtifactTrait);
    case Condition::EventPuppet: return ability.event_card && has_trait(definition(ability.event_card), kPuppetTrait);
    case Condition::EventSoldier: return ability.event_card && has_trait(definition(ability.event_card), "兵士");
    case Condition::EventLarge: return ability.event_card && is_large_follower(definition(ability.event_card));
    case Condition::EventManaria:
        return ability.event_card && has_trait(definition(ability.event_card), kManariaTrait);
    case Condition::EventEvolvedSpell:
        return source && source->evolution != Evolution::None && ability.event_card && definition(ability.event_card).kind == Kind::Spell;
    case Condition::AtLeastArtifacts: return player.artifact_types_entered >= effect.threshold;
    case Condition::BelowArtifacts: return player.artifact_types_entered < effect.threshold;
    case Condition::NoDeckDuplicates: return !has_deck_duplicates(player, rules);
    case Condition::HasDeckDuplicates: return has_deck_duplicates(player, rules);
    case Condition::OwnLargeFollower:
        return std::any_of(player.board.begin(), player.board.begin() + player.board_count,
            [](const auto& object) { return is_large_follower(definition(object.card_id)); });
    case Condition::CostMultiplicity:
        for (int i = 0; i < player.hand_count; ++i) {
            const int price = card_cost(definition(player.hand[i]), player.hand_modifiers[i]);
            int copies = 1;
            for (int j = i + 1; j < player.hand_count; ++j) {
                copies += price == card_cost(definition(player.hand[j]), player.hand_modifiers[j]);
            }
            if (copies >= effect.threshold) return true;
        }
        return false;
    }
    return false;
}

void grant_faith(PlayerState& player, CardId card) {
    const auto end = player.faiths.begin() + player.faith_count;
    if (player.faith_count + player.crest_count < kCrestLimit && definition(card).kind == Kind::Faith &&
        std::find(player.faiths.begin(), end, card) == end) {
        player.faiths[player.faith_count++] = card;
    }
}

const std::array<Effect, kEffectSlots>& script(const Ability& ability) {
    if (ability.crest != CrestId::None) {
        const auto* crest = find_crest(ability.crest);
        assert(crest != nullptr);
        return crest->effects;
    }
    return definition(ability.card).effects;
}

class Resolver {
public:
    Resolver(GameState& state, const RuleConfig& rules, bool* certain = nullptr,
             LethalEvidence* evidence = nullptr, std::uint16_t* unobserved_hand_mask = nullptr,
             int observer = -1)
        : state_(state), rules_(rules), certain_(certain), evidence_(evidence),
          unobserved_hand_mask_(unobserved_hand_mask),
          observer_(observer >= 0 ? observer : state.turn_player) {}

    void play(Action action);
    void manual_evolve(Action action);
    void act(Action action);
    void attack(Action action);
    void fuse(Action action);
    void start_turn();
    void end_turn();
    void immediate(int owner, std::span<const Effect> effects, EntityId source, EntityId target);

private:
    void mark_chance() const {
        if (certain_) *certain_ = false;
        if (evidence_) evidence_->crossed_unresolved_chance = true;
    }
    void mark_hidden() const {
        if (certain_) *certain_ = false;
        if (evidence_) evidence_->depends_on_hidden_information = true;
    }
    void mark_unobserved_card() const {
        if (certain_) *certain_ = false;
        if (evidence_) evidence_->used_unobserved_card = true;
    }
    void uncertain() const {
        mark_chance();
        mark_hidden();
    }
    bool has_unobserved_hand(int seat) const {
        return seat == observer_ && unobserved_hand_mask_ && *unobserved_hand_mask_ != 0;
    }
    bool slot_is_unobserved(int seat, int slot) const {
        return seat == observer_ && unobserved_hand_mask_ && slot >= 0 && slot < kHandLimit &&
               ((*unobserved_hand_mask_ & static_cast<std::uint16_t>(1U << slot)) != 0);
    }
    void add_unobserved_card(int seat, CardId id, CardModifiers modifiers = {}) {
        auto& side = state_.players[seat];
        if (seat == observer_) {
            if (side.hand_count == kHandLimit) {
                mark_unobserved_card();
            } else if (unobserved_hand_mask_) {
                *unobserved_hand_mask_ |= static_cast<std::uint16_t>(1U << side.hand_count);
            }
        }
        add_card(side, id, modifiers);
    }
    void erase_player_hand(int seat, int slot) {
        if (seat == observer_ && unobserved_hand_mask_ && slot >= 0 && slot < kHandLimit) {
            const auto bit = static_cast<std::uint16_t>(1U << slot);
            if ((*unobserved_hand_mask_ & bit) != 0) mark_unobserved_card();
            const auto low = static_cast<std::uint16_t>(*unobserved_hand_mask_ & (bit - 1U));
            const auto high = static_cast<std::uint16_t>((*unobserved_hand_mask_ >> 1) & ~(bit - 1U));
            *unobserved_hand_mask_ = static_cast<std::uint16_t>(low | high);
        }
        erase_hand(state_.players[seat], slot);
    }
    int pick_index(int count) {
        if (count > 1) mark_chance();
        return random_index(state_, count);
    }
    void enqueue(Ability ability);
    void board_ability(Ref ref, Trigger trigger, EntityId chosen = 0, int mode = 0, std::uint16_t selection = 0,
                       int effective_cost = 0, int enhanced = 0);
    void broadcast(int seat, const Ability& event, bool board = true, bool crests = true);
    void progress_faith(int seat, Trigger trigger, CardId event_card = 0);
    void flush();
    void resolve();
    void run_steps(const Ability& ability, std::span<const Effect> effects);
    void check_health(int source);
    void settle(Ref cause);
    bool protected_super(Ref ref, const Permanent& object) const;
    bool protected_destruction(Ref ref, const Permanent& object) const;
    void damage(Ref target, int amount);
    int damage_leader(int seat, int amount);
    void heal_leader(int seat, int amount);
    void destroy(Ref target, Ref cause, bool by_ability, std::vector<Ability>* deferred = nullptr);
    void banish(Ref target);
    Ref copy_board(int owner, Permanent copy);
    void collect_dead(Ref cause);
    void evolve(Ref ref, Evolution evolution, bool manual, EntityId chosen = 0, std::uint16_t selection = 0, int mode = 0);
    void transform(Ref ref, CardId card, CardModifiers modifiers, Evolution evolution);
    void effect(const Ability& ability, const Effect& effect, EntityId random_target);
    void custom_effect(const Ability& ability, const Effect& step, Ref target, int hand_slot);
    int draw_cards(int seat, int amount, DeckFilter filter = DeckFilter::Any, int discount = 0);
    void keep_drawn(int seat, CardId card, CardModifiers modifiers, int discount = 0);
    void discard(int seat, int slot);
    CardId past_card(int seat, int limit, bool highest, bool amulets, std::span<const CardId> excluded = {},
                     DeckFilter filter = DeckFilter::Any);
    EntityId random_enemy(int owner);
    Ref enter(int seat, CardId card, CardModifiers modifiers = {});
    void reanimate(int seat, int cost);
    void grant_crest(int seat, CrestId id);
    void destroy_crest(int seat, EntityId order, bool expired);
    void tick_crests(int seat, CrestClock clock);
    void refresh(int seat);
    void direct_summon(int seat);

    GameState& state_;
    const RuleConfig& rules_;
    bool* certain_;
    LethalEvidence* evidence_;
    std::uint16_t* unobserved_hand_mask_;
    int observer_;
    std::vector<Ability> waiting_;
    std::vector<Ability> triggered_;
    Ref super_attacker_{};
    bool super_damage_queued_ = false;
    unsigned fanfare_repeats_ = 0;
};

void Resolver::enqueue(Ability ability) {
    const auto& effects = script(ability);
    if (std::any_of(effects.begin(), effects.end(), [&](const Effect& effect) {
            return effect.trigger == ability.trigger && takes_mode(effect, ability.mode);
        })) {
        triggered_.push_back(ability);
    }
}

void Resolver::board_ability(Ref ref, Trigger trigger, EntityId chosen, int mode, std::uint16_t selection,
                             int effective_cost, int enhanced) {
    if (const auto* object = find(state_, ref); object && !object->silenced) {
        Ability ability{trigger, Origin::Board, ref, object->card_id, CrestId::None, chosen, mode,
                        object->modifiers.spellboost, object->modifiers.fused_materials, selection};
        ability.effective_cost = effective_cost;
        ability.enhance_cost = enhanced;
        enqueue(ability);
    }
}

void Resolver::progress_faith(int seat, Trigger trigger, CardId event_card) {
    auto& player = state_.players[seat];
    for (int i = 0; i < player.faith_count; ++i) {
        const auto card = player.faiths[i];
        const auto& def = definition(card);
        if (def.faith_trigger != trigger || (def.faith_event_card && def.faith_event_card != event_card)) continue;
        if (player.faith_values[i] < std::numeric_limits<std::uint16_t>::max()) ++player.faith_values[i];
        for (int upgrade = 0; upgrade < player.faith_upgrades[i]; ++upgrade) {
            enqueue({trigger, Origin::Faith, {seat}, card});
        }
    }
}

void Resolver::broadcast(int seat, const Ability& event, bool board, bool crests) {
    const auto& player = state_.players[seat];
    auto ability = event;
    if (board) {
        ability.origin = Origin::Board;
        ability.crest = CrestId::None;
        for (int i = 0; i < player.board_count; ++i) {
            const auto& object = player.board[i];
            if (object.silenced || object.creation_order == event.event_order) continue;
            ability.source = {seat, object.creation_order};
            ability.card = object.card_id;
            enqueue(ability);
        }
    }
    if (crests) {
        ability.origin = Origin::Crest;
        for (int i = 0; i < player.crest_count; ++i) {
            ability.source = {seat, player.crests[i].creation_order};
            ability.crest = player.crests[i].id;
            enqueue(ability);
        }
    }
}

void Resolver::flush() {
    const auto priority = [&](const Ability& ability) {
        if (ability.origin == Origin::DirectSummon) return Priority::Deck;
        if (ability.trigger == OnPlay || ability.trigger == OnEvolve ||
            ability.trigger == OnSuperEvolve || ability.trigger == OnAct || ability.trigger == OnFuse) {
            return Priority::Manual;
        }
        if (ability.origin == Origin::Removed || ability.origin == Origin::Banished || ability.origin == Origin::Rebirth) {
            return Priority::LastWords;
        }
        if (ability.origin == Origin::Crest || ability.origin == Origin::ExpiredCrest || ability.origin == Origin::Faith) {
            return ability.source.seat == state_.turn_player ? Priority::ActiveCrest : Priority::OtherCrest;
        }
        if (ability.trigger == OnEnter || ability.trigger == OnEvolved) {
            return Priority::Transition;
        }
        return ability.source.seat == state_.turn_player ? Priority::ActiveBoard : Priority::OtherBoard;
    };
    if (triggered_.size() > 1) {
        std::stable_sort(triggered_.begin(), triggered_.end(), [&](const auto& a, const auto& b) {
            const auto rank = [&](const Ability& ability) {
                return std::tuple{rules_.priority[static_cast<int>(priority(ability))],
                                  ability.source.seat != state_.turn_player, ability.source.order};
            };
            return rank(a) < rank(b);
        });
    }
    waiting_.insert(waiting_.end(), triggered_.begin(), triggered_.end());
    triggered_.clear();
}

void Resolver::check_health(int source) {
    if (state_.outcome != Outcome::Ongoing) {
        return;
    }
    const bool first = state_.players[0].leader_hp <= 0;
    const bool second = state_.players[1].leader_hp <= 0;
    if (first && second) {
        switch (rules_.simultaneous_lethal) {
        case SimultaneousLethal::SourceOpponent:
            lose(state_, source, EndReason::Health);
            break;
        case SimultaneousLethal::NonActive:
            lose(state_, state_.turn_player, EndReason::Health);
            break;
        case SimultaneousLethal::BothLose:
            state_.outcome = Outcome::BothLose;
            state_.end_reason = EndReason::Health;
            break;
        }
    } else if (first || second) {
        lose(state_, first ? 0 : 1, EndReason::Health);
    }
}

bool Resolver::protected_super(Ref ref, const Permanent& object) const {
    return ref.seat == state_.turn_player && object.evolution == Evolution::Super &&
        (!object.silenced || !rules_.silence_removes_super_abilities);
}

bool Resolver::protected_destruction(Ref ref, const Permanent& object) const {
    return object.earth_count > 0 || protected_super(ref, object) ||
        (!object.silenced && definition(object.card_id).destroy_immune);
}

void Resolver::damage(Ref target, int amount) {
    auto* object = find(state_, target);
    if (!object || !follower(*object) || object->hp <= 0) {
        return;
    }
    if (protected_super(target, *object)) {
        amount = 0;
    }
    if (!object->silenced && amount > 3 && definition(object->card_id).damage_cap) {
        amount = std::min(amount, definition(object->card_id).damage_cap);
    }
    if (has(*object, kBarrier) && (amount > 0 || rules_.zero_damage_consumes_barrier)) {
        object->abilities &= ~kBarrier;
        amount = 0;
    }
    object->hp = static_cast<std::int16_t>(object->hp - amount);
    if (amount > 0 || rules_.zero_damage_triggers) {
        board_ability(target, OnDamage);
    }
}

int Resolver::damage_leader(int seat, int amount) {
    auto& player = state_.players[seat];
    if (player.leader_barrier && (amount > 0 || rules_.zero_damage_consumes_barrier)) {
        player.leader_barrier = false;
        return 0;
    }
    player.leader_hp = static_cast<std::int16_t>(player.leader_hp - amount);
    return amount;
}

void Resolver::heal_leader(int seat, int amount) {
    auto& player = state_.players[seat];
    auto& hp = player.leader_hp;
    const int before = hp;
    hp = static_cast<std::int16_t>(std::min(static_cast<int>(player.leader_max_hp), hp + amount));
    if (hp > before) broadcast(seat, {OnLeaderHeal});
}

void Resolver::destroy(Ref target, Ref cause, bool by_ability, std::vector<Ability>* deferred) {
    auto* object = find(state_, target);
    if (!object || (by_ability && protected_destruction(target, *object))) {
        return;
    }
    auto& player = state_.players[target.seat];
    const auto removed = *object;
    if (!removed.silenced && definition(removed.card_id).banish_when_leaving) {
        banish(target);
        return;
    }
    erase(player.board, player.board_count, static_cast<int>(object - player.board.data()));
    ++player.shadows;
    if (follower(removed)) {
        const auto card = printed_card(removed.card_id);
        ++(legacy_counter(card) ? player.destroyed_followers[card - kFirstCard] : ensure_counter(player, card).destroyed);
        if (definition(card).official_id == 90051110) {
            Ability event{OnFollowerDestroyed};
            event.event_card = card;
            broadcast(target.seat, event, true, false);
            broadcast(1 - target.seat, event, true, false);
        }
    } else {
        const auto card = removed.card_id;
        ++(legacy_counter(card) ? player.destroyed_amulets[card - kFirstCard] : ensure_counter(player, card).destroyed);
        progress_faith(target.seat, OnAmuletDestroyed, card);
        if (deferred) {
            for (int i = 0; i < player.board_count; ++i) {
                const auto& observer = player.board[i];
                if (!observer.silenced && std::any_of(definition(observer.card_id).effects.begin(), definition(observer.card_id).effects.end(),
                    [](const auto& effect) { return effect.trigger == OnAmuletDestroyed; })) {
                    deferred->push_back({OnAmuletDestroyed, Origin::Board, {target.seat, observer.creation_order}, observer.card_id});
                }
            }
        } else {
            broadcast(target.seat, {OnAmuletDestroyed}, true, false);
        }
    }
    if (!removed.silenced && !(removed.modifiers.effect_flags & kLoseLastWords)) {
        const Ability last_words{LastWords, Origin::Removed, target, removed.card_id};
        if (deferred) {
            deferred->push_back(last_words);
        } else {
            enqueue(last_words);
        }
    }
    if (!removed.silenced && (removed.modifiers.effect_flags & kRebirthLastWords)) {
        const Ability rebirth{LastWords, Origin::Rebirth, target, printed_card(removed.card_id)};
        if (deferred) deferred->push_back(rebirth);
        else triggered_.push_back(rebirth);
    }
    if (super_attacker_.order != 0 && cause == super_attacker_ && target.seat != super_attacker_.seat &&
        follower(removed) && (!super_damage_queued_ || rules_.super_damage_per_follower)) {
        triggered_.push_back({OnAttack, Origin::SuperDamage, super_attacker_});
        super_damage_queued_ = true;
    }
}

void Resolver::banish(Ref target) {
    const auto* object = find(state_, target);
    if (!object) return;
    auto& player = state_.players[target.seat];
    const auto removed = *object;
    erase(player.board, player.board_count, static_cast<int>(object - player.board.data()));
    if (!removed.silenced) enqueue({OnBanish, Origin::Banished, target, removed.card_id});
    broadcast(target.seat, {OnOwnBanish}, true, false);
    if (follower(removed)) {
        Ability event{OnEnemyFollowerBanished};
        event.event_object = removed;
        broadcast(1 - target.seat, event, true, false);
    }
}

Ref Resolver::copy_board(int owner, Permanent copy) {
    const auto created = enter(owner, copy.card_id, copy.modifiers);
    if (auto* added = find(state_, created)) {
        copy.creation_order = created.order;
        copy.entry_ply = static_cast<std::uint8_t>(ply(state_));
        copy.readied = copy.attacked_this_turn = copy.attacked_twice = copy.acted_this_turn = false;
        copy.attacked_thrice = false;
        copy.earth_count = added->earth_count;
        copy.clue_count = added->clue_count;
        *added = copy;
    }
    return created;
}

void Resolver::collect_dead(Ref cause) {
    for (int offset = 0; offset < kPlayers; ++offset) {
        const int seat = (state_.turn_player + offset) % kPlayers;
        auto& player = state_.players[seat];
        for (int i = 0; i < player.board_count;) {
            if (follower(player.board[i]) && player.board[i].hp <= 0) {
                destroy({seat, player.board[i].creation_order}, cause, false);
            } else {
                ++i;
            }
        }
    }
}

void Resolver::settle(Ref cause) {
    check_health(cause.seat);
    if (state_.outcome == Outcome::Ongoing) {
        collect_dead(cause);
        flush();
    }
}

void Resolver::resolve() {
    flush();
    for (std::size_t cursor = 0; cursor < waiting_.size() && state_.outcome == Outcome::Ongoing; ++cursor) {
        if (cursor >= 2048) {
            throw std::logic_error("Ability queue did not terminate");
        }
        const auto ability = waiting_[cursor];
        if (ability.origin == Origin::DirectSummon) {
            direct_summon(ability.source.seat);
            continue;
        }
        if (ability.origin == Origin::SuperDamage) {
            damage_leader(1 - ability.source.seat, 1);
            settle(ability.source);
            continue;
        }
        if (ability.origin == Origin::DelayedBanish) {
            const auto* object = find(state_, ability.source);
            if (object && object->banish_at_end && !object->silenced) {
                banish(ability.source);
                settle(ability.source);
            }
            continue;
        }
        if (ability.origin == Origin::Rebirth) {
            enter(ability.source.seat, ability.card);
            settle(ability.source);
            continue;
        }
        if (rules_.require_live_ability_source) {
            if (ability.origin == Origin::Board) {
                const auto* object = find(state_, ability.source);
                if (!object || object->silenced || object->card_id != ability.card) {
                    continue;
                }
            }
            if (ability.origin == Origin::Crest &&
                !find(state_, ability.source.seat, ability.source.order)) {
                continue;
            }
        }
        run_steps(ability, script(ability));
    }
    waiting_.clear();
    triggered_.clear();
}

void Resolver::run_steps(const Ability& ability, std::span<const Effect> effects) {
    std::array<EntityId, kEffectSlots> picked{};
    if (rules_.random_targets == RandomTargets::AtAbilityStart) {
        for (std::size_t i = 0; i < effects.size(); ++i) {
            if (effects[i].trigger == ability.trigger && takes_mode(effects[i], ability.mode) &&
                effects[i].target == Target::RandomEnemyFollower) {
                picked[i] = random_enemy(ability.source.seat);
            }
        }
    }
    auto& player = state_.players[ability.source.seat];
    bool paid = false;
    const auto conditions = [&](const Effect& step) {
        if (step.condition == Condition::NoDeckDuplicates || step.condition == Condition::HasDeckDuplicates) {
            mark_hidden();
        }
        if (step.operation == Operation::DiscardEvidence ||
            step.condition == Condition::HandSpells || step.condition == Condition::CostMultiplicity) {
            if (ability.source.seat != observer_) {
                mark_hidden();
                mark_chance();
            } else if (has_unobserved_hand(ability.source.seat)) {
                mark_unobserved_card();
                mark_hidden();
            }
        }
        return condition_met(state_, step, ability, paid, rules_);
    };
    const bool limited = ability.origin == Origin::Board && definition(ability.card).once_per_own_turn &&
        (ability.trigger == OnBuffed || ability.trigger == OnOtherPlay);
    if (limited) {
        auto* source = find(state_, ability.source);
        if (!source || ability.source.seat != state_.turn_player || source->last_trigger_ply == ply(state_) ||
            std::none_of(effects.begin(), effects.end(), [&](const auto& step) {
                return step.trigger == ability.trigger && conditions(step);
            })) return;
        source->last_trigger_ply = static_cast<std::uint8_t>(ply(state_));
    }
    for (std::size_t i = 0; i < effects.size() && state_.outcome == Outcome::Ongoing; ++i) {
        const auto& step = effects[i];
        if (step.trigger == ability.trigger && takes_mode(step, ability.mode) && conditions(step)) {
            if (step.operation == Operation::Necromancy) {
                paid = step.amount >= 0 && player.shadows >= step.amount;
                if (!paid) continue;
            } else if (step.operation == Operation::EarthRite) {
                const int slot = earth_slot(player);
                paid = step.amount > 0 && slot >= 0 && player.board[slot].earth_count >= step.amount;
                if (!paid) continue;
            } else if (step.operation == Operation::DiscardEvidence) {
                paid = std::any_of(player.hand.begin(), player.hand.begin() + player.hand_count,
                    [](CardId id) { return definition(id).clue; });
                if (!paid) continue;
            } else if (step.operation == Operation::SpendClue && step.target == Target::None) {
                const int slot = clue_slot(player);
                paid = slot >= 0 && player.board[slot].clue_count >= step.amount;
                if (!paid) continue;
            } else if (step.operation == Operation::PayFusionPp) {
                paid = player.available_pp() >= step.amount;
                if (!paid) continue;
            } else if (step.operation == Operation::SpendFaith) {
                paid = false;
                const auto faith = definition(ability.card).faith;
                for (int slot = 0; slot < player.faith_count; ++slot) {
                    if (player.faiths[slot] != faith || player.faith_values[slot] < step.amount) continue;
                    player.faith_values[slot] -= static_cast<std::uint16_t>(step.amount);
                    paid = true;
                    break;
                }
                continue;
            }
            effect(ability, step, picked[i]);
            settle(ability.source);
        }
    }
}

void Resolver::immediate(int owner, std::span<const Effect> effects, EntityId source, EntityId target) {
    if (state_.outcome == Outcome::Ongoing) {
        run_steps({Trigger::None, Origin::Spell, {owner, source}, 0, CrestId::None, target}, effects);
        resolve();
    }
}

Ref Resolver::enter(int seat, CardId card, CardModifiers modifiers) {
    const auto& def = definition(card);
    auto& player = state_.players[seat];
    if (!can_enter(player, def, rules_)) {
        return {};
    }
    assert(def.kind == Kind::Follower || def.kind == Kind::Amulet);
    int clues = 0;
    if (def.clue) {
        clues = 1;
        const int old = clue_slot(player);
        if (old >= 0) {
            clues += player.board[old].clue_count;
            banish({seat, player.board[old].creation_order});
        }
    }
    int earth = 0;
    if (def.earth) {
        earth = 1;
        const int old = earth_slot(player);
        if (old >= 0) {
            earth += player.board[old].earth_count;
            erase(player.board, player.board_count, old);
        }
    }
    const Ref ref{seat, state_.next_creation_order++};
    const auto hp = static_cast<std::int16_t>(def.hp + modifiers.defense);
    player.board[player.board_count++] = {
        ref.order, card, static_cast<std::int16_t>(std::max(0, def.atk + modifiers.attack)), hp, hp,
        static_cast<std::uint16_t>(def.abilities | modifiers.abilities), static_cast<std::uint16_t>(clues),
        static_cast<std::uint8_t>(def.countdown), static_cast<std::uint8_t>(ply(state_)),
        Evolution::None, false, false, false, false, false, static_cast<std::uint16_t>(earth), modifiers,
    };
    player.board[player.board_count - 1].attack_limit = static_cast<std::uint8_t>(def.attack_limit);
    if (def.kind == Kind::Follower) ++player.followers_entered;
    const bool artifact = def.kind == Kind::Follower && has_trait(def, kArtifactTrait);
    if (def.track_entries || artifact) {
        auto& entered = ensure_counter(player, card).entered;
        if (artifact && entered == 0) ++player.artifact_types_entered;
        if (entered < 255) ++entered;
    }
    if (def.kind == Kind::Follower) {
        const bool large = is_large_follower(def);
        for (int i = 0; i < player.hand_count; ++i) {
            const auto& held = definition(player.hand[i]);
            if (large && held.hand_large_entry_discount) {
                modify_hand_cost(player.hand_modifiers[i], -held.hand_large_entry_discount, true);
            }
            if (held.hand_discount_entry_card == card) {
                modify_hand_cost(player.hand_modifiers[i], -1, false);
            }
        }
    }
    board_ability(ref, OnEnter);
    if (def.kind == Kind::Follower) {
        progress_faith(seat, OnFollowerEntered, card);
        Ability event{OnFollowerEntered};
        event.event_order = ref.order;
        event.event_card = card;
        event.event_object = player.board[player.board_count - 1];
        broadcast(seat, event);
        event.trigger = OnEnemyFollowerEntered;
        broadcast(1 - seat, event, true, false);
    }
    return ref;
}

void Resolver::evolve(Ref ref, Evolution evolution, bool manual, EntityId chosen, std::uint16_t selection, int mode) {
    auto* object = find(state_, ref);
    if (!object || !follower(*object) || object->evolution != Evolution::None || object->hp <= 0) {
        return;
    }
    set_evolved_form(*object, evolution);
    auto& player = state_.players[ref.seat];
    for (int i = 0; i < player.hand_count; ++i) {
        const auto& card = definition(player.hand[i]);
        auto& cost = player.hand_modifiers[i].cost;
        int next_cost = cost - card.hand_evolve_discount;
        if (evolution == Evolution::Super) {
            next_cost -= card.super_evolve_hand_discount;
            if (card.hand_super_cost >= 0) next_cost = card.hand_super_cost - card.cost;
        }
        cost = static_cast<std::int8_t>(std::max(-127, next_cost));
    }
    if (manual) {
        --(evolution == Evolution::Super ? player.sep : player.ep);
        player.manual_evolved = true;
        // A crest granted by the evolve ability can be extended by the super-evolve ability.
        board_ability(ref, OnEvolve, chosen, mode, selection);
        if (evolution == Evolution::Super) {
            board_ability(ref, OnSuperEvolve, chosen, mode, selection);
        }
    }
    board_ability(ref, OnEvolved);
    progress_faith(ref.seat, OnAllyEvolved);
    broadcast(ref.seat, {OnAllyEvolved}, true, false);
}

void Resolver::transform(Ref ref, CardId card, CardModifiers modifiers, Evolution evolution) {
    auto* object = find(state_, ref);
    if (!object) return;
    const auto& form = definition(card);
    const auto hp = static_cast<std::int16_t>(form.hp + modifiers.defense);
    *object = {ref.order, card, static_cast<std::int16_t>(std::max(0, form.atk + modifiers.attack)), hp, hp,
               static_cast<std::uint16_t>(form.abilities | modifiers.abilities),
               static_cast<std::uint16_t>(form.clue), static_cast<std::uint8_t>(form.countdown),
               static_cast<std::uint8_t>(ply(state_)), Evolution::None, false, false, false, false,
               false, static_cast<std::uint16_t>(form.earth), modifiers};
    object->attack_limit = static_cast<std::uint8_t>(form.attack_limit);
    if (form.kind == Kind::Follower && evolution != Evolution::None) set_evolved_form(*object, evolution);
    if (rules_.reset_name_counts_on_transform) counts(state_.players[ref.seat], form.id) = {};
}

EntityId Resolver::random_enemy(int owner) {
    const auto& player = state_.players[1 - owner];
    std::array<EntityId, kBoardLimit> candidates{};
    int count = 0;
    for (int i = 0; i < player.board_count; ++i) {
        if (follower(player.board[i])) {
            candidates[count++] = player.board[i].creation_order;
        }
    }
    return count > 0 ? candidates[pick_index(count)] : 0;
}

void Resolver::grant_crest(int seat, CrestId id) {
    const auto* crest = find_crest(id);
    if (!crest) return;
    auto& player = state_.players[seat];
    const auto end = player.crests.begin() + player.crest_count;
    if (std::any_of(player.crests.begin(), end, [id](const Crest& crest) { return crest.id == id; })) {
        return;
    }
    if (player.crest_count + player.faith_count == kCrestLimit) {
        if (!rules_.replace_oldest_crest_when_full || player.crest_count == 0) {
            return;
        }
        erase(player.crests, player.crest_count, 0);
    }
    player.crests[player.crest_count++] = {state_.next_creation_order++, id, crest->countdown};
}

void Resolver::destroy_crest(int seat, EntityId order, bool expired) {
    auto* crest = find(state_, seat, order);
    if (!crest) return;
    auto& player = state_.players[seat];
    const auto id = crest->id;
    erase(player.crests, player.crest_count, static_cast<int>(crest - player.crests.data()));
    if (expired && rules_.expired_crest_adds_shadow) ++player.shadows;
    enqueue({LastWords, Origin::ExpiredCrest, {seat, order}, 0, id});
}

bool deck_filter(const CardDef& card, DeckFilter filter, CardModifiers modifiers = {});

CardId Resolver::past_card(int seat, int limit, bool highest, bool only_amulets, std::span<const CardId> excluded,
                           DeckFilter filter) {
    const auto& player = state_.players[seat];
    std::array<std::pair<CardId, int>, kCardSlots + kAdditionalCardCounters> candidates{};
    int best = -1;
    int count = 0;
    int total = 0;
    const auto consider = [&](CardId id, int destroyed) {
        if (destroyed == 0 || std::find(excluded.begin(), excluded.end(), id) != excluded.end()) return;
        const auto& card = definition(id);
        if (card.cost > limit || (highest && card.cost < best) || !deck_filter(card, filter)) return;
        if (only_amulets) {
            if (card.kind != Kind::Amulet || std::none_of(card.effects.begin(), card.effects.end(),
                    [](const auto& step) { return step.trigger == LastWords; })) return;
        } else if (card.kind != Kind::Follower) {
            return;
        }
        if (highest && card.cost > best) {
            best = card.cost;
            count = total = 0;
        }
        candidates[count++] = {id, destroyed};
        total += destroyed;
    };
    const auto& history = only_amulets ? player.destroyed_amulets : player.destroyed_followers;
    for (int i = 0; i < kCardSlots; ++i) consider(static_cast<CardId>(kFirstCard + i), history[i]);
    for (int i = 0; i < player.other_counter_count; ++i) {
        consider(player.other_counters[i].id, player.other_counters[i].destroyed);
    }
    if (total == 0) return 0;
    int pick = pick_index(total);
    for (int i = 0; i < count; ++i) {
        const auto [id, weight] = candidates[i];
        if (pick < weight) {
            return id;
        }
        pick -= weight;
    }
    return 0;
}

void Resolver::reanimate(int seat, int limit) {
    if (state_.players[seat].board_count < kBoardLimit) {
        if (const auto card = past_card(seat, limit, true, false)) {
            CardModifiers modifiers{};
            modifiers.effect_flags = kDeadTrait;
            enter(seat, card, modifiers);
        }
    }
}

std::pair<CardId, CardModifiers> take_deck_card(PlayerState& player, int slot) {
    const auto card = player.deck[slot];
    const auto modifiers = player.deck_modifiers[slot];
    if (slot != player.deck_idx) {
        std::move_backward(player.deck.begin() + player.deck_idx, player.deck.begin() + slot, player.deck.begin() + slot + 1);
        std::move_backward(player.deck_modifiers.begin() + player.deck_idx, player.deck_modifiers.begin() + slot,
                           player.deck_modifiers.begin() + slot + 1);
    }
    ++player.deck_idx;
    return {card, modifiers};
}

constexpr std::string_view kNeutralClass = "ニュートラル";
constexpr std::string_view kAssailantTrait = "アサイラント";

bool deck_filter(const CardDef& card, DeckFilter filter, CardModifiers modifiers) {
    switch (filter) {
    case DeckFilter::Any: return true;
    case DeckFilter::Follower: return card.kind == Kind::Follower;
    case DeckFilter::Amulet: return card.kind == Kind::Amulet;
    case DeckFilter::Neutral: return card.class_name == kNeutralClass;
    case DeckFilter::RoyalFollower: return card.kind == Kind::Follower && card.class_name == kRoyalClass;
    case DeckFilter::NightmareFollower: return card.kind == Kind::Follower && card.class_name == kNightmareClass;
    case DeckFilter::PrintedCostFive: return card.cost >= 5;
    case DeckFilter::Spell: return card.kind == Kind::Spell;
    case DeckFilter::NemesisBane:
        return card.kind == Kind::Follower && card.class_name == kNemesisClass && ((card.abilities | modifiers.abilities) & kBane);
    case DeckFilter::Fusion: return card.fusion != FusionMaterial::None;
    case DeckFilter::ArtifactFollower: return card.kind == Kind::Follower && has_trait(card, kArtifactTrait);
    }
    return false;
}

void Resolver::keep_drawn(int seat, CardId card, CardModifiers modifiers, int discount) {
    auto& player = state_.players[seat];
    const auto& def = definition(card);
    const int drawn_cost = card_cost(def, modifiers);
    modifiers.cost = static_cast<std::int8_t>(std::max(-127, modifiers.cost - discount));
    if (def.drawn_cost >= 0) {
        const int base = def.cost + modifiers.cost - def.spellboost_cost_reduction * modifiers.spellboost;
        modifiers.temporary_cost_delta = static_cast<std::int8_t>(std::clamp(def.drawn_cost - base, -127, 127));
    }
    const bool kept = player.hand_count < kHandLimit;
    add_unobserved_card(seat, card, modifiers);
    if (kept) {
        const auto before_triggered = triggered_.size();
        Ability event{OnDraw};
        event.event_cost = drawn_cost;
        broadcast(seat, event);
        if (triggered_.size() != before_triggered) {
            mark_unobserved_card();
        }
    }
}

int Resolver::draw_cards(int seat, int amount, DeckFilter filter, int discount) {
    auto& player = state_.players[seat];
    if (amount > 0) {
        mark_chance();
        if (player.deck_remaining() > 0) mark_hidden();
    }
    int last_cost = -1;
    for (int i = 0; i < amount && state_.outcome == Outcome::Ongoing; ++i) {
        int slot = player.deck_idx;
        if (filter != DeckFilter::Any) {
            std::array<int, kDeckCapacity> candidates{};
            int count = 0;
            for (int index = player.deck_idx; index < player.deck_end; ++index) {
                if (deck_filter(definition(player.deck[index]), filter, player.deck_modifiers[index])) candidates[count++] = index;
            }
            if (count == 0) break;
            slot = candidates[pick_index(count)];
        } else if (player.deck_remaining() == 0) {
            lose(state_, seat, EndReason::EmptyDeck);
            break;
        }
        auto [card, modifiers] = take_deck_card(player, slot);
        last_cost = player.hand_count < kHandLimit ? card_cost(definition(card), modifiers) : -1;
        keep_drawn(seat, card, modifiers, discount);
    }
    return last_cost;
}

void Resolver::discard(int seat, int slot) {
    if (seat != observer_) mark_hidden();
    auto& player = state_.players[seat];
    const auto id = player.hand[slot];
    Ability ability{OnDiscard, Origin::Spell, {seat}, id};
    ability.effective_cost = card_cost(definition(id), player.hand_modifiers[slot]);
    erase_player_hand(seat, slot);
    ++player.shadows;
    enqueue(ability);
}

void Resolver::effect(const Ability& ability, const Effect& step, EntityId random_target) {
    const auto source = ability.source;
    const int owner = source.seat;
    const int enemy = 1 - owner;
    auto& player = state_.players[owner];
    const auto magnitude_value = [&] {
        switch (step.magnitude) {
        case Magnitude::Fixed: return step.amount;
        case Magnitude::SourceAttack:
        case Magnitude::NegativeSourceAttack:
            if (const auto* object = find(state_, source)) {
                return object->atk * (step.magnitude == Magnitude::SourceAttack ? 1 : -1);
            }
            return 0;
        case Magnitude::Combo: return static_cast<int>(player.plays_this_turn);
        case Magnitude::OwnFollowers: return static_cast<int>(player.board_count) - amulets(player);
        case Magnitude::AllFollowerCount: {
            const auto& other = state_.players[enemy];
            return player.board_count - amulets(player) + other.board_count - amulets(other);
        }
        case Magnitude::EffectiveCost: return ability.effective_cost;
        case Magnitude::EnemyMissingHp: return state_.players[enemy].leader_max_hp - state_.players[enemy].leader_hp;
        case Magnitude::Evidence:
            if (const int slot = clue_slot(player); slot >= 0) return static_cast<int>(player.board[slot].clue_count);
            return 0;
        case Magnitude::ArtifactTypes: return static_cast<int>(player.artifact_types_entered);
        case Magnitude::OwnOtherFollowers: return player.board_count - amulets(player) - (find(state_, source) != nullptr);
        case Magnitude::OwnLargeFollowers:
            return static_cast<int>(std::count_if(player.board.begin(), player.board.begin() + player.board_count,
                [](const auto& object) { return is_large_follower(definition(object.card_id)); }));
        case Magnitude::EarthSigils:
            if (const int slot = earth_slot(player); slot >= 0) return static_cast<int>(player.board[slot].earth_count);
            return 0;
        }
        return 0;
    };
    const int amount = magnitude_value();
    std::array<Ref, 2 * kBoardLimit> targets{};
    int target_count = 0;
    bool group = true;
    bool random_group = false;
    int picks = 1;
    int highest_attack = -1;
    const auto gather = [&](int seat, auto predicate) {
        const auto& side = state_.players[seat];
        for (int i = 0; i < side.board_count; ++i) {
            const auto& object = side.board[i];
            if (predicate(object)) targets[target_count++] = {seat, object.creation_order};
        }
    };
    const auto other_follower = [&](const Permanent& object) { return follower(object) && object.creation_order != source.order; };
    switch (step.target) {
    case Target::AllEnemyFollowers: case Target::AllEnemyFollowersAndLeader: case Target::AllEnemyFollowersAndBothLeaders:
        gather(enemy, follower); break;
    case Target::AllOwnFollowers: gather(owner, follower); break;
    case Target::FirstTwoOwnFollowers:
        for (int i = player.board_count - 1; i >= 0 && target_count < 2; --i) {
            if (follower(player.board[i])) targets[target_count++] = {owner, player.board[i].creation_order};
        }
        break;
    case Target::AllOwnEvidence:
        gather(owner, [](const auto& object) { return object.clue_count > 0; });
        break;
    case Target::AllOtherOwnFollowers: gather(owner, other_follower); break;
    case Target::AllOwnFairyFollowers:
        gather(owner, [](const auto& object) { return follower(object) && has_trait(definition(object.card_id), kFairyTrait); });
        break;
    case Target::AllOtherOwnRoyalFollowers:
        gather(owner, [&](const auto& object) {
            return other_follower(object) && definition(object.card_id).class_name == kRoyalClass;
        });
        break;
    case Target::AllOtherOwnNightmareFollowers:
        gather(owner, [&](const auto& object) {
            return other_follower(object) && definition(object.card_id).class_name == kNightmareClass;
        });
        break;
    case Target::AllOwnNamedCards:
    case Target::AllOwnNamedFollowers:
        gather(owner, [&](const auto& object) { return object.card_id == step.card; });
        break;
    case Target::AllFollowers: gather(owner, follower); gather(enemy, follower); break;
    case Target::AllOtherFollowers: gather(owner, other_follower); gather(enemy, follower); break;
    case Target::AllOtherRandomSixFollowers:
        random_group = true; picks = 6; gather(owner, other_follower); gather(enemy, follower); break;
    case Target::AllOwnAmulets: gather(owner, [](const auto& object) { return !follower(object); }); break;
    case Target::AllAmulets:
        gather(owner, [](const auto& object) { return !follower(object); });
        gather(enemy, [](const auto& object) { return !follower(object); });
        break;
    case Target::EnemyOneHpFollowers:
        gather(enemy, [](const auto& object) { return follower(object) && object.hp == 1; });
        break;
    case Target::RandomOwnNamedPermanent:
        random_group = true;
        gather(owner, [&](const auto& object) { return object.card_id == step.card; });
        break;
    case Target::ChosenOtherThreePermanents:
        for (int offset = 0; offset < kPlayers; ++offset) {
            const auto seat = (owner + offset) % kPlayers;
            const auto& side = state_.players[seat];
            for (int slot = 0; slot < side.board_count; ++slot) {
                const auto& object = side.board[slot];
                if (object.creation_order != source.order && (ability.selection & (1 << (offset * kBoardLimit + slot)))) {
                    targets[target_count++] = {seat, object.creation_order};
                }
            }
        }
        break;
    case Target::AllSmallEnemyFollowers:
        gather(enemy, [](const auto& object) { return follower(object) && object.hp <= 3; }); break;
    case Target::AllNonAssailantFollowers: {
        const auto non_assailant = [](const auto& object) {
            return follower(object) && definition(object.card_id).traits.find(kAssailantTrait) == std::string_view::npos;
        };
        gather(owner, non_assailant); gather(enemy, non_assailant); break;
    }
    case Target::ChosenOtherOwnTwoFollowers:
    case Target::ChosenEnemyTwoFollowers: {
        const int seat = step.target == Target::ChosenEnemyTwoFollowers ? enemy : owner;
        const auto& chosen = state_.players[seat];
        for (int i = 0; i < chosen.board_count; ++i) {
            if (ability.selection & (1 << i)) targets[target_count++] = {seat, chosen.board[i].creation_order};
        }
        break;
    }
    case Target::RandomOtherEnemyFollower:
        random_group = true;
        gather(enemy, [&](const auto& object) { return follower(object) && object.creation_order != ability.chosen; });
        break;
    case Target::RandomEnemyTwo:
        random_group = true; picks = 2; gather(enemy, follower); break;
    case Target::RandomEnemyThree:
        random_group = true; picks = 3; gather(enemy, follower); break;
    case Target::RandomEnemyTwoWard:
        random_group = true; picks = 2;
        gather(enemy, [](const auto& object) { return follower(object) && has(object, FLAG_WARD); });
        break;
    case Target::RandomOtherUnevolvedWard:
        random_group = true;
        gather(owner, [&](const auto& object) {
            return other_follower(object) && object.evolution == Evolution::None && has(object, FLAG_WARD);
        });
        break;
    case Target::RandomOtherOwnUnevolved:
        random_group = true;
        gather(owner, [&](const auto& object) { return other_follower(object) && object.evolution == Evolution::None; });
        break;
    case Target::RandomOwnDead:
        random_group = true;
        gather(owner, [](const auto& object) {
            return follower(object) && ((object.modifiers.effect_flags & kDeadTrait) ||
                has_trait(definition(object.card_id), kDeadName));
        });
        break;
    case Target::RandomOwnLargeUnevolved:
        random_group = true;
        gather(owner, [](const auto& object) {
            return object.evolution == Evolution::None && is_large_follower(definition(object.card_id));
        });
        break;
    case Target::StrongestEnemyFollower:
        random_group = true;
        gather(enemy, [&](const auto& object) {
            if (!follower(object) || object.atk < highest_attack) return false;
            if (object.atk > highest_attack) target_count = 0;
            highest_attack = object.atk;
            return true;
        });
        break;
    default: group = false; break;
    }
    if (group && step.operation != Operation::Custom) {
        auto single = step;
        single.target = Target::ChosenOtherPermanent;
        if (step.operation == Operation::Damage) {
            single.magnitude = Magnitude::Fixed;
            single.amount = amount;
        }
        const int count = random_group ? std::min(picks, target_count) : target_count;
        for (int i = 0; i < count; ++i) {
            if (random_group) std::swap(targets[i], targets[i + pick_index(target_count - i)]);
            auto chosen = ability;
            chosen.chosen = targets[i].order;
            effect(chosen, single, 0);
        }
        if ((step.target == Target::AllEnemyFollowersAndLeader || step.target == Target::AllEnemyFollowersAndBothLeaders) &&
            step.operation == Operation::Damage) {
            damage_leader(enemy, amount + step.spellboost_multiplier * ability.spellboost);
            if (step.target == Target::AllEnemyFollowersAndBothLeaders) damage_leader(owner, amount);
        }
        return;
    }
    Ref target{};
    switch (step.target) {
    case Target::Self:
        target = source;
        break;
    case Target::ChosenEnemyFollower:
    case Target::ChosenSmallEnemyFollower:
    case Target::ChosenEnemyOrLeader:
    case Target::ClashOpponent:
        target = {enemy, ability.chosen};
        break;
    case Target::ChosenEnemyPermanent:
        if (const auto* object = find(state_, {enemy, ability.chosen});
            object && can_choose_enemy_permanent(*object)) {
            target = {enemy, ability.chosen};
        }
        break;
    case Target::ChosenOwnFollower:
    case Target::ChosenOwnPermanent:
    case Target::ChosenUnevolvedOwnFollower:
    case Target::ChosenOwnNamedFollower:
    case Target::ChosenOwnGolem:
    case Target::ChosenOtherUnevolvedOwnFollower:
    case Target::ChosenOwnLargeFollower:
    case Target::ChosenOtherOwnFollower:
    case Target::AttackingFollower:
        target = {owner, ability.chosen};
        break;
    case Target::EventFollower:
    case Target::EnemyEventFollower:
        target = {step.target == Target::EventFollower ? owner : enemy, ability.event_order};
        break;
    case Target::ChosenOtherFollower: case Target::ChosenOtherPermanent:
        target = {find(state_, {owner, ability.chosen}) ? owner : enemy, ability.chosen};
        break;
    case Target::RandomEnemyFollower:
        target = {enemy, rules_.random_targets == RandomTargets::PerStep ? random_enemy(owner) : random_target};
        break;
    default: break;
    }
    const int hand_slot = step.target == Target::ChosenHandAndEnemy ? ability.discarded_slot :
        (step.target == Target::ChosenOwnHand || step.target == Target::ChosenOwnSpell ||
         step.target == Target::ChosenOwnHandFollower || step.target == Target::ChosenBoostedHand ||
         step.target == Target::ChosenAffordableArtifactHand) && (ability.chosen & kHandTargetFlag)
            ? static_cast<int>(ability.chosen & ~kHandTargetFlag) : -1;
    using enum Operation;
    switch (step.operation) {
    case Damage: {
        const int damage_amount = amount + step.spellboost_multiplier * ability.spellboost;
        if (step.target == Target::BothLeaders) {
            damage_leader(owner, damage_amount);
            damage_leader(enemy, damage_amount);
        } else if (step.target == Target::LowestHpLeaders) {
            const auto own_hp = player.leader_hp;
            const auto enemy_hp = state_.players[enemy].leader_hp;
            if (own_hp <= enemy_hp) damage_leader(owner, damage_amount);
            if (enemy_hp <= own_hp) damage_leader(enemy, damage_amount);
        } else if (step.target == Target::EnemyLeader || step.target == Target::OwnLeader ||
                   (step.target == Target::ChosenEnemyOrLeader && ability.chosen == kLeaderTargetFlag)) {
            damage_leader(step.target == Target::OwnLeader ? owner : enemy, damage_amount);
        } else {
            damage(target, damage_amount);
        }
        break;
    }
    case SplitDamage: {
        const auto& opponent = state_.players[enemy];
        std::array<Ref, kBoardLimit> followers{};
        int count = 0;
        for (int i = 0; i < opponent.board_count; ++i) {
            const auto& object = opponent.board[i];
            if (follower(object)) followers[count++] = {enemy, object.creation_order};
        }
        int remaining = amount;
        for (int i = 0; i < count && remaining > 0; ++i) {
            const int apportioned = i + 1 == count ? remaining :
                std::min(remaining, static_cast<int>(find(state_, followers[i])->hp));
            damage(followers[i], apportioned);
            remaining -= apportioned;
        }
        break;
    }
    case Heal:
        heal_leader(step.target == Target::OwnLeader ? owner : enemy, step.amount);
        break;
    case Draw:
        draw_cards(owner, amount);
        break;
    case DrawEnemy:
        draw_cards(enemy, amount);
        break;
    case DrawFiltered:
        draw_cards(owner, step.amount, step.filter);
        break;
    case AddHand:
        for (int i = 0; i < amount; ++i) {
            CardModifiers modifiers{};
            modifiers.abilities = step.granted_abilities;
            modifiers.effect_flags = step.effect_flags;
            if (step.cost_override >= 0) modifiers.cost = static_cast<std::int8_t>(step.cost_override - definition(step.card).cost);
            add_public_card(player, step.card, modifiers);
        }
        break;
    case AddHandModified: {
        CardModifiers modifiers{};
        modifiers.attack = static_cast<std::int8_t>(amount);
        modifiers.defense = static_cast<std::int8_t>(step.health_modifier);
        modifiers.abilities = step.granted_abilities;
        add_public_card(player, step.card, modifiers);
        break;
    }
    case Summon: case SummonEnemy:
        for (int i = 0; i < step.amount; ++i) {
            CardModifiers modifiers{};
            modifiers.abilities = step.granted_abilities;
            modifiers.effect_flags = step.effect_flags;
            enter(step.operation == Summon ? owner : enemy, step.card, modifiers);
        }
        break;
    case SummonModified: {
        CardModifiers modifiers{step.granted_abilities, static_cast<std::int8_t>(step.amount),
            static_cast<std::int8_t>(step.health_modifier), 0, 0, 0, 0, 0, step.effect_flags};
        enter(owner, step.card, modifiers);
        break;
    }
    case SummonEvolved:
        for (int i = 0; i < std::max(1, step.amount); ++i) {
            CardModifiers modifiers{};
            modifiers.abilities = step.granted_abilities;
            evolve(enter(owner, step.card, modifiers), Evolution::Normal, false);
        }
        break;
    case ReduceCountdown:
        if (auto* object = find(state_, target); object && object->countdown > 0) {
            const int remaining = object->countdown - step.amount;
            if (remaining <= 0) {
                destroy(target, source, false);
            } else {
                object->countdown = static_cast<std::uint8_t>(remaining);
            }
        }
        break;
    case SpendClue:
        if (step.target == Target::None) {
            const int slot = clue_slot(player);
            if (slot >= 0) target = {owner, player.board[slot].creation_order};
        }
        if (auto* object = find(state_, target); object && object->clue_count > 0) {
            const int remaining = object->clue_count - step.amount;
            if (remaining <= 0) {
                destroy(target, source, false);
            } else {
                object->clue_count = static_cast<std::uint16_t>(remaining);
            }
        }
        break;
    case Silence:
        if (auto* object = find(state_, target)) {
            object->silenced = true;
            object->abilities = 0;
            object->attack_limit = 0;
            object->temporary_abilities = 0;
            object->banish_at_end = false;
        }
        break;
    case Banish:
        banish(target);
        break;
    case Bounce:
        if (const auto* object = find(state_, target)) {
            if (!object->silenced && definition(object->card_id).banish_when_leaving) {
                banish(target);
                break;
            }
            auto& recipient = state_.players[target.seat];
            const auto card = printed_card(object->card_id);
            erase(recipient.board, recipient.board_count, static_cast<int>(object - recipient.board.data()));
            add_public_card(recipient, card);
        }
        break;
    case ReturnToDeck:
        if (step.target == Target::ChosenOwnHandTwo) {
            for (int slot = player.hand_count - 1; slot >= 0; --slot) {
                if (!(ability.selection & (1 << slot))) continue;
                return_to_deck(state_, player, player.hand[slot], player.hand_modifiers[slot]);
                erase_player_hand(owner, slot);
            }
        } else if (hand_slot >= 0 && hand_slot < player.hand_count) {
            return_to_deck(state_, player, player.hand[hand_slot], player.hand_modifiers[hand_slot]);
            erase_player_hand(owner, hand_slot);
        } else if (const auto* object = find(state_, target)) {
            if (!object->silenced && definition(object->card_id).banish_when_leaving) {
                banish(target);
                break;
            }
            auto& recipient = state_.players[target.seat];
            const auto printed = printed_card(object->card_id);
            remember_generated(recipient, printed);
            return_to_deck(state_, recipient, printed, object->modifiers);
            erase(recipient.board, recipient.board_count, static_cast<int>(object - recipient.board.data()));
        }
        break;
    case Buff: {
        const int health = step.magnitude == Magnitude::NegativeSourceAttack ? amount :
            step.magnitude == Magnitude::Combo ? amount : step.health_modifier;
        const int attack = step.magnitude == Magnitude::NegativeSourceAttack ? 0 : amount;
        if (hand_slot >= 0 && hand_slot < player.hand_count) {
            auto& modifiers = player.hand_modifiers[hand_slot];
            modifiers.attack = static_cast<std::int8_t>(std::clamp(modifiers.attack + attack, -127, 127));
            modifiers.defense = static_cast<std::int8_t>(std::clamp(modifiers.defense + health, -127, 127));
            modifiers.abilities |= step.granted_abilities;
        } else if (auto* object = find(state_, target); object && follower(*object)) {
            auto& modifiers = object->modifiers;
            modifiers.attack = static_cast<std::int8_t>(std::clamp(modifiers.attack + attack, -127, 127));
            modifiers.defense = static_cast<std::int8_t>(std::clamp(modifiers.defense + health, -127, 127));
            modifiers.abilities |= step.granted_abilities;
            object->atk = static_cast<std::int16_t>(std::max(0, object->atk + attack));
            object->hp = static_cast<std::int16_t>(object->hp + health);
            object->max_hp = static_cast<std::int16_t>(object->max_hp + health);
            object->abilities |= step.granted_abilities;
            if (attack > 0 || health > 0) board_ability(target, OnBuffed);
        }
        break;
    }
    case BuffDeck:
        for (int i = player.deck_idx; i < player.deck_end; ++i) {
            if (definition(player.deck[i]).kind != Kind::Follower) continue;
            auto& modifiers = player.deck_modifiers[i];
            modifiers.attack = static_cast<std::int8_t>(std::clamp(modifiers.attack + amount, -127, 127));
            modifiers.defense = static_cast<std::int8_t>(std::clamp(modifiers.defense + step.health_modifier, -127, 127));
        }
        break;
    case DrawNamed:
        if (player.deck_remaining() > 0) uncertain();
        for (int i = player.deck_idx; i < player.deck_end; ++i) {
            if (player.deck[i] != step.card) continue;
            auto [card, modifiers] = take_deck_card(player, i);
            modifiers.abilities |= step.granted_abilities;
            keep_drawn(owner, card, modifiers);
        }
        break;
    case TemporaryAttack:
        if (auto* object = find(state_, target); object && follower(*object)) {
            object->atk = static_cast<std::int16_t>(object->atk + amount);
            object->temporary_attack = static_cast<std::int16_t>(object->temporary_attack + amount);
        }
        break;
    case LastWordsFlags:
        if (auto* object = find(state_, target)) object->modifiers.effect_flags |= step.effect_flags;
        break;
    case ShuffleIntoDeck:
        for (int i = 0; i < std::max(1, amount); ++i) {
            return_to_deck(state_, player, step.card, {});
            remember_generated(player, step.card);
        }
        break;
    case TransformNamed:
        for (int i = 0; i < player.hand_count; ++i) {
            if (player.hand[i] != step.card) continue;
            if (slot_is_unobserved(owner, i)) mark_unobserved_card();
            player.hand[i] = step.alternate_card;
            player.hand_modifiers[i] = {};
        }
        for (int i = player.deck_idx; i < player.deck_end; ++i) {
            if (player.deck[i] != step.card) continue;
            player.deck[i] = step.alternate_card;
            player.deck_modifiers[i] = {};
        }
        break;
    case DiscardEvidence: {
        std::array<int, kHandLimit> slots{};
        int count = 0;
        for (int i = 0; i < player.hand_count; ++i) if (definition(player.hand[i]).clue) slots[count++] = i;
        if (count > 0) discard(owner, slots[pick_index(count)]);
        break;
    }
    case AddShadows:
        player.shadows = static_cast<std::uint16_t>(player.shadows + amount);
        break;
    case DiscountNightmareHand:
        for (int i = 0; i < player.hand_count; ++i) {
            if (definition(player.hand[i]).class_name != kNightmareClass) continue;
            auto& cost = player.hand_modifiers[i].cost;
            cost = static_cast<std::int8_t>(std::max(-127, cost - amount));
        }
        break;
    case Copy:
        if (hand_slot >= 0 && hand_slot < player.hand_count) {
            if (slot_is_unobserved(owner, hand_slot)) {
                mark_unobserved_card();
                add_unobserved_card(owner, player.hand[hand_slot], player.hand_modifiers[hand_slot]);
            } else {
                add_card(player, player.hand[hand_slot], player.hand_modifiers[hand_slot]);
            }
        } else if (const auto* object = find(state_, target)) {
            copy_board(owner, *object);
        }
        break;
    case Destroy:
        destroy(target, source, true);
        break;
    case Transform:
        transform(target, step.card, {}, step.transformed_evolution);
        break;
    case RestorePp:
        player.pp = static_cast<std::uint8_t>(std::min(static_cast<int>(player.max_pp), player.pp + amount));
        break;
    case RestoreEp:
        player.ep = static_cast<std::uint8_t>(std::min(255, player.ep + step.amount));
        break;
    case RestoreSep:
        player.sep = static_cast<std::uint8_t>(std::min(2, player.sep + amount));
        break;
    case SetAttackLimit:
        if (auto* object = find(state_, target)) {
            object->attack_limit = static_cast<std::uint8_t>(std::max(static_cast<int>(object->attack_limit), amount));
        }
        break;
    case RemoveAbilities:
        if (auto* object = find(state_, target)) {
            object->abilities &= ~step.granted_abilities;
            object->temporary_abilities &= ~step.granted_abilities;
            object->modifiers.abilities &= ~step.granted_abilities;
        }
        break;
    case AutoEvolve:
        if (const auto* object = find(state_, target); object && object->evolution == Evolution::None) {
            evolve(target, Evolution::Normal, false);
            if (step.amount || step.health_modifier) {
                auto buff = step;
                buff.operation = Buff;
                effect(ability, buff, 0);
            }
        }
        break;
    case TemporaryAbilities:
        if (auto* object = find(state_, target)) {
            object->temporary_abilities |= step.granted_abilities & ~object->abilities;
            object->abilities |= step.granted_abilities;
            object->expires_end_ply = static_cast<std::uint8_t>(ply(state_) + step.amount);
        }
        break;
    case AddCountdown:
        if (auto* object = find(state_, target); object && object->countdown > 0) {
            object->countdown = static_cast<std::uint8_t>(std::min(255, object->countdown + step.amount));
        }
        break;
    case IncreaseMaxPp:
        player.max_pp = static_cast<std::uint8_t>(std::clamp(player.max_pp + step.amount, 0, kMaxPp));
        player.pp = std::min(player.pp, player.max_pp);
        break;
    case Necromancy:
        player.shadows = static_cast<std::uint16_t>(player.shadows - step.amount);
        break;
    case AddEarth: {
        int slot = earth_slot(player);
        if (slot < 0 && step.amount > 0) {
            const auto added = enter(owner, kEarthSigil);
            if (added.order != 0) {
                player.board[player.board_count - 1].earth_count = static_cast<std::uint16_t>(step.amount);
            }
        } else if (slot >= 0 && step.amount > 0) {
            player.board[slot].earth_count = static_cast<std::uint16_t>(
                std::min(65535, player.board[slot].earth_count + step.amount));
        }
        break;
    }
    case EarthRite: {
        const int slot = earth_slot(player);
        auto& sigil = player.board[slot];
        sigil.earth_count = static_cast<std::uint16_t>(sigil.earth_count - step.amount);
        if (sigil.earth_count == 0) destroy({owner, sigil.creation_order}, source, false);
        for (int i = 0; i < player.hand_count; ++i) {
            const int discount = definition(player.hand[i]).hand_earth_discount;
            if (discount) modify_hand_cost(player.hand_modifiers[i], -discount, false);
        }
        break;
    }
    case Reanimate:
        reanimate(owner, step.amount);
        break;
    case AddCombo:
        player.plays_this_turn = static_cast<std::uint16_t>(player.plays_this_turn + amount);
        break;
    case UpgradeFaith: {
        const CardId faith = definition(ability.card).faith;
        for (int i = 0; i < player.faith_count; ++i) {
            if (player.faiths[i] != faith || player.faith_values[i] < amount) continue;
            player.faith_values[i] -= static_cast<std::uint16_t>(amount);
            ++player.faith_upgrades[i];
            if (step.card) add_card(player, step.card);
            break;
        }
        break;
    }
    case GrantFaith:
        grant_faith(player, step.card);
        break;
    case SpendFaith:
        break; // Payment is resolved before the following CostPaid step.
    case Discard:
        if (step.target == Target::ChosenOwnHandTwo) {
            for (int slot = player.hand_count - 1; slot >= 0; --slot) {
                if (ability.selection & (1 << slot)) discard(owner, slot);
            }
        } else if (hand_slot >= 0 && hand_slot < player.hand_count) {
            discard(owner, hand_slot);
        } else if (step.target == Target::None) {
            for (int count = 0; count < step.amount && player.hand_count > 0; ++count) {
                discard(owner, pick_index(player.hand_count));
            }
        }
        break;
    case GrantCrest:
        grant_crest(step.target == Target::EnemyLeader ? enemy : owner, step.crest);
        break;
    case BanishCrests:
        for (auto& side : state_.players) {
            side.crests = {};
            side.crest_count = 0;
        }
        break;
    case DestroyCrest:
    case ReduceCrest: {
        const auto end = player.crests.begin() + player.crest_count;
        const auto position = std::find_if(player.crests.begin(), end, [&](const auto& object) { return object.id == step.crest; });
        if (position == end) break;
        if (step.operation == DestroyCrest || position->countdown <= amount) destroy_crest(owner, position->creation_order, false);
        else position->countdown = static_cast<std::uint8_t>(position->countdown - amount);
        break;
    }
    case ReduceMaxHp: {
        auto& opponent = state_.players[enemy];
        opponent.leader_max_hp = static_cast<std::int16_t>(std::max(0, opponent.leader_max_hp - amount));
        opponent.leader_hp = std::min(opponent.leader_hp, opponent.leader_max_hp);
        break;
    }
    case HalveMaxHp: {
        auto& opponent = state_.players[enemy];
        opponent.leader_max_hp = static_cast<std::int16_t>((opponent.leader_max_hp + rules_.round_half_max_hp_up) / 2);
        opponent.leader_hp = std::min(opponent.leader_hp, opponent.leader_max_hp);
        break;
    }
    case DestroyMatchingEnemy:
        if (const auto* chosen = find(state_, target)) {
            const auto name = definition(chosen->card_id).name;
            auto& opponent = state_.players[enemy];
            for (int slot = opponent.board_count - 1; slot >= 0; --slot) {
                const auto& object = opponent.board[slot];
                if (follower(object) && definition(object.card_id).name == name) {
                    destroy({enemy, object.creation_order}, source, true);
                }
            }
        }
        break;
    case Spellboost:
        for (int i = 0; i < player.hand_count; ++i) {
            if (step.target == Target::ChosenBoostedHand && i != hand_slot) continue;
            if (slot_is_unobserved(owner, i) && definition(player.hand[i]).spellboost) {
                mark_unobserved_card();
            }
            boost_card(definition(player.hand[i]), player.hand_modifiers[i], amount);
        }
        break;
    case BuffHand:
        for (int i = 0; i < player.hand_count; ++i) {
            const auto& card = definition(player.hand[i]);
            auto& modifiers = player.hand_modifiers[i];
            if (step.target == Target::AllHandFollowers && card.kind != Kind::Follower) continue;
            if (step.target == Target::AllManariaSpellsInHand &&
                (card.kind != Kind::Spell || !has_trait(card, kManariaTrait))) continue;
            modifiers.attack = static_cast<std::int8_t>(std::clamp(modifiers.attack + amount, -127, 127));
            modifiers.defense = static_cast<std::int8_t>(std::clamp(modifiers.defense + step.health_modifier, -127, 127));
            if (step.cost_override >= 0) modify_hand_cost(modifiers, -step.cost_override, false);
        }
        break;
    case ApplyTemporaryHandCost:
        for (int i = 0; i < player.hand_count; ++i) modify_hand_cost(player.hand_modifiers[i], amount, true);
        break;
    case ShuffleHandDraw: {
        const int count = player.hand_count;
        while (player.hand_count) {
            return_to_deck(state_, player, player.hand[0], player.hand_modifiers[0]);
            erase_player_hand(owner, 0);
        }
        draw_cards(owner, count);
        break;
    }
    case PayFusionPp:
        spend(player, amount);
        break;
    case ExtendCrest:
        for (int i = 0; i < player.crest_count; ++i) {
            auto& crest = player.crests[i];
            if (crest.id == step.crest) {
                crest.countdown = static_cast<std::uint8_t>(crest.countdown + step.amount);
                break;
            }
        }
        break;
    case Custom:
        custom_effect(ability, step, target, hand_slot);
        break;
    }
}

void Resolver::custom_effect(const Ability& ability, const Effect& step, Ref target, int hand_slot) {
    const auto source = ability.source;
    const int owner = source.seat;
    const int enemy = 1 - owner;
    auto& player = state_.players[owner];
    const auto& opponent = state_.players[enemy];
    if (owner != observer_ && player.hand_count > 0) uncertain();
    const auto primitive = [&](Operation operation, Target destination, int amount = 0, CardId card = 0) {
        effect(ability, {ability.trigger, operation, destination, amount, card},
               destination == Target::RandomEnemyFollower && rules_.random_targets == RandomTargets::AtAbilityStart
                   ? random_enemy(owner) : 0);
    };
    const auto hand_cost_sum = [](const PlayerState& side) {
        std::array<int, kHandLimit> costs{};
        for (int i = 0; i < side.hand_count; ++i) costs[i] = definition(side.hand[i]).cost;
        std::partial_sort(costs.begin(), costs.begin() + 3, costs.end(), std::greater{});
        return costs[0] + costs[1] + costs[2];
    };
    switch (step.handler) {
    case CardHandler::None:
        break;
    case CardHandler::EvolveWithAlly:
        if (std::any_of(player.board.begin(), player.board.begin() + player.board_count,
                        [&](const Permanent& object) {
                            return object.creation_order != source.order && object.evolution != Evolution::None;
                        })) {
            evolve(target, Evolution::Normal, false);
        }
        break;
    case CardHandler::BanishLowestAndEvolve: {
        if (player.hand_count == 0) {
            if (rules_.rabbit_evolves_with_empty_hand) {
                evolve(target, Evolution::Normal, false);
            }
            break;
        }
        if (has_unobserved_hand(owner)) {
            mark_unobserved_card();
            mark_hidden();
        }
        int lowest = std::numeric_limits<int>::max();
        std::array<int, kHandLimit> candidates{};
        int count = 0;
        for (int i = 0; i < player.hand_count; ++i) {
            const int cost = card_cost(definition(player.hand[i]), player.hand_modifiers[i]);
            if (cost < lowest) {
                lowest = cost;
                count = 0;
            }
            if (cost == lowest) {
                candidates[count++] = i;
            }
        }
        erase_player_hand(owner, candidates[pick_index(count)]);
        evolve(target, Evolution::Normal, false);
        break;
    }
    case CardHandler::MatchPlayedCost:
        if (board_cost(player, ability.event_cost, false, source.order, ability.event_order) ||
            board_cost(opponent, ability.event_cost, false)) {
            primitive(Operation::ReduceCountdown, Target::Self, 1);
        }
        break;
    case CardHandler::SequenceCity:
    case CardHandler::SequenceOmerio:
        if (auto* object = find(state_, source)) {
            const int index = object->sequence;
            object->sequence = static_cast<std::uint8_t>((index + 1) % 3);
            if (step.handler == CardHandler::SequenceCity) {
                if (index == 0) primitive(Operation::Damage, Target::RandomEnemyFollower, 2);
                if (index == 1) heal_leader(owner, 2);
                if (index == 2) {
                    damage_leader(enemy, 2);
                    check_health(owner);
                    if (state_.outcome == Outcome::Ongoing) destroy(source, source, true);
                }
            } else {
                if (index == 0) primitive(Operation::Damage, Target::RandomEnemyTwo, 3);
                if (index == 1) heal_leader(owner, 2);
                if (index == 2) enter(owner, step.card);
            }
        }
        break;
    case CardHandler::DrawCrest:
        if (ability.event_cost >= 1 && ability.event_cost <= 6) {
            enter(ability.event_cost % 2 ? owner : enemy,
                  pick_index(2) == 0 ? step.card : kHolyFalcon);
        }
        break;
    case CardHandler::CopyBanish:
        copy_board(owner, ability.event_object);
        break;
    case CardHandler::RandomKeywords: {
        std::array<std::uint16_t, 6> options{FLAG_STORM, kBane, kIntimidate, kDrain, kAura, kBarrier};
        if (auto* object = find(state_, target)) {
            for (int i = 0; i < 3; ++i) {
                std::swap(options[i], options[i + pick_index(static_cast<int>(options.size()) - i)]);
                object->abilities |= options[i];
                object->modifiers.abilities |= options[i];
            }
        }
        break;
    }
    case CardHandler::MarkHoneyTarget:
        if (auto* object = find(state_, source)) object->abilities |= kBarrier;
        if (auto* object = find(state_, target)) {
            object->abilities |= kAttackBlocked;
            object->banish_at_end = true;
        }
        break;
    case CardHandler::TopHandCosts:
        if (opponent.hand_count > 0) mark_hidden();
        if (has_unobserved_hand(owner)) {
            mark_unobserved_card();
            mark_hidden();
        }
        if (hand_cost_sum(player) > hand_cost_sum(opponent)) primitive(Operation::Destroy, Target::AllEnemyFollowers);
        break;
    case CardHandler::ReplaceHandFromEnemyDeck:
        if (hand_slot >= 0 && hand_slot < player.hand_count && opponent.deck_remaining() > 0) {
            uncertain();
            const int slot = opponent.deck_idx + pick_index(opponent.deck_remaining());
            player.hand[hand_slot] = opponent.deck[slot];
            player.hand_modifiers[hand_slot] = opponent.deck_modifiers[slot];
            if (owner == observer_ && unobserved_hand_mask_) {
                *unobserved_hand_mask_ |= static_cast<std::uint16_t>(1U << hand_slot);
            }
        }
        break;
    case CardHandler::CopyEnemyHand:
        if (opponent.hand_count > 0) {
            uncertain();
            const int slot = pick_index(opponent.hand_count);
            auto modifiers = opponent.hand_modifiers[slot];
            modifiers.cost = static_cast<std::int8_t>(std::max(-127, modifiers.cost - step.amount));
            add_unobserved_card(owner, opponent.hand[slot], modifiers);
        }
        break;
    case CardHandler::DiscardThenCopy:
        for (int i = player.hand_count - 1; i >= 0; --i) {
            if (ability.selection & (1 << i)) discard(owner, i);
        }
        for (int i = 0, count = std::min(3, static_cast<int>(player.hand_count)); i < count; ++i) {
            if (slot_is_unobserved(owner, i)) {
                mark_unobserved_card();
                add_unobserved_card(owner, player.hand[i], player.hand_modifiers[i]);
            } else {
                add_card(player, player.hand[i], player.hand_modifiers[i]);
            }
        }
        break;
    case CardHandler::DrawDiscounted:
        draw_cards(owner, step.amount, DeckFilter::Any, step.threshold);
        break;
    case CardHandler::DrawWardCount: {
        const auto end = player.board.begin() + player.board_count;
        const int count = static_cast<int>(std::count_if(player.board.begin(), end,
            [](const auto& object) { return follower(object) && has(object, FLAG_WARD); }));
        draw_cards(owner, count);
        break;
    }
    case CardHandler::NeutralHandDamage:
    case CardHandler::AmuletHandDamage: {
        if (has_unobserved_hand(owner)) {
            mark_unobserved_card();
            mark_hidden();
        }
        const auto filter = step.handler == CardHandler::NeutralHandDamage ? DeckFilter::Neutral : DeckFilter::Amulet;
        const int count = static_cast<int>(std::count_if(player.hand.begin(), player.hand.begin() + player.hand_count,
            [&](CardId id) { return deck_filter(definition(id), filter); }));
        primitive(Operation::Damage, step.target, count);
        break;
    }
    case CardHandler::DestroyAndReward:
        if (const auto* object = find(state_, target)) {
            const bool own_amulet = target.seat == owner && !follower(*object);
            destroy(target, source, true);
            if (own_amulet) {
                if (ability.trigger == OnPlay) primitive(Operation::RestorePp, Target::None, step.amount);
                else primitive(Operation::Damage, Target::AllEnemyFollowers, step.amount);
            }
        }
        break;
    case CardHandler::SummonFromDeckEvolved:
        if (player.board_count < kBoardLimit) {
            if (player.deck_remaining() > 0) uncertain();
            std::array<int, kDeckCapacity> slots{};
            int count = 0;
            for (int i = player.deck_idx; i < player.deck_end; ++i) {
                const auto& card = definition(player.deck[i]);
                if (card.kind == Kind::Follower && card_cost(card, player.deck_modifiers[i]) <= step.amount) {
                    slots[count++] = i;
                }
            }
            if (count > 0) {
                const auto [card, modifiers] = take_deck_card(player, slots[pick_index(count)]);
                evolve(enter(owner, card, modifiers), Evolution::Super, false);
            }
        }
        break;
    case CardHandler::DestroyedToHand:
    case CardHandler::DestroyedToDeck:
        if (const auto card = past_card(owner, std::numeric_limits<int>::max(), step.handler == CardHandler::DestroyedToDeck, false)) {
            if (step.handler == CardHandler::DestroyedToHand) add_card(player, card);
            else return_to_deck(state_, player, card, {});
        }
        break;
    case CardHandler::SummonDestroyedFollowers:
    case CardHandler::SummonDestroyedAmulets: {
        std::array<CardId, kBoardLimit> used{};
        const bool amulet = step.handler == CardHandler::SummonDestroyedAmulets;
        for (int i = 0; i < step.amount && player.board_count < kBoardLimit; ++i) {
            const auto card = past_card(owner, amulet ? 2 : std::numeric_limits<int>::max(), false, amulet,
                                        std::span(used).first(i));
            if (card == 0) break;
            used[i] = card;
            enter(owner, card);
        }
        break;
    }
    case CardHandler::GrantAttackTwice:
        if (const auto* object = find(state_, target); object && object->evolution == Evolution::Super) {
            effect(ability, {ability.trigger, Operation::TemporaryAbilities, Target::AttackingFollower,
                             0, 0, 0, 0, CrestId::None, kDoubleStrike}, 0);
        }
        break;
    case CardHandler::ReturnRandomHand:
        for (int i = 0; i < step.amount && player.hand_count > 0; ++i) {
            const int slot = pick_index(player.hand_count);
            return_to_deck(state_, player, player.hand[slot], player.hand_modifiers[slot]);
            erase_player_hand(owner, slot);
        }
        break;
    case CardHandler::BuddingBuddies:
        evolve(enter(owner, step.card), Evolution::Normal, false);
        evolve(source, Evolution::Normal, false);
        break;
    case CardHandler::CopyEnemyDeck: {
        std::array<int, kDeckCapacity> slots{};
        const int remaining = opponent.deck_remaining();
        if (remaining > 0) uncertain();
        for (int i = 0; i < remaining; ++i) slots[i] = opponent.deck_idx + i;
        for (int i = 0; i < std::min(step.amount, remaining); ++i) {
            std::swap(slots[i], slots[i + pick_index(remaining - i)]);
            add_unobserved_card(owner, opponent.deck[slots[i]], opponent.deck_modifiers[slots[i]]);
        }
        break;
    }
    case CardHandler::SummonDeckDistinct: {
        if (player.deck_remaining() > 0) uncertain();
        std::array<CardId, kBoardLimit> selected{};
        for (int i = 0; i < step.amount && player.board_count < kBoardLimit; ++i) {
            std::array<int, kDeckCapacity> slots{};
            int count = 0;
            for (int index = player.deck_idx; index < player.deck_end; ++index) {
                const auto& def = definition(player.deck[index]);
                if (def.kind == Kind::Follower && deck_filter(def, step.filter) &&
                    card_cost(def, player.deck_modifiers[index]) <= step.threshold &&
                    std::find(selected.begin(), selected.begin() + i, def.id) == selected.begin() + i) {
                    slots[count++] = index;
                }
            }
            if (count == 0) break;
            const auto [card, modifiers] = take_deck_card(player, slots[pick_index(count)]);
            selected[i] = card;
            enter(owner, card, modifiers);
        }
        break;
    }
    case CardHandler::Marlon: {
        std::array<Ref, kBoardLimit> targets{};
        int count = 0;
        for (int i = 0; i < opponent.board_count; ++i) {
            if (follower(opponent.board[i])) targets[count++] = {enemy, opponent.board[i].creation_order};
        }
        const int difference = count - (player.board_count - amulets(player));
        for (int i = 0; i < difference; ++i) {
            std::swap(targets[i], targets[i + pick_index(count - i)]);
            destroy(targets[i], source, true);
        }
        break;
    }
    case CardHandler::AlmerizeSequence:
        if (auto* crest = find(state_, owner, source.order)) {
            const auto card = crest->sequence == 0 ? step.card : step.alternate_card;
            crest->sequence ^= 1;
            CardModifiers modifiers{};
            modifiers.cost = static_cast<std::int8_t>(-definition(card).cost);
            add_card(player, card, modifiers);
        }
        break;
    case CardHandler::RepeatedAttackDamage: {
        const auto* attacker = find(state_, source);
        const int count = attacker && attacker->evolution != Evolution::None ? 3 : 1;
        for (int i = 0; i < count && find(state_, target) && state_.outcome == Outcome::Ongoing; ++i) {
            damage(target, step.amount);
            settle(source);
        }
        break;
    }
    case CardHandler::CopySource:
        if (const auto* object = find(state_, source)) copy_board(owner, *object);
        break;
    case CardHandler::NewspaperCopies:
        if (const auto* object = find(state_, source)) {
            const auto& printed = definition(step.card);
            CardModifiers modifiers{};
            modifiers.attack = static_cast<std::int8_t>(std::clamp(object->atk - printed.atk, -127, 127));
            modifiers.defense = static_cast<std::int8_t>(std::clamp(object->hp - printed.hp, -127, 127));
            for (int i = 0; i < step.amount; ++i) enter(owner, step.card, modifiers);
        }
        break;
    case CardHandler::DiscardAndHeal: {
        int amount = step.amount;
        if (hand_slot >= 0 && hand_slot < player.hand_count) {
            if (definition(player.hand[hand_slot]).kind == Kind::Spell) amount *= 2;
            discard(owner, hand_slot);
        }
        heal_leader(owner, amount);
        break;
    }
    case CardHandler::EnhanceTroops: {
        const auto buff = Effect{.operation=Operation::Buff, .target=Target::ChosenOwnFollower, .amount=step.threshold,
                                 .health_modifier=step.health_modifier, .granted_abilities=step.granted_abilities};
        const auto apply_buff = [&](Ref ref) {
            auto subject = ability;
            subject.chosen = ref.order;
            effect(subject, buff, 0);
        };
        for (int i = 0; i < step.amount; ++i) apply_buff(enter(owner, step.card));
        if (step.target == Target::Self) apply_buff(source);
        break;
    }
    case CardHandler::DrawCostDamage:
        if (const int drawn = draw_cards(owner, 1, DeckFilter::Follower); drawn >= 0) {
            primitive(Operation::Damage, Target::RandomEnemyFollower, drawn);
        }
        break;
    case CardHandler::ExileOdds: {
        if (player.deck_remaining() > 0) uncertain();
        int retained = player.deck_idx;
        int removed = 0;
        for (int i = player.deck_idx; i < player.deck_end; ++i) {
            const int cost = card_cost(definition(player.deck[i]), player.deck_modifiers[i]);
            if (cost >= 1 && cost <= 9 && cost % 2 == 1) {
                ++removed;
            } else {
                player.deck[retained] = player.deck[i];
                player.deck_modifiers[retained++] = player.deck_modifiers[i];
            }
        }
        player.deck_end = static_cast<std::uint8_t>(retained);
        primitive(Operation::SplitDamage, Target::None, removed);
        break;
    }
    case CardHandler::ReflectionEntry: {
        const int prior = std::max(0, static_cast<int>(ensure_counter(player, ability.card).entered) - 1);
        effect(ability, {.operation=Operation::Buff, .target=Target::Self, .amount=prior, .health_modifier=prior}, 0);
        if (prior >= 2) evolve(source, Evolution::Normal, false);
        break;
    }
    case CardHandler::ReanimateMultiple:
        for (int i = 0; i < step.amount; ++i) reanimate(owner, step.threshold);
        break;
    case CardHandler::BanishAndCopy:
        if (const auto* object = find(state_, target)) {
            const auto snapshot = *object;
            banish(target);
            copy_board(owner, snapshot);
        }
        break;
    case CardHandler::DestroyLastWordsPair: {
        std::array<EntityId, kBoardLimit> candidates{};
        int count = 0;
        for (int i = 0; i < player.board_count; ++i) {
            const auto& object = player.board[i];
            if (has_last_words(object)) candidates[count++] = object.creation_order;
        }
        if (count > 0) {
            destroy({owner, candidates[pick_index(count)]}, source, true);
            destroy({enemy, random_enemy(owner)}, source, true);
        }
        break;
    }
    case CardHandler::YubeAttack:
        if (auto* crest = find(state_, owner, source.order)) {
            primitive(Operation::TemporaryAttack, Target::EventFollower, 1);
            if (crest->sequence != ply(state_)) {
                crest->sequence = static_cast<std::uint8_t>(ply(state_));
                add_card(player, step.card);
            }
        }
        break;
    case CardHandler::ChainDamage:
        for (int i = 0; i < step.threshold && state_.outcome == Outcome::Ongoing; ++i) {
            primitive(Operation::Damage, Target::RandomEnemyFollower, step.amount);
            settle(source);
            if (step.health_modifier) {
                primitive(Operation::Damage, Target::BothLeaders, step.health_modifier);
                settle(source);
            }
        }
        break;
    case CardHandler::DrawSuperDiscounted:
        draw_cards(owner, step.amount, DeckFilter::Any, state_.turn_number >= 7 - owner ? 1 : 0);
        break;
    case CardHandler::KittyTrick: {
        const int omitted = pick_index(3);
        if (omitted != 0) enter(owner, step.card);
        if (omitted != 1) heal_leader(owner, 2);
        if (omitted != 2) primitive(Operation::AddEarth, Target::None, 3);
        break;
    }
    case CardHandler::TransformBoardFromDeck: {
        if (player.deck_remaining() > 0) uncertain();
        std::array<int, kDeckCapacity> choices{};
        int count = 0;
        for (int i = player.deck_idx; i < player.deck_end; ++i) {
            if (definition(player.deck[i]).kind == Kind::Follower) choices[count++] = i;
        }
        if (count == 0) break;
        for (int i = 0; i < player.board_count; ++i) {
            if (!follower(player.board[i])) continue;
            const int index = choices[pick_index(count)];
            transform({owner, player.board[i].creation_order}, player.deck[index], player.deck_modifiers[index], Evolution::None);
        }
        break;
    }
    case CardHandler::NoinSequence:
        if (auto* crest = find(state_, owner, source.order)) {
            switch (crest->sequence++) {
            case 0: primitive(Operation::Damage, Target::RandomEnemyFollower, 3); break;
            case 1: heal_leader(owner, 2); break;
            case 2:
                primitive(Operation::RestoreEp, Target::None, 1);
                destroy_crest(owner, source.order, false);
                break;
            default: break;
            }
        }
        break;
    case CardHandler::OnceCrest:
        if (auto* crest = find(state_, owner, source.order);
            crest && state_.turn_player == owner && crest->sequence != ply(state_)) {
            crest->sequence = static_cast<std::uint8_t>(ply(state_));
            if (ability.trigger == OnAmuletAct) {
                primitive(Operation::RestorePp, Target::None, 1);
            } else if (ability.trigger == OnPlayedFollower) {
                primitive(Operation::AutoEvolve, Target::EventFollower);
            } else {
                effect(ability, {.operation=Operation::Buff, .target=Target::EventFollower, .granted_abilities=FLAG_STORM}, 0);
            }
        }
        break;
    case CardHandler::SlothRandom: {
        const bool curse = ability.origin == Origin::Crest;
        auto* crest = curse ? find(state_, owner, source.order) : nullptr;
        auto* body = curse ? nullptr : find(state_, source);
        if (!crest && !body) break;
        auto& used = crest ? crest->sequence : body->sequence;
        std::array<int, 3> choices{};
        int count = 0;
        for (int i = 0; i < 3; ++i) if (!(used & (1 << i))) choices[count++] = i;
        if (count == 0) break;
        const int choice = choices[pick_index(count)];
        used |= static_cast<std::uint8_t>(1 << choice);
        const int delta = curse ? -1 : 1;
        switch (choice) {
        case 0: primitive(Operation::ApplyTemporaryHandCost, Target::AllHand, -delta); break;
        case 1:
            effect(ability, {.operation=Operation::Buff, .target=Target::AllOwnFollowers,
                             .amount=2 * delta, .health_modifier=2 * delta}, 0);
            break;
        case 2:
            if (curse) damage_leader(owner, 3);
            else heal_leader(owner, 3);
            break;
        }
        break;
    }
    case CardHandler::AddFromDestroyed: {
        std::array<CardId, kHandLimit> selected{};
        for (int i = 0; i < step.amount; ++i) {
            const auto card = past_card(owner, std::numeric_limits<int>::max(), false, false,
                                        std::span(selected).first(i), step.filter);
            if (card == 0) break;
            selected[i] = card;
            add_card(player, card);
        }
        break;
    }
    case CardHandler::DrawDistinctOneCostSpells: {
        if (player.deck_remaining() > 0) uncertain();
        std::array<std::string_view, kHandLimit> selected{};
        for (int i = 0; i < step.amount; ++i) {
            std::array<int, kDeckCapacity> slots{};
            int count = 0;
            for (int index = player.deck_idx; index < player.deck_end; ++index) {
                const auto& def = definition(player.deck[index]);
                if (def.kind == Kind::Spell && card_cost(def, player.deck_modifiers[index]) == 1 &&
                    std::find(selected.begin(), selected.begin() + i, def.name) == selected.begin() + i) slots[count++] = index;
            }
            if (!count) break;
            const auto [card, modifiers] = take_deck_card(player, slots[pick_index(count)]);
            selected[i] = definition(card).name;
            keep_drawn(owner, card, modifiers);
        }
        break;
    }
    case CardHandler::CopyHandToBoard:
        if (hand_slot >= 0 && hand_slot < player.hand_count) enter(owner, player.hand[hand_slot], player.hand_modifiers[hand_slot]);
        break;
    case CardHandler::GiveRandomPuppetAbility: {
        CardModifiers modifiers{};
        modifiers.abilities = pick_index(2) == 0 ? kBane : FLAG_WARD;
        add_card(player, step.card, modifiers);
        break;
    }
    case CardHandler::GiveChosenDiscounted:
        if (const auto* object = find(state_, target)) {
            CardModifiers modifiers{};
            modifiers.cost = static_cast<std::int8_t>(-step.amount);
            add_card(player, printed_card(object->card_id), modifiers);
        }
        break;
    case CardHandler::DrawToBoard: {
        const int slot = player.hand_count;
        draw_cards(owner, 1);
        if (state_.outcome != Outcome::Ongoing || player.hand_count <= slot || player.board_count == kBoardLimit) break;
        const auto id = player.hand[slot];
        if (definition(id).kind == Kind::Follower && definition(id).class_name == "エルフ") {
            const auto modifiers = player.hand_modifiers[slot];
            erase_player_hand(owner, slot);
            enter(owner, id, modifiers);
        }
        break;
    }
    case CardHandler::LightOff: {
        if (player.deck_remaining() > 0) uncertain();
        int retained = player.deck_idx;
        for (int slot = player.deck_idx; slot < player.deck_end; ++slot) {
            if (definition(player.deck[slot]).cost <= 4) continue;
            player.deck[retained] = player.deck[slot];
            player.deck_modifiers[retained++] = player.deck_modifiers[slot];
        }
        player.deck_end = static_cast<std::uint8_t>(retained);
        int discarded = 0;
        for (int slot = player.hand_count - 1; slot >= 0; --slot) {
            if (!definition(player.hand[slot]).clue) continue;
            discard(owner, slot);
            ++discarded;
        }
        for (int hit = 0; hit < discarded && state_.outcome == Outcome::Ongoing; ++hit) {
            if (const auto selected = random_enemy(owner)) damage({enemy, selected}, 8);
            settle(source);
        }
        break;
    }
    case CardHandler::Omega: {
        const int first = pick_index(4);
        const int other = pick_index(3);
        const int second = other + (other >= first);
        for (int index = 0; index < 4 && state_.outcome == Outcome::Ongoing; ++index) {
            if (index != first && index != second) continue;
            switch (index) {
            case 0: destroy({enemy, random_enemy(owner)}, source, true); break;
            case 1: damage_leader(enemy, 2); break;
            case 2: primitive(Operation::RestorePp, Target::None, 2); break;
            case 3:
                effect(ability, {.operation=Operation::Buff, .target=Target::Self, .amount=4, .health_modifier=4}, 0);
                if (!follower_fanfare_suppressed(player) && fanfare_repeats_ < rules_.fanfare_repeat_limit) {
                    ++fanfare_repeats_;
                    board_ability(source, OnPlay);
                }
                break;
            }
            settle(source);
        }
        break;
    }
    case CardHandler::Orluon:
        for (int hit = 0; hit < 3 && state_.outcome == Outcome::Ongoing; ++hit) {
            std::array<Ref, 2 + 2 * kBoardLimit> choices{{{owner}, {enemy}}};
            int count = 2;
            for (int seat = 0; seat < kPlayers; ++seat) {
                const auto& side = state_.players[seat];
                for (int slot = 0; slot < side.board_count; ++slot) {
                    const auto& object = side.board[slot];
                    if (follower(object) && object.creation_order != source.order) choices[count++] = {seat, object.creation_order};
                }
            }
            const auto selected = choices[pick_index(count)];
            if (selected.order) damage(selected, 7);
            else damage_leader(selected.seat, 7);
            settle(source);
        }
        break;
    case CardHandler::NoLife:
        if (const auto* original = find(state_, source)) {
            const auto copy = copy_board(owner, *original);
            const auto* object = find(state_, source);
            if (object->atk == object->hp) break;
            const bool higher_attack = object->atk > object->hp;
            auto twin = ability;
            twin.source = copy;
            effect(twin, {.operation=Operation::Buff, .target=Target::Self,
                          .granted_abilities=static_cast<std::uint16_t>(higher_attack ? FLAG_WARD : kBane)}, 0);
            effect(ability, {.operation=Operation::Buff, .target=Target::Self,
                             .granted_abilities=static_cast<std::uint16_t>(higher_attack ? FLAG_STORM : kDrain)}, 0);
        }
        break;
    case CardHandler::AbyssCrystal: {
        int faith = 0;
        for (int slot = 0; slot < player.faith_count; ++slot) {
            if (player.faiths[slot] == step.alternate_card) faith = player.faith_values[slot];
        }
        std::array<int, 3> portions{};
        for (int roll = 0; roll < faith; ++roll) ++portions[pick_index(3)];
        if (const auto gem = enter(owner, step.card); gem.order) {
            auto summoned = ability;
            summoned.source = gem;
            effect(summoned, {.operation=Operation::Buff, .target=Target::Self, .amount=portions[0],
                              .health_modifier=portions[0]}, 0);
        }
        if (portions[1]) heal_leader(owner, portions[1]);
        if (portions[2]) damage_leader(enemy, portions[2]);
        break;
    }
    case CardHandler::AbyssBook:
        if (const auto* object = find(state_, target)) {
            const bool own_amulet = target.seat == owner && !follower(*object);
            destroy(target, source, true);
            if (own_amulet) {
                damage_leader(enemy, 2);
                if (state_.players[enemy].leader_hp > 0) add_card(player, ability.card);
            }
        }
        break;
    case CardHandler::BurnedHeal:
        if (auto* crest = find(state_, owner, source.order);
            crest && (owner == state_.turn_player || rules_.burn_crest_heal_on_enemy_turn) &&
            crest->last_trigger_round != state_.turn_number) {
            crest->last_trigger_round = state_.turn_number;
            damage_leader(owner, 1);
        }
        break;
    }
}

void Resolver::play(Action action) {
    const int seat = state_.turn_player;
    auto& player = state_.players[seat];
    const CardId card = player.hand[action.src];
    reveal_played_card(player, card);
    const auto modifiers = player.hand_modifiers[action.src];
    const bool suppress = follower_fanfare_suppressed(player);
    const auto payment = play_cost(definition(card), player.available_pp(), modifiers, suppress);
    const EntityId chosen = chosen_entity(state_, action, true);
    const int mode = play_mode(payment, action.choice);
    const int effective_cost = card_cost(definition(card), modifiers);
    const int enhanced = payment.enhanced ? payment.cost : 0;
    int selection = action.selection;
    if (action.selection_in_hand) {
        const int below = (1 << action.src) - 1;
        selection = (selection & below) | ((selection >> 1) & ~below);
    }
    spend(player, payment.cost);
    erase_player_hand(seat, action.src);
    for (int slot = 0; slot < player.hand_count; ++slot) {
        const int discount = definition(player.hand[slot]).hand_play_discount;
        if (discount) modify_hand_cost(player.hand_modifiers[slot], -discount, true);
    }
    ++counts(player, card).play;
    ++player.plays_this_turn;
    if (payment.form.cost >= 1 && payment.form.cost <= 8) player.played_cost_mask |= 1 << payment.form.cost;
    Ref created{seat};
    if (payment.form.kind == Kind::Spell) {
        ++player.shadows;
        for (int i = 0; i < player.hand_count; ++i) {
            if (slot_is_unobserved(seat, i) && definition(player.hand[i]).spellboost) {
                mark_unobserved_card();
            }
            boost_card(definition(player.hand[i]), player.hand_modifiers[i], 1);
        }
        Ability ability{OnPlay, Origin::Spell, {seat}, payment.form.id, CrestId::None, chosen, mode,
                        modifiers.spellboost, modifiers.fused_materials, static_cast<std::uint16_t>(selection)};
        ability.effective_cost = effective_cost;
        ability.enhance_cost = enhanced;
        if (action.secondary_slot >= 0) {
            ability.discarded_slot = action.secondary_slot - (action.secondary_slot > action.src);
        }
        enqueue(ability);
    } else {
        created = enter(seat, payment.form.id, modifiers);
        if (!(suppress && payment.form.kind == Kind::Follower)) {
            board_ability(created, OnPlay, chosen, mode, static_cast<std::uint16_t>(selection), effective_cost, enhanced);
        }
    }
    Ability event{OnOtherPlay};
    event.event_order = created.order;
    event.event_cost = payment.form.cost;
    event.event_card = payment.form.id;
    broadcast(seat, event, true, false);
    if (payment.form.kind == Kind::Follower) {
        event.trigger = OnPlayedFollower;
        broadcast(seat, event, false, true);
    } else if (payment.form.kind == Kind::Spell) {
        event.trigger = OnPlayedSpell;
        broadcast(seat, event, false, true);
    }
    if (payment.enhanced) {
        progress_faith(seat, OnEnhanced);
        event.trigger = OnEnhanced;
        broadcast(seat, event, false, true);
    }
    settle({seat});
    resolve();
}

void Resolver::fuse(Action action) {
    const int seat = state_.turn_player;
    auto& player = state_.players[seat];
    const auto card = player.hand[action.src];
    if (slot_is_unobserved(seat, action.src)) mark_unobserved_card();
    const auto& host = definition(card);
    auto modifiers = player.hand_modifiers[action.src];
    modifiers.fused_ply = static_cast<std::uint8_t>(ply(state_));
    modifiers.fused_materials = static_cast<std::uint8_t>(std::min(255, modifiers.fused_materials + std::popcount(action.choice)));
    int slot = action.src;
    int sum_cost = 0;
    bool fused_amulet = false;
    for (int i = player.hand_count - 1; i >= 0; --i) {
        if (i != action.src && (action.choice & (1 << (i - (i > action.src))))) {
            const auto& material = definition(player.hand[i]);
            sum_cost += card_cost(material, player.hand_modifiers[i]);
            fused_amulet |= material.kind == Kind::Amulet;
            if (host.fusion_recipe == FusionRecipe::DistinctPair) {
                modifiers.effect_flags |= material.id == host.fusion_materials[0] ? kFusedBeta : kFusedGamma;
            }
            erase_player_hand(seat, i);
            slot -= i < action.src;
        }
    }
    CardId result = 0;
    switch (host.fusion_recipe) {
    case FusionRecipe::None: break;
    case FusionRecipe::Fixed: result = host.fusion_results[0]; break;
    case FusionRecipe::ByCost:
        if (sum_cost > 0) result = host.fusion_results[std::min(3, sum_cost) - 1];
        break;
    case FusionRecipe::DistinctPair:
        if ((modifiers.effect_flags & (kFusedBeta | kFusedGamma)) == (kFusedBeta | kFusedGamma)) {
            result = host.fusion_results[0];
        }
        break;
    }
    if (result) {
        player.hand[slot] = result;
        modifiers = {};
        if (!rules_.refusion_after_hand_transform) modifiers.fused_ply = static_cast<std::uint8_t>(ply(state_));
    }
    player.hand_modifiers[slot] = modifiers;
    enqueue({OnFuse, Origin::Spell, {seat}, card});
    broadcast(seat, {OnAnyFusion});
    if (fused_amulet) broadcast(seat, {OnAmuletFused}, true, false);
    resolve();
}

void Resolver::manual_evolve(Action action) {
    const int seat = state_.turn_player;
    const auto ref = Ref{seat, state_.players[seat].board[action.src].creation_order};
    const EntityId chosen = chosen_entity(state_, action);
    evolve(ref, action.type == ActionType::SuperEvolve ? Evolution::Super : Evolution::Normal, true, chosen, action.selection, action.choice);
    settle(ref);
    resolve();
}

void Resolver::act(Action action) {
    const int seat = state_.turn_player;
    auto& player = state_.players[seat];
    auto& object = player.board[action.src];
    spend(player, definition(object.card_id).act_cost);
    ++counts(player, object.card_id).act;
    object.acted_this_turn = true;
    player.acted_amulet = true;
    for (int slot = 0; slot < player.hand_count; ++slot) {
        const int discount = definition(player.hand[slot]).hand_act_discount;
        if (discount) modify_hand_cost(player.hand_modifiers[slot], -discount, false);
    }
    if (definition(object.card_id).clue) {
        player.acted_evidence = true;
        for (int slot = 0; slot < player.hand_count; ++slot) {
            const auto& card = definition(player.hand[slot]);
            if (!card.hand_swap_on_evidence) continue;
            auto& mods = player.hand_modifiers[slot];
            const int attack = card.atk + mods.attack;
            const int health = card.hp + mods.defense;
            mods.attack = static_cast<std::int8_t>(std::clamp(health + 1 - card.atk, -127, 127));
            mods.defense = static_cast<std::int8_t>(std::clamp(attack - card.hp, -127, 127));
        }
    }
    board_ability({seat, object.creation_order}, OnAct, chosen_entity(state_, action));
    broadcast(seat, {OnAmuletAct});
    resolve();
}

void Resolver::attack(Action action) {
    const int seat = state_.turn_player;
    const int enemy = 1 - seat;
    auto& player = state_.players[seat];
    auto& attacker = player.board[action.src];
    const Ref source{seat, attacker.creation_order};
    ++counts(player, attacker.card_id).attack;
    attacker.attacked_thrice = attacker.attacked_twice;
    attacker.attacked_twice = attacker.attacked_this_turn;
    attacker.attacked_this_turn = true;
    attacker.abilities &= ~kAmbush;
    const bool face = action.dst == kEnemyLeader;
    if (face) player.attacked_leader_this_turn = true;
    const Ref target{enemy, face ? 0U : state_.players[enemy].board[action.dst].creation_order};
    if (!face && protected_super(source, attacker)) {
        super_attacker_ = source;
    }
    board_ability(source, OnAttack, target.order);
    Ability attack_event{OnAlliedAttack};
    attack_event.event_order = source.order;
    attack_event.event_card = attacker.card_id;
    broadcast(seat, attack_event, false, true);
    if (!face) {
        Ability event{OnAttack};
        event.chosen = source.order;
        if (attacker.evolution == Evolution::Super) broadcast(seat, event, false, true);
        board_ability(source, OnClash, target.order);
        board_ability(target, OnClash, source.order);
    }
    resolve();

    auto* active = find(state_, source);
    if (state_.outcome != Outcome::Ongoing || !active) {
        return;
    }
    if (face) {
        const int dealt = damage_leader(enemy, active->atk);
        check_health(seat);
        if (state_.outcome == Outcome::Ongoing && has(*active, kDrain)) {
            heal_leader(seat, dealt);
            resolve();
        }
        return;
    }
    const auto* defending = find(state_, target);
    if (!defending) {
        return;
    }
    const auto defense = *defending;
    const auto offense = *active;
    damage(target, offense.atk);
    const int dealt = defense.hp - find(state_, target)->hp;
    const bool retaliates = rules_.blocked_attacker_can_retaliate || !has(defense, kAttackBlocked);
    if (retaliates) {
        damage(source, defense.atk);
    }
    if (has(offense, kBane) && !protected_destruction(target, defense)) {
        find(state_, target)->hp = 0;
    }
    if (retaliates && has(defense, kBane) && !protected_destruction(source, offense)) {
        find(state_, source)->hp = 0;
    }
    if (has(offense, kDrain)) {
        heal_leader(seat, dealt);
    }
    settle(source);
    resolve();
}

void Resolver::tick_crests(int seat, CrestClock clock) {
    auto& player = state_.players[seat];
    const auto start = player.crests;
    const int count = player.crest_count;
    for (int i = 0; i < count && state_.outcome == Outcome::Ongoing; ++i) {
        const auto order = start[i].creation_order;
        const auto id = start[i].id;
        const auto* def = find_crest(id);
        if (!def || !find(state_, seat, order)) {
            continue;
        }
        enqueue({clock == CrestClock::Start ? TurnStart : TurnEnd, Origin::Crest, {seat, order}, 0, id});
        if (id == CrestId::Lala && clock == CrestClock::Start && rules_.lala_summon_timing == LalaSummonTiming::OwnerTurnStart) {
            enqueue({EnemyTurnEnd, Origin::Crest, {seat, order}, 0, id});
        }
        resolve();
        if (state_.outcome != Outcome::Ongoing) {
            return;
        }
        const auto timing = rules_.crest_clock == CrestClock::Printed ? def->clock : rules_.crest_clock;
        auto* current = find(state_, seat, order);
        if (timing == clock && current && current->countdown > 0 && --current->countdown == 0) {
            destroy_crest(seat, order, true);
            resolve();
        }
    }
}

void Resolver::refresh(int seat) {
    auto& player = state_.players[seat];
    player.max_pp = static_cast<std::uint8_t>(std::min(kMaxPp, player.max_pp + 1));
    player.pp = player.max_pp;
    player.bonus_pp = 0;
    player.manual_evolved = false;
    player.plays_this_turn = 0;
    player.acted_evidence = false;
    player.acted_amulet = false;
    player.actions = {};
    for (int i = 0; i < player.other_counter_count; ++i) player.other_counters[i].actions = {};
    if (seat == 1 && state_.turn_number == kExtraPpRefillTurn && player.extra_pp_uses == 1) {
        player.extra_pp_available = true;
    }
    for (int i = 0; i < player.board_count; ++i) {
        auto& object = player.board[i];
        object.readied = follower(object) && object.entry_ply != ply(state_);
        object.attacked_this_turn = false;
        object.attacked_twice = false;
        object.attacked_thrice = false;
        object.acted_this_turn = false;
    }
}

void Resolver::start_turn() {
    const int seat = state_.turn_player;
    auto& player = state_.players[seat];
    if (rules_.refresh_before_start_effects) {
        refresh(seat);
    }
    std::vector<Ability> field;
    for (int index = 0; index < player.board_count;) {
        auto& object = player.board[index];
        const Ref ref{seat, object.creation_order};
        if (object.countdown > 0 && --object.countdown == 0) {
            destroy(ref, {seat}, false, &field);
            continue;
        }
        if (!object.silenced) {
            const auto& effects = definition(object.card_id).effects;
            if (std::any_of(effects.begin(), effects.end(), [](const auto& effect) {
                    return effect.trigger == TurnStart;
                })) {
                field.push_back({TurnStart, Origin::Board, ref, object.card_id});
            }
        }
        ++index;
    }
    tick_crests(seat, CrestClock::Start);
    if (state_.outcome != Outcome::Ongoing) {
        return;
    }
    // Destruction observers and detached countdown Last Words share the board pass.
    if (field.size() > 1) {
        std::stable_sort(field.begin(), field.end(), [](const auto& a, const auto& b) {
            return a.source.order < b.source.order;
        });
    }
    waiting_ = std::move(field);
    resolve();
    if (state_.outcome != Outcome::Ongoing) {
        return;
    }
    if (!rules_.refresh_before_start_effects) {
        refresh(seat);
    }
    draw_cards(seat, 1);
    resolve();
}

void Resolver::direct_summon(int seat) {
    auto& player = state_.players[seat];
    if (player.board_count == kBoardLimit) return;
    std::array<int, kDeckCapacity> candidates{};
    int count = 0;
    for (int slot = player.deck_idx; slot < player.deck_end; ++slot) {
        if (definition(player.deck[slot]).direct_summon_cost_curve) candidates[count++] = slot;
    }
    if (player.deck_remaining() > 0) uncertain();
    if (count == 0) return;
    const auto [card, modifiers] = take_deck_card(player, candidates[pick_index(count)]);
    reveal_played_card(player, card);
    enter(seat, card, modifiers);
    settle({seat});
}

void Resolver::end_turn() {
    const int seat = state_.turn_player;
    const bool last_turn = seat == 1 && state_.turn_number == kTurnLimit;
    const auto expire_match = [&] {
        state_.outcome = Outcome::BothLose;
        state_.end_reason = EndReason::TurnLimit;
    };
    if (last_turn && !rules_.turn_limit_after_end_effects) {
        expire_match();
        return;
    }
    auto& current = state_.players[seat];
    current.attacked_leader_last_turn = current.attacked_leader_this_turn;
    current.attacked_leader_this_turn = false;
    for (int slot = 0; slot < current.hand_count; ++slot) {
        const auto& card = definition(current.hand[slot]);
        const int combo = card.hand_end_combo;
        const int leader_hp = card.hand_end_hp_discount_threshold;
        auto& mods = current.hand_modifiers[slot];
        if (combo && current.plays_this_turn >= combo) mods.cost = static_cast<std::int8_t>(std::max(-127, mods.cost - 1));
        if (leader_hp && current.leader_hp <= leader_hp) modify_hand_cost(mods, -1, false);
    }
    tick_crests(seat, CrestClock::End);
    if (state_.outcome == Outcome::Ongoing && rules_.lala_summon_timing == LalaSummonTiming::OpponentTurnEnd) {
        broadcast(1 - seat, {EnemyTurnEnd}, false, true);
        resolve();
    }
    if (state_.outcome != Outcome::Ongoing) {
        return;
    }
    for (int offset = 0; offset < kPlayers; ++offset) {
        const int owner = (seat + offset) % kPlayers;
        const auto& player = state_.players[owner];
        for (int i = 0; i < player.board_count; ++i) {
            const auto& object = player.board[i];
            const Ref ref{owner, object.creation_order};
            board_ability(ref, offset == 0 ? TurnEnd : EnemyTurnEnd);
            if (offset == 0 && object.banish_at_end && !object.silenced) {
                triggered_.push_back({TurnEnd, Origin::DelayedBanish, ref});
            }
        }
    }
    if ((current.played_cost_mask & 0x1fe) == 0x1fe) {
        triggered_.push_back({TurnEnd, Origin::DirectSummon, {seat}});
    }
    resolve();
    if (state_.outcome != Outcome::Ongoing) {
        return;
    }
    for (auto& player : state_.players) {
        for (int i = 0; i < player.hand_count; ++i) player.hand_modifiers[i].temporary_cost_delta = 0;
        for (int i = player.deck_idx; i < player.deck_end; ++i) player.deck_modifiers[i].temporary_cost_delta = 0;
        for (int i = 0; i < player.board_count; ++i) {
            auto& object = player.board[i];
            object.modifiers.temporary_cost_delta = 0;
            object.atk = static_cast<std::int16_t>(std::max(0, object.atk - object.temporary_attack));
            object.temporary_attack = 0;
            if (object.temporary_abilities && object.expires_end_ply <= ply(state_)) {
                object.abilities &= ~object.temporary_abilities;
                object.temporary_abilities = 0;
                object.expires_end_ply = 0;
            }
        }
    }
    if (last_turn) {
        expire_match();
        return;
    }
    state_.players[seat].bonus_pp = 0;
    state_.turn_player = static_cast<std::uint8_t>(1 - seat);
    state_.turn_number += static_cast<std::uint8_t>(seat == 1);
    start_turn();
}

void replace_opening_cards(GameState& state, int mask, const RuleConfig& rules) {
    auto& player = state.players[state.turn_player];
    std::array<CardId, kOpeningHand> returned{};
    int corrections = 0;
    for (int i = 0; i < kOpeningHand; ++i) {
        if (mask & (1 << i)) {
            returned[corrections++] = player.hand[i];
            player.hand[i] = player.deck[player.deck_idx++];
        }
    }
    if (corrections != 0) {
        const auto end = std::move(player.deck.begin() + player.deck_idx, player.deck.begin() + player.deck_end,
                                   player.deck.begin() + kOpeningHand);
        std::copy_n(returned.begin(), corrections, end);
        std::copy_n(player.hand.begin(), kOpeningHand, player.deck.begin());
        player.deck_idx = kOpeningHand;
        if (rules.mulligan_return == MulliganReturn::Shuffle) {
            shuffle(state, player, kOpeningHand);
        }
    }
}

void check_deck(const Deck& deck) {
    int faiths = 0;
    std::array<int, kCardSlots> legacy_copies{};
    for (auto item = deck.begin(); item != deck.end(); ++item) {
        const auto id = *item;
        if (!is_deck_card(id)) {
            throw std::invalid_argument("Deck uses a non-deck card or exceeds the three-copy limit");
        }
        const bool faith = definition(id).kind == Kind::Faith;
        const auto copies = legacy_counter(id) ? ++legacy_copies[id - kFirstCard] : 1 + std::count(deck.begin(), item, id);
        if (copies > (faith ? 1 : 3) || (faith && ++faiths > kCrestLimit)) {
            throw std::invalid_argument("Deck exceeds the copy limit or leader area limit");
        }
    }
}

} // namespace

GameState new_game(const Deck& first, const Deck& second, std::uint64_t seed) {
    check_deck(first);
    check_deck(second);
    GameState state{};
    state.rng_state = seed;
    state.next_creation_order = 1;
    state.turn_number = 1;
    for (int seat = 0; seat < kPlayers; ++seat) {
        auto& player = state.players[seat];
        const auto& deck = seat == 0 ? first : second;
        player.public_deck = deck;
        std::sort(player.public_deck.begin(), player.public_deck.end());
        for (const CardId id : deck) {
            if (definition(id).kind == Kind::Faith) {
                grant_faith(player, id);
            } else {
                player.deck[player.deck_end++] = id;
                if (const auto faith = definition(id).faith) grant_faith(player, faith);
            }
        }
        player.leader_hp = kLeaderMaxHp;
        player.leader_max_hp = kLeaderMaxHp;
        player.ep = player.sep = 2;
        player.extra_pp_available = seat == 1;
        shuffle(state, player, 0);
        draw(state, seat, kOpeningHand);
    }
    return state;
}

bool can_choose_enemy_permanent(const Permanent& object) {
    return object.creation_order != 0 && object.clue_count == 0 && object.earth_count == 0 && !has(object, kAura | kAmbush);
}

bool follower_fanfare_suppressed(const PlayerState& player) {
    return std::any_of(player.crests.begin(), player.crests.begin() + player.crest_count,
        [](const auto& crest) { return crest.id == CrestId::Miltio; });
}

void resolve_effect_steps(GameState& state, int owner, std::span<const Effect> effects,
                          EntityId source, EntityId target, const RuleConfig& rules) {
    if (owner < 0 || owner >= kPlayers || effects.size() > kEffectSlots ||
        std::any_of(effects.begin(), effects.end(), [](const Effect& effect) {
            return effect.trigger != Trigger::None;
        })) {
        throw std::invalid_argument("Invalid immediate effect sequence");
    }
    Resolver(state, rules).immediate(owner, effects, source, target);
}

ActionList legal_actions(const GameState& state, const RuleConfig& rules, PlayCopies copies) {
    ActionList actions;
    if (state.outcome != Outcome::Ongoing) {
        return actions;
    }
    const auto add = [&](ActionType type, int src = kNoTarget, int dst = kNoTarget, int choice = 0, int selection = 0,
                         bool selection_in_hand = false, bool selection_on_enemy = false, int secondary = kNoTarget) {
        actions.push({type, static_cast<std::int8_t>(src), static_cast<std::int8_t>(dst),
                      static_cast<std::uint8_t>(choice), static_cast<std::uint16_t>(selection), selection_in_hand,
                      selection_on_enemy, static_cast<std::int8_t>(secondary)});
    };
    if (state.phase == Phase::Mulligan) {
        for (int mask = 0; mask < (1 << kOpeningHand); ++mask) {
            add(ActionType::Mulligan, kNoTarget, kNoTarget, mask);
        }
        return actions;
    }
    const int seat = state.turn_player;
    const auto& player = state.players[seat];
    const auto& enemy = state.players[1 - seat];
    const bool suppress = follower_fanfare_suppressed(player);
    const auto add_targets = [&](ActionType type, int src, int mode, ChoiceTarget choice, bool mandatory) {
        const auto target = choice.target;
        bool found = false;
        if (target == Target::ChosenOtherThreePermanents) {
            int available = (1 << player.board_count) - 1;
            if (type != ActionType::Play) available &= ~(1 << src);
            for (int slot = 0; slot < enemy.board_count; ++slot) {
                if (can_choose_enemy_permanent(enemy.board[slot])) available |= 1 << (kBoardLimit + slot);
            }
            const int count = std::min(3, std::popcount(static_cast<unsigned>(available)));
            if (mandatory && count < 3) return;
            for (int subset = available; subset != 0; subset = (subset - 1) & available) {
                if (std::popcount(static_cast<unsigned>(subset)) == count) {
                    add(type, src, kNoTarget, mode, subset | kBothBoardsSelection);
                }
            }
            found = count > 0;
        } else if (target == Target::ChosenOwnHandThree || target == Target::ChosenOwnHandTwo ||
            target == Target::ChosenOtherOwnTwoFollowers || target == Target::ChosenEnemyTwoFollowers) {
            int available = 0;
            int maximum = 2;
            if (target == Target::ChosenOwnHandThree || target == Target::ChosenOwnHandTwo) {
                maximum = target == Target::ChosenOwnHandThree ? 3 : 2;
                available = (1 << player.hand_count) - 1;
                if (type == ActionType::Play) available &= ~(1 << src);
            } else if (target == Target::ChosenEnemyTwoFollowers) {
                for (int i = 0; i < enemy.board_count; ++i) {
                    if (follower(enemy.board[i]) && can_choose_enemy_permanent(enemy.board[i])) available |= 1 << i;
                }
            } else {
                for (int i = 0; i < player.board_count; ++i) {
                    if ((type == ActionType::Play || i != src) && follower(player.board[i])) available |= 1 << i;
                }
            }
            const int count = std::min(maximum, std::popcount(static_cast<unsigned>(available)));
            if (mandatory && count != maximum) return;
            for (int subset = available; subset; subset = (subset - 1) & available) {
                if (std::popcount(static_cast<unsigned>(subset)) == count) {
                    add(type, src, kNoTarget, mode, subset, target == Target::ChosenOwnHandTwo,
                        target == Target::ChosenEnemyTwoFollowers);
                }
            }
            found = count > 0;
        } else if (target == Target::ChosenOwnHand || target == Target::ChosenOwnSpell ||
                   target == Target::ChosenOwnHandFollower || target == Target::ChosenBoostedHand ||
                   target == Target::ChosenAffordableArtifactHand) {
            for (int dst = 0; dst < player.hand_count; ++dst) {
                if (type == ActionType::Play && src == dst) continue;
                const auto& candidate = definition(player.hand[dst]);
                if (target == Target::ChosenOwnSpell && candidate.kind != Kind::Spell) continue;
                if (target == Target::ChosenOwnHandFollower && candidate.kind != Kind::Follower) continue;
                if (target == Target::ChosenBoostedHand && !candidate.spellboost) continue;
                if (target == Target::ChosenAffordableArtifactHand &&
                    (!deck_filter(candidate, DeckFilter::ArtifactFollower) || card_cost(candidate, player.hand_modifiers[dst]) > 5)) continue;
                add(type, src, kOwnHandTarget + dst, mode);
                found = true;
            }
        } else if (target == Target::ChosenHandAndEnemy) {
            for (int dst = 0; dst < enemy.board_count; ++dst) {
                if (!follower(enemy.board[dst]) || !can_choose_enemy_permanent(enemy.board[dst])) continue;
                for (int hand = 0; hand < player.hand_count; ++hand) {
                    if (hand == src) continue;
                    add(type, src, dst, mode, 0, false, false, hand);
                    found = true;
                }
            }
        } else if (target == Target::ChosenOwnFollower || target == Target::ChosenOtherOwnFollower ||
                   target == Target::ChosenOwnPermanent || target == Target::ChosenOtherPermanent || target == Target::ChosenOtherFollower ||
                   target == Target::ChosenUnevolvedOwnFollower || target == Target::ChosenOwnNamedFollower ||
                   target == Target::ChosenOwnGolem || target == Target::ChosenOtherUnevolvedOwnFollower ||
                   target == Target::ChosenOwnLargeFollower) {
            for (int dst = 0; dst < player.board_count; ++dst) {
                const auto& object = player.board[dst];
                const auto& candidate = definition(object.card_id);
                if (type != ActionType::Play && dst == src &&
                    (target == Target::ChosenOtherPermanent || target == Target::ChosenOtherFollower || target == Target::ChosenOtherOwnFollower ||
                     target == Target::ChosenOtherUnevolvedOwnFollower)) continue;
                if ((target == Target::ChosenUnevolvedOwnFollower || target == Target::ChosenOtherUnevolvedOwnFollower) &&
                    object.evolution != Evolution::None) continue;
                if (target == Target::ChosenOwnNamedFollower && object.card_id != choice.card) continue;
                if (target == Target::ChosenOwnGolem && !has_trait(candidate, kGolemTrait)) continue;
                if (target == Target::ChosenOwnLargeFollower && !is_large_follower(candidate)) continue;
                if (target == Target::ChosenOwnPermanent || target == Target::ChosenOtherPermanent || follower(object)) {
                    add(type, src, kOwnBoardTarget + dst, mode);
                    found = true;
                }
            }
        }
        if (target == Target::ChosenEnemyFollower || target == Target::ChosenEnemyPermanent ||
            target == Target::ChosenOtherPermanent || target == Target::ChosenOtherFollower || target == Target::ChosenSmallEnemyFollower ||
            target == Target::ChosenEnemyOrLeader) {
            for (int dst = 0; dst < enemy.board_count; ++dst) {
                const auto& object = enemy.board[dst];
                if (can_choose_enemy_permanent(object) &&
                    (target == Target::ChosenEnemyPermanent || target == Target::ChosenOtherPermanent || follower(object)) &&
                    (target != Target::ChosenSmallEnemyFollower || object.hp <= 3)) {
                    add(type, src, dst, mode);
                    found = true;
                }
            }
        }
        if (target == Target::ChosenEnemyOrLeader) {
            add(type, src, kEnemyLeader, mode);
            found = true;
        }
        if (target == Target::None || (!found && !mandatory)) {
            add(type, src, kNoTarget, mode);
        }
    };

    add(ActionType::EndTurn);
    if (seat == 1 && player.extra_pp_available) {
        add(ActionType::ExtraPp);
    }
    for (int src = 0; src < player.hand_count; ++src) {
        const auto& card = definition(player.hand[src]);
        const auto& modifiers = player.hand_modifiers[src];
        if (card.fusion != FusionMaterial::None && modifiers.fused_ply != ply(state) &&
            (!rules.fusion_requires_trigger_pp || player.available_pp() >= card.fusion_trigger_pp)) {
            int mask = 0;
            for (int material = 0; material < player.hand_count; ++material) {
                if (material != src && fusion_matches(card, definition(player.hand[material]))) {
                    mask |= 1 << (material - (material > src));
                }
            }
            for (int subset = mask; subset != 0; subset = (subset - 1) & mask) add(ActionType::Fuse, src, kNoTarget, subset);
        }
        if (!card.may_play_from_hand) continue;
        const auto cost = play_cost(card, player.available_pp(), modifiers, suppress);
        if (cost.cost > player.available_pp() || !can_enter(player, cost.form, rules) ||
            used_actions(player, card.id).play >= kNameActionLimit) {
            continue;
        }
        if (copies == PlayCopies::Canonical) {
            bool duplicate = false;
            for (int earlier = 0; earlier < src; ++earlier) {
                if (player.hand[earlier] == card.id && player.hand_modifiers[earlier] == modifiers) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
        }
        if (suppress && cost.form.kind == Kind::Follower) {
            add(ActionType::Play, src);
            continue;
        }
        if (cost.form.modes_chosen > 1) {
            for (int subset = 1; subset < (1 << cost.form.modes); ++subset) {
                if (std::popcount(static_cast<unsigned>(subset)) != cost.form.modes_chosen) continue;
                const int mode = kModeMask | subset;
                add_targets(ActionType::Play, src, mode, chosen_target(cost.form, player, OnPlay, mode, rules), cost.form.kind == Kind::Spell);
            }
            continue;
        }
        const bool all_modes = (cost.form.all_modes_with_high_cost && board_cost(player, 6, true)) ||
            (cost.form.full_combo_modes && player.plays_this_turn + 1 >= cost.form.full_combo_modes) ||
            (cost.form.full_board_modes && player.board_count >= cost.form.full_board_modes) ||
            (cost.form.full_artifact_modes && player.artifact_types_entered >= cost.form.full_artifact_modes) ||
            (cost.form.enhanced_all_modes && cost.enhanced);
        const int first_mode = all_modes ? kAllModes : cost.form.modes > 0 ? 1 : 0;
        const int last_mode = all_modes ? kAllModes : cost.form.modes;
        for (int mode = first_mode; mode <= last_mode; ++mode) {
            const auto target = chosen_target(cost.form, player, OnPlay, play_mode(cost, mode), rules,
                                              false, modifiers.fused_materials != 0);
            add_targets(ActionType::Play, src, mode, target, cost.form.kind == Kind::Spell);
        }
    }
    const bool ward = std::any_of(enemy.board.begin(), enemy.board.begin() + enemy.board_count,
        [](const auto& object) { return can_attack_target(object) && has(object, FLAG_WARD); });
    for (int src = 0; src < player.board_count; ++src) {
        const auto& object = player.board[src];
        const auto& card = definition(object.card_id);
        const auto named = used_actions(player, object.card_id);
        if (card.kind == Kind::Amulet) {
            if (!object.silenced && !object.acted_this_turn && named.act < kNameActionLimit && card.act_cost >= 0 &&
                card.act_cost <= player.available_pp() && (rules.act_on_entry_turn || object.entry_ply != ply(state))) {
                add_targets(ActionType::Act, src, 0, chosen_target(card, player, OnAct, 0, rules), false);
            }
            continue;
        }
        if (!player.manual_evolved && object.evolution == Evolution::None) {
            const int modes = !object.silenced && card.evolve_modes ? card.modes : 0;
            for (int mode = modes ? 1 : 0; mode <= modes; ++mode) {
                const bool fused = object.modifiers.fused_materials != 0;
                const auto target = object.silenced ? ChoiceTarget{} : chosen_target(card, player, OnEvolve, mode, rules, false, fused);
                if (player.ep > 0 && state.turn_number >= 5 - seat) {
                    add_targets(ActionType::Evolve, src, mode, target, false);
                }
                if (player.sep > 0 && state.turn_number >= 7 - seat) {
                    auto super_target = object.silenced ? ChoiceTarget{} : chosen_target(card, player, OnEvolve, mode, rules, true, fused);
                    if (!object.silenced && super_target.target == Target::None) {
                        super_target = chosen_target(card, player, OnSuperEvolve, mode, rules, true, fused);
                    }
                    add_targets(ActionType::SuperEvolve, src, mode, super_target, false);
                }
            }
        }
        if (!attack_ready(object) || named.attack >= kNameActionLimit) {
            continue;
        }
        const bool guarded = ward && !has(object, kIgnoreWard);
        for (int dst = 0; dst < enemy.board_count; ++dst) {
            if (can_attack_target(enemy.board[dst]) && (!guarded || has(enemy.board[dst], FLAG_WARD))) {
                add(ActionType::Attack, src, dst);
            }
        }
        if (!guarded && (object.readied || has(object, FLAG_STORM))) {
            add(ActionType::Attack, src, kEnemyLeader);
        }
    }
    return actions;
}

bool apply_action(GameState& state, Action action, const RuleConfig& rules) {
    const auto legal = legal_actions(state, rules);
    if (std::find(legal.begin(), legal.end(), action) == legal.end()) {
        return false;
    }
    apply_legal_action(state, action, rules);
    return true;
}

static void resolve_action(GameState& state, Action action, const RuleConfig& rules, bool* certain,
                           LethalEvidence* evidence = nullptr, std::uint16_t* unobserved_hand_mask = nullptr,
                           int observer = -1) {
    Resolver resolver(state, rules, certain, evidence, unobserved_hand_mask, observer);
    switch (action.type) {
    case ActionType::EndTurn:
        resolver.end_turn();
        break;
    case ActionType::Play:
        resolver.play(action);
        break;
    case ActionType::Evolve:
    case ActionType::SuperEvolve:
        resolver.manual_evolve(action);
        break;
    case ActionType::Attack:
        resolver.attack(action);
        break;
    case ActionType::Act:
        resolver.act(action);
        break;
    case ActionType::ExtraPp: {
        auto& player = state.players[state.turn_player];
        player.extra_pp_available = false;
        ++player.extra_pp_uses;
        player.bonus_pp = 1;
        break;
    }
    case ActionType::Mulligan:
        if (certain) *certain = false;
        if (evidence) {
            evidence->crossed_unresolved_chance = true;
            evidence->depends_on_hidden_information = true;
        }
        replace_opening_cards(state, action.choice, rules);
        if (state.turn_player == 0) {
            state.turn_player = 1;
        } else {
            state.turn_player = 0;
            state.phase = Phase::Main;
            resolver.start_turn();
        }
        break;
    case ActionType::Fuse:
        resolver.fuse(action);
        break;
    }
}

void apply_legal_action(GameState& state, Action action, const RuleConfig& rules) {
    resolve_action(state, action, rules, nullptr);
}

bool apply_observed_action(GameState& state, Action action, const RuleConfig& rules) {
    bool certain = true;
    resolve_action(state, action, rules, &certain);
    return certain;
}

LethalStatus classify_lethal(const LethalEvidence& evidence) noexcept {
    if (!evidence.terminal_win_verified) return LethalStatus::NotFound;
    if (evidence.is_proven()) return LethalStatus::Proven;
    if (evidence.crossed_unresolved_chance) return LethalStatus::Probabilistic;
    return LethalStatus::Possible;
}

LethalEvidence apply_action_with_evidence(GameState& state, Action action, int observer,
                                          std::uint16_t* unobserved_hand_mask, const RuleConfig& rules) {
    bool certain = true;
    LethalEvidence ev{};
    resolve_action(state, action, rules, &certain, &ev, unobserved_hand_mask, observer);
    ev.terminal_win_verified =
        state.outcome != Outcome::Ongoing && state.end_reason == EndReason::Health &&
        state.players[observer].leader_hp > 0 && state.players[1 - observer].leader_hp <= 0 &&
        ((observer == 0 && state.outcome == Outcome::FirstWins) ||
         (observer == 1 && state.outcome == Outcome::SecondWins));
    return ev;
}

} // namespace svwb::wb
