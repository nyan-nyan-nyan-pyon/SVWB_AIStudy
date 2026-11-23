#include "card_db.hpp"
#include "engine.hpp"
#include "search.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
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
    std::filesystem::path weights;
    std::filesystem::path opponent_weights;
    std::filesystem::path log_dir;
    SearchOptions search;
};

void usage(std::ostream& out) {
    out << "Usage: svwb_ai [--games N] [--seconds N] [--threads N] [--seed N]\n"
           "               [--deck-a A|B] [--deck-b A|B] [--weights FILE]\n"
           "               [--opponent-weights FILE | --opponent-seed N]\n"
           "               [--simulations N] [--no-exploration] [--log-dir DIR]\n"
           "Starts at most N games, or stops starting games after N seconds, whichever comes first.\n"
           "Games in progress finish. Seats alternate between the two competitors.\n"
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

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (flag == "--no-exploration") {
            options.search.exploration = false;
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
            options.deck_a = deck(value);
        } else if (flag == "--deck-b") {
            options.deck_b = deck(value);
        } else if (flag == "--weights") {
            options.weights = value;
        } else if (flag == "--opponent-weights") {
            options.opponent_weights = value;
        } else if (flag == "--log-dir") {
            options.log_dir = value;
        } else {
            throw std::invalid_argument("Unknown option " + std::string(flag));
        }
    }
    if (options.opponent_seed && !options.opponent_weights.empty()) {
        throw std::invalid_argument("--opponent-seed and --opponent-weights are mutually exclusive");
    }
    if (options.games == 0 && options.seconds == 0) {
        options.games = 1;
    }
    return options;
}

struct TrainingSample {
    Features features;
    float target;
};

static_assert(sizeof(TrainingSample) == (kFeatureDim + 1) * sizeof(float));

struct GameLog {
    std::vector<TrainingSample> values;
    std::vector<TrainingSample> policies;
    std::vector<std::int32_t> policy_sizes;
};

void write_word(std::ofstream& output, std::uint32_t word) {
    std::array<unsigned char, 4> bytes{};
    for (unsigned i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<unsigned char>(word >> (8 * i));
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void write_samples(std::ofstream& output, std::span<const TrainingSample> samples) {
    if constexpr (std::endian::native == std::endian::little) {
        output.write(reinterpret_cast<const char*>(samples.data()),
                     static_cast<std::streamsize>(samples.size_bytes()));
    } else {
        for (const auto& sample : samples) {
            for (const float feature : sample.features) {
                write_word(output, std::bit_cast<std::uint32_t>(feature));
            }
            write_word(output, std::bit_cast<std::uint32_t>(sample.target));
        }
    }
}

class TrainingLogs {
public:
    explicit TrainingLogs(const std::filesystem::path& directory) {
        std::filesystem::create_directories(directory);
        values_.exceptions(std::ios::badbit | std::ios::failbit);
        policies_.exceptions(std::ios::badbit | std::ios::failbit);
        values_.open(directory / "val_log.bin", std::ios::binary | std::ios::trunc);
        policies_.open(directory / "pol_log.bin", std::ios::binary | std::ios::trunc);
    }

    // The caller holds the match output lock so a whole game's two streams stay in order.
    void append(const GameLog& log) {
        write_samples(values_, log.values);
        std::span<const TrainingSample> remaining(log.policies);
        for (const auto size : log.policy_sizes) {
            write_word(policies_, static_cast<std::uint32_t>(size));
            write_samples(policies_, remaining.first(size));
            remaining = remaining.subspan(size);
        }
    }

    void finish() {
        values_.close();
        policies_.close();
    }

private:
    std::ofstream values_;
    std::ofstream policies_;
};

struct GameResult {
    GameLog log;
    int winner;
    int turns;
    double elapsed;
};

GameResult play_game(const Options& options, const Mlp& a, const Mlp& b, std::uint64_t game_id) {
    const auto started = Clock::now();
    const bool a_first = game_id % 2 == 0;
    std::seed_seq seeds{options.seed, static_cast<std::uint32_t>(game_id),
                        static_cast<std::uint32_t>(game_id >> 32)};
    std::mt19937 rng(seeds);
    auto state = new_game(a_first ? *options.deck_a : *options.deck_b,
                          a_first ? *options.deck_b : *options.deck_a, rng);
    TurnSearch search(a_first ? a : b, a_first ? b : a, rng, options.search);
    const bool logging = !options.log_dir.empty();
    GameResult result{};
    std::vector<int> seats;
    while (state.winner == kOngoing) {
        if (result.turns >= 2 * (kDeckSize - kOpeningHand + 1)) {
            throw std::logic_error("Turn limit exceeded in game " + std::to_string(game_id));
        }
        if (logging) {
            result.log.values.push_back({extract_features(state), 0.0F});
            seats.push_back(state.turn_player);
        }
        auto move = search.choose_turn(state, logging);
        if (logging) {
            result.log.policy_sizes.push_back(static_cast<std::int32_t>(move.policy.size()));
            for (const auto& sample : move.policy) {
                result.log.policies.push_back({sample.features, sample.probability});
            }
        }
        state = move.next;
        ++result.turns;
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
    std::uint64_t b_wins = 0;
    std::uint64_t draws = 0;
    double game_seconds = 0.0;
};

void run(const Options& options) {
    const auto model_a = options.weights.empty() ? Mlp::random(options.seed) : Mlp::load(options.weights);
    const auto model_b = !options.opponent_weights.empty() ? Mlp::load(options.opponent_weights)
                       : options.opponent_seed ? Mlp::random(*options.opponent_seed) : model_a;
    std::optional<TrainingLogs> logs;
    if (!options.log_dir.empty()) {
        logs.emplace(options.log_dir);
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
                const auto result = play_game(options, model_a, model_b, game_id);
                const std::scoped_lock lock(output_lock);
                if (logs) {
                    logs->append(result.log);
                }
                const auto a_seat = static_cast<int>(game_id % 2);
                ++stats.games;
                stats.turns += result.turns;
                stats.policy_samples += result.log.policies.size();
                stats.a_first += a_seat == 0;
                stats.a_wins += result.winner == a_seat;
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
    const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    const auto games = static_cast<double>(stats.games);
    std::cout << "games=" << stats.games << " threads=" << threads << " turns=" << stats.turns
              << " seed=" << options.seed << " simulations=" << options.search.simulations << '\n'
              << "A_first=" << stats.a_first << " A_second=" << stats.games - stats.a_first
              << " A_wins=" << stats.a_wins << " B_wins=" << stats.b_wins << " draws=" << stats.draws << '\n'
              << std::fixed << std::setprecision(4)
              << "wall_seconds=" << elapsed << " avg_game_seconds=" << (games > 0 ? stats.game_seconds / games : 0)
              << " wall_seconds_per_game=" << (games > 0 ? elapsed / games : 0)
              << " games_per_second=" << games / elapsed << '\n';
    if (logs) {
        std::cout << "value_samples=" << stats.turns << " policy_turns=" << stats.turns
                  << " policy_samples=" << stats.policy_samples << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        usage(std::cout);
        return 0;
    }
    try {
        run(parse_options(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "svwb_ai: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
