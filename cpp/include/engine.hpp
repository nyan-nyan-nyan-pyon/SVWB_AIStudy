#pragma once

#include "state.hpp"

#include <cstddef>
#include <random>

namespace svwb {

// End turn, up to six targets for each play/evolution, and six per attacker.
inline constexpr std::size_t kMaxLegalActions =
    1 + (kHandLimit + 2 * kBoardLimit) * (kBoardLimit + 1);

class ActionList {
public:
    [[nodiscard]] const Action* begin() const noexcept { return actions_.data(); }
    [[nodiscard]] const Action* end() const noexcept { return begin() + size_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] const Action& operator[](std::size_t index) const noexcept {
        return actions_[index];
    }

private:
    friend ActionList legal_actions(const GameState& state);

    std::array<Action, kMaxLegalActions> actions_{};
    std::size_t size_ = 0;
};

// Player 0 goes first. The initial draw and PP refresh are already applied.
[[nodiscard]] GameState new_game(const Deck& first, const Deck& second, std::mt19937& rng);
[[nodiscard]] ActionList legal_actions(const GameState& state);

// Returns false without modifying the state if the action is illegal.
[[nodiscard]] bool apply_action(GameState& state, Action action);

// The caller must already know that the action is legal for this position.
void apply_legal_action(GameState& state, Action action);

} // namespace svwb
