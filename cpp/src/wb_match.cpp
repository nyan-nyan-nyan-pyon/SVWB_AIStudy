#include "wb_match.hpp"

#include "training_logs.hpp"
#include "wb_search.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace svwb::wb {
namespace {

using Clock = std::chrono::steady_clock;
using Hand = std::array<CardId, kOpeningHand>;

struct Opening {
    Hand offered{};
    Hand kept{}; // Zero in slots that were replaced.
    Hand final{};
    unsigned mask = 0;
};

struct Game {
    GameLog log;
    std::array<Opening, kPlayers> opening;
    Outcome outcome = Outcome::Ongoing;
    int turns = 0;
    int rounds = 0;
    double elapsed = 0;
    std::string trace;
};

struct Counts {
    std::uint64_t games = 0;
    std::uint64_t wins = 0;
    std::uint64_t both_lose = 0;

    void observe(Outcome outcome, int seat) {
        ++games;
        wins += outcome_value(outcome, seat) == 1;
        both_lose += outcome == Outcome::BothLose;
    }

    double rate() const {
        return static_cast<double>(wins) / static_cast<double>(games);
    }
};

struct Cohorts {
    std::array<Counts, 2> kept;
    std::array<Counts, 2> present;
};

class OpeningTable {
public:
    void observe(const Opening& opening, int seat, Outcome outcome) {
        for (const int group : {seat, kPlayers}) {
            for (std::size_t i = 0; i < kMirrorCardIds.size(); ++i) {
                const auto id = kMirrorCardIds[i];
                auto& cell = rows_[group][i];
                cell.kept[contains(opening.kept, id)].observe(outcome, seat);
                cell.present[contains(opening.final, id)].observe(outcome, seat);
            }
        }
    }

    void write(std::ostream& out) const {
        out << "seat,card_id,official_card_id,card_name,"
               "kept_games,kept_wins,kept_both_lose,kept_win_rate,"
               "not_kept_games,not_kept_wins,not_kept_both_lose,not_kept_win_rate,kept_diff_pp,"
               "present_games,present_wins,present_both_lose,present_win_rate,"
               "absent_games,absent_wins,absent_both_lose,absent_win_rate,present_diff_pp\n";
        constexpr std::array<std::string_view, 3> labels{"first", "second", "combined"};
        out << std::fixed << std::setprecision(8);
        for (std::size_t group = 0; group < rows_.size(); ++group) {
            for (std::size_t i = 0; i < kMirrorCardIds.size(); ++i) {
                const auto id = kMirrorCardIds[i];
                const auto& card = *find_card(id);
                out << labels[group] << ',' << id << ',';
                if (card.official_id) {
                    out << card.official_id;
                }
                out << ",\"";
                for (const char byte : card.name) {
                    if (byte == '"') {
                        out << '"';
                    }
                    out << byte;
                }
                out << "\",";
                write_comparison(out, rows_[group][i].kept);
                out << ',';
                write_comparison(out, rows_[group][i].present);
                out << '\n';
            }
        }
    }

private:
    static bool contains(const Hand& hand, CardId id) {
        return std::find(hand.begin(), hand.end(), id) != hand.end();
    }

    static void write_comparison(std::ostream& out, const std::array<Counts, 2>& groups) {
        for (const bool yes : {true, false}) {
            const auto& counts = groups[yes];
            out << counts.games << ',' << counts.wins << ',' << counts.both_lose << ',';
            if (counts.games) {
                out << counts.rate();
            }
            out << ',';
        }
        if (groups[0].games && groups[1].games) {
            out << 100.0 * (groups[1].rate() - groups[0].rate());
        }
    }

