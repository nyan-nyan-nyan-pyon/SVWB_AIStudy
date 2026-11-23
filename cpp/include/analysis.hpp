#pragma once

#include "search.hpp"

#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace svwb {

using OpeningHands = std::array<std::array<CardId, kOpeningHand>, kPlayers>;

// new_game has drawn the first turn's card for seat 0, after these opening cards.
[[nodiscard]] OpeningHands opening_hands(const GameState& initial);

class OpeningStats {
public:
    OpeningStats(const Deck& deck_a, const Deck& deck_b);
    void observe(const OpeningHands& hands, int a_seat, int winner);
    void write_csv(std::ostream& out, const std::array<char, kPlayers>& deck_labels) const;

private:
    struct Counts {
        std::uint64_t games = 0;
        std::uint64_t wins = 0;
        std::uint64_t draws = 0;
    };
    // Indexed by competitor (A/B), then by card ID and absent/present status.
    std::array<std::map<CardId, std::array<Counts, 2>>, kPlayers> competitors_;
};

class GameTrace {
public:
    GameTrace(std::uint64_t game_number, std::uint32_t seed, int a_seat,
              const std::array<char, kPlayers>& deck_labels, const OpeningHands& hands);
    void search_turn(const GameState& before, const SearchResult& move, std::size_t top);
    void random_turn(const GameState& before, std::span<const Action> actions, const GameState& next);
    [[nodiscard]] std::string finish(int winner);

private:
    void start_turn(const GameState& before, std::string_view controller);

    std::ostringstream output_;
    bool wrote_turn_ = false;
};

} // namespace svwb
