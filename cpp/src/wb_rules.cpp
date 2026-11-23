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

PlayCost play_cost(const CardDef& card, int pp, CardModifiers modifiers) {
    if (card.enhance_cost > 0 && pp >= card.enhance_cost) {
        return {card, card.enhance_cost, true};
    }
    const auto price = card_cost(card, modifiers);
    if (pp < price && card.crystal_form != 0) {
        const auto& form = definition(card.crystal_form);
        return {form, form.cost, false};
    }
    return {card, price, false};
}

EntityId chosen_entity(const GameState& state, Action action, bool consumes_source = false) {
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
    return effect.mode == 0 || effect.mode == mode;
}

Target chosen_target(const CardDef& card, Trigger trigger, int mode) {
    for (const auto& effect : card.effects) {
        if (effect.trigger == trigger && takes_mode(effect, mode) &&
            (effect.target == Target::ChosenEnemyFollower || effect.target == Target::ChosenEnemyPermanent ||
             effect.target == Target::ChosenOwnFollower || effect.target == Target::ChosenOwnPermanent ||
             effect.target == Target::ChosenOwnHand)) {
            return effect.target;
        }
    }
    return Target::None;
}

bool can_attack_target(const Permanent& object) {
    return follower(object) && !has(object, kAmbush | kIntimidate);
}

enum class Origin : std::uint8_t { Board, Spell, Removed, Crest, ExpiredCrest, SuperDamage };

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
        auto traits = material.traits;
        while (!traits.empty()) {
            const auto newline = traits.find('\n');
            if (traits.substr(0, newline) == host.fusion_trait) return true;
            if (newline == std::string_view::npos) break;
            traits.remove_prefix(newline + 1);
        }
        return false;
    }
    }
    return false;
}

