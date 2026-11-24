#include "analysis.hpp"
#include "card_db.hpp"
#include "engine.hpp"
#include "search.hpp"
#include "training_logs.hpp"
#include "wb_match.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using namespace svwb;
using Clock = std::chrono::steady_clock;

struct Options {
    std::uint64_t games = 0;
    int seconds = 0;
    unsigned threads = 1;
    std::uint32_t seed = 2026;
    std::optional<std::uint32_t> opponent_seed;
    const Deck* deck_a = &kDeckA;
    const Deck* deck_b = &kDeckB;
    std::filesystem::path deck_a_file;
    std::filesystem::path deck_b_file;
    std::filesystem::path weights;
    std::filesystem::path opponent_weights;
    SnapshotOpponent snapshot;
    std::filesystem::path log_dir;
    std::filesystem::path analysis_dir;
    std::optional<std::uint64_t> trace_game;
    std::optional<std::size_t> trace_top;
    bool wb_rules = false;
    bool evaluate = false;
    bool random_opponent = false;
    bool opponent_perfect_information = false;
    attn::ModelType model_type = attn::ModelType::Mlp;
    std::optional<attn::ModelType> opponent_model_type;
    SearchOptions search;
};

void usage(std::ostream& out) {
    out << "Usage: svwb_ai [--games N] [--seconds N] [--threads N] [--seed N]\n"
           "               [--deck-a A|B|JSON] [--deck-b A|B|JSON] [--weights FILE]\n"
           "               [--opponent-weights FILE | --opponent-seed N]\n"
           "               [--snapshot-weights FILE --snapshot-bps 1..10000]\n"
           "               [--simulations N] [--no-exploration] [--log-dir DIR]\n"
           "               [--evaluate [--random-opponent]]\n"
           "               [--perfect-information | --opponent-perfect-information] (WB)\n"
           "               [--rules legacy|wb] [--model-type mlp|attention]\n"
           "               [--opponent-model-type mlp|attention]\n"
           "               [--analysis-dir DIR [--trace-game N] [--trace-top N]]\n"
           "Evaluation uses greedy selection and no search noise. --random-opponent\n"
           "samples uniformly from all legal actions, including end turn.\n"
           "Starts at most N games, or stops starting games after N seconds, whichever comes first.\n"
           "Games in progress finish. Seats alternate between the two competitors.\n"
           "--rules wb defaults to the fixed 40-card mirror. Deck JSON selects WB rules.\n"
           "Analysis writes opening_win_rates.csv and JSON traces (top 5 by default).\n"
           "Without a game or time limit, runs one game. Without weights, uses seeded random weights.\n";
}

template<class T>
T number(std::string_view value, std::string_view flag, T minimum,
         T maximum = std::numeric_limits<T>::max()) {
    T result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result < minimum || result > maximum) {
        throw std::invalid_argument("Invalid value for " + std::string(flag) + ": " + std::string(value));
    }
    return result;
}

const Deck* deck(std::string_view value) {
    if (value == "A") {
        return &kDeckA;
    }
    if (value == "B") {
        return &kDeckB;
    }
    throw std::invalid_argument("Deck must be A or B");
}

