#include "attention.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

#if defined(__AVX2__) && defined(__FMA__) || defined(__AVX2__) && defined(_MSC_VER)
#define SVWB_ATTN_AVX2 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

namespace svwb::attn {
namespace {

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(kModel % 16 == 0 && kFfn % 16 == 0 && kValueHidden % 16 == 0 && kPolicyHidden % 16 == 0);

constexpr char kMagic[8] = {'S', 'V', 'W', 'B', 'A', 'T', 'T', 'N'};
constexpr std::uint32_t kHeaderWordsLegacy = 18;
constexpr std::uint32_t kHeaderBytesLegacy = sizeof(kMagic) + 4 * kHeaderWordsLegacy; // 80
constexpr std::uint32_t kHeaderWordsV3 = 26;
constexpr std::uint32_t kHeaderBytesV3 = sizeof(kMagic) + 4 * kHeaderWordsV3;         // 112
constexpr float kLayerNormEps = 1e-5F;

// Per-thread scratch memory. Inference never touches the heap.
struct Workspace {
    float x[kMaxTokens * kModel];
    float h[kMaxTokens * kModel];
    float qkv[kMaxTokens * 3 * kModel];
    float att[kMaxTokens * kModel];
    float ffn[kMaxTokens * kFfn];
    float scores[kMaxTokens];
    float hidden[kPolicyHidden];
};
thread_local Workspace workspace;

constexpr std::array<std::uint32_t, 256> kCrcTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) value = (value & 1U) ? 0xEDB88320U ^ (value >> 1) : value >> 1;
        table[i] = value;
    }
    return table;
}();

#if SVWB_ATTN_AVX2
bool cpu_supports_avx2() {
#if defined(_MSC_VER)
    int info[4]{};
    __cpuid(info, 0);
    if (info[0] < 7) return false;
    __cpuid(info, 1);
    const bool fma = (info[2] & (1 << 12)) != 0;
    const bool osxsave = (info[2] & (1 << 27)) != 0;
    const bool avx = (info[2] & (1 << 28)) != 0;
    if (!fma || !osxsave || !avx || (_xgetbv(0) & 6) != 6) return false;
    __cpuidex(info, 7, 0);
    return (info[1] & (1 << 5)) != 0;
#else
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif
}

inline float hsum(__m256 v) {
    const __m128 low = _mm256_castps256_ps128(v);
    const __m128 high = _mm256_extractf128_ps(v, 1);
    __m128 sum = _mm_add_ps(low, high);
    sum = _mm_add_ps(sum, _mm_movehl_ps(sum, sum));
    sum = _mm_add_ss(sum, _mm_movehdup_ps(sum));
    return _mm_cvtss_f32(sum);
}
#endif

inline float dot(const float* a, const float* b, int n) {
#if SVWB_ATTN_AVX2
    __m256 s0 = _mm256_setzero_ps();
    __m256 s1 = _mm256_setzero_ps();
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        s0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), s0);
        s1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), s1);
    }
    float sum = hsum(_mm256_add_ps(s0, s1));
    for (; i < n; ++i) sum += a[i] * b[i];
    return sum;
#else
    float sum = 0.0F;
    for (int i = 0; i < n; ++i) sum += a[i] * b[i];
    return sum;
#endif
}

// y[r] (+)= x[r] * W + bias for `rows` rows. W is input-major with row stride w_stride; out % 16 == 0.
template<bool Accumulate, bool Relu = false>
void linear(const float* x, int rows, int x_stride, int in, const float* w, int w_stride, int out,
            const float* bias, float* y, int y_stride) {
#if SVWB_ATTN_AVX2
    const __m256 zero = _mm256_setzero_ps();
    int r = 0;
    for (; r + 4 <= rows; r += 4) {
        const float* x0 = x + (r + 0) * x_stride;
        const float* x1 = x + (r + 1) * x_stride;
        const float* x2 = x + (r + 2) * x_stride;
        const float* x3 = x + (r + 3) * x_stride;
        float* y0 = y + (r + 0) * y_stride;
        float* y1 = y + (r + 1) * y_stride;
        float* y2 = y + (r + 2) * y_stride;
        float* y3 = y + (r + 3) * y_stride;
        for (int c = 0; c < out; c += 16) {
            const __m256 b0 = bias ? _mm256_loadu_ps(bias + c) : zero;
            const __m256 b1 = bias ? _mm256_loadu_ps(bias + c + 8) : zero;
            __m256 a00 = b0, a01 = b1, a10 = b0, a11 = b1, a20 = b0, a21 = b1, a30 = b0, a31 = b1;
            const float* wc = w + c;
            for (int k = 0; k < in; ++k) {
                const __m256 w0 = _mm256_loadu_ps(wc + k * w_stride);
                const __m256 w1 = _mm256_loadu_ps(wc + k * w_stride + 8);
                __m256 v = _mm256_broadcast_ss(x0 + k);
                a00 = _mm256_fmadd_ps(v, w0, a00);
                a01 = _mm256_fmadd_ps(v, w1, a01);
                v = _mm256_broadcast_ss(x1 + k);
                a10 = _mm256_fmadd_ps(v, w0, a10);
                a11 = _mm256_fmadd_ps(v, w1, a11);
                v = _mm256_broadcast_ss(x2 + k);
                a20 = _mm256_fmadd_ps(v, w0, a20);
                a21 = _mm256_fmadd_ps(v, w1, a21);
                v = _mm256_broadcast_ss(x3 + k);
                a30 = _mm256_fmadd_ps(v, w0, a30);
                a31 = _mm256_fmadd_ps(v, w1, a31);
            }
            const auto store = [&](float* dst, __m256 lo, __m256 hi) {
                if constexpr (Accumulate) {
                    lo = _mm256_add_ps(lo, _mm256_loadu_ps(dst + c));
                    hi = _mm256_add_ps(hi, _mm256_loadu_ps(dst + c + 8));
                }
                if constexpr (Relu) {
                    lo = _mm256_max_ps(lo, zero);
                    hi = _mm256_max_ps(hi, zero);
                }
                _mm256_storeu_ps(dst + c, lo);
                _mm256_storeu_ps(dst + c + 8, hi);
            };
            store(y0, a00, a01);
            store(y1, a10, a11);
            store(y2, a20, a21);
            store(y3, a30, a31);
        }
    }
    for (; r < rows; ++r) {
        const float* xr = x + r * x_stride;
        float* yr = y + r * y_stride;
        for (int c = 0; c < out; c += 32) {
            const bool wide = c + 32 <= out;
            __m256 a0 = bias ? _mm256_loadu_ps(bias + c) : zero;
            __m256 a1 = bias ? _mm256_loadu_ps(bias + c + 8) : zero;
            __m256 a2 = wide ? (bias ? _mm256_loadu_ps(bias + c + 16) : zero) : zero;
            __m256 a3 = wide ? (bias ? _mm256_loadu_ps(bias + c + 24) : zero) : zero;
            const float* wc = w + c;
            if (wide) {
                for (int k = 0; k < in; ++k) {
                    if (xr[k] == 0.0F) continue;
                    const __m256 v = _mm256_broadcast_ss(xr + k);
                    const float* row = wc + k * w_stride;
                    a0 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row), a0);
                    a1 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 8), a1);
                    a2 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 16), a2);
                    a3 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 24), a3);
                }
            } else {
                for (int k = 0; k < in; ++k) {
                    if (xr[k] == 0.0F) continue;
                    const __m256 v = _mm256_broadcast_ss(xr + k);
                    const float* row = wc + k * w_stride;
                    a0 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row), a0);
                    a1 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 8), a1);
                }
            }
            __m256 lanes[4] = {a0, a1, a2, a3};
            for (int part = 0; part < (wide ? 4 : 2); ++part) {
                __m256 value = lanes[part];
                if constexpr (Accumulate) value = _mm256_add_ps(value, _mm256_loadu_ps(yr + c + 8 * part));
                if constexpr (Relu) value = _mm256_max_ps(value, zero);
                _mm256_storeu_ps(yr + c + 8 * part, value);
            }
        }
    }
