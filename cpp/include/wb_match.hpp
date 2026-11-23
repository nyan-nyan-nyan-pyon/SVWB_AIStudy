#pragma once

#include "search.hpp"

namespace svwb::wb {

struct MatchOptions {
    std::uint64_t games;
    int seconds;
    unsigned threads;
    std::uint32_t seed;
    std::optional<std::uint32_t> opponent_seed;
    std::filesystem::path weights;
    std::filesystem::path opponent_weights;
    std::filesystem::path log_dir;
    std::filesystem::path analysis_dir;
    std::optional<std::uint64_t> trace_game;
    std::size_t trace_top;
    bool evaluate;
    bool random_opponent;
    SearchOptions search;
};

void run_match(const MatchOptions& options);

} // namespace svwb::wb
