#include "engine.hpp"

#include "card_db.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace svwb {
namespace {

constexpr std::uint16_t kAttackFlags =
    FLAG_CAN_ATTACK | FLAG_CAN_HIT_FACE | FLAG_ATTACKED_THIS_TURN;

bool has_flag(const Follower& follower, FollowerFlags flag) {
    return (follower.flags & flag) != 0;
}

void draw_cards(GameState& state, std::uint8_t owner, int count) {
    auto& player = state.players[owner];
    for (int i = 0; i < count; ++i) {
        if (player.deck_idx == kDeckSize) {
            state.winner = static_cast<std::int8_t>(1 - owner);
            return;
        }
        const CardId id = player.deck[player.deck_idx++];
        if (player.hand_count == kHandLimit) {
            ++player.shadows;
        } else {
            player.hand[player.hand_count++] = id;
        }
    }
}

void summon(PlayerState& player, const CardDef& card) {
    assert(player.board_count < kBoardLimit);
    auto flags = card.flags;
    if (flags & FLAG_STORM) {
        flags |= FLAG_CAN_ATTACK | FLAG_CAN_HIT_FACE;
    } else if (flags & FLAG_RUSH) {
        flags |= FLAG_CAN_ATTACK;
    }
    player.board[player.board_count++] = {card.id, card.atk, card.hp, card.hp, 0, flags};
}

void start_turn(GameState& state) {
    auto& player = state.players[state.turn_player];
    player.max_pp = static_cast<std::uint8_t>(std::min(player.max_pp + 1, kMaxPp));
    player.pp = player.max_pp;
    player.evolved_this_turn = 0;
    for (int slot = 0; slot < player.board_count; ++slot) {
        auto& follower = player.board[slot];
        follower.flags &= ~kAttackFlags;
        follower.flags |= FLAG_CAN_ATTACK | FLAG_CAN_HIT_FACE;
    }
    draw_cards(state, state.turn_player, 1);
}

void end_turn(GameState& state) {
    auto& player = state.players[state.turn_player];
    for (int slot = 0; slot < player.board_count; ++slot) {
        player.board[slot].flags &= ~kAttackFlags;
    }
    player.evolved_this_turn = 0;
    state.turn_player ^= 1;
    if (state.turn_player == 0) {
        ++state.turn_number;
    }
    start_turn(state);
}

struct Event {
    Effect effect;
    std::uint8_t owner;
    std::uint8_t target_player;
    std::int8_t target_slot;
};

class EventQueue {
public:
    void push(Event event) {
        if (tail_ == events_.size()) {
            throw std::logic_error("Phase 1 effect queue capacity exceeded");
        }
        events_[tail_++] = event;
    }

    [[nodiscard]] bool empty() const { return head_ == tail_; }

    Event pop() { return events_[head_++]; }