#else
    for (int r = 0; r < rows; ++r) {
        const float* xr = x + r * x_stride;
        float* yr = y + r * y_stride;
        for (int c = 0; c < out; ++c) {
            float sum = bias ? bias[c] : 0.0F;
            for (int k = 0; k < in; ++k) sum += xr[k] * w[k * w_stride + c];
            if constexpr (Accumulate) sum += yr[c];
            if constexpr (Relu) sum = std::max(0.0F, sum);
            yr[c] = sum;
        }
    }
#endif
}

void layer_norm(const float* x, int rows, const float* weight, const float* bias, float* y) {
    for (int r = 0; r < rows; ++r) {
        const float* xr = x + r * kModel;
        float* yr = y + r * kModel;
        float mean = 0.0F;
        for (int i = 0; i < kModel; ++i) mean += xr[i];
        mean /= kModel;
        float variance = 0.0F;
        for (int i = 0; i < kModel; ++i) {
            const float d = xr[i] - mean;
            variance += d * d;
        }
        variance /= kModel;
        const float scale = 1.0F / std::sqrt(variance + kLayerNormEps);
        for (int i = 0; i < kModel; ++i) yr[i] = (xr[i] - mean) * scale * weight[i] + bias[i];
    }
}

// Multi-head attention over `tokens` keys for the first `queries` rows of qkv.
void attention(const float* qkv, int tokens, int queries, float* out, float* scores) {
    constexpr int stride = 3 * kModel;
    const float scale = 1.0F / std::sqrt(static_cast<float>(kHeadDim));
    for (int q = 0; q < queries; ++q) {
        for (int head = 0; head < kHeads; ++head) {
            const float* query = qkv + q * stride + head * kHeadDim;
            float maximum = -std::numeric_limits<float>::infinity();
            for (int s = 0; s < tokens; ++s) {
                scores[s] = dot(query, qkv + s * stride + kModel + head * kHeadDim, kHeadDim) * scale;
                maximum = std::max(maximum, scores[s]);
            }
            float sum = 0.0F;
            for (int s = 0; s < tokens; ++s) {
                scores[s] = std::exp(scores[s] - maximum);
                sum += scores[s];
            }
            const float inverse = 1.0F / sum;
            float* dst = out + q * kModel + head * kHeadDim;
#if SVWB_ATTN_AVX2
            __m256 o0 = _mm256_setzero_ps(), o1 = o0, o2 = o0, o3 = o0;
            for (int s = 0; s < tokens; ++s) {
                const float* value = qkv + s * stride + 2 * kModel + head * kHeadDim;
                const __m256 p = _mm256_set1_ps(scores[s] * inverse);
                o0 = _mm256_fmadd_ps(p, _mm256_loadu_ps(value), o0);
                o1 = _mm256_fmadd_ps(p, _mm256_loadu_ps(value + 8), o1);
                o2 = _mm256_fmadd_ps(p, _mm256_loadu_ps(value + 16), o2);
                o3 = _mm256_fmadd_ps(p, _mm256_loadu_ps(value + 24), o3);
            }
            _mm256_storeu_ps(dst, o0);
            _mm256_storeu_ps(dst + 8, o1);
            _mm256_storeu_ps(dst + 16, o2);
            _mm256_storeu_ps(dst + 24, o3);
#else
            std::fill_n(dst, kHeadDim, 0.0F);
            for (int s = 0; s < tokens; ++s) {
                const float* value = qkv + s * stride + 2 * kModel + head * kHeadDim;
                const float p = scores[s] * inverse;
                for (int d = 0; d < kHeadDim; ++d) dst[d] += p * value[d];
            }
#endif
        }
    }
}

float sigmoid(float logit) {
    return 1.0F / (1.0F + std::exp(-logit));
}

void check_finite(float value) {
    if (!std::isfinite(value)) throw std::overflow_error("Attention prediction overflow");
}

void write_u32(std::ofstream& out, std::uint32_t word) {
    unsigned char bytes[4];
    for (int i = 0; i < 4; ++i) bytes[i] = static_cast<unsigned char>(word >> (8 * i));
    out.write(reinterpret_cast<const char*>(bytes), 4);
}

std::uint32_t read_u32(const unsigned char* bytes) {
    std::uint32_t word = 0;
    for (int i = 0; i < 4; ++i) word |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
    return word;
}

std::array<std::uint32_t, kHeaderWordsLegacy> legacy_header_words(std::uint32_t crc) {
    return {kFormatVersionLegacy, kHeaderBytesLegacy, kVocab, kCardEmbed, kAttrDim, kGlobalDimV2,
            kModel, kHeads, kFfn, kLayers, kCandidateDim, kValueHidden, kPolicyHidden, kMaxTokens,
            kActivationRelu, static_cast<std::uint32_t>(kParameterCountV2), crc, 0};
}

std::array<std::uint32_t, kHeaderWordsV3> v3_header_words(const ModelMetadata& meta, std::uint32_t crc) {
    return {kFormatVersionV3, kHeaderBytesV3, kVocab, kCardEmbed, kAttrDim,
            meta.global_dim, kModel, kHeads, kFfn, kLayers,
            meta.candidate_dim, kValueHidden, kPolicyHidden, kMaxTokens,
            kActivationRelu, static_cast<std::uint32_t>(meta.parameter_count()),
            crc, meta.model_version, meta.candidate_embdiff_dim, meta.token_schema_version,
            meta.card_vocab_hash, meta.rules_version, meta.search_version, meta.reward_definition, 0, 0};
}

std::vector<unsigned char> little_endian_payload(const std::vector<float>& values) {
    std::vector<unsigned char> bytes(values.size() * 4);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto word = std::bit_cast<std::uint32_t>(values[i]);
        for (int b = 0; b < 4; ++b) bytes[4 * i + b] = static_cast<unsigned char>(word >> (8 * b));
    }
    return bytes;
}

