#include "analysis.hpp"

#include "card_db.hpp"
#include "engine.hpp"

#include <algorithm>
#include <iomanip>
#include <locale>
#include <stdexcept>
#include <utility>

namespace svwb {
namespace {

void json_string(std::ostream& out, std::string_view value) {
    constexpr std::string_view hex = "0123456789abcdef";
    out << '"';
    for (const unsigned char ch : value) {
        if (ch == '"' || ch == '\\') {
            out << '\\' << static_cast<char>(ch);
        } else if (ch < 0x20) {
            out << "\\u00" << hex[ch >> 4] << hex[ch & 15];
        } else {
            out << static_cast<char>(ch);
        }
    }
    out << '"';
}

void csv_string(std::ostream& out, std::string_view value) {
    out << '"';
    for (const char ch : value) {
        if (ch == '"') {
            out << '"';
        }
        out << ch;
    }
    out << '"';
}

void write_card_fields(std::ostream& out, CardId id) {
    const auto* card = find_card(id);
    if (!card) {
        throw std::logic_error("Trace contains an unknown card ID");
    }
    out << "\"card_id\":" << id << ",\"name\":";
    json_string(out, card->name);
}

std::string_view action_name(ActionType type) {
    switch (type) {
    case ActionType::END_TURN:
        return "end_turn";
    case ActionType::PLAY_CARD:
        return "play_card";
    case ActionType::EVOLVE:
        return "evolve";
    case ActionType::ATTACK:
        return "attack";
    }
    throw std::logic_error("Unknown action type");
}

void write_action(std::ostream& out, const GameState& state, Action action) {
    out << "{\"type\":";
    json_string(out, action_name(action.type));
    const auto seat = state.turn_player;
    const auto enemy = 1 - seat;
    if (action.type != ActionType::END_TURN) {
        const auto& own = state.players[seat];
        const bool play = action.type == ActionType::PLAY_CARD;
        out << ",\"source\":{\"zone\":\"" << (play ? "hand" : "board")
            << "\",\"slot\":" << action.src + 1 << ',';
        write_card_fields(out, play ? own.hand[action.src] : own.board[action.src].card_id);
        out << '}';
    }
    if (action.dst == kEnemyLeader) {
        out << ",\"target\":{\"kind\":\"leader\",\"seat\":" << enemy + 1 << '}';
    } else if (action.dst >= 0) {
        out << ",\"target\":{\"kind\":\"follower\",\"seat\":" << enemy + 1
            << ",\"slot\":" << action.dst + 1 << ',';
        write_card_fields(out, state.players[enemy].board[action.dst].card_id);
        out << '}';
    }
    out << '}';
}

GameState write_actions(std::ostream& out, const GameState& before, std::span<const Action> actions) {
    auto state = before;
    out << '[';
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const auto action = actions[i];
        auto next = state;
        if (state.turn_player != before.turn_player || !apply_action(next, action)) {
            throw std::logic_error("Trace action is illegal when replayed from the turn start");
        }
        if (i != 0) {
            out << ", ";
        }
        write_action(out, state, action);
        state = next;
    }
    out << ']';
    if (state.winner == kOngoing && state.turn_player == before.turn_player) {
        throw std::logic_error("Trace sequence does not finish its turn");
    }
    return state;
}

} // namespace

OpeningHands opening_hands(const GameState& initial) {
    OpeningHands hands{};
    for (int seat = 0; seat < kPlayers; ++seat) {
        if (initial.players[seat].hand_count < kOpeningHand) {
            throw std::logic_error("Missing initial opening cards");
        }
        std::copy_n(initial.players[seat].hand.begin(), kOpeningHand, hands[seat].begin());
    }
    return hands;
}

OpeningStats::OpeningStats(const Deck& deck_a, const Deck& deck_b) {
    const std::array decks{&deck_a, &deck_b};
    for (int competitor = 0; competitor < kPlayers; ++competitor) {
        for (const auto card : *decks[competitor]) {
            if (!find_card(card)) {
                throw std::invalid_argument("Unknown card in opening stats deck");
            }
            competitors_[competitor].try_emplace(card);
        }
    }
}

void OpeningStats::observe(const OpeningHands& hands, int a_seat, int winner) {
    for (int competitor = 0; competitor < kPlayers; ++competitor) {
        const int seat = competitor == 0 ? a_seat : 1 - a_seat;
        const auto& hand = hands[seat];
        for (auto& [card, buckets] : competitors_[competitor]) {
            const bool present = std::find(hand.begin(), hand.end(), card) != hand.end();
            auto& count = buckets[present];
            ++count.games;
            count.wins += winner == seat;
            count.draws += winner == kDraw;
        }
    }
}