    std::array<std::array<Cohorts, kDeckCardKinds>, kPlayers + 1> rows_{};
};

void json_string(std::ostream& out, std::string_view name) {
    constexpr std::string_view hex = "0123456789abcdef";
    out << '"';
    for (const unsigned char byte : name) {
        if (byte == '"' || byte == '\\') {
            out << '\\' << static_cast<char>(byte);
        } else if (byte < 32) {
            out << "\\u00" << hex[byte >> 4] << hex[byte & 15];
        } else {
            out << static_cast<char>(byte);
        }
    }
    out << '"';
}

void card_fields(std::ostream& out, CardId id) {
    out << "\"card_id\":" << id << ",\"name\":";
    json_string(out, find_card(id)->name);
}

void cards_json(std::ostream& out, const Hand& hand) {
    out << '[';
    bool comma = false;
    for (const auto id : hand) {
        if (id) {
            out << (comma ? ", {" : "{");
            card_fields(out, id);
            out << '}';
            comma = true;
        }
    }
    out << ']';
}

std::string_view type_name(ActionType type) {
    constexpr std::array<std::string_view, 8> names{
        "end_turn", "play_card", "evolve", "super_evolve", "attack", "act", "extra_pp", "mulligan",
    };
    return names[static_cast<std::size_t>(type)];
}

GameState actions_json(std::ostream& out, const GameState& initial, std::span<const Action> actions) {
    auto state = initial;
    out << '[';
    bool comma = false;
    for (const auto action : actions) {
        const auto before = state;
        if (before.turn_player != initial.turn_player || !apply_action(state, action)) {
            throw std::logic_error("Trace action cannot be replayed");
        }
        auto& player = before.players[before.turn_player];
        out << (comma ? ", {" : "{") << "\"type\":";
        json_string(out, type_name(action.type));
        if (action.src >= 0) {
            const bool play = action.type == ActionType::Play;
            out << ",\"source\":{\"zone\":\"" << (play ? "hand" : "board") << "\",\"slot\":"
                << action.src + 1 << ',';
            card_fields(out, play ? player.hand[action.src] : player.board[action.src].card_id);
            if (play && state.players[before.turn_player].board_count > 0 &&
                player.hand[action.src] == kAlmiraj && player.available_pp() < 3) {
                out << ",\"form\":\"crystal\"";
            }
            out << '}';
        }
        if (action.choice) {
            out << ",\"mode\":" << static_cast<int>(action.choice);
        }
        const int enemy = 1 - before.turn_player;
        if (action.dst == kEnemyLeader) {
            out << ",\"target\":{\"kind\":\"leader\",\"seat\":" << 1 + enemy << '}';
        } else if (action.dst >= 0) {
            out << ",\"target\":{\"kind\":\"follower\",\"seat\":" << 1 + enemy
                << ",\"slot\":" << action.dst + 1 << ',';
            card_fields(out, before.players[enemy].board[action.dst].card_id);
            out << '}';
        }
        out << '}';
        comma = true;
    }
    out << ']';
    if (state.outcome == Outcome::Ongoing && state.turn_player == initial.turn_player) {
        throw std::logic_error("Trace did not finish the turn");
    }
    return state;
}

void write_turn(std::ostream& out, const GameState& state, const Decision& decision,
                int turn, std::size_t top, std::span<const Action> random_play = {}) {
    if (turn != 0) {
        out << ",\n";
    }
    out << "    {\n      \"round\":" << static_cast<int>(state.turn_number)
        << ",\"seat\":" << 1 + state.turn_player << ",\"pp_before\":" << state.players[state.turn_player].available_pp()
        << ",\"total_visits\":" << decision.visits << ",\"selected_rank\":";
    const auto selected = std::find_if(decision.ranked.begin(), decision.ranked.end(),
                                       [](const auto& candidate) { return candidate.selected; });
    if (selected != decision.ranked.end()) {
        out << selected - decision.ranked.begin() + 1;
    } else {
        out << "null";
    }
    out << ",\n      \"selected_actions\":";
    const auto actions = selected == decision.ranked.end() ? random_play : std::span<const Action>(selected->actions);
    if (actions_json(out, state, actions) != decision.next) {
        throw std::logic_error("Trace does not reproduce the selected successor");
    }
    out << ",\n      \"candidates\":[";
    for (std::size_t i = 0; i < std::min(top, decision.ranked.size()); ++i) {
        const auto& candidate = decision.ranked[i];
        out << (i ? ",\n" : "\n") << "        {\"rank\":" << i + 1 << ",\"visits\":" << candidate.visits
            << ",\"visit_share\":" << static_cast<double>(candidate.visits) / decision.visits << ",\"value\":";
        if (candidate.value) {
            out << *candidate.value;
        } else {
            out << "null";
        }
        out << ",\"actions\":";
        static_cast<void>(actions_json(out, state, candidate.actions));
        out << '}';
    }
    out << "]\n    }";
}

void start_trace(std::ostream& out, const Game& game, std::uint64_t id, const MatchOptions& options) {
    out << std::setprecision(9) << "{\n  \"rules\":\"wb\",\"seed\":" << options.seed << ",\"game\":" << id + 1
        << ",\"competitor_a_seat\":" << 1 + id % 2
        << ",\"simulations\":" << options.search.simulations << ",\n  \"opening_hands\":[\n";
    for (int seat = 0; seat < kPlayers; ++seat) {
        const auto& opening = game.opening[seat];
        out << "    {\"seat\":" << seat + 1 << ",\"mulligan_mask\":" << opening.mask << ",\"offered\":";
        cards_json(out, opening.offered);
        out << ",\"kept\":";
        cards_json(out, opening.kept);
        out << ",\"after_mulligan\":";
        cards_json(out, opening.final);
        out << (seat == 0 ? "},\n" : "}\n");
    }
    out << "  ],\n  \"turns\":[\n";
}

Game play(const MatchOptions& options, const Mlp& a, const Mlp& b, std::uint64_t id, bool trace) {
    const auto start = Clock::now();
    const int a_seat = static_cast<int>(id % 2);
    std::seed_seq seed{options.seed, static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(id >> 32)};
    std::mt19937 rng(seed);
    const auto high = static_cast<std::uint64_t>(rng()) << 32;
    const auto deck_seed = high | rng();
    auto state = new_game(kMirrorDeck, kMirrorDeck, deck_seed);
    const std::array<const Mlp*, kPlayers> models{a_seat == 0 ? &a : &b, a_seat == 0 ? &b : &a};
    auto search_options = options.search;
    search_options.record_actions = trace;
    Search search(*models[0], *models[1], rng, search_options);
    Game game;
    for (int seat = 0; seat < kPlayers; ++seat) {
        auto& opening = game.opening[seat];
        std::copy_n(state.players[seat].hand.begin(), kOpeningHand, opening.offered.begin());
        const auto action = options.random_opponent && seat != a_seat
            ? Action{ActionType::Mulligan, kNoTarget, kNoTarget,
                     static_cast<std::uint8_t>(std::uniform_int_distribution<int>(0, (1 << kOpeningHand) - 1)(rng))}
            : choose_mulligan(state, *models[seat], rng);
        opening.mask = action.choice;
        for (int i = 0; i < kOpeningHand; ++i) {
            if (!(action.choice & (1 << i))) {
                opening.kept[i] = opening.offered[i];
            }
        }
        apply_legal_action(state, action);
        std::copy_n(state.players[seat].hand.begin(), kOpeningHand, opening.final.begin());
    }
    std::ostringstream trace_data;
    if (trace) {
        start_trace(trace_data, game, id, options);
    }
    const bool logging = !options.log_dir.empty() && !trace;
    std::vector<int> value_seats;
    while (state.outcome == Outcome::Ongoing) {
        if (game.turns >= 2 * kTurnLimit) {
            throw std::logic_error("Match exceeded the turn limit");
        }
        if (options.random_opponent && state.turn_player != a_seat) {
            const auto before = state;
            std::vector<Action> random_play;
            const int actor = state.turn_player;
            int actions = 0;
            while (state.outcome == Outcome::Ongoing && state.turn_player == actor) {
                if (++actions > 1024) {
                    throw std::logic_error("Random turn exceeded the action limit");
                }
                const auto legal = legal_actions(state);
                const auto action = legal[std::uniform_int_distribution<int>(0, legal.size() - 1)(rng)];
                if (trace) {
                    random_play.push_back(action);
                }
                apply_legal_action(state, action);
            }
            if (trace) {
                Decision random{};
                random.next = state;
                write_turn(trace_data, before, random, game.turns, options.trace_top, random_play);
            }
        } else {
            if (logging) {
                game.log.values.push_back({extract_features(state), 0});
                value_seats.push_back(state.turn_player);
            }
            const auto decision = search.choose(state, logging);
            if (trace) {
                write_turn(trace_data, state, decision, game.turns, options.trace_top);
            }
            if (logging) {
                game.log.policy_sizes.push_back(static_cast<std::int32_t>(decision.policy.size()));
                for (const auto& sample : decision.policy) {
                    game.log.policies.push_back({sample.features, sample.probability});
                }
            }
            state = decision.next;
        }
        ++game.turns;
    }
    game.outcome = state.outcome;
    game.rounds = state.turn_number;
    game.elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    for (std::size_t i = 0; i < value_seats.size(); ++i) {
        game.log.values[i].target = outcome_value(game.outcome, value_seats[i]);
    }
    if (trace) {
        trace_data << "\n  ],\n  \"winner_seat\":";
        if (game.outcome == Outcome::BothLose) {
            trace_data << "null";
        } else {
            trace_data << (game.outcome == Outcome::FirstWins ? 1 : 2);
        }
        trace_data << ",\"both_lose\":" << (game.outcome == Outcome::BothLose ? "true" : "false") << "\n}\n";
        game.trace = std::move(trace_data).str();
    }
    return game;
}

struct Totals {
    std::uint64_t games = 0, a_first = 0, a_wins = 0, b_wins = 0, both_lose = 0;
    std::uint64_t first_wins = 0, second_wins = 0, turns = 0, rounds = 0, policy_samples = 0;
    double game_seconds = 0;