// Column mapping from Legacy v2 global[0..31] to v3 global[0..63] (excluding v2[27] which splits 0.5/0.5 to 37 & 38).
constexpr std::array<std::pair<int, int>, 31> kGlobalV2ToV3Map{{
    {0, 56}, {1, 57}, {2, 3}, {3, 4}, {4, 5}, {5, 6}, {6, 7}, {7, 9},
    {8, 10}, {9, 11}, {10, 0}, {11, 1}, {12, 2}, {13, 14}, {14, 15}, {15, 16},
    {16, 17}, {17, 18}, {18, 19}, {19, 20}, {20, 21}, {21, 22}, {22, 23},
    {23, 28}, {24, 29}, {25, 30}, {26, 32}, {28, 42}, {29, 44}, {30, 47}, {31, 49},
}};

} // namespace

ModelMetadata ModelMetadata::for_variant(ArchVariant v, std::uint32_t search_ver) {
    ModelMetadata meta{};
    meta.search_version = search_ver;
    switch (v) {
    case ArchVariant::LegacyV2:
        meta.model_version = search_ver >= 2 ? 2U : 1U;
        meta.global_dim = kGlobalDimV2;
        meta.candidate_dim = kCandidateDim;
        meta.candidate_embdiff_dim = 0;
        meta.token_schema_version = 1;
        break;
    case ArchVariant::V3Global:
        meta.model_version = 3;
        meta.global_dim = kGlobalDim;
        meta.candidate_dim = kCandidateDim;
        meta.candidate_embdiff_dim = 0;
        meta.token_schema_version = 3;
        break;
    case ArchVariant::V3GlobalEmbDiff:
        meta.model_version = 3;
        meta.global_dim = kGlobalDim;
        meta.candidate_dim = kCandidateDim;
        meta.candidate_embdiff_dim = kCandidateEmbDiffDim;
        meta.token_schema_version = 3;
        break;
    }
    return meta;
}

ArchVariant ModelMetadata::validate_and_resolve() const {
    if (candidate_dim != kCandidateDim) {
        throw std::runtime_error("Unsupported Attention candidate_dim: " + std::to_string(candidate_dim));
    }
    if (card_vocab_hash != 0 && card_vocab_hash != kCardVocabHash) {
        throw std::runtime_error("Attention card_vocab_hash mismatch");
    }
    if (global_dim == kGlobalDimV2 && candidate_embdiff_dim == 0) {
        if (token_schema_version != 1) {
            throw std::runtime_error("Attention LegacyV2 requires token_schema_version == 1");
        }
        return ArchVariant::LegacyV2;
    }
    if (global_dim == kGlobalDim && candidate_embdiff_dim == 0) {
        if (token_schema_version != 3) {
            throw std::runtime_error("Attention v3-G requires token_schema_version == 3");
        }
        return ArchVariant::V3Global;
    }
    if (global_dim == kGlobalDim && candidate_embdiff_dim == kCandidateEmbDiffDim) {
        if (token_schema_version != 3) {
            throw std::runtime_error("Attention v3-GD requires token_schema_version == 3");
        }
        return ArchVariant::V3GlobalEmbDiff;
    }
    throw std::runtime_error("Unsupported Attention dimension tuple (" + std::to_string(global_dim) + "," +
                             std::to_string(candidate_dim) + "," + std::to_string(candidate_embdiff_dim) + ")");
}

std::size_t ModelMetadata::parameter_count() const {
    return compute_parameter_count(static_cast<int>(global_dim), static_cast<int>(candidate_dim),
                                   static_cast<int>(candidate_embdiff_dim));
}

std::uint32_t crc32(const void* data, std::size_t bytes, std::uint32_t crc) {
    const auto* p = static_cast<const unsigned char*>(data);
    crc = ~crc;
    for (std::size_t i = 0; i < bytes; ++i) crc = kCrcTable[(crc ^ p[i]) & 0xFFU] ^ (crc >> 8);
    return ~crc;
}

bool is_attention_weights_file(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    char magic[sizeof(kMagic)]{};
    return stream.read(magic, sizeof(magic)) && std::memcmp(magic, kMagic, sizeof(kMagic)) == 0;
}

AttentionModel AttentionModel::from_parameters(std::vector<float> ordered, ModelMetadata meta) {
    const auto variant = meta.validate_and_resolve();
    const auto expected_params = meta.parameter_count();
    if (ordered.size() != expected_params) {
        throw std::invalid_argument("Attention model parameter count mismatch: expected " +
                                    std::to_string(expected_params) + ", got " + std::to_string(ordered.size()));
    }
    if (!std::all_of(ordered.begin(), ordered.end(), [](float v) { return std::isfinite(v); })) {
        throw std::runtime_error("Attention weights contain a non-finite float");
    }
#if SVWB_ATTN_AVX2
    if (!cpu_supports_avx2()) throw std::runtime_error("The attention model build requires AVX2 and FMA");
#endif
    AttentionModel model;
    model.meta_ = meta;
    model.variant_ = variant;
    model.ordered_ = std::move(ordered);
    model.prepare();
    return model;
}

