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
    const auto alternate = card.crystal_form ? card.crystal_form : card.accelerate_form;
    if (pp < price && alternate != 0) {
        const auto& form = definition(alternate);
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

Target chosen_target(const CardDef& card, const PlayerState& player, Trigger trigger, int mode, bool super = false) {
    for (const auto& effect : card.effects) {
        if (effect.trigger != trigger || !takes_mode(effect, mode) ||
            (super && effect.condition == Condition::NormalEvolution) ||
            (effect.condition == Condition::OwnAmulets && amulets(player) < effect.threshold)) continue;
        switch (effect.target) {
        case Target::ChosenEnemyFollower: case Target::ChosenEnemyPermanent: case Target::ChosenOwnFollower:
        case Target::ChosenOwnPermanent: case Target::ChosenOwnHand: case Target::ChosenSmallEnemyFollower:
        case Target::ChosenOtherOwnFollower: case Target::ChosenOtherPermanent:
        case Target::ChosenOwnHandThree: case Target::ChosenOtherOwnTwoFollowers:
            return effect.target;
        default: break;
        }
    }
    return Target::None;
}

bool can_attack_target(const Permanent& object) {
    return follower(object) && !has(object, kAmbush | kIntimidate);
}

enum class Origin : std::uint8_t { Board, Spell, Removed, Crest, ExpiredCrest, SuperDamage, Banished, DelayedBanish };

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

bool condition_met(GameState& state, const Effect& effect, const Ability& ability, bool paid) {
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
    void board_ability(Ref ref, Trigger trigger, EntityId chosen = 0, int mode = 0, std::uint16_t selection = 0);
    void broadcast(int seat, const Ability& event, bool board = true, bool crests = true);
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
    void banish(Ref target);
    void copy_board(int owner, Permanent copy);
    void collect_dead(Ref cause);
    void evolve(Ref ref, Evolution evolution, bool manual, EntityId chosen = 0, std::uint16_t selection = 0);
    void effect(const Ability& ability, const Effect& effect, EntityId random_target);
    void custom_effect(const Ability& ability, const Effect& step, Ref target, int hand_slot);
    void draw_cards(int seat, int amount, DeckFilter filter = DeckFilter::Any, int discount = 0);
    CardId past_card(int seat, int limit, bool highest, bool amulets, std::span<const CardId> excluded = {});
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

void Resolver::board_ability(Ref ref, Trigger trigger, EntityId chosen, int mode, std::uint16_t selection) {
    if (const auto* object = find(state_, ref); object && !object->silenced) {
        enqueue({trigger, Origin::Board, ref, object->card_id, CrestId::None, chosen, mode,
                 object->modifiers.spellboost, object->modifiers.fused_materials, selection});
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
        if (ability.trigger == OnPlay || ability.trigger == OnEvolve ||
            ability.trigger == OnSuperEvolve || ability.trigger == OnAct || ability.trigger == OnFuse) {
            return Priority::Manual;
        }
        if (ability.origin == Origin::Removed || ability.origin == Origin::Banished) {
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
    const int before = hp;
    hp = static_cast<std::int16_t>(std::min(kLeaderMaxHp, hp + amount));
    if (hp > before) broadcast(seat, {OnLeaderHeal});
}

void Resolver::destroy(Ref target, Ref cause, bool by_ability, std::vector<Ability>* deferred) {
    auto* object = find(state_, target);
    if (!object || (by_ability && protected_super(target, *object))) {
        return;
    }
    auto& player = state_.players[target.seat];
    const auto removed = *object;
    erase(player.board, player.board_count, static_cast<int>(object - player.board.data()));
    ++player.shadows;
    if (follower(removed)) {
        const auto card = printed_card(removed.card_id);
        ++(legacy_counter(card) ? player.destroyed_followers[card - kFirstCard] : ensure_counter(player, card).destroyed);
    } else {
        const auto card = removed.card_id;
        ++(legacy_counter(card) ? player.destroyed_amulets[card - kFirstCard] : ensure_counter(player, card).destroyed);
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

void Resolver::banish(Ref target) {
    const auto* object = find(state_, target);
    if (!object) return;
    auto& player = state_.players[target.seat];
    const auto removed = *object;
    erase(player.board, player.board_count, static_cast<int>(object - player.board.data()));
    if (!removed.silenced) enqueue({OnBanish, Origin::Banished, target, removed.card_id});
    if (follower(removed)) {
        Ability event{OnEnemyFollowerBanished};
        event.event_object = removed;
        broadcast(1 - target.seat, event, true, false);
    }
}

void Resolver::copy_board(int owner, Permanent copy) {
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
        if (ability.origin == Origin::DelayedBanish) {
            const auto* object = find(state_, ability.source);
            if (object && object->banish_at_end && !object->silenced) {
                banish(ability.source);
                settle(ability.source);
            }
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
        if (step.trigger == ability.trigger && takes_mode(step, ability.mode) && condition_met(state_, step, ability, paid)) {
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
    if (def.kind == Kind::Follower) ++player.followers_entered;
    board_ability(ref, OnEnter);
    return ref;
}

void Resolver::evolve(Ref ref, Evolution evolution, bool manual, EntityId chosen, std::uint16_t selection) {
    auto* object = find(state_, ref);
    if (!object || !follower(*object) || object->evolution != Evolution::None || object->hp <= 0) {
        return;
    }
    set_evolved_form(*object, evolution);
    auto& player = state_.players[ref.seat];
    if (evolution == Evolution::Super) {
        for (int i = 0; i < player.hand_count; ++i) {
            const int discount = definition(player.hand[i]).super_evolve_hand_discount;
            auto& cost = player.hand_modifiers[i].cost;
            cost = static_cast<std::int8_t>(std::max(-127, cost - discount));
        }
    }
    if (manual) {
        --(evolution == Evolution::Super ? player.sep : player.ep);
        player.manual_evolved = true;
        // A crest granted by the evolve ability can be extended by the super-evolve ability.
        board_ability(ref, OnEvolve, chosen, 0, selection);
        if (evolution == Evolution::Super) {
            board_ability(ref, OnSuperEvolve, chosen, 0, selection);
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

CardId Resolver::past_card(int seat, int limit, bool highest, bool only_amulets, std::span<const CardId> excluded) {
    const auto& player = state_.players[seat];
    std::array<std::pair<CardId, int>, kCardSlots + kAdditionalCardCounters> candidates{};
    int best = -1;
    int count = 0;
    int total = 0;
    const auto consider = [&](CardId id, int destroyed) {
        if (destroyed == 0 || std::find(excluded.begin(), excluded.end(), id) != excluded.end()) return;
        const auto& card = definition(id);
        if (card.cost > limit || (highest && card.cost < best)) return;
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
    int pick = random_index(state_, total);
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
        if (const auto card = past_card(seat, limit, true, false)) enter(seat, card);
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

bool deck_filter(const CardDef& card, DeckFilter filter) {
    switch (filter) {
    case DeckFilter::Any: return true;
    case DeckFilter::Follower: return card.kind == Kind::Follower;
    case DeckFilter::Amulet: return card.kind == Kind::Amulet;
    case DeckFilter::Neutral: return card.class_name == kNeutralClass;
    }
    return false;
}

void Resolver::draw_cards(int seat, int amount, DeckFilter filter, int discount) {
    auto& player = state_.players[seat];
    for (int i = 0; i < amount && state_.outcome == Outcome::Ongoing; ++i) {
        int slot = player.deck_idx;
        if (filter != DeckFilter::Any) {
            std::array<int, kDeckCapacity> candidates{};
            int count = 0;
            for (int index = player.deck_idx; index < player.deck_end; ++index) {
                if (deck_filter(definition(player.deck[index]), filter)) candidates[count++] = index;
            }
            if (count == 0) return;
            slot = candidates[random_index(state_, count)];
        } else if (player.deck_remaining() == 0) {
            lose(state_, seat, EndReason::EmptyDeck);
            return;
        }
        auto [card, modifiers] = take_deck_card(player, slot);
        const int drawn_cost = card_cost(definition(card), modifiers);
        modifiers.cost = static_cast<std::int8_t>(std::max(-127, modifiers.cost - discount));
        const bool kept = player.hand_count < kHandLimit;
        add_card(player, card, modifiers);
        if (kept) {
            Ability event{OnDraw};
            event.event_cost = drawn_cost;
            broadcast(seat, event);
        }
    }
}

void Resolver::effect(const Ability& ability, const Effect& step, EntityId random_target) {
    const auto source = ability.source;
    const int owner = source.seat;
    const int enemy = 1 - owner;
    auto& player = state_.players[owner];
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
    case Target::AllEnemyFollowers: case Target::AllEnemyFollowersAndLeader:
        gather(enemy, follower); break;
    case Target::AllOwnFollowers: gather(owner, follower); break;
    case Target::AllOtherOwnFollowers: gather(owner, other_follower); break;
    case Target::AllFollowers: gather(owner, follower); gather(enemy, follower); break;
    case Target::AllOtherFollowers: gather(owner, other_follower); gather(enemy, follower); break;
    case Target::AllOwnAmulets: gather(owner, [](const auto& object) { return !follower(object); }); break;
    case Target::AllSmallEnemyFollowers:
        gather(enemy, [](const auto& object) { return follower(object) && object.hp <= 3; }); break;
    case Target::AllNonAssailantFollowers: {
        const auto non_assailant = [](const auto& object) {
            return follower(object) && definition(object.card_id).traits.find(kAssailantTrait) == std::string_view::npos;
        };
        gather(owner, non_assailant); gather(enemy, non_assailant); break;
    }
    case Target::ChosenOtherOwnTwoFollowers:
        for (int i = 0; i < player.board_count; ++i) {
            if (ability.selection & (1 << i)) targets[target_count++] = {owner, player.board[i].creation_order};
        }
        break;
    case Target::RandomOtherEnemyFollower:
        random_group = true;
        gather(enemy, [&](const auto& object) { return follower(object) && object.creation_order != ability.chosen; });
        break;
    case Target::RandomEnemyTwo:
        random_group = true; picks = 2; gather(enemy, follower); break;
    case Target::RandomOtherUnevolvedWard:
        random_group = true;
        gather(owner, [&](const auto& object) {
            return other_follower(object) && object.evolution == Evolution::None && has(object, FLAG_WARD);
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
        const int count = random_group ? std::min(picks, target_count) : target_count;
        for (int i = 0; i < count; ++i) {
            if (random_group) std::swap(targets[i], targets[i + random_index(state_, target_count - i)]);
            auto chosen = ability;
            chosen.chosen = targets[i].order;
            effect(chosen, single, 0);
        }
        if (step.target == Target::AllEnemyFollowersAndLeader && step.operation == Operation::Damage) {
            damage_leader(enemy, step.amount + step.spellboost_multiplier * ability.spellboost);
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
    case Target::ChosenOtherOwnFollower:
    case Target::AttackingFollower:
        target = {owner, ability.chosen};
        break;
    case Target::ChosenOtherPermanent:
        target = {find(state_, {owner, ability.chosen}) ? owner : enemy, ability.chosen};
        break;
    case Target::RandomEnemyFollower:
        target = {enemy, rules_.random_targets == RandomTargets::PerStep ? random_enemy(owner) : random_target};
        break;
    default: break;
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
        } else {
            damage(target, amount);
        }
        break;
    }
    case Heal:
        heal_leader(step.target == Target::OwnLeader ? owner : enemy, step.amount);
        break;
    case Draw:
        draw_cards(owner, step.amount);
        break;
    case DrawFiltered:
        draw_cards(owner, step.amount, step.filter);
        break;
    case AddHand:
        for (int i = 0; i < step.amount; ++i) {
            add_card(player, step.card);
        }
        break;
    case Summon: case SummonEnemy:
        for (int i = 0; i < step.amount; ++i) {
            enter(step.operation == Summon ? owner : enemy, step.card);
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
            object->temporary_abilities = 0;
            object->banish_at_end = false;
        }
        break;
    case Banish:
        banish(target);
        break;
    case Bounce:
        if (const auto* object = find(state_, target)) {
            auto& recipient = state_.players[target.seat];
            const auto card = printed_card(object->card_id);
            erase(recipient.board, recipient.board_count, static_cast<int>(object - recipient.board.data()));
            add_card(recipient, card);
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
        } else if (const auto* object = find(state_, target)) {
            copy_board(owner, *object);
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
    case RestoreEp:
        player.ep = static_cast<std::uint8_t>(std::min(255, player.ep + step.amount));
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
                  random_index(state_, 2) == 0 ? step.card : kHolyFalcon);
        }
        break;
    case CardHandler::CopyBanish:
        copy_board(owner, ability.event_object);
        break;
    case CardHandler::RandomKeywords: {
        std::array<std::uint16_t, 6> options{FLAG_STORM, kBane, kIntimidate, kDrain, kAura, kBarrier};
        if (auto* object = find(state_, target)) {
            for (int i = 0; i < 3; ++i) {
                std::swap(options[i], options[i + random_index(state_, static_cast<int>(options.size()) - i)]);
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
        if (hand_cost_sum(player) > hand_cost_sum(opponent)) primitive(Operation::Destroy, Target::AllEnemyFollowers);
        break;
    case CardHandler::ReplaceHandFromEnemyDeck:
        if (hand_slot >= 0 && hand_slot < player.hand_count && opponent.deck_remaining() > 0) {
            const int slot = opponent.deck_idx + random_index(state_, opponent.deck_remaining());
            player.hand[hand_slot] = opponent.deck[slot];
            player.hand_modifiers[hand_slot] = opponent.deck_modifiers[slot];
        }
        break;
    case CardHandler::CopyEnemyHand:
        if (opponent.hand_count > 0) {
            const int slot = random_index(state_, opponent.hand_count);
            auto modifiers = opponent.hand_modifiers[slot];
            modifiers.cost = static_cast<std::int8_t>(std::max(-127, modifiers.cost - step.amount));
            add_card(player, opponent.hand[slot], modifiers);
        }
        break;
    case CardHandler::DiscardThenCopy:
        for (int i = player.hand_count - 1; i >= 0; --i) {
            if (ability.selection & (1 << i)) {
                erase_hand(player, i);
                ++player.shadows;
            }
        }
        for (int i = 0, count = std::min(3, static_cast<int>(player.hand_count)); i < count; ++i) {
            add_card(player, player.hand[i], player.hand_modifiers[i]);
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
            std::array<int, kDeckCapacity> slots{};
            int count = 0;
            for (int i = player.deck_idx; i < player.deck_end; ++i) {
                const auto& card = definition(player.deck[i]);
                if (card.kind == Kind::Follower && card_cost(card, player.deck_modifiers[i]) <= step.amount) {
                    slots[count++] = i;
                }
            }
            if (count > 0) {
                const auto [card, modifiers] = take_deck_card(player, slots[random_index(state_, count)]);
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
            const int slot = random_index(state_, player.hand_count);
            return_to_deck(state_, player, player.hand[slot], player.hand_modifiers[slot]);
            erase_hand(player, slot);
        }
        break;
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
    if (payment.form.cost >= 1 && payment.form.cost <= 8) player.played_cost_mask |= 1 << payment.form.cost;
    Ref created{seat};
    if (payment.form.kind == Kind::Spell) {
        ++player.shadows;
        for (int i = 0; i < player.hand_count; ++i) {
            auto& boost = player.hand_modifiers[i].spellboost;
            if (definition(player.hand[i]).spellboost && boost < 255) ++boost;
        }
        enqueue({OnPlay, Origin::Spell, {seat}, payment.form.id, CrestId::None, chosen, mode,
                 modifiers.spellboost, modifiers.fused_materials, action.selection});
    } else {
        created = enter(seat, payment.form.id, modifiers);
        board_ability(created, OnPlay, chosen, mode, action.selection);
    }
    Ability event{OnOtherPlay};
    event.event_order = created.order;
    event.event_cost = payment.form.cost;
    broadcast(seat, event, true, false);
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
    evolve(ref, action.type == ActionType::SuperEvolve ? Evolution::Super : Evolution::Normal, true, chosen, action.selection);
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
            const auto& object = player.board[i];
            const Ref ref{owner, object.creation_order};
            board_ability(ref, offset == 0 ? TurnEnd : EnemyTurnEnd);
            if (offset == 0 && object.banish_at_end && !object.silenced) {
                triggered_.push_back({TurnEnd, Origin::DelayedBanish, ref});
            }
        }
    }
    resolve();
    if (state_.outcome != Outcome::Ongoing) {
        return;
    }
    for (auto& player : state_.players) {
        for (int i = 0; i < player.board_count; ++i) {
            auto& object = player.board[i];
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
    const auto add = [&](ActionType type, int src = kNoTarget, int dst = kNoTarget, int choice = 0, int selection = 0) {
        actions.push({type, static_cast<std::int8_t>(src), static_cast<std::int8_t>(dst),
                      static_cast<std::uint8_t>(choice), static_cast<std::uint16_t>(selection)});
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
        if (target == Target::ChosenOwnHandThree || target == Target::ChosenOtherOwnTwoFollowers) {
            int available = 0;
            int maximum = 2;
            if (target == Target::ChosenOwnHandThree) {
                maximum = 3;
                available = (1 << player.hand_count) - 1;
                if (type == ActionType::Play) available &= ~(1 << src);
            } else {
                for (int i = 0; i < player.board_count; ++i) {
                    if ((type == ActionType::Play || i != src) && follower(player.board[i])) available |= 1 << i;
                }
            }
            const int count = std::min(maximum, std::popcount(static_cast<unsigned>(available)));
            for (int subset = available; subset; subset = (subset - 1) & available) {
                if (std::popcount(static_cast<unsigned>(subset)) == count) add(type, src, kNoTarget, mode, subset);
            }
            found = count > 0;
        } else if (target == Target::ChosenOwnHand) {
            for (int dst = 0; dst < player.hand_count; ++dst) {
                if (type == ActionType::Play && src == dst) continue;
                add(type, src, kOwnHandTarget + dst, mode);
                found = true;
            }
        } else if (target == Target::ChosenOwnFollower || target == Target::ChosenOtherOwnFollower ||
                   target == Target::ChosenOwnPermanent || target == Target::ChosenOtherPermanent) {
            for (int dst = 0; dst < player.board_count; ++dst) {
                if (type != ActionType::Play && dst == src &&
                    (target == Target::ChosenOtherPermanent || target == Target::ChosenOtherOwnFollower)) continue;
                if (target == Target::ChosenOwnPermanent || target == Target::ChosenOtherPermanent || follower(player.board[dst])) {
                    add(type, src, kOwnBoardTarget + dst, mode);
                    found = true;
                }
            }
        }
        if (target == Target::ChosenEnemyFollower || target == Target::ChosenEnemyPermanent ||
            target == Target::ChosenOtherPermanent || target == Target::ChosenSmallEnemyFollower) {
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
        const bool all_modes = cost.form.all_modes_with_high_cost && board_cost(player, 6, true);
        const int first_mode = all_modes ? kAllModes : cost.form.modes > 0 ? 1 : 0;
        const int last_mode = all_modes ? kAllModes : cost.form.modes;
        for (int mode = first_mode; mode <= last_mode; ++mode) {
            const auto target = chosen_target(cost.form, player, OnPlay, cost.enhanced ? kEnhancedMode : mode);
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
                add_targets(ActionType::Act, src, 0, chosen_target(card, player, OnAct, 0), false);
            }
            continue;
        }
        if (!player.manual_evolved && object.evolution == Evolution::None) {
            const auto target = object.silenced ? Target::None : chosen_target(card, player, OnEvolve, 0);
            if (player.ep > 0 && state.turn_number >= 5 - seat) {
                add_targets(ActionType::Evolve, src, 0, target, false);
            }
            if (player.sep > 0 && state.turn_number >= 7 - seat) {
                auto super_target = object.silenced ? Target::None : chosen_target(card, player, OnEvolve, 0, true);
                if (!object.silenced && super_target == Target::None) {
                    super_target = chosen_target(card, player, OnSuperEvolve, 0);
                }
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