    void remove_target(std::uint8_t player, int slot) {
        for (auto i = head_; i < tail_; ++i) {
            auto& event = events_[i];
            if (event.target_player == player) {
                if (event.target_slot == slot) {
                    event.target_slot = kNoTarget;
                } else if (event.target_slot > slot) {
                    --event.target_slot;
                }
            }
        }
    }

private:
    // One initiating trigger and at most two boards' worth of death triggers.
    // Phase 1 death effects only draw cards and cannot add followers or damage.
    std::array<Event, (1 + kPlayers * kBoardLimit) * kEffectsPerCard> events_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
};

void enqueue_trigger(EventQueue& queue, const CardDef& card, Trigger trigger,
                     std::uint8_t owner, std::int8_t source, std::int8_t chosen) {
    const auto opponent = static_cast<std::uint8_t>(1 - owner);
    for (int i = 0; i < card.effect_count; ++i) {
        const auto& effect = card.effects[i];
        if (effect.trigger != trigger) {
            continue;
        }
        Event event{effect, owner, opponent, kNoTarget};
        switch (effect.target) {
        case TargetType::CHOSEN_ENEMY_FOLLOWER:
        case TargetType::CHOSEN_ANY_ENEMY:
            event.target_slot = chosen;
            break;
        case TargetType::ENEMY_LEADER:
            event.target_slot = kEnemyLeader;
            break;
        case TargetType::SELF_LEADER:
            event.target_player = owner;
            event.target_slot = kEnemyLeader;
            break;
        case TargetType::SELF_FOLLOWER:
            event.target_player = owner;
            event.target_slot = source;
            break;
        case TargetType::ALL_ENEMY_FOLLOWERS:
        case TargetType::NONE:
            break;
        }
        queue.push(event);
    }
}

void remove_dead(GameState& state, EventQueue& queue) {
    for (int offset = 0; offset < kPlayers; ++offset) {
        const auto owner = static_cast<std::uint8_t>((state.turn_player + offset) % kPlayers);
        auto& player = state.players[owner];
        for (int slot = 0; slot < player.board_count;) {
            if (player.board[slot].hp > 0) {
                ++slot;
                continue;
            }
            const auto id = player.board[slot].card_id;
            std::move(player.board.begin() + slot + 1,
                      player.board.begin() + player.board_count, player.board.begin() + slot);
            player.board[--player.board_count] = {};
            ++player.shadows;
            queue.remove_target(owner, slot);
            enqueue_trigger(queue, *find_card(id), Trigger::ON_DEATH, owner, kNoTarget, kNoTarget);
        }
    }
}

void apply_follower_effect(Follower& follower, const Effect& effect) {
    switch (effect.type) {
    case EffectType::DAMAGE:
        follower.hp -= effect.amount;
        break;
    case EffectType::HEAL:
        follower.hp = static_cast<std::int8_t>(
            std::min(follower.hp + effect.amount, static_cast<int>(follower.max_hp)));
        break;
    case EffectType::BUFF:
        follower.atk += effect.amount;
        follower.max_hp += effect.hp_buff;
        follower.hp += effect.hp_buff;
        break;
    case EffectType::DRAW:
    case EffectType::SUMMON:
        break;
    }
}

void apply_effect(GameState& state, const Event& event) {
    const auto& effect = event.effect;
    if (effect.type == EffectType::DRAW) {
        draw_cards(state, event.owner, effect.amount);
        return;
    }
    if (effect.type == EffectType::SUMMON) {
        auto& player = state.players[event.owner];
        for (int i = 0; i < effect.amount && player.board_count < kBoardLimit; ++i) {
            summon(player, *find_card(effect.summon_id));
        }
        return;
    }
    auto& target = state.players[event.target_player];
    if (effect.target == TargetType::ALL_ENEMY_FOLLOWERS) {
        for (int slot = 0; slot < target.board_count; ++slot) {
            apply_follower_effect(target.board[slot], effect);
        }
    } else if (event.target_slot >= 0) {
        apply_follower_effect(target.board[event.target_slot], effect);
    } else if (event.target_slot == kEnemyLeader) {
        if (effect.type == EffectType::DAMAGE) {
            target.leader_hp -= effect.amount;
        } else if (effect.type == EffectType::HEAL) {
            target.leader_hp = static_cast<std::int8_t>(
                std::min(target.leader_hp + effect.amount, kLeaderMaxHp));
        }
    }
}

void resolve(GameState& state, EventQueue& queue) {
    while (state.winner == kOngoing) {
        const bool first_dead = state.players[0].leader_hp <= 0;
        const bool second_dead = state.players[1].leader_hp <= 0;
        if (first_dead || second_dead) {
            state.winner = first_dead && second_dead ? kDraw : first_dead ? 1 : 0;
            return;
        }
        remove_dead(state, queue);
        if (queue.empty()) {
            return;
        }
        apply_effect(state, queue.pop());
    }
}

TargetType chosen_target(const CardDef& card, Trigger trigger) {
    for (int i = 0; i < card.effect_count; ++i) {
        const auto& effect = card.effects[i];
        if (effect.trigger == trigger &&
            (effect.target == TargetType::CHOSEN_ENEMY_FOLLOWER ||
             effect.target == TargetType::CHOSEN_ANY_ENEMY)) {
            return effect.target;
        }
    }
    return TargetType::NONE;
}

void play_card(GameState& state, Action action, EventQueue& queue) {
    auto& player = state.players[state.turn_player];
    const auto& card = *find_card(player.hand[action.src]);
    player.pp -= card.cost;
    std::move(player.hand.begin() + action.src + 1,
              player.hand.begin() + player.hand_count, player.hand.begin() + action.src);
    player.hand[--player.hand_count] = 0;
    auto source = kNoTarget;
    if (card.type == CardType::SPELL) {
        ++player.shadows;
    } else {
        source = static_cast<std::int8_t>(player.board_count);
        summon(player, card);
    }
    enqueue_trigger(queue, card, Trigger::ON_PLAY, state.turn_player, source, action.dst);
}

void evolve(GameState& state, Action action, EventQueue& queue) {
    auto& player = state.players[state.turn_player];
    auto& follower = player.board[action.src];
    --player.ep;
    player.evolved_this_turn = 1;
    follower.flags |= FLAG_EVOLVED;
    if (!has_flag(follower, FLAG_ATTACKED_THIS_TURN)) {
        follower.flags |= FLAG_CAN_ATTACK;
    }
    follower.atk += 2;
    follower.hp += 2;
    follower.max_hp += 2;
    enqueue_trigger(queue, *find_card(follower.card_id), Trigger::ON_EVOLVE,
                    state.turn_player, action.src, action.dst);
}

void attack(GameState& state, Action action) {
    auto& attacker = state.players[state.turn_player].board[action.src];
    auto& enemy = state.players[1 - state.turn_player];
    attacker.flags &= ~kAttackFlags;
    attacker.flags |= FLAG_ATTACKED_THIS_TURN;
    if (action.dst == kEnemyLeader) {
        enemy.leader_hp -= attacker.atk;
    } else {
        auto& defender = enemy.board[action.dst];
        defender.hp -= attacker.atk;
        attacker.hp -= defender.atk;
    }
}

} // namespace