void AttentionModel::prepare() {
    const int g_dim = static_cast<int>(meta_.global_dim);
    const int c_dim = static_cast<int>(meta_.candidate_dim);
    const int d_dim = static_cast<int>(meta_.candidate_embdiff_dim);
    const int pol_in = kModel + c_dim + d_dim;

    const float* src = ordered_.data();
    const auto take = [&](std::size_t count) {
        const float* start = src;
        src += count;
        return start;
    };
    const float* embedding = take(static_cast<std::size_t>(kVocab) * kCardEmbed);
    const float* token_w = take(static_cast<std::size_t>(kModel) * kTokenInput);
    const float* token_b = take(kModel);
    const float* global_w = take(static_cast<std::size_t>(kModel) * g_dim);
    const float* global_b = take(kModel);
    const float* cls = take(kModel);

    packed_.clear();
    const auto reserve = [&](std::size_t count) {
        const auto offset = packed_.size();
        packed_.resize(offset + count);
        return offset;
    };
    const auto transpose = [&](const float* w, int out, int row_width, int first, int in) {
        const auto offset = reserve(static_cast<std::size_t>(out) * in);
        for (int o = 0; o < out; ++o) {
            for (int i = 0; i < in; ++i) {
                packed_[offset + static_cast<std::size_t>(i) * out + o] = w[o * row_width + first + i];
            }
        }
        return offset;
    };
    const auto copy = [&](const float* values, std::size_t count) {
        const auto offset = reserve(count);
        std::copy_n(values, count, packed_.begin() + static_cast<std::ptrdiff_t>(offset));
        return offset;
    };

    offsets_.token_table = reserve(static_cast<std::size_t>(kVocab) * kModel);
    for (int v = 0; v < kVocab; ++v) {
        for (int o = 0; o < kModel; ++o) {
            double sum = token_b[o];
            // Note: slot vocab == 0 is skipped during token gathering, and for v > 0 uses embedding[v].
            if (v > 0) {
                for (int i = 0; i < kCardEmbed; ++i) {
                    sum += static_cast<double>(token_w[o * kTokenInput + i]) * embedding[v * kCardEmbed + i];
                }
            } else {
                for (int i = 0; i < kCardEmbed; ++i) {
                    sum += static_cast<double>(token_w[o * kTokenInput + i]) * embedding[i];
                }
            }
            packed_[offsets_.token_table + static_cast<std::size_t>(v) * kModel + o] = static_cast<float>(sum);
        }
    }
    offsets_.attr_weight = transpose(token_w, kModel, kTokenInput, kCardEmbed, kAttrDim);
    offsets_.global_weight = transpose(global_w, kModel, g_dim, 0, g_dim);
    offsets_.global_bias = reserve(kModel);
    for (int o = 0; o < kModel; ++o) packed_[offsets_.global_bias + o] = global_b[o] + cls[o];

    for (auto& layer : offsets_.layers) {
        layer.norm1_w = copy(take(kModel), kModel);
        layer.norm1_b = copy(take(kModel), kModel);
        layer.qkv_w = transpose(take(3 * kModel * kModel), 3 * kModel, kModel, 0, kModel);
        layer.qkv_b = copy(take(3 * kModel), 3 * kModel);
        layer.out_w = transpose(take(kModel * kModel), kModel, kModel, 0, kModel);
        layer.out_b = copy(take(kModel), kModel);
        layer.norm2_w = copy(take(kModel), kModel);
        layer.norm2_b = copy(take(kModel), kModel);
        layer.ffn1_w = transpose(take(kFfn * kModel), kFfn, kModel, 0, kModel);
        layer.ffn1_b = copy(take(kFfn), kFfn);
        layer.ffn2_w = transpose(take(kModel * kFfn), kModel, kFfn, 0, kFfn);
        layer.ffn2_b = copy(take(kModel), kModel);
    }
    offsets_.final_w = copy(take(kModel), kModel);
    offsets_.final_b = copy(take(kModel), kModel);
    offsets_.value1_w = transpose(take(kValueHidden * kModel), kValueHidden, kModel, 0, kModel);
    offsets_.value1_b = copy(take(kValueHidden), kValueHidden);
    offsets_.value2_w = copy(take(kValueHidden), kValueHidden);
    offsets_.value2_b = copy(take(1), 1);

    const float* policy1_w = take(static_cast<std::size_t>(kPolicyHidden) * pol_in);
    offsets_.policy_cls_w = transpose(policy1_w, kPolicyHidden, pol_in, 0, kModel);
    offsets_.policy_candidate_w = transpose(policy1_w, kPolicyHidden, pol_in, kModel, c_dim);
    if (d_dim > 0) {
        offsets_.policy_embdiff_w = transpose(policy1_w, kPolicyHidden, pol_in, kModel + c_dim, d_dim);
        // Precompute projected_emb[5][600][128] (~1.46 MiB); row v == 0 is explicitly zero-masked.
        offsets_.projected_emb = reserve(static_cast<std::size_t>(kDiffZones) * kVocab * kPolicyHidden);
        for (int z = 0; z < kDiffZones; ++z) {
            const int col_base = kModel + c_dim + z * kCardEmbed;
            float* zone_dst = packed_.data() + offsets_.projected_emb +
                              static_cast<std::size_t>(z) * kVocab * kPolicyHidden;
            std::fill_n(zone_dst, kPolicyHidden, 0.0F); // vocab_id == 0 -> all zeros
            for (int v = 1; v < kVocab; ++v) {
                float* row_dst = zone_dst + static_cast<std::size_t>(v) * kPolicyHidden;
                const float* emb_row = embedding + static_cast<std::size_t>(v) * kCardEmbed;
                for (int o = 0; o < kPolicyHidden; ++o) {
                    const float* w_row = policy1_w + static_cast<std::size_t>(o) * pol_in + col_base;
                    double sum = 0.0;
                    for (int i = 0; i < kCardEmbed; ++i) {
                        sum += static_cast<double>(w_row[i]) * emb_row[i];
                    }
                    row_dst[o] = static_cast<float>(sum);
                }
            }
        }
    } else {
        offsets_.policy_embdiff_w = 0;
        offsets_.projected_emb = 0;
    }
    offsets_.policy1_b = copy(take(kPolicyHidden), kPolicyHidden);
    offsets_.policy2_w = copy(take(kPolicyHidden), kPolicyHidden);
    offsets_.policy2_b = copy(take(1), 1);
    if (src != ordered_.data() + ordered_.size()) throw std::logic_error("Attention parameter layout mismatch");
}

AttentionModel AttentionModel::load(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open attention weights: " + file.string());
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (bytes.size() < kHeaderBytesLegacy) {
        throw std::runtime_error("attn_weights.bin is truncated: " + file.string());
    }
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        throw std::runtime_error("attn_weights.bin has a wrong magic number: " + file.string());
    }
    const auto format_ver = read_u32(bytes.data() + sizeof(kMagic));
    const auto hdr_bytes = read_u32(bytes.data() + sizeof(kMagic) + 4);

    if (format_ver == kFormatVersionLegacy) {
        if (hdr_bytes != kHeaderBytesLegacy) {
            throw std::runtime_error("attn_weights.bin v1 header size mismatch: " + file.string());
        }
        const std::size_t expected = kHeaderBytesLegacy + 4 * kParameterCountV2;
        if (bytes.size() != expected) {
            throw std::runtime_error("attn_weights.bin must contain exactly " + std::to_string(expected) +
                                     " bytes: " + file.string());
        }
        const auto* payload = bytes.data() + kHeaderBytesLegacy;
        const auto crc = crc32(payload, 4 * kParameterCountV2);
        const auto expected_words = legacy_header_words(crc);
        for (std::uint32_t i = 0; i < kHeaderWordsLegacy; ++i) {
            const auto word = read_u32(bytes.data() + sizeof(kMagic) + 4 * i);
            if (word != expected_words[i]) {
                if (i == 16) throw std::runtime_error("attn_weights.bin checksum mismatch: " + file.string());
                throw std::runtime_error("attn_weights.bin architecture field " + std::to_string(i) +
                                         " mismatch: " + file.string());
            }
        }
        std::vector<float> ordered(kParameterCountV2);
        for (std::size_t i = 0; i < kParameterCountV2; ++i) {
            ordered[i] = std::bit_cast<float>(read_u32(payload + 4 * i));
        }
        ModelMetadata meta = ModelMetadata::for_variant(ArchVariant::LegacyV2, 1);
        meta.reward_definition = 0;
        return from_parameters(std::move(ordered), meta);
    }

    if (format_ver == kFormatVersionV3) {
        if (hdr_bytes != kHeaderBytesV3 || bytes.size() < kHeaderBytesV3) {
            throw std::runtime_error("attn_weights.bin v3 header size mismatch: " + file.string());
        }
        std::array<std::uint32_t, kHeaderWordsV3> w{};
        for (std::uint32_t i = 0; i < kHeaderWordsV3; ++i) {
            w[i] = read_u32(bytes.data() + sizeof(kMagic) + 4 * i);
        }
        if (w[2] != kVocab || w[3] != kCardEmbed || w[4] != kAttrDim || w[6] != kModel || w[7] != kHeads ||
            w[8] != kFfn || w[9] != kLayers || w[11] != kValueHidden || w[12] != kPolicyHidden ||
            w[13] != kMaxTokens || w[14] != kActivationRelu) {
            throw std::runtime_error("attn_weights.bin architecture field mismatch: " + file.string());
        }
        ModelMetadata meta{};
        meta.global_dim = w[5];
        meta.candidate_dim = w[10];
        meta.model_version = w[17];
        meta.candidate_embdiff_dim = w[18];
        meta.token_schema_version = w[19];
        meta.card_vocab_hash = w[20];
        meta.rules_version = w[21];
        meta.search_version = w[22];
        meta.reward_definition = w[23];
        static_cast<void>(meta.validate_and_resolve());
        const auto param_count = meta.parameter_count();
        if (w[15] != param_count || bytes.size() != kHeaderBytesV3 + 4 * param_count) {
            throw std::runtime_error("attn_weights.bin parameter count or file size mismatch: " + file.string());
        }
        const auto* payload = bytes.data() + kHeaderBytesV3;
        const auto crc = crc32(payload, 4 * param_count);
        if (w[16] != crc) {
            throw std::runtime_error("attn_weights.bin checksum mismatch: " + file.string());
        }
        std::vector<float> ordered(param_count);
        for (std::size_t i = 0; i < param_count; ++i) {
            ordered[i] = std::bit_cast<float>(read_u32(payload + 4 * i));
        }
        return from_parameters(std::move(ordered), meta);
    }

    throw std::runtime_error("Unsupported attn_weights.bin version: " + std::to_string(format_ver));
}