bool condition_met(const PlayerState& player, const Effect& effect, const Ability& ability, bool paid) {
    switch (effect.condition) {
    case Condition::Always: return true;
    case Condition::Combo: return player.plays_this_turn >= effect.threshold;
    case Condition::Rally: return player.followers_entered >= effect.threshold;
    case Condition::Overflow: return player.overflow();
    case Condition::CostPaid: return paid;
    case Condition::AtMaxPp: return player.max_pp == kMaxPp;
    case Condition::Fused: return ability.fused_materials > 0;
    case Condition::NotFused: return ability.fused_materials == 0;
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
    Resolver(GameState& state, const RuleConfig& rules) : state_(state), rules_(rules) {}

    void play(Action action);
    void manual_evolve(Action action);
    void act(Action action);
    void attack(Action action);
    void fuse(Action action);
    void start_turn();
    void end_turn();
    void immediate(int owner, std::span<const Effect> effects, EntityId source, EntityId target);

private:
    void enqueue(Ability ability);
    void board_ability(Ref ref, Trigger trigger, EntityId chosen = 0, int mode = 0);
    void flush();
    void resolve();
    void run_steps(const Ability& ability, std::span<const Effect> effects);
    void check_health(int source);
    void settle(Ref cause);
    bool protected_super(Ref ref, const Permanent& object) const;
    void damage(Ref target, int amount);
    int damage_leader(int seat, int amount);
    void heal_leader(int seat, int amount);
    void destroy(Ref target, Ref cause, bool by_ability, std::vector<Ability>* deferred = nullptr);
    void collect_dead(Ref cause);
    void evolve(Ref ref, Evolution evolution, bool manual, EntityId chosen = 0);
    void effect(const Ability& ability, const Effect& effect, EntityId random_target);
    void custom_effect(CardHandler handler, Ref source, Ref target);
    EntityId random_enemy(int owner);
    Ref enter(int seat, CardId card, CardModifiers modifiers = {});
    void reanimate(int seat, int cost);
    void grant_crest(int seat, CrestId id);
    void tick_crests(int seat, CrestClock clock);
    void refresh(int seat);

    GameState& state_;
    const RuleConfig& rules_;
    std::vector<Ability> waiting_;
    std::vector<Ability> triggered_;
    Ref super_attacker_{};
    bool super_damage_queued_ = false;
};

void Resolver::enqueue(Ability ability) {
    const auto& effects = script(ability);
    if (std::any_of(effects.begin(), effects.end(), [&](const Effect& effect) {
            return effect.trigger == ability.trigger && takes_mode(effect, ability.mode);
        })) {
        triggered_.push_back(ability);
    }
}

void Resolver::board_ability(Ref ref, Trigger trigger, EntityId chosen, int mode) {
    if (const auto* object = find(state_, ref); object && !object->silenced) {
        enqueue({trigger, Origin::Board, ref, object->card_id, CrestId::None, chosen, mode,
                 object->modifiers.spellboost, object->modifiers.fused_materials});
    }
}

void Resolver::flush() {
    const auto priority = [&](const Ability& ability) {
        if (ability.trigger == OnPlay || ability.trigger == OnEvolve ||
            ability.trigger == OnSuperEvolve || ability.trigger == OnAct || ability.trigger == OnFuse) {
            return Priority::Manual;
        }
        if (ability.origin == Origin::Removed) {
            return Priority::LastWords;
        }
        if (ability.origin == Origin::Crest || ability.origin == Origin::ExpiredCrest) {
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

void Resolver::damage(Ref target, int amount) {
    auto* object = find(state_, target);
    if (!object || !follower(*object) || object->hp <= 0) {
        return;
    }
    if (protected_super(target, *object)) {
        amount = 0;
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
    auto& hp = state_.players[seat].leader_hp;
    hp = static_cast<std::int16_t>(std::min(kLeaderMaxHp, hp + amount));
}

void Resolver::destroy(Ref target, Ref cause, bool by_ability, std::vector<Ability>* deferred) {
    auto* object = find(state_, target);
    if (!object || (by_ability && (object->clue_count > 0 || object->earth_count > 0 || protected_super(target, *object)))) {
        return;
    }
    auto& player = state_.players[target.seat];
    const auto removed = *object;
    erase(player.board, player.board_count, static_cast<int>(object - player.board.data()));
    ++player.shadows;
    if (follower(removed)) {
        const auto card = printed_card(removed.card_id);
        ++(legacy_counter(card) ? player.destroyed_followers[card - kFirstCard] : ensure_counter(player, card).destroyed);
    }
    if (!removed.silenced) {
        const Ability last_words{LastWords, Origin::Removed, target, removed.card_id};
        if (deferred) {
            deferred->push_back(last_words);
        } else {
            enqueue(last_words);
        }
    }
    if (super_attacker_.order != 0 && cause == super_attacker_ && target.seat != super_attacker_.seat &&
        follower(removed) && (!super_damage_queued_ || rules_.super_damage_per_follower)) {
        triggered_.push_back({OnAttack, Origin::SuperDamage, super_attacker_});
        super_damage_queued_ = true;
    }
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
        if (ability.origin == Origin::SuperDamage) {
            damage_leader(1 - ability.source.seat, 1);
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
    for (std::size_t i = 0; i < effects.size() && state_.outcome == Outcome::Ongoing; ++i) {
        const auto& step = effects[i];
        if (step.trigger == ability.trigger && takes_mode(step, ability.mode) && condition_met(player, step, ability, paid)) {
            if (step.operation == Operation::Necromancy) {
                paid = step.amount >= 0 && player.shadows >= step.amount;
                if (!paid) continue;
            } else if (step.operation == Operation::EarthRite) {
                const int slot = earth_slot(player);
                paid = step.amount > 0 && slot >= 0 && player.board[slot].earth_count >= step.amount;
                if (!paid) continue;
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
            erase(player.board, player.board_count, old);
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
    if (def.kind == Kind::Follower) ++player.followers_entered;
    board_ability(ref, OnEnter);
    return ref;
}

void Resolver::evolve(Ref ref, Evolution evolution, bool manual, EntityId chosen) {
    auto* object = find(state_, ref);
    if (!object || !follower(*object) || object->evolution != Evolution::None || object->hp <= 0) {
        return;
    }
    set_evolved_form(*object, evolution);
    if (manual) {
        auto& player = state_.players[ref.seat];
        --(evolution == Evolution::Super ? player.sep : player.ep);
        player.manual_evolved = true;
        // A crest granted by the evolve ability can be extended by the super-evolve ability.
        board_ability(ref, OnEvolve, chosen);
        if (evolution == Evolution::Super) {
            board_ability(ref, OnSuperEvolve, chosen);
        }
    }
    board_ability(ref, OnEvolved);
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
    return count > 0 ? candidates[random_index(state_, count)] : 0;
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

void Resolver::reanimate(int seat, int limit) {
    auto& player = state_.players[seat];
    if (player.board_count >= kBoardLimit) return;
    std::array<std::pair<CardId, int>, kCardSlots + kAdditionalCardCounters> candidates{};
    int best = -1;
    int count = 0;
    int total = 0;
    const auto consider = [&](CardId id, int destroyed) {
        if (destroyed == 0) return;
        const auto& card = definition(id);
        if (card.cost > limit || card.cost < best) return;
        if (card.cost > best) {
            best = card.cost;
            count = total = 0;
        }
        candidates[count++] = {id, destroyed};
        total += destroyed;
    };
    for (int i = 0; i < kCardSlots; ++i) consider(static_cast<CardId>(kFirstCard + i), player.destroyed_followers[i]);
    for (int i = 0; i < player.other_counter_count; ++i) {
        consider(player.other_counters[i].id, player.other_counters[i].destroyed);
    }
    if (total == 0) return;
    int pick = random_index(state_, total);
    for (int i = 0; i < count; ++i) {
        const auto [id, weight] = candidates[i];
        if (pick < weight) {
            enter(seat, id);
            return;
        }
        pick -= weight;
    }
}

void Resolver::effect(const Ability& ability, const Effect& step, EntityId random_target) {
    const auto source = ability.source;
    const int owner = source.seat;
    const int enemy = 1 - owner;
    auto& player = state_.players[owner];
    auto& opponent = state_.players[enemy];
    Ref target{};
    switch (step.target) {
    case Target::Self:
        target = source;
        break;
    case Target::ChosenEnemyFollower:
    case Target::ChosenEnemyPermanent:
        target = {enemy, ability.chosen};
        break;
    case Target::ChosenOwnFollower:
    case Target::ChosenOwnPermanent:
        target = {owner, ability.chosen};
        break;
    case Target::RandomEnemyFollower:
        target = {enemy, rules_.random_targets == RandomTargets::PerStep ? random_enemy(owner) : random_target};
        break;
    case Target::None:
    case Target::OwnLeader:
    case Target::EnemyLeader:
    case Target::AllEnemyFollowers:
    case Target::BothLeaders:
    case Target::ChosenOwnHand:
        break;
    }
    const int hand_slot = step.target == Target::ChosenOwnHand && (ability.chosen & kHandTargetFlag)
        ? static_cast<int>(ability.chosen & ~kHandTargetFlag) : -1;
    using enum Operation;
    switch (step.operation) {
    case Damage: {
        const int amount = step.amount + step.spellboost_multiplier * ability.spellboost;
        if (step.target == Target::BothLeaders) {
            damage_leader(owner, amount);
            damage_leader(enemy, amount);
        } else if (step.target == Target::EnemyLeader || step.target == Target::OwnLeader) {
            damage_leader(step.target == Target::EnemyLeader ? enemy : owner, amount);
        } else if (step.target == Target::AllEnemyFollowers) {
            for (int i = 0; i < opponent.board_count; ++i) {
                damage({enemy, opponent.board[i].creation_order}, amount);
            }
        } else {
            damage(target, amount);
        }
        break;
    }
    case Heal:
        heal_leader(step.target == Target::OwnLeader ? owner : enemy, step.amount);
        break;
    case Draw:
        draw(state_, owner, step.amount);
        break;
    case AddHand:
        for (int i = 0; i < step.amount; ++i) {
            add_card(player, step.card);
        }
        break;
    case Summon:
        for (int i = 0; i < step.amount; ++i) {
            enter(owner, step.card);
        }
        break;
    case SummonModified:
        enter(owner, step.card, {step.granted_abilities, static_cast<std::int8_t>(step.amount),
                                 static_cast<std::int8_t>(step.health_modifier), 0, 0, 0, 0});
        break;
    case SummonEvolved:
        evolve(enter(owner, step.card), Evolution::Normal, false);
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
        }
        break;
    case Banish:
    case Bounce:
        if (const auto* object = find(state_, target)) {
            auto& recipient = state_.players[target.seat];
            const auto card = printed_card(object->card_id);
            erase(recipient.board, recipient.board_count, static_cast<int>(object - recipient.board.data()));
            if (step.operation == Bounce) {
                add_card(recipient, card);
            }
        }
        break;
    case ReturnToDeck:
        if (hand_slot >= 0 && hand_slot < player.hand_count) {
            return_to_deck(state_, player, player.hand[hand_slot], player.hand_modifiers[hand_slot]);
            erase_hand(player, hand_slot);
        } else if (const auto* object = find(state_, target)) {
            auto& recipient = state_.players[target.seat];
            return_to_deck(state_, recipient, printed_card(object->card_id), object->modifiers);
            erase(recipient.board, recipient.board_count, static_cast<int>(object - recipient.board.data()));
        }
        break;
    case Buff:
        if (hand_slot >= 0 && hand_slot < player.hand_count) {
            auto& modifiers = player.hand_modifiers[hand_slot];
            modifiers.attack = static_cast<std::int8_t>(std::clamp(modifiers.attack + step.amount, -127, 127));
            modifiers.defense = static_cast<std::int8_t>(std::clamp(modifiers.defense + step.health_modifier, -127, 127));
            modifiers.abilities |= step.granted_abilities;
        } else if (auto* object = find(state_, target); object && follower(*object)) {
            auto& modifiers = object->modifiers;
            modifiers.attack = static_cast<std::int8_t>(std::clamp(modifiers.attack + step.amount, -127, 127));
            modifiers.defense = static_cast<std::int8_t>(std::clamp(modifiers.defense + step.health_modifier, -127, 127));
            modifiers.abilities |= step.granted_abilities;
            object->atk = static_cast<std::int16_t>(std::max(0, object->atk + step.amount));
            object->hp = static_cast<std::int16_t>(object->hp + step.health_modifier);
            object->max_hp = static_cast<std::int16_t>(object->max_hp + step.health_modifier);
            object->abilities |= step.granted_abilities;
        }
        break;
    case Copy:
        if (hand_slot >= 0 && hand_slot < player.hand_count) {
            add_card(player, player.hand[hand_slot], player.hand_modifiers[hand_slot]);
        } else if (const auto* object = find(state_, target); object && can_enter(player, definition(object->card_id), rules_)) {
            auto copy = *object;
            const auto created = enter(owner, copy.card_id, copy.modifiers);
            if (auto* added = find(state_, created)) {
                copy.creation_order = created.order;
                copy.entry_ply = static_cast<std::uint8_t>(ply(state_));
                copy.readied = copy.attacked_this_turn = copy.attacked_twice = copy.acted_this_turn = false;
                copy.earth_count = added->earth_count;
                copy.clue_count = added->clue_count;
                *added = copy;
            }
        }
        break;
    case Destroy:
        destroy(target, source, true);
        break;
    case Transform:
        if (auto* object = find(state_, target)) {
            const auto& form = definition(step.card);
            const auto hp = static_cast<std::int16_t>(form.hp);
            *object = {target.order, step.card, static_cast<std::int16_t>(form.atk), hp, hp, form.abilities,
                       static_cast<std::uint16_t>(form.clue), static_cast<std::uint8_t>(form.countdown),
                       static_cast<std::uint8_t>(ply(state_)), Evolution::None, false, false, false, false,
                       false, static_cast<std::uint16_t>(form.earth), {}};
            if (form.kind == Kind::Follower && step.transformed_evolution != Evolution::None) {
                // An evolved destination form does not trigger evolution abilities.
                set_evolved_form(*object, step.transformed_evolution);
            }
            if (rules_.reset_name_counts_on_transform) {
                counts(state_.players[target.seat], form.id) = {};
            }
        }
        break;
    case RestorePp:
        player.pp = static_cast<std::uint8_t>(std::min(static_cast<int>(player.max_pp), player.pp + step.amount));
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
        break;
    }
    case Reanimate:
        reanimate(owner, step.amount);
        break;
    case GrantFaith:
        grant_faith(player, step.card);
        break;
    case Discard:
        if (hand_slot >= 0 && hand_slot < player.hand_count) {
            erase_hand(player, hand_slot);
            ++player.shadows;
        } else if (step.target == Target::None) {
            for (int count = 0; count < step.amount && player.hand_count > 0; ++count) {
                erase_hand(player, random_index(state_, player.hand_count));
                ++player.shadows;
            }
        }
        break;
    case GrantCrest:
        grant_crest(owner, step.crest);
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
        custom_effect(step.handler, source, target);
        break;
    }
}

void Resolver::custom_effect(CardHandler handler, Ref source, Ref target) {
    auto& player = state_.players[source.seat];
    switch (handler) {
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
        erase_hand(player, candidates[random_index(state_, count)]);
        evolve(target, Evolution::Normal, false);
        break;
    }
    }
}

void Resolver::play(Action action) {
    const int seat = state_.turn_player;
    auto& player = state_.players[seat];
    const CardId card = player.hand[action.src];
    const auto modifiers = player.hand_modifiers[action.src];
    const auto payment = play_cost(definition(card), player.available_pp(), modifiers);
    const EntityId chosen = chosen_entity(state_, action, true);
    const int mode = payment.enhanced ? kEnhancedMode : action.choice;
    spend(player, payment.cost);
    erase_hand(player, action.src);
    ++counts(player, card).play;
    ++player.plays_this_turn;
    if (payment.form.kind == Kind::Spell) {
        ++player.shadows;
        for (int i = 0; i < player.hand_count; ++i) {
            auto& boost = player.hand_modifiers[i].spellboost;
            if (definition(player.hand[i]).spellboost && boost < 255) ++boost;
        }
        enqueue({OnPlay, Origin::Spell, {seat}, card, CrestId::None, chosen, mode,
                 modifiers.spellboost, modifiers.fused_materials});
    } else {
        const auto created = enter(seat, payment.form.id, modifiers);
        board_ability(created, OnPlay, chosen, mode);
    }
    settle({seat});
    resolve();
}

void Resolver::fuse(Action action) {
    const int seat = state_.turn_player;
    auto& player = state_.players[seat];
    const auto card = player.hand[action.src];
    auto& source = player.hand_modifiers[action.src];
    source.fused_ply = static_cast<std::uint8_t>(ply(state_));
    source.fused_materials = static_cast<std::uint8_t>(std::min(255, source.fused_materials + std::popcount(action.choice)));
    for (int i = player.hand_count - 1; i >= 0; --i) {
        if (i != action.src && (action.choice & (1 << (i - (i > action.src))))) {
            erase_hand(player, i);
        }
    }
    enqueue({OnFuse, Origin::Spell, {seat}, card});
    resolve();
}

void Resolver::manual_evolve(Action action) {
    const int seat = state_.turn_player;
    const auto ref = Ref{seat, state_.players[seat].board[action.src].creation_order};
    const EntityId chosen = chosen_entity(state_, action);
    evolve(ref, action.type == ActionType::SuperEvolve ? Evolution::Super : Evolution::Normal, true, chosen);
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
    board_ability({seat, object.creation_order}, OnAct, chosen_entity(state_, action));
    resolve();
}

void Resolver::attack(Action action) {
    const int seat = state_.turn_player;
    const int enemy = 1 - seat;
    auto& player = state_.players[seat];
    auto& attacker = player.board[action.src];
    const Ref source{seat, attacker.creation_order};
    ++counts(player, attacker.card_id).attack;
    attacker.attacked_twice = attacker.attacked_this_turn;
    attacker.attacked_this_turn = true;
    attacker.abilities &= ~kAmbush;
    const bool face = action.dst == kEnemyLeader;
    const Ref target{enemy, face ? 0U : state_.players[enemy].board[action.dst].creation_order};
    if (!face && protected_super(source, attacker)) {
        super_attacker_ = source;
    }
    board_ability(source, OnAttack, target.order);
    if (!face) {
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
    if (has(offense, kBane) && !protected_super(target, defense)) {
        find(state_, target)->hp = 0;
    }
    if (retaliates && has(defense, kBane) && !protected_super(source, offense)) {
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
        resolve();
        if (state_.outcome != Outcome::Ongoing) {
            return;
        }
        const auto timing = rules_.crest_clock == CrestClock::Printed ? def->clock : rules_.crest_clock;
        auto* current = find(state_, seat, order);
        if (timing == clock && current && current->countdown > 0 && --current->countdown == 0) {
            erase(player.crests, player.crest_count, static_cast<int>(current - player.crests.data()));
            player.shadows += static_cast<std::uint16_t>(rules_.expired_crest_adds_shadow);
            enqueue({LastWords, Origin::ExpiredCrest, {seat, order}, 0, id});
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
    // The board pass is collected in creation order, including detached countdown last words.
    waiting_ = std::move(field);
    resolve();
    if (state_.outcome != Outcome::Ongoing) {
        return;
    }
    if (!rules_.refresh_before_start_effects) {
        refresh(seat);
    }
    draw(state_, seat, 1);
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
    tick_crests(seat, CrestClock::End);
    if (state_.outcome != Outcome::Ongoing) {
        return;
    }
    for (int offset = 0; offset < kPlayers; ++offset) {
        const int owner = (seat + offset) % kPlayers;
        const auto& player = state_.players[owner];
        for (int i = 0; i < player.board_count; ++i) {
            board_ability({owner, player.board[i].creation_order}, offset == 0 ? TurnEnd : EnemyTurnEnd);
        }
    }
    resolve();
    if (state_.outcome != Outcome::Ongoing) {
        return;
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
        for (const CardId id : seat == 0 ? first : second) {
            if (definition(id).kind == Kind::Faith) {
                grant_faith(player, id);
            } else {
                player.deck[player.deck_end++] = id;
            }
        }
        player.leader_hp = kLeaderMaxHp;
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

ActionList legal_actions(const GameState& state, const RuleConfig& rules) {
    ActionList actions;
    if (state.outcome != Outcome::Ongoing) {
        return actions;
    }
    const auto add = [&](ActionType type, int src = kNoTarget, int dst = kNoTarget, int choice = 0) {
        actions.push({type, static_cast<std::int8_t>(src), static_cast<std::int8_t>(dst), static_cast<std::uint8_t>(choice)});
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
    const auto add_targets = [&](ActionType type, int src, int mode, Target target, bool mandatory) {
        bool found = false;
        if (target == Target::ChosenOwnHand) {
            for (int dst = 0; dst < player.hand_count; ++dst) {
                if (type == ActionType::Play && src == dst) continue;
                add(type, src, kOwnHandTarget + dst, mode);
                found = true;
            }
        } else if (target == Target::ChosenOwnFollower || target == Target::ChosenOwnPermanent) {
            for (int dst = 0; dst < player.board_count; ++dst) {
                if (target == Target::ChosenOwnPermanent || follower(player.board[dst])) {
                    add(type, src, kOwnBoardTarget + dst, mode);
                    found = true;
                }
            }
        } else if (target != Target::None) {
            for (int dst = 0; dst < enemy.board_count; ++dst) {
                const auto& object = enemy.board[dst];
                if (can_choose_enemy_permanent(object) &&
                    (target == Target::ChosenEnemyPermanent || follower(object))) {
                    add(type, src, dst, mode);
                    found = true;
                }
            }
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
        if (card.fusion != FusionMaterial::None && modifiers.fused_ply != ply(state)) {
            int mask = 0;
            for (int material = 0; material < player.hand_count; ++material) {
                if (material != src && fusion_matches(card, definition(player.hand[material]))) {
                    mask |= 1 << (material - (material > src));
                }
            }
            for (int subset = mask; subset != 0; subset = (subset - 1) & mask) add(ActionType::Fuse, src, kNoTarget, subset);
        }
        const auto cost = play_cost(card, player.available_pp(), modifiers);
        if (cost.cost > player.available_pp() || !can_enter(player, cost.form, rules) ||
            used_actions(player, card.id).play >= kNameActionLimit) {
            continue;
        }
        const int first_mode = cost.form.modes > 0 ? 1 : 0;
        for (int mode = first_mode; mode <= cost.form.modes; ++mode) {
            const auto target = chosen_target(cost.form, OnPlay, cost.enhanced ? kEnhancedMode : mode);
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
                add_targets(ActionType::Act, src, 0, chosen_target(card, OnAct, 0), false);
            }
            continue;
        }
        if (!player.manual_evolved && object.evolution == Evolution::None) {
            const auto target = object.silenced ? Target::None : chosen_target(card, OnEvolve, 0);
            if (player.ep > 0 && state.turn_number >= 5 - seat) {
                add_targets(ActionType::Evolve, src, 0, target, false);
            }
            if (player.sep > 0 && state.turn_number >= 7 - seat) {
                const auto super_target = !object.silenced && target == Target::None
                    ? chosen_target(card, OnSuperEvolve, 0) : target;
                add_targets(ActionType::SuperEvolve, src, 0, super_target, false);
            }
        }
        if (!attack_ready(object) || named.attack >= kNameActionLimit) {
            continue;
        }
        for (int dst = 0; dst < enemy.board_count; ++dst) {
            if (can_attack_target(enemy.board[dst]) && (!ward || has(enemy.board[dst], FLAG_WARD))) {
                add(ActionType::Attack, src, dst);
            }
        }
        if (!ward && (object.readied || has(object, FLAG_STORM))) {
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

void apply_legal_action(GameState& state, Action action, const RuleConfig& rules) {
    Resolver resolver(state, rules);
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

} // namespace svwb::wb