std::filesystem::path utf8_path(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

Options parse_options(int argc, char** argv) {
    Options options;
    bool explicit_rules = false;
    bool explicit_model_type = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (flag == "--no-exploration") {
            options.search.exploration = false;
            continue;
        }
        if (flag == "--evaluate") {
            options.evaluate = true;
            options.search.exploration = false;
            continue;
        }
        if (flag == "--random-opponent") {
            options.random_opponent = true;
            continue;
        }
        if (flag == "--perfect-information" || flag == "--opponent-perfect-information") {
            if (flag == "--perfect-information") options.search.determinization = false;
            else options.opponent_perfect_information = true;
            continue;
        }
        if (i + 1 == argc || std::string_view(argv[i + 1]).starts_with("--")) {
            throw std::invalid_argument("Missing value for " + std::string(flag));
        }
        const std::string_view value(argv[++i]);
        if (flag == "--games") {
            options.games = number<std::uint64_t>(value, flag, 1);
        } else if (flag == "--seconds") {
            options.seconds = number(value, flag, 1, 86'400 * 365);
        } else if (flag == "--threads") {
            options.threads = number<unsigned>(value, flag, 1, 256);
        } else if (flag == "--seed") {
            options.seed = number<std::uint32_t>(value, flag, 0);
        } else if (flag == "--opponent-seed") {
            options.opponent_seed = number<std::uint32_t>(value, flag, 0);
        } else if (flag == "--simulations") {
            options.search.simulations = number(value, flag, 1, 100'000);
        } else if (flag == "--deck-a") {
            if (value == "A" || value == "B") options.deck_a = deck(value);
            else options.deck_a_file = utf8_path(value);
        } else if (flag == "--deck-b") {
            if (value == "A" || value == "B") options.deck_b = deck(value);
            else options.deck_b_file = utf8_path(value);
        } else if (flag == "--weights") {
            options.weights = utf8_path(value);
        } else if (flag == "--opponent-weights") {
            options.opponent_weights = utf8_path(value);
        } else if (flag == "--snapshot-weights") {
            options.snapshot.weights = utf8_path(value);
        } else if (flag == "--snapshot-bps") {
            options.snapshot.basis_points = number(value, flag, 1U, 10'000U);
        } else if (flag == "--log-dir") {
            options.log_dir = utf8_path(value);
        } else if (flag == "--analysis-dir") {
            options.analysis_dir = utf8_path(value);
        } else if (flag == "--trace-game") {
            options.trace_game = number<std::uint64_t>(value, flag, 1);
        } else if (flag == "--trace-top") {
            options.trace_top = number<std::size_t>(value, flag, 1, kMaxCandidates);
        } else if (flag == "--rules") {
            if (value != "legacy" && value != "wb") {
                throw std::invalid_argument("--rules must be legacy or wb");
            }
            explicit_rules = true;
            options.wb_rules = value == "wb";
        } else if (flag == "--model-type") {
            if (value != "mlp" && value != "attention") {
                throw std::invalid_argument("--model-type must be mlp or attention");
            }
            explicit_model_type = true;
            options.model_type = value == "attention" ? attn::ModelType::Attention : attn::ModelType::Mlp;
        } else if (flag == "--opponent-model-type") {
            if (value != "mlp" && value != "attention") {
                throw std::invalid_argument("--opponent-model-type must be mlp or attention");
            }
            options.opponent_model_type = value == "attention" ? attn::ModelType::Attention : attn::ModelType::Mlp;
        } else {
            throw std::invalid_argument("Unknown option " + std::string(flag));
        }
    }
    if (!explicit_model_type && !options.weights.empty() && attn::is_attention_weights_file(options.weights)) {
        options.model_type = attn::ModelType::Attention;
    }
    if (!options.opponent_model_type && !options.opponent_weights.empty()) {
        options.opponent_model_type = attn::is_attention_weights_file(options.opponent_weights)
            ? attn::ModelType::Attention : attn::ModelType::Mlp;
    }
    const auto opp_type = options.opponent_model_type.value_or(options.model_type);
    if (options.model_type == attn::ModelType::Attention || opp_type == attn::ModelType::Attention) {
        if (explicit_rules && !options.wb_rules) {
            throw std::invalid_argument("Attention models require WB rules");
        }
        options.wb_rules = true;
    }
    if (!options.log_dir.empty() && opp_type != options.model_type) {
        throw std::invalid_argument("Training logs require both competitors to use the same model type");
    }
    if (!options.deck_a_file.empty() || !options.deck_b_file.empty()) {
        if (explicit_rules && !options.wb_rules) throw std::invalid_argument("Deck JSON requires WB rules");
        options.wb_rules = true;
    }
    if (options.opponent_seed && !options.opponent_weights.empty()) {
        throw std::invalid_argument("--opponent-seed and --opponent-weights are mutually exclusive");
    }
    if ((!options.search.determinization || options.opponent_perfect_information) && !options.wb_rules) {
        throw std::invalid_argument("Search information options require WB rules");
    }
    if (options.opponent_perfect_information &&
        (!options.evaluate || options.random_opponent || !options.search.determinization)) {
        throw std::invalid_argument("--opponent-perfect-information requires evaluation against a model");
    }
    if (options.snapshot.weights.empty() != (options.snapshot.basis_points == 0) ||
        (options.snapshot.basis_points && (options.evaluate || options.opponent_seed || !options.opponent_weights.empty()))) {
        throw std::invalid_argument("Use --snapshot-weights and --snapshot-bps together for self-play with one current model");
    }
    if (options.random_opponent && (!options.evaluate || options.opponent_seed ||
                                    !options.opponent_weights.empty() || !options.log_dir.empty())) {
        throw std::invalid_argument("Use --random-opponent only with --evaluate, without opponent weights or training logs");
    }
    if (options.games == 0 && options.seconds == 0) {
        options.games = 1;
    }
    if (options.analysis_dir.empty() && (options.trace_game || options.trace_top)) {
        throw std::invalid_argument("--trace-game and --trace-top require --analysis-dir");
    }
    if (options.games != 0 && options.trace_game.value_or(1) > options.games) {
        throw std::invalid_argument("--trace-game exceeds --games");
    }
    return options;
}

struct GameResult {
    GameLog log;
    OpeningHands opening;
    std::string trace;
    int winner;
    int turns;
    double elapsed;
};

void play_random_turn(GameState& state, std::mt19937& rng, std::vector<Action>* record) {
    const auto actor = state.turn_player;
    int actions_taken = 0;
    while (state.winner == kOngoing && state.turn_player == actor) {
        if (++actions_taken > 2 * kBeamSteps) {
            throw std::logic_error("Random turn exceeded the action limit");
        }
        const auto actions = legal_actions(state);
        const auto selected = std::uniform_int_distribution<std::size_t>(0, actions.size() - 1)(rng);
        const auto action = actions[selected];
        if (record) {
            record->push_back(action);
        }
        apply_legal_action(state, action);
    }
}

GameResult play_game(const Options& options, const Mlp& a, const Mlp& b, std::uint64_t game_id,
                     const std::array<char, kPlayers>& deck_labels) {
    const auto started = Clock::now();
    const bool a_first = game_id % 2 == 0;
    const int a_seat = a_first ? 0 : 1;
    std::seed_seq seeds{options.seed, static_cast<std::uint32_t>(game_id),
                        static_cast<std::uint32_t>(game_id >> 32)};
    std::mt19937 rng(seeds);
    auto state = new_game(a_first ? *options.deck_a : *options.deck_b,
                          a_first ? *options.deck_b : *options.deck_a, rng);
    const bool logging = !options.log_dir.empty();
    GameResult result{};
    std::optional<GameTrace> trace;
    if (!options.analysis_dir.empty()) {
        result.opening = opening_hands(state);
        if (game_id == options.trace_game.value_or(1) - 1) {
            trace.emplace(game_id + 1, options.seed, a_seat, deck_labels, result.opening);
        }
    }
    auto search_options = options.search;
    search_options.record_actions = trace.has_value();
    TurnSearch search(a_first ? a : b, a_first ? b : a, rng, search_options);
    std::vector<int> seats;
    while (state.winner == kOngoing) {
        if (result.turns >= 2 * (kDeckSize - kOpeningHand + 1)) {
            throw std::logic_error("Turn limit exceeded in game " + std::to_string(game_id));
        }
        if (options.random_opponent && state.turn_player != a_seat) {
            if (trace) {
                const auto before = state;
                std::vector<Action> actions;
                play_random_turn(state, rng, &actions);
                trace->random_turn(before, actions, state);
            } else {
                play_random_turn(state, rng, nullptr);
            }
            ++result.turns;
            continue;
        }
        if (logging) {
            result.log.values.push_back({extract_features(state), 0.0F});
            seats.push_back(state.turn_player);
        }
        auto move = search.choose_turn(state, logging);
        if (trace) {
            trace->search_turn(state, move, options.trace_top.value_or(5));
        }
        if (logging) {
            result.log.policy_sizes.push_back(static_cast<std::int32_t>(move.policy.size()));
            for (const auto& sample : move.policy) {
                result.log.policies.push_back({sample.features, sample.probability});
            }
        }
        state = move.next;
        ++result.turns;
    }
    if (trace) {
        result.trace = trace->finish(state.winner);
    }
    for (std::size_t i = 0; i < seats.size(); ++i) {
        result.log.values[i].target = state.winner == kDraw ? 0.5F : static_cast<float>(state.winner == seats[i]);
    }
    result.winner = state.winner;
    result.elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    return result;
}

struct MatchStats {
    std::uint64_t games = 0;
    std::uint64_t turns = 0;
    std::uint64_t policy_samples = 0;
    std::uint64_t a_first = 0;
    std::uint64_t a_wins = 0;
    std::array<std::uint64_t, 2> a_seat_wins{};
    std::array<std::uint64_t, 2> seat_wins{};
    std::uint64_t b_wins = 0;
    std::uint64_t draws = 0;
    std::uint64_t snapshot_games = 0;
    double game_seconds = 0.0;
};

void run(const Options& options) {
    const auto model_a = options.weights.empty() ? Mlp::random(options.seed) : Mlp::load(options.weights);
    const auto model_b = !options.opponent_weights.empty() ? Mlp::load(options.opponent_weights)
                       : options.opponent_seed ? Mlp::random(*options.opponent_seed) : model_a;
    std::optional<Mlp> snapshot;
    if (options.snapshot.basis_points) snapshot = Mlp::load(options.snapshot.weights);
    std::optional<TrainingLogs> logs;
    if (!options.log_dir.empty()) {
        logs.emplace(options.log_dir);
    }
    const std::array deck_labels{options.deck_a == &kDeckA ? 'A' : 'B',
                                 options.deck_b == &kDeckA ? 'A' : 'B'};
    std::optional<OpeningStats> openings;
    std::string traced_game;
    if (!options.analysis_dir.empty()) {
        openings.emplace(*options.deck_a, *options.deck_b);
    }

    MatchStats stats;
    std::mutex output_lock;
    std::exception_ptr failure;
    std::atomic<bool> stop = false;
    std::atomic<std::uint64_t> next_game = 0;
    const auto started = Clock::now();
    const auto deadline = options.seconds > 0 ? started + std::chrono::seconds(options.seconds)
                                             : Clock::time_point::max();
    const auto worker = [&](std::stop_token token) {
        try {
            while (!token.stop_requested() && !stop.load(std::memory_order_relaxed) && Clock::now() < deadline) {
                const auto game_id = next_game.fetch_add(1, std::memory_order_relaxed);
                if (options.games != 0 && game_id >= options.games) {
                    break;
                }
                const bool against_snapshot = snapshot && options.snapshot.plays(game_id);
                auto result = play_game(options, model_a, against_snapshot ? *snapshot : model_b, game_id, deck_labels);
                const std::scoped_lock lock(output_lock);
                if (logs) {
                    logs->append(result.log);
                }
                const auto a_seat = static_cast<int>(game_id % 2);
                if (openings) {
                    openings->observe(result.opening, a_seat, result.winner);
                    if (!result.trace.empty()) {
                        traced_game = std::move(result.trace);
                    }
                }
                ++stats.games;
                stats.snapshot_games += against_snapshot;
                stats.turns += result.turns;
                stats.policy_samples += result.log.policies.size();
                stats.a_first += a_seat == 0;
                stats.a_wins += result.winner == a_seat;
                stats.a_seat_wins[a_seat] += result.winner == a_seat;
                stats.seat_wins[0] += result.winner == 0;
                stats.seat_wins[1] += result.winner == 1;
                stats.b_wins += result.winner == 1 - a_seat;
                stats.draws += result.winner == kDraw;
                stats.game_seconds += result.elapsed;
            }
        } catch (...) {
            const std::scoped_lock lock(output_lock);
            if (!failure) {
                failure = std::current_exception();
            }
            stop.store(true, std::memory_order_relaxed);
        }
    };

    const auto threads = options.games == 0 ? options.threads
        : static_cast<unsigned>(std::min<std::uint64_t>(options.threads, options.games));
    std::vector<std::jthread> workers;
    workers.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
        workers.emplace_back(worker);
    }
    for (auto& thread : workers) {
        thread.join();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (logs) {
        logs->finish();
    }
    if (openings) {
        if (traced_game.empty()) {
            throw std::runtime_error("The requested trace game did not finish within this run");
        }
        std::filesystem::create_directories(options.analysis_dir);
        std::ofstream trace;
        trace.exceptions(std::ios::badbit | std::ios::failbit);
        trace.open(options.analysis_dir / "turn_trace.json", std::ios::binary | std::ios::trunc);
        trace << traced_game;
        trace.close();
        std::ofstream csv;
        csv.exceptions(std::ios::badbit | std::ios::failbit);
        csv.open(options.analysis_dir / "opening_win_rates.csv", std::ios::binary | std::ios::trunc);
        openings->write_csv(csv, deck_labels);
        csv.close();
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    const auto games = static_cast<double>(stats.games);
    std::cout << "games=" << stats.games << " threads=" << threads << " turns=" << stats.turns
              << " seed=" << options.seed << " simulations=" << options.search.simulations
              << " mode=" << (options.evaluate ? "evaluate" : "selfplay") << '\n'
              << "A_first=" << stats.a_first << " A_second=" << stats.games - stats.a_first
              << " A_wins=" << stats.a_wins << " B_wins=" << stats.b_wins << " draws=" << stats.draws << '\n'
              << "A_first_wins=" << stats.a_seat_wins[0] << " A_second_wins=" << stats.a_seat_wins[1]
              << " first_wins=" << stats.seat_wins[0] << " second_wins=" << stats.seat_wins[1] << '\n'
              << std::fixed << std::setprecision(4)
              << "wall_seconds=" << elapsed << " avg_game_seconds=" << (games > 0 ? stats.game_seconds / games : 0)
              << " wall_seconds_per_game=" << (games > 0 ? elapsed / games : 0)
              << " games_per_second=" << games / elapsed << '\n';
    if (logs) {
        std::cout << "value_samples=" << stats.turns << " policy_turns=" << stats.turns
                  << " policy_samples=" << stats.policy_samples << '\n';
    }
    if (snapshot) std::cout << "snapshot_games=" << stats.snapshot_games << '\n';
}

} // namespace

int run_application(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        usage(std::cout);
        return 0;
    }
    try {
        const auto options = parse_options(argc, argv);
        if (options.wb_rules) {
            svwb::wb::run_match({options.games, options.seconds, options.threads, options.seed,
                options.opponent_seed, options.weights, options.opponent_weights, options.log_dir,
                options.analysis_dir, options.trace_game, options.trace_top.value_or(5),
                options.evaluate, options.random_opponent, options.search,
                options.deck_a_file.empty() ? svwb::wb::kMirrorDeck : svwb::wb::read_deck(options.deck_a_file),
                options.deck_b_file.empty() ? svwb::wb::kMirrorDeck : svwb::wb::read_deck(options.deck_b_file),
                options.snapshot, options.opponent_perfect_information, options.model_type,
                options.opponent_model_type});
        } else {
            run(options);
        }
    } catch (const std::exception& error) {
        std::cerr << "svwb_ai: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    for (int i = 0; i < argc; ++i) {
        const auto utf8 = std::filesystem::path(argv[i]).u8string();
        arguments.emplace_back(utf8.begin(), utf8.end());
    }
    std::vector<char*> pointers;
    for (auto& argument : arguments) pointers.push_back(argument.data());
    return run_application(argc, pointers.data());
}
#else
int main(int argc, char** argv) {
    return run_application(argc, argv);
}
#endif