void AttentionModel::save(const std::filesystem::path& file, bool legacy_format) const {
    const auto payload = little_endian_payload(ordered_);
    const auto checksum = crc32(payload.data(), payload.size());
    std::ofstream out;
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.open(file, std::ios::binary | std::ios::trunc);
    out.write(kMagic, sizeof(kMagic));
    if (legacy_format) {
        if (variant_ != ArchVariant::LegacyV2) {
            throw std::invalid_argument("Legacy 80-byte format can only save LegacyV2 models");
        }
        for (const auto word : legacy_header_words(checksum)) write_u32(out, word);
    } else {
        for (const auto word : v3_header_words(meta_, checksum)) write_u32(out, word);
    }
    out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
}

AttentionModel AttentionModel::random(std::uint32_t seed, ArchVariant variant) {
    const auto meta = ModelMetadata::for_variant(variant, 2);
    const int g_dim = static_cast<int>(meta.global_dim);
    const int pol_in = kModel + static_cast<int>(meta.candidate_dim) + static_cast<int>(meta.candidate_embdiff_dim);
    const auto param_count = meta.parameter_count();

    std::mt19937 rng(seed);
    std::vector<float> ordered;
    ordered.reserve(param_count);
    const auto uniform = [&](std::size_t count, float limit) {
        std::uniform_real_distribution<float> dist(-limit, limit);
        for (std::size_t i = 0; i < count; ++i) ordered.push_back(dist(rng));
    };
    const auto constant = [&](std::size_t count, float value) { ordered.insert(ordered.end(), count, value); };
    const auto linear_layer = [&](int in, int out) {
        const float limit = 1.0F / std::sqrt(static_cast<float>(in));
        uniform(static_cast<std::size_t>(in) * out, limit);
        uniform(out, limit);
    };
    uniform(static_cast<std::size_t>(kVocab) * kCardEmbed, 1.0F);
    // Keep embedding row 0 explicitly zero.
    std::fill_n(ordered.begin(), kCardEmbed, 0.0F);
    linear_layer(kTokenInput, kModel);
    linear_layer(g_dim, kModel);
    uniform(kModel, 0.1F);
    for (int layer = 0; layer < kLayers; ++layer) {
        constant(kModel, 1.0F);
        constant(kModel, 0.0F);
        linear_layer(kModel, 3 * kModel);
        linear_layer(kModel, kModel);
        constant(kModel, 1.0F);
        constant(kModel, 0.0F);
        linear_layer(kModel, kFfn);
        linear_layer(kFfn, kModel);
    }
    constant(kModel, 1.0F);
    constant(kModel, 0.0F);
    linear_layer(kModel, kValueHidden);
    linear_layer(kValueHidden, 1);
    linear_layer(pol_in, kPolicyHidden);
    linear_layer(kPolicyHidden, 1);
    return from_parameters(std::move(ordered), meta);
}

AttentionModel AttentionModel::migrate_v2_to_v3(const AttentionModel& v2_model, ArchVariant target) {
    if (v2_model.variant() != ArchVariant::LegacyV2) {
        throw std::invalid_argument("migrate_v2_to_v3 requires a LegacyV2 source model");
    }
    if (v2_model.metadata().card_vocab_hash != 0 && v2_model.metadata().card_vocab_hash != kCardVocabHash) {
        throw std::runtime_error("Cannot migrate v2 weights with mismatched card_vocab_hash");
    }
    const auto dst_meta = ModelMetadata::for_variant(target, 2);
    const int dst_g_dim = static_cast<int>(dst_meta.global_dim);
    const int dst_pol_in = kModel + static_cast<int>(dst_meta.candidate_dim) +
                           static_cast<int>(dst_meta.candidate_embdiff_dim);

    const float* src = v2_model.ordered_.data();
    const auto take = [&](std::size_t count) {
        const float* start = src;
        src += count;
        return start;
    };

    std::vector<float> dst;
    dst.reserve(dst_meta.parameter_count());
    const auto append = [&](const float* p, std::size_t count) { dst.insert(dst.end(), p, p + count); };

    // 1. Card Embedding (600 x 32): copy all, keep row 0 explicitly 0.0f.
    const float* emb = take(static_cast<std::size_t>(kVocab) * kCardEmbed);
    append(emb, static_cast<std::size_t>(kVocab) * kCardEmbed);
    std::fill_n(dst.begin(), kCardEmbed, 0.0F);

    // 2. Token Projection (64 -> 128 + bias): copy all.
    append(take(static_cast<std::size_t>(kModel) * kTokenInput + kModel),
           static_cast<std::size_t>(kModel) * kTokenInput + kModel);

    // 3. Global Projection (32 -> 64 or 32 -> 32 for B40) + bias:
    const float* gw_v2 = take(static_cast<std::size_t>(kModel) * kGlobalDimV2);
    const float* gb_v2 = take(kModel);
    if (dst_g_dim == kGlobalDimV2) {
        append(gw_v2, static_cast<std::size_t>(kModel) * kGlobalDimV2);
    } else {
        std::vector<float> gw_v3(static_cast<std::size_t>(kModel) * kGlobalDim, 0.0F);
        for (int o = 0; o < kModel; ++o) {
            const float* src_row = gw_v2 + static_cast<std::size_t>(o) * kGlobalDimV2;
            float* dst_row = gw_v3.data() + static_cast<std::size_t>(o) * kGlobalDim;
            for (const auto [s_col, d_col] : kGlobalV2ToV3Map) {
                dst_row[d_col] = src_row[s_col];
            }
            // v2[27] = (opp.ep + opp.sep) / 4.0f -> 0.5f * v3[37] (opp.ep / 2) + 0.5f * v3[38] (opp.sep / 2).
            dst_row[37] = 0.5F * src_row[27];
            dst_row[38] = 0.5F * src_row[27];
        }
        append(gw_v3.data(), gw_v3.size());
    }
    append(gb_v2, kModel);

    // 4. CLS + Transformer (2 layers) + Final LayerNorm + Value Head: copy all verbatim.
    const std::size_t trunk_and_value =
        kModel + kLayers * kLayerParameters + 2 * kModel +
        (static_cast<std::size_t>(kModel) * kValueHidden + kValueHidden) + (kValueHidden + 1);
    append(take(trunk_and_value), trunk_and_value);

    // 5. Policy Head (192 -> dst_pol_in -> 128 -> 1):
    const float* pw1_v2 = take(static_cast<std::size_t>(kPolicyHidden) * kPolicyInputV2);
    const float* pb1_v2 = take(kPolicyHidden);
    const float* pw2_v2 = take(kPolicyHidden);
    const float* pb2_v2 = take(1);
    if (dst_pol_in == kPolicyInputV2) {
        append(pw1_v2, static_cast<std::size_t>(kPolicyHidden) * kPolicyInputV2);
    } else {
        std::vector<float> pw1_v3(static_cast<std::size_t>(kPolicyHidden) * dst_pol_in, 0.0F);
        for (int o = 0; o < kPolicyHidden; ++o) {
            std::copy_n(pw1_v2 + static_cast<std::size_t>(o) * kPolicyInputV2, kPolicyInputV2,
                        pw1_v3.data() + static_cast<std::size_t>(o) * dst_pol_in);
        }
        append(pw1_v3.data(), pw1_v3.size());
    }
    append(pb1_v2, kPolicyHidden);
    append(pw2_v2, kPolicyHidden);
    append(pb2_v2, 1);

    if (src != v2_model.ordered_.data() + v2_model.ordered_.size()) {
        throw std::logic_error("v2 migration source layout mismatch");
    }
    return from_parameters(std::move(dst), dst_meta);
}