    void observe(const Game& game, int a_seat) {
        ++games;
        a_first += a_seat == 0;
        a_wins += outcome_value(game.outcome, a_seat) == 1;
        b_wins += outcome_value(game.outcome, 1 - a_seat) == 1;
        first_wins += game.outcome == Outcome::FirstWins;
        second_wins += game.outcome == Outcome::SecondWins;
        both_lose += game.outcome == Outcome::BothLose;
        turns += game.turns;
        rounds += game.rounds;
        policy_samples += game.log.policies.size();
        game_seconds += game.elapsed;
    }
};

std::ofstream output_file(const std::filesystem::path& path) {
    std::ofstream stream;
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.open(path, std::ios::binary | std::ios::trunc);
    return stream;
}

} // namespace

void run_match(const MatchOptions& options) {
    const auto a = options.weights.empty() ? Mlp::random(options.seed) : Mlp::load(options.weights);
    const auto b = !options.opponent_weights.empty() ? Mlp::load(options.opponent_weights)
                 : options.opponent_seed ? Mlp::random(*options.opponent_seed) : a;
    std::optional<TrainingLogs> logs;
    if (!options.log_dir.empty()) {
        logs.emplace(options.log_dir);
    }
    const bool analyzing = !options.analysis_dir.empty();
    OpeningTable table;
    std::array<std::optional<std::uint64_t>, kPlayers> representatives;
    std::optional<Outcome> explicit_game;
    Totals totals;
    std::mutex mutex;
    std::exception_ptr failure;
    std::atomic<bool> stop = false;
    std::atomic<std::uint64_t> next = 0;
    const auto start = Clock::now();
    const auto deadline = options.seconds ? start + std::chrono::seconds(options.seconds) : Clock::time_point::max();
    const auto worker = [&](std::stop_token token) {
        try {
            while (!token.stop_requested() && !stop.load(std::memory_order_relaxed) && Clock::now() < deadline) {
                const auto id = next.fetch_add(1, std::memory_order_relaxed);
                if (options.games && id >= options.games) {
                    break;
                }
                const auto game = play(options, a, b, id, false);
                const auto a_seat = static_cast<int>(id % 2);
                const std::scoped_lock lock(mutex);
                if (logs) {
                    logs->append(game.log);
                }
                if (analyzing) {
                    table.observe(game.opening[a_seat], a_seat, game.outcome);
                    if (game.outcome != Outcome::BothLose) {
                        auto& slot = representatives[game.outcome == Outcome::FirstWins ? 0 : 1];
                        if (!slot || id < *slot) {
                            slot = id;
                        }
                    }
                    if (options.trace_game && id + 1 == *options.trace_game) {
                        explicit_game = game.outcome;
                    }
                }
                totals.observe(game, a_seat);
            }
        } catch (...) {
            const std::scoped_lock lock(mutex);
            if (!failure) {
                failure = std::current_exception();
            }
            stop.store(true, std::memory_order_relaxed);
        }
    };
    const auto threads = options.games ? std::min<std::uint64_t>(options.threads, options.games) : options.threads;
    std::vector<std::jthread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (unsigned i = 0; i < threads; ++i) {
        workers.emplace_back(worker);
    }
    for (auto& thread : workers) {
        thread.join();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (!totals.games) {
        throw std::runtime_error("No matches completed");
    }
    if (logs) {
        logs->finish();
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    if (analyzing) {
        std::filesystem::create_directories(options.analysis_dir);
        auto csv = output_file(options.analysis_dir / "opening_win_rates.csv");
        table.write(csv);
        csv.close();
        const auto trace = [&](std::uint64_t id, Outcome expected, std::string_view name) {
            const auto game = play(options, a, b, id, true);
            if (game.outcome != expected) {
                throw std::logic_error("Replayed trace winner differs from the match result");
            }
            auto out = output_file(options.analysis_dir / name);
            out << game.trace;
            out.close();
        };
        if (options.trace_game) {
            if (!explicit_game) {
                throw std::runtime_error("The requested game did not finish");
            }
            trace(*options.trace_game - 1, *explicit_game, "turn_trace.json");
        } else {
            if (representatives[0]) {
                trace(*representatives[0], Outcome::FirstWins, "first_win_trace.json");
            }
            if (representatives[1]) {
                trace(*representatives[1], Outcome::SecondWins, "second_win_trace.json");
            }
        }
    }
    const double games = static_cast<double>(totals.games);
    std::cout << "games=" << totals.games << " threads=" << threads << " turns=" << totals.turns
              << " seed=" << options.seed << " simulations=" << options.search.simulations
              << " mode=" << (options.evaluate ? "evaluate" : "selfplay") << " rules=wb\n"
              << "A_first=" << totals.a_first << " A_second=" << totals.games - totals.a_first
              << " A_wins=" << totals.a_wins << " B_wins=" << totals.b_wins << " draws=" << totals.both_lose
              << " first_wins=" << totals.first_wins << " second_wins=" << totals.second_wins
              << " both_lose=" << totals.both_lose << '\n'
              << std::fixed << std::setprecision(4)
              << "wall_seconds=" << elapsed << " avg_game_seconds=" << totals.game_seconds / games
              << " wall_seconds_per_game=" << elapsed / games << " games_per_second=" << games / elapsed
              << " avg_turns=" << static_cast<double>(totals.turns) / games
              << " avg_rounds=" << static_cast<double>(totals.rounds) / games << '\n';
    if (logs) {
        std::cout << "value_samples=" << totals.turns << " policy_turns=" << totals.turns
                  << " policy_samples=" << totals.policy_samples << '\n';
    }
}

} // namespace svwb::wb