void OpeningStats::write_csv(std::ostream& out, const std::array<char, kPlayers>& deck_labels) const {
    out << "competitor,deck,card_id,card_name,with_games,with_wins,with_draws,with_win_rate,"
           "without_games,without_wins,without_draws,without_win_rate,win_rate_diff_pp\n";
    const auto rate = [](const Counts& count) {
        return static_cast<double>(count.wins) / static_cast<double>(count.games);
    };
    const auto write_group = [&](const Counts& count) {
        out << count.games << ',' << count.wins << ',' << count.draws << ',';
        if (count.games > 0) {
            out << rate(count);
        }
    };
    out << std::fixed << std::setprecision(8);
    for (int competitor = 0; competitor < kPlayers; ++competitor) {
        for (const auto& [card, buckets] : competitors_[competitor]) {
            out << static_cast<char>('A' + competitor) << ',' << deck_labels[competitor] << ',' << card << ',';
            csv_string(out, find_card(card)->name);
            out << ',';
            write_group(buckets[1]);
            out << ',';
            write_group(buckets[0]);
            out << ',';
            if (buckets[1].games > 0 && buckets[0].games > 0) {
                out << 100.0 * (rate(buckets[1]) - rate(buckets[0]));
            }
            out << '\n';
        }
    }
}

GameTrace::GameTrace(std::uint64_t game_number, std::uint32_t seed, int a_seat,
                     const std::array<char, kPlayers>& deck_labels, const OpeningHands& hands) {
    output_.imbue(std::locale::classic());
    output_ << std::setprecision(9)
            << "{\n  \"seed\": " << seed << ",\n  \"game\": " << game_number << ",\n  \"opening_hands\": [\n";
    for (int seat = 0; seat < kPlayers; ++seat) {
        const int competitor = seat == a_seat ? 0 : 1;
        output_ << "    {\"seat\":" << seat + 1 << ",\"competitor\":\"" << static_cast<char>('A' + competitor)
                << "\",\"deck\":\"" << deck_labels[competitor] << "\",\"cards\":[";
        for (std::size_t i = 0; i < hands[seat].size(); ++i) {
            if (i != 0) {
                output_ << ", ";
            }
            output_ << '{';
            write_card_fields(output_, hands[seat][i]);
            output_ << '}';
        }
        output_ << "]}" << (seat + 1 == kPlayers ? "\n" : ",\n");
    }
    output_ << "  ],\n  \"turns\": [\n";
}

void GameTrace::start_turn(const GameState& before, std::string_view controller) {
    if (wrote_turn_) {
        output_ << ",\n";
    }
    wrote_turn_ = true;
    output_ << "    {\n      \"round\": " << static_cast<int>(before.turn_number)
            << ", \"seat\": " << before.turn_player + 1
            << ", \"pp_before\": " << static_cast<int>(before.players[before.turn_player].pp)
            << ", \"controller\": ";
    json_string(output_, controller);
}

void GameTrace::search_turn(const GameState& before, const SearchResult& move, std::size_t top) {
    const auto& ranked = move.trace.ranked;
    const auto selected = std::find_if(ranked.begin(), ranked.end(), [](const auto& candidate) {
        return candidate.selected;
    });
    if (selected == ranked.end() || move.trace.total_visits <= 0) {
        throw std::logic_error("Missing search trace");
    }
    start_turn(before, "search");
    output_ << ",\n      \"total_visits\": " << move.trace.total_visits
            << ", \"selected_rank\": " << (selected - ranked.begin() + 1)
            << ", \"selected_actions\": ";
    if (write_actions(output_, before, selected->actions) != move.next) {
        throw std::logic_error("Selected trace actions do not reproduce the next state");
    }
    output_ << ",\n      \"candidates\": [\n";
    const auto shown = std::min(top, ranked.size());
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& candidate = ranked[i];
        output_ << "        {\"rank\":" << (i + 1) << ",\"visits\":" << candidate.visits
                << ",\"visit_share\":" << static_cast<double>(candidate.visits) / move.trace.total_visits
                << ",\"value\":";
        if (candidate.value) {
            output_ << *candidate.value;
        } else {
            output_ << "null";
        }
        output_ << ",\"selected\":" << (candidate.selected ? "true" : "false") << ",\"actions\":";
        write_actions(output_, before, candidate.actions);
        output_ << (i + 1 == shown ? "}\n" : "},\n");
    }
    output_ << "      ]\n    }";
}

void GameTrace::random_turn(const GameState& before, std::span<const Action> actions, const GameState& next) {
    start_turn(before, "random");
    output_ << ",\n      \"total_visits\": 0, \"selected_rank\": null, \"selected_actions\": ";
    if (write_actions(output_, before, actions) != next) {
        throw std::logic_error("Random trace actions do not reproduce the next state");
    }
    output_ << ",\n      \"candidates\": []\n    }";
}

std::string GameTrace::finish(int winner) {
    output_ << "\n  ],\n  \"winner_seat\": ";
    if (winner == kDraw) {
        output_ << "null";
    } else {
        output_ << winner + 1;
    }
    output_ << "\n}\n";
    return std::move(output_).str();
}

} // namespace svwb