float AttentionModel::encode(const Tokens& tokens, float* cls, DebugStageOutputs* debug) const {
    auto& ws = workspace;
    const float* p = packed_.data();
    const int g_dim = static_cast<int>(meta_.global_dim);

    if (debug) {
        debug->token_proj.fill(0.0F);
    }

    int count = 0;
    std::array<std::uint16_t, kCardTokens> vocab{};
    std::array<int, kCardTokens> orig_slots{};
    for (int slot = 0; slot < kCardTokens; ++slot) {
        const auto id = tokens.vocab[slot];
        if (id == 0) continue;
        if (id < 0 || id >= kVocab) throw std::out_of_range("Attention token vocabulary index out of range");
        vocab[count] = static_cast<std::uint16_t>(id);
        orig_slots[count] = slot;
        std::copy_n(tokens.attrs.data() + slot * kAttrDim, kAttrDim, ws.att + count * kAttrDim);
        ++count;
    }
    const int total = count + 1;
    float* x = ws.x;
    linear<false>(tokens.global.data(), 1, g_dim, g_dim, p + offsets_.global_weight, kModel, kModel,
                  p + offsets_.global_bias, x, kModel);
    for (int t = 0; t < count; ++t) {
        std::copy_n(p + offsets_.token_table + static_cast<std::size_t>(vocab[t]) * kModel, kModel,
                    x + (t + 1) * kModel);
    }
    linear<true>(ws.att, count, kAttrDim, kAttrDim, p + offsets_.attr_weight, kModel, kModel, nullptr,
                 x + kModel, kModel);

    if (debug) {
        std::copy_n(x, kModel, debug->global_cls_init.data());
        for (int t = 0; t < count; ++t) {
            std::copy_n(x + (t + 1) * kModel, kModel,
                        debug->token_proj.data() + static_cast<std::size_t>(orig_slots[t]) * kModel);
        }
    }

    for (int l = 0; l < kLayers; ++l) {
        const auto& layer = offsets_.layers[l];
        const bool last = l + 1 == kLayers;
        const int queries = last ? 1 : total;
        layer_norm(x, total, p + layer.norm1_w, p + layer.norm1_b, ws.h);
        linear<false>(ws.h, queries, kModel, kModel, p + layer.qkv_w, 3 * kModel, kModel, p + layer.qkv_b,
                      ws.qkv, 3 * kModel);
        linear<false>(ws.h, total, kModel, kModel, p + layer.qkv_w + kModel, 3 * kModel, 2 * kModel,
                      p + layer.qkv_b + kModel, ws.qkv + kModel, 3 * kModel);
        attention(ws.qkv, total, queries, ws.att, ws.scores);
        linear<true>(ws.att, queries, kModel, kModel, p + layer.out_w, kModel, kModel, p + layer.out_b, x, kModel);
        layer_norm(x, queries, p + layer.norm2_w, p + layer.norm2_b, ws.h);
        linear<false, true>(ws.h, queries, kModel, kModel, p + layer.ffn1_w, kFfn, kFfn, p + layer.ffn1_b, ws.ffn, kFfn);
        linear<true>(ws.ffn, queries, kFfn, kFfn, p + layer.ffn2_w, kModel, kModel, p + layer.ffn2_b, x, kModel);
    }
    layer_norm(x, 1, p + offsets_.final_w, p + offsets_.final_b, cls);
    linear<false, true>(cls, 1, kModel, kModel, p + offsets_.value1_w, kValueHidden, kValueHidden, p + offsets_.value1_b,
                        ws.hidden, kValueHidden);
    const float logit = dot(ws.hidden, p + offsets_.value2_w, kValueHidden) + p[offsets_.value2_b];
    check_finite(logit);
    if (debug) {
        std::copy_n(cls, kModel, debug->final_cls.data());
        debug->value_logit = logit;
        debug->value_prob = sigmoid(logit);
    }
    return logit;
}

float AttentionModel::value_logit(const Tokens& tokens) const {
    alignas(32) float cls[kModel];
    return encode(tokens, cls);
}

float AttentionModel::value(const Tokens& tokens) const {
    return sigmoid(value_logit(tokens));
}

float AttentionModel::evaluate_root(const Tokens& tokens, RootContext& context) const {
    const float logit = encode(tokens, context.cls.data());
    const float* p = packed_.data();
    linear<false>(context.cls.data(), 1, kModel, kModel, p + offsets_.policy_cls_w, kPolicyHidden, kPolicyHidden,
                  p + offsets_.policy1_b, context.policy_base.data(), kPolicyHidden);
    return sigmoid(logit);
}

