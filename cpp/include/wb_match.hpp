#pragma once

#include "attention.hpp"
#include "search.hpp"
#include "wb_cards.hpp"

namespace svwb {

struct SnapshotOpponent {
    std::filesystem::path weights;
    unsigned basis_points = 0;

    [[nodiscard]] bool plays(std::uint64_t game) const {
        // Schedule seat pairs so both opponents get first and second under either run limit.
        constexpr std::uint64_t scale = 10'000;
        const auto offset = (game / 2 % scale) * basis_points + scale / 2;
        return (offset + basis_points) / scale != offset / scale;
    }
};

namespace wb {

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
    Deck deck_a = kMirrorDeck;
    Deck deck_b = kMirrorDeck;
    SnapshotOpponent snapshot;
    bool opponent_perfect_information = false;
    attn::ModelType model_type = attn::ModelType::Mlp;
    std::optional<attn::ModelType> opponent_model_type;
    std::optional<SearchVersion> opponent_search_version;
    bool paired_seeds = false;
    attn::ArchVariant attention_variant = attn::ArchVariant::V3GlobalEmbDiff;
    std::filesystem::path outcomes_json;
};

void run_match(const MatchOptions& options);

} // namespace wb
} // namespace svwb