GameState new_game(const Deck& first, const Deck& second, std::mt19937& rng) {
    const auto valid_deck = [](const Deck& deck) {
        return std::all_of(deck.begin(), deck.end(), [](CardId id) {
            const auto* card = find_card(id);
            return card != nullptr && card->type != CardType::TOKEN;
        });
    };
    if (!valid_deck(first) || !valid_deck(second)) {
        throw std::invalid_argument("A deck must contain only registered, non-token cards");
    }

    GameState state{};
    state.winner = kOngoing;
    state.turn_number = 1;
    state.players[0].deck = first;
    state.players[1].deck = second;
    for (std::uint8_t owner = 0; owner < kPlayers; ++owner) {
        auto& player = state.players[owner];
        player.leader_hp = kLeaderMaxHp;
        player.ep = static_cast<std::uint8_t>(2 + owner);
        std::shuffle(player.deck.begin(), player.deck.end(), rng);
        draw_cards(state, owner, kOpeningHand);
    }
    start_turn(state);
    return state;
}

ActionList legal_actions(const GameState& state) {
    ActionList actions;
    if (state.winner != kOngoing) {
        return actions;
    }

    const auto add = [&actions](ActionType type, int src, int dst) {
        assert(actions.size_ < actions.actions_.size());
        actions.actions_[actions.size_++] =
            {type, static_cast<std::int8_t>(src), static_cast<std::int8_t>(dst)};
    };
    const auto& player = state.players[state.turn_player];
    const auto& enemy = state.players[1 - state.turn_player];
    const auto add_trigger_actions = [&](ActionType type, int src, TargetType target) {
        if (target == TargetType::NONE ||
            (type == ActionType::EVOLVE && target == TargetType::CHOSEN_ENEMY_FOLLOWER &&
             enemy.board_count == 0)) {
            add(type, src, kNoTarget);
            return;
        }
        for (int dst = 0; dst < enemy.board_count; ++dst) {
            add(type, src, dst);
        }
        if (target == TargetType::CHOSEN_ANY_ENEMY) {
            add(type, src, kEnemyLeader);
        }
    };

    add(ActionType::END_TURN, kNoTarget, kNoTarget);
    for (int src = 0; src < player.hand_count; ++src) {
        const auto& card = *find_card(player.hand[src]);
        if (card.cost <= player.pp &&
            (card.type == CardType::SPELL || player.board_count < kBoardLimit)) {
            add_trigger_actions(ActionType::PLAY_CARD, src, chosen_target(card, Trigger::ON_PLAY));
        }
    }

    const bool can_evolve =
        player.ep > 0 && !player.evolved_this_turn && state.turn_number >= 5 - state.turn_player;
    const bool has_ward = std::any_of(enemy.board.begin(), enemy.board.begin() + enemy.board_count,
        [](const Follower& follower) { return has_flag(follower, FLAG_WARD); });
    for (int src = 0; src < player.board_count; ++src) {
        const auto& follower = player.board[src];
        if (can_evolve && !has_flag(follower, FLAG_EVOLVED)) {
            const auto& card = *find_card(follower.card_id);
            add_trigger_actions(ActionType::EVOLVE, src, chosen_target(card, Trigger::ON_EVOLVE));
        }
        if (!has_flag(follower, FLAG_CAN_ATTACK) || follower.atk <= 0) {
            continue;
        }
        for (int dst = 0; dst < enemy.board_count; ++dst) {
            if (!has_ward || has_flag(enemy.board[dst], FLAG_WARD)) {
                add(ActionType::ATTACK, src, dst);
            }
        }
        if (!has_ward && has_flag(follower, FLAG_CAN_HIT_FACE)) {
            add(ActionType::ATTACK, src, kEnemyLeader);
        }
    }
    return actions;
}

bool apply_action(GameState& state, Action action) {
    const auto actions = legal_actions(state);
    if (std::find(actions.begin(), actions.end(), action) == actions.end()) {
        return false;
    }
    if (action.type == ActionType::END_TURN) {
        end_turn(state);
        return true;
    }

    EventQueue queue;
    switch (action.type) {
    case ActionType::PLAY_CARD:
        play_card(state, action, queue);
        break;
    case ActionType::EVOLVE:
        evolve(state, action, queue);
        break;
    case ActionType::ATTACK:
        attack(state, action);
        break;
    case ActionType::END_TURN:
        break;
    }
    resolve(state, queue);
    return true;
}

} // namespace svwb