CandidateEmbDiff AttentionModel::materialize_embdiff(const CandidateSparseDiff& diff) const {
    CandidateEmbDiff out{};
    const float* embedding = ordered_.data(); // First tensor in ordered_ is card_embed[600][32].
    for (int z = 0; z < kDiffZones; ++z) {
        float* dst = out.data() + z * kCardEmbed;
        const int begin = diff.zone_offsets[z];
        const int end = diff.zone_offsets[z + 1];
        assert(begin <= end && end <= kMaxSparseDiffEntries);
#if SVWB_ATTN_AVX2
        __m256 d0 = _mm256_setzero_ps();
        __m256 d1 = _mm256_setzero_ps();
        __m256 d2 = _mm256_setzero_ps();
        __m256 d3 = _mm256_setzero_ps();
        for (int idx = begin; idx < end; ++idx) {
            const auto entry = diff.entries[idx];
            if (entry.vocab_id == 0 || entry.signed_count == 0) continue; // Explicit vocab_id == 0 zero mask
            if (entry.vocab_id >= kVocab) throw std::out_of_range("SparseDiff vocab_id out of range");
            const float* emb = embedding + static_cast<std::size_t>(entry.vocab_id) * kCardEmbed;
            const __m256 s = _mm256_set1_ps(static_cast<float>(entry.signed_count));
            d0 = _mm256_fmadd_ps(s, _mm256_loadu_ps(emb), d0);
            d1 = _mm256_fmadd_ps(s, _mm256_loadu_ps(emb + 8), d1);
            d2 = _mm256_fmadd_ps(s, _mm256_loadu_ps(emb + 16), d2);
            d3 = _mm256_fmadd_ps(s, _mm256_loadu_ps(emb + 24), d3);
        }
        _mm256_storeu_ps(dst, d0);
        _mm256_storeu_ps(dst + 8, d1);
        _mm256_storeu_ps(dst + 16, d2);
        _mm256_storeu_ps(dst + 24, d3);
#else
        for (int idx = begin; idx < end; ++idx) {
            const auto entry = diff.entries[idx];
            if (entry.vocab_id == 0 || entry.signed_count == 0) continue;
            if (entry.vocab_id >= kVocab) throw std::out_of_range("SparseDiff vocab_id out of range");
            const float* emb = embedding + static_cast<std::size_t>(entry.vocab_id) * kCardEmbed;
            const float scale = static_cast<float>(entry.signed_count);
            for (int d = 0; d < kCardEmbed; ++d) dst[d] += scale * emb[d];
        }
#endif
    }
    return out;
}

float AttentionModel::policy_logit_direct(const RootContext& context, const float* candidate,
                                          const CandidateSparseDiff* diff) const {
    const float* p = packed_.data();
    const bool use_diff = meta_.candidate_embdiff_dim > 0 && diff != nullptr && diff->total_entries() > 0;
    CandidateEmbDiff dense_diff{};
    if (use_diff) {
        dense_diff = materialize_embdiff(*diff);
    }
#if SVWB_ATTN_AVX2
    const float* base = context.policy_base.data();
    const float* w_cand = p + offsets_.policy_candidate_w;
    const float* w_diff = p + offsets_.policy_embdiff_w;
    const float* w2 = p + offsets_.policy2_w;
    const __m256 zero = _mm256_setzero_ps();
    __m256 acc0 = zero, acc1 = zero;
    for (int c = 0; c < kPolicyHidden; c += 64) {
        __m256 a0 = _mm256_load_ps(base + c);
        __m256 a1 = _mm256_load_ps(base + c + 8);
        __m256 a2 = _mm256_load_ps(base + c + 16);
        __m256 a3 = _mm256_load_ps(base + c + 24);
        __m256 a4 = _mm256_load_ps(base + c + 32);
        __m256 a5 = _mm256_load_ps(base + c + 40);
        __m256 a6 = _mm256_load_ps(base + c + 48);
        __m256 a7 = _mm256_load_ps(base + c + 56);
        const float* wc = w_cand + c;
        for (int k = 0; k < kCandidateDim; ++k) {
            const float value = candidate[k];
            if (value == 0.0F) continue;
            const __m256 v = _mm256_set1_ps(value);
            const float* row = wc + k * kPolicyHidden;
            a0 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row), a0);
            a1 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 8), a1);
            a2 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 16), a2);
            a3 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 24), a3);
            a4 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 32), a4);
            a5 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 40), a5);
            a6 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 48), a6);
            a7 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 56), a7);
        }
        if (use_diff) {
            const float* wd = w_diff + c;
            for (int k = 0; k < kCandidateEmbDiffDim; ++k) {
                const float value = dense_diff[k];
                if (value == 0.0F) continue;
                const __m256 v = _mm256_set1_ps(value);
                const float* row = wd + k * kPolicyHidden;
                a0 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row), a0);
                a1 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 8), a1);
                a2 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 16), a2);
                a3 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 24), a3);
                a4 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 32), a4);
                a5 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 40), a5);
                a6 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 48), a6);
                a7 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 56), a7);
            }
        }
        acc0 = _mm256_fmadd_ps(_mm256_max_ps(a0, zero), _mm256_loadu_ps(w2 + c), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_max_ps(a1, zero), _mm256_loadu_ps(w2 + c + 8), acc1);
        acc0 = _mm256_fmadd_ps(_mm256_max_ps(a2, zero), _mm256_loadu_ps(w2 + c + 16), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_max_ps(a3, zero), _mm256_loadu_ps(w2 + c + 24), acc1);
        acc0 = _mm256_fmadd_ps(_mm256_max_ps(a4, zero), _mm256_loadu_ps(w2 + c + 32), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_max_ps(a5, zero), _mm256_loadu_ps(w2 + c + 40), acc1);
        acc0 = _mm256_fmadd_ps(_mm256_max_ps(a6, zero), _mm256_loadu_ps(w2 + c + 48), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_max_ps(a7, zero), _mm256_loadu_ps(w2 + c + 56), acc1);
    }
    const float logit = hsum(_mm256_add_ps(acc0, acc1)) + p[offsets_.policy2_b];
#else
    auto& ws = workspace;
    std::copy(context.policy_base.begin(), context.policy_base.end(), ws.hidden);
    linear<true>(candidate, 1, kCandidateDim, kCandidateDim, p + offsets_.policy_candidate_w, kPolicyHidden,
                 kPolicyHidden, nullptr, ws.hidden, kPolicyHidden);
    if (use_diff) {
        linear<true>(dense_diff.data(), 1, kCandidateEmbDiffDim, kCandidateEmbDiffDim, p + offsets_.policy_embdiff_w,
                     kPolicyHidden, kPolicyHidden, nullptr, ws.hidden, kPolicyHidden);
    }
    for (int i = 0; i < kPolicyHidden; ++i) ws.hidden[i] = std::max(0.0F, ws.hidden[i]);
    const float logit = dot(ws.hidden, p + offsets_.policy2_w, kPolicyHidden) + p[offsets_.policy2_b];
#endif
    check_finite(logit);
    return logit;
}

