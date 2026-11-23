#pragma once

#include "mlp.hpp"

#include <bit>
#include <fstream>
#include <span>
#include <vector>

namespace svwb {

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

inline void write_word(std::ofstream& output, std::uint32_t word) {
    std::array<unsigned char, 4> bytes{};
    for (unsigned i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<unsigned char>(word >> (8 * i));
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

inline void write_samples(std::ofstream& output, std::span<const TrainingSample> samples) {
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

} // namespace svwb
