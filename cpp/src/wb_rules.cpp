#include "wb_cards.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace svwb::wb {
namespace {

using enum Trigger;

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

NamedActions& counts(PlayerState& player, CardId card) {
    return player.actions[printed_card(card) - kFirstCard];
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

bool can_enter(const PlayerState& player, const CardDef& card, const RuleConfig& rules) {
    return card.kind == Kind::Spell || player.board_count < kBoardLimit ||
        (card.clue && rules.merge_clue_on_full_board && clue_slot(player) >= 0);
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

void shuffle(GameState& state, Deck& deck, int first) {
    for (int i = static_cast<int>(deck.size()) - 1; i > first; --i) {
        std::swap(deck[i], deck[first + random_index(state, i - first + 1)]);
    }
}

void spend(PlayerState& player, int amount) {
    const int regular = std::min(amount, static_cast<int>(player.pp));
    player.pp = static_cast<std::uint8_t>(player.pp - regular);
    player.bonus_pp = static_cast<std::uint8_t>(player.bonus_pp - (amount - regular));
}

void add_card(PlayerState& player, CardId id) {
    if (player.hand_count == kHandLimit) {
        ++player.shadows;
    } else {
        player.hand[player.hand_count++] = id;
    }
}

void lose(GameState& state, int seat, EndReason reason) {
    state.outcome = seat == 0 ? Outcome::SecondWins : Outcome::FirstWins;
    state.end_reason = reason;
}

void draw(GameState& state, int seat, int amount) {
    auto& player = state.players[seat];
    for (int i = 0; i < amount; ++i) {
        if (player.deck_idx == kDeckSize) {
            lose(state, seat, EndReason::EmptyDeck);
            return;
        }
        add_card(player, player.deck[player.deck_idx++]);
    }
}

struct PlayCost {
    const CardDef& form;
    int cost;
    bool enhanced;
};

PlayCost play_cost(const CardDef& card, int pp) {
    if (card.enhance_cost > 0 && pp >= card.enhance_cost) {
        return {card, card.enhance_cost, true};
    }
    if (pp < card.cost && card.crystal_form != 0) {
        const auto& form = definition(card.crystal_form);
        return {form, form.cost, false};
    }
    return {card, card.cost, false};
}

bool takes_mode(const Effect& effect, int mode) {
    return effect.mode == 0 || effect.mode == mode;
}

Target chosen_target(const CardDef& card, Trigger trigger, int mode) {
    for (const auto& effect : card.effects) {
        if (effect.trigger == trigger && takes_mode(effect, mode) &&
            (effect.target == Target::ChosenEnemyFollower || effect.target == Target::ChosenEnemyPermanent)) {
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
};

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
    EntityId random_enemy(int owner);
    Ref enter(int seat, CardId card, int atk_delta = 0, int hp_delta = 0, std::uint16_t flags = 0);
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
        enqueue({trigger, Origin::Board, ref, object->card_id, CrestId::None, chosen, mode});
    }
}

void Resolver::flush() {
    const auto priority = [&](const Ability& ability) {
        if (ability.trigger == OnPlay || ability.trigger == OnEvolve ||
            ability.trigger == OnSuperEvolve || ability.trigger == OnAct) {
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
    if (!object || (by_ability && (object->clue_count > 0 || protected_super(target, *object)))) {
        return;
    }
    auto& player = state_.players[target.seat];
    const auto removed = *object;
    erase(player.board, player.board_count, static_cast<int>(object - player.board.data()));
    ++player.shadows;
    if (follower(removed)) {
        ++player.destroyed_followers[printed_card(removed.card_id) - kFirstCard];
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
    for (std::size_t i = 0; i < effects.size() && state_.outcome == Outcome::Ongoing; ++i) {
        const auto& step = effects[i];
        if (step.trigger == ability.trigger && takes_mode(step, ability.mode)) {
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

Ref Resolver::enter(int seat, CardId card, int atk_delta, int hp_delta, std::uint16_t flags) {
    const auto& def = definition(card);
    auto& player = state_.players[seat];
    if (!can_enter(player, def, rules_)) {
        return {};
    }
    assert(def.kind != Kind::Spell);
    int clues = 0;
    if (def.clue) {
        clues = 1;
        const int old = clue_slot(player);
        if (old >= 0) {
            clues += player.board[old].clue_count;
            erase(player.board, player.board_count, old);
        }
    }
    const Ref ref{seat, state_.next_creation_order++};
    const auto hp = static_cast<std::int16_t>(def.hp + hp_delta);
    player.board[player.board_count++] = {
        ref.order, card, static_cast<std::int16_t>(std::max(0, def.atk + atk_delta)), hp, hp,
        static_cast<std::uint16_t>(def.abilities | flags), static_cast<std::uint16_t>(clues),
        static_cast<std::uint8_t>(def.countdown), static_cast<std::uint8_t>(ply(state_)),
        Evolution::None, false, false, false, false,
    };
    board_ability(ref, OnEnter);
    return ref;
}

void Resolver::evolve(Ref ref, Evolution evolution, bool manual, EntityId chosen) {
    auto* object = find(state_, ref);
    if (!object || !follower(*object) || object->evolution != Evolution::None || object->hp <= 0) {
        return;
    }
    object->evolution = evolution;
    object->abilities |= FLAG_RUSH;
    const int bonus = evolution == Evolution::Super ? 3 : 2;
    object->atk = static_cast<std::int16_t>(object->atk + bonus);
    object->hp = static_cast<std::int16_t>(object->hp + bonus);
    object->max_hp = static_cast<std::int16_t>(object->max_hp + bonus);
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
    auto& player = state_.players[seat];
    const auto end = player.crests.begin() + player.crest_count;
    if (std::any_of(player.crests.begin(), end, [id](const Crest& crest) { return crest.id == id; })) {
        return;
    }
    if (player.crest_count == kCrestLimit) {
        if (!rules_.replace_oldest_crest_when_full) {
            return;
        }
        erase(player.crests, player.crest_count, 0);
    }
    player.crests[player.crest_count++] = {state_.next_creation_order++, id, find_crest(id)->countdown};
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
    case Target::RandomEnemyFollower:
        target = {enemy, rules_.random_targets == RandomTargets::PerStep ? random_enemy(owner) : random_target};
        break;
    case Target::None:
    case Target::OwnLeader:
    case Target::EnemyLeader:
    case Target::AllEnemyFollowers:
    case Target::BothLeaders:
        break;
    }
    using enum Operation;
    switch (step.operation) {
    case Damage:
        if (step.target == Target::BothLeaders) {
            damage_leader(owner, step.amount);
            damage_leader(enemy, step.amount);
        } else if (step.target == Target::EnemyLeader || step.target == Target::OwnLeader) {
            damage_leader(step.target == Target::EnemyLeader ? enemy : owner, step.amount);
        } else if (step.target == Target::AllEnemyFollowers) {
            for (int i = 0; i < opponent.board_count; ++i) {
                damage({enemy, opponent.board[i].creation_order}, step.amount);
            }
        } else {
            damage(target, step.amount);
        }
        break;
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
        enter(owner, step.card, step.amount, step.health_modifier, step.granted_abilities);
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
    case Destroy:
        destroy(target, source, true);
        break;
    case Transform:
        if (auto* object = find(state_, target)) {
            const auto& form = definition(step.card);
            const auto hp = static_cast<std::int16_t>(form.hp);
            *object = {target.order, step.card, static_cast<std::int16_t>(form.atk), hp, hp, form.abilities,
                       static_cast<std::uint16_t>(form.clue), static_cast<std::uint8_t>(form.countdown),
                       static_cast<std::uint8_t>(ply(state_)), Evolution::None, false, false, false, false};
            if (rules_.reset_name_counts_on_transform) {
                counts(state_.players[target.seat], form.id) = {};
            }
        }
        break;
    case RestorePp:
        player.pp = static_cast<std::uint8_t>(std::min(static_cast<int>(player.max_pp), player.pp + step.amount));
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
    case EvolveWithAlly:
        if (std::any_of(player.board.begin(), player.board.begin() + player.board_count,
                        [&](const Permanent& object) {
                            return object.creation_order != source.order && object.evolution != Evolution::None;
                        })) {
            evolve(target, Evolution::Normal, false);
        }
        break;
    case BanishLowestAndEvolve: {
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
            const int cost = definition(player.hand[i]).cost;
            if (cost < lowest) {
                lowest = cost;
                count = 0;
            }
            if (cost == lowest) {
                candidates[count++] = i;
            }
        }
        erase(player.hand, player.hand_count, candidates[random_index(state_, count)]);
        evolve(target, Evolution::Normal, false);
        break;
    }
    }
}

void Resolver::play(Action action) {
    const int seat = state_.turn_player;
    auto& player = state_.players[seat];
    const CardId card = player.hand[action.src];
    const auto payment = play_cost(definition(card), player.available_pp());
    const EntityId chosen = action.dst >= 0 ? state_.players[1 - seat].board[action.dst].creation_order : 0;
    const int mode = payment.enhanced ? kEnhancedMode : action.choice;
    spend(player, payment.cost);
    erase(player.hand, player.hand_count, action.src);
    ++counts(player, card).play;
    if (payment.form.kind == Kind::Spell) {
        ++player.shadows;
        enqueue({OnPlay, Origin::Spell, {seat}, card, CrestId::None, chosen, mode});
    } else {
        const auto created = enter(seat, payment.form.id);
        board_ability(created, OnPlay, chosen, mode);
    }
    settle({seat});
    resolve();
}

void Resolver::manual_evolve(Action action) {
    const int seat = state_.turn_player;
    const auto ref = Ref{seat, state_.players[seat].board[action.src].creation_order};
    const EntityId chosen = action.dst >= 0 ? state_.players[1 - seat].board[action.dst].creation_order : 0;
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
    board_ability({seat, object.creation_order}, OnAct);
    resolve();
}

void Resolver::attack(Action action) {
    const int seat = state_.turn_player;
    const int enemy = 1 - seat;
    auto& player = state_.players[seat];
    auto& attacker = player.board[action.src];
    const Ref source{seat, attacker.creation_order};
    ++counts(player, attacker.card_id).attack;
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
    player.actions = {};
    if (seat == 1 && state_.turn_number == 6 && player.extra_pp_uses == 1) {
        player.extra_pp_available = true;
    }
    for (int i = 0; i < player.board_count; ++i) {
        auto& object = player.board[i];
        object.readied = follower(object) && object.entry_ply != ply(state_);
        object.attacked_this_turn = false;
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
        const auto end = std::move(player.deck.begin() + player.deck_idx, player.deck.end(),
                                   player.deck.begin() + kOpeningHand);
        std::copy_n(returned.begin(), corrections, end);
        std::copy_n(player.hand.begin(), kOpeningHand, player.deck.begin());
        player.deck_idx = kOpeningHand;
        if (rules.mulligan_return == MulliganReturn::Shuffle) {
            shuffle(state, player.deck, kOpeningHand);
        }
    }
}

void check_deck(const Deck& deck) {
    std::array<int, kCardSlots> copies{};
    for (CardId id : deck) {
        if (!is_deck_card(id) || ++copies[id - kFirstCard] > 3) {
            throw std::invalid_argument("Deck uses a non-deck card or exceeds the three-copy limit");
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
    state.players[0].deck = first;
    state.players[1].deck = second;
    for (int seat = 0; seat < kPlayers; ++seat) {
        auto& player = state.players[seat];
        player.leader_hp = kLeaderMaxHp;
        player.ep = player.sep = 2;
        player.extra_pp_available = seat == 1;
        shuffle(state, player.deck, 0);
        draw(state, seat, kOpeningHand);
    }
    return state;
}

bool can_choose_enemy_permanent(const Permanent& object) {
    return object.creation_order != 0 && object.clue_count == 0 && !has(object, kAura | kAmbush);
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
        assert(actions.count_ < kMaxActions);
        actions.actions_[actions.count_++] =
            {type, static_cast<std::int8_t>(src), static_cast<std::int8_t>(dst), static_cast<std::uint8_t>(choice)};
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
        if (target != Target::None) {
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
        const auto cost = play_cost(card, player.available_pp());
        if (cost.cost > player.available_pp() || !can_enter(player, cost.form, rules) ||
            player.actions[printed_card(card.id) - kFirstCard].play >= kNameActionLimit) {
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
        const auto& named = player.actions[printed_card(object.card_id) - kFirstCard];
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
        if (object.attacked_this_turn || named.attack >= kNameActionLimit || has(object, kAttackBlocked) ||
            (!object.readied && !has(object, FLAG_RUSH | FLAG_STORM))) {
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
    }
}

} // namespace svwb::wb