float AttentionModel::policy_logit_preprojected(const RootContext& context, const float* candidate,
                                                const CandidateSparseDiff* diff) const {
    const float* p = packed_.data();
    const bool use_diff = meta_.candidate_embdiff_dim > 0 && diff != nullptr && diff->total_entries() > 0;
#if SVWB_ATTN_AVX2
    const float* base = context.policy_base.data();
    const float* w_cand = p + offsets_.policy_candidate_w;
    const float* proj_base = p + offsets_.projected_emb;
    const float* w2 = p + offsets_.policy2_w;
    const __m256 zero = _mm256_setzero_ps();
    __m256 acc0 = zero, acc1 = zero;
    for (int c = 0; c < kPolicyHidden; c += 64) {
        __m256 a0 = _mm256_load_ps(base + c);
        __m256 a1 = _mm256_load_ps(base + c + 8);
        __m256 a2 = _mm256_load_ps(base + c + 16);
        __m256 a3 = _mm256_load_ps(base + c + 24);
        __m256 a4 = _mm256_load_ps(base + c + 32);
        __m256 a5 = _mm256_load_ps(base + c + 40);
        __m256 a6 = _mm256_load_ps(base + c + 48);
        __m256 a7 = _mm256_load_ps(base + c + 56);
        const float* wc = w_cand + c;
        for (int k = 0; k < kCandidateDim; ++k) {
            const float value = candidate[k];
            if (value == 0.0F) continue;
            const __m256 v = _mm256_set1_ps(value);
            const float* row = wc + k * kPolicyHidden;
            a0 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row), a0);
            a1 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 8), a1);
            a2 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 16), a2);
            a3 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 24), a3);
            a4 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 32), a4);
            a5 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 40), a5);
            a6 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 48), a6);
            a7 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 56), a7);
        }
        if (use_diff) {
            for (int z = 0; z < kDiffZones; ++z) {
                const float* zone_tbl = proj_base + static_cast<std::size_t>(z) * kVocab * kPolicyHidden + c;
                const int begin = diff->zone_offsets[z];
                const int end = diff->zone_offsets[z + 1];
                for (int idx = begin; idx < end; ++idx) {
                    const auto entry = diff->entries[idx];
                    if (entry.vocab_id == 0 || entry.signed_count == 0) continue;
                    const float* row = zone_tbl + static_cast<std::size_t>(entry.vocab_id) * kPolicyHidden;
                    if (entry.signed_count == 1) {
                        a0 = _mm256_add_ps(a0, _mm256_loadu_ps(row));
                        a1 = _mm256_add_ps(a1, _mm256_loadu_ps(row + 8));
                        a2 = _mm256_add_ps(a2, _mm256_loadu_ps(row + 16));
                        a3 = _mm256_add_ps(a3, _mm256_loadu_ps(row + 24));
                        a4 = _mm256_add_ps(a4, _mm256_loadu_ps(row + 32));
                        a5 = _mm256_add_ps(a5, _mm256_loadu_ps(row + 40));
                        a6 = _mm256_add_ps(a6, _mm256_loadu_ps(row + 48));
                        a7 = _mm256_add_ps(a7, _mm256_loadu_ps(row + 56));
                    } else if (entry.signed_count == -1) {
                        a0 = _mm256_sub_ps(a0, _mm256_loadu_ps(row));
                        a1 = _mm256_sub_ps(a1, _mm256_loadu_ps(row + 8));
                        a2 = _mm256_sub_ps(a2, _mm256_loadu_ps(row + 16));
                        a3 = _mm256_sub_ps(a3, _mm256_loadu_ps(row + 24));
                        a4 = _mm256_sub_ps(a4, _mm256_loadu_ps(row + 32));
                        a5 = _mm256_sub_ps(a5, _mm256_loadu_ps(row + 40));
                        a6 = _mm256_sub_ps(a6, _mm256_loadu_ps(row + 48));
                        a7 = _mm256_sub_ps(a7, _mm256_loadu_ps(row + 56));
                    } else {
                        const __m256 v = _mm256_set1_ps(static_cast<float>(entry.signed_count));
                        a0 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row), a0);
                        a1 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 8), a1);
                        a2 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 16), a2);
                        a3 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 24), a3);
                        a4 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 32), a4);
                        a5 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 40), a5);
                        a6 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 48), a6);
                        a7 = _mm256_fmadd_ps(v, _mm256_loadu_ps(row + 56), a7);
                    }
                }
            }
        }
        acc0 = _mm256_fmadd_ps(_mm256_max_ps(a0, zero), _mm256_loadu_ps(w2 + c), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_max_ps(a1, zero), _mm256_loadu_ps(w2 + c + 8), acc1);
        acc0 = _mm256_fmadd_ps(_mm256_max_ps(a2, zero), _mm256_loadu_ps(w2 + c + 16), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_max_ps(a3, zero), _mm256_loadu_ps(w2 + c + 24), acc1);
        acc0 = _mm256_fmadd_ps(_mm256_max_ps(a4, zero), _mm256_loadu_ps(w2 + c + 32), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_max_ps(a5, zero), _mm256_loadu_ps(w2 + c + 40), acc1);
        acc0 = _mm256_fmadd_ps(_mm256_max_ps(a6, zero), _mm256_loadu_ps(w2 + c + 48), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_max_ps(a7, zero), _mm256_loadu_ps(w2 + c + 56), acc1);
    }
    const float logit = hsum(_mm256_add_ps(acc0, acc1)) + p[offsets_.policy2_b];
    check_finite(logit);
    return logit;
#else
    return policy_logit_direct(context, candidate, diff);
#endif
}

float AttentionModel::policy_logit(const RootContext& context, const float* candidate,
                                   const CandidateSparseDiff* diff) const {
    return kernel_ == PolicyScorerKernel::PreprojectedTable
               ? policy_logit_preprojected(context, candidate, diff)
               : policy_logit_direct(context, candidate, diff);
}

void AttentionModel::debug_encode(const Tokens& tokens, DebugStageOutputs& out) const {
    alignas(32) float cls[kModel];
    static_cast<void>(encode(tokens, cls, &out));
}

void AttentionModel::debug_policy(const RootContext& context, const float* candidate, const CandidateSparseDiff* diff,
                                  CandidateEmbDiff& dense_diff, std::array<float, kPolicyHidden>& h1_relu,
                                  float& out_logit) const {
    const float* p = packed_.data();
    if (meta_.candidate_embdiff_dim > 0 && diff != nullptr) {
        dense_diff = materialize_embdiff(*diff);
    } else {
        dense_diff.fill(0.0F);
    }
    std::copy(context.policy_base.begin(), context.policy_base.end(), h1_relu.begin());
    linear<true>(candidate, 1, kCandidateDim, kCandidateDim, p + offsets_.policy_candidate_w, kPolicyHidden,
                 kPolicyHidden, nullptr, h1_relu.data(), kPolicyHidden);
    if (meta_.candidate_embdiff_dim > 0 && diff != nullptr && diff->total_entries() > 0) {
        linear<true>(dense_diff.data(), 1, kCandidateEmbDiffDim, kCandidateEmbDiffDim, p + offsets_.policy_embdiff_w,
                     kPolicyHidden, kPolicyHidden, nullptr, h1_relu.data(), kPolicyHidden);
    }
    for (int i = 0; i < kPolicyHidden; ++i) h1_relu[i] = std::max(0.0F, h1_relu[i]);
    out_logit = policy_logit(context, candidate, diff);
}

} // namespace svwb::attn
