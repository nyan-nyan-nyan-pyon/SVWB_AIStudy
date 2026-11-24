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
constexpr std::uint32_t kHeaderWords = 18;
constexpr std::uint32_t kHeaderBytes = sizeof(kMagic) + 4 * kHeaderWords;
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

std::array<std::uint32_t, kHeaderWords> header_words(std::uint32_t crc) {
    return {kFormatVersion, kHeaderBytes, kVocab, kCardEmbed, kAttrDim, kGlobalDim, kModel, kHeads, kFfn, kLayers,
            kCandidateDim, kValueHidden, kPolicyHidden, kMaxTokens, kActivationRelu,
            static_cast<std::uint32_t>(kParameterCount), crc, 0};
}

std::vector<unsigned char> little_endian_payload(const std::vector<float>& values) {
    std::vector<unsigned char> bytes(values.size() * 4);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto word = std::bit_cast<std::uint32_t>(values[i]);
        for (int b = 0; b < 4; ++b) bytes[4 * i + b] = static_cast<unsigned char>(word >> (8 * b));
    }
    return bytes;
}

} // namespace

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

AttentionModel AttentionModel::from_parameters(std::vector<float> ordered) {
    if (ordered.size() != kParameterCount) throw std::invalid_argument("Attention model needs 461826 parameters");
    if (!std::all_of(ordered.begin(), ordered.end(), [](float v) { return std::isfinite(v); })) {
        throw std::runtime_error("Attention weights contain a non-finite float");
    }
#if SVWB_ATTN_AVX2
    if (!cpu_supports_avx2()) throw std::runtime_error("The attention model build requires AVX2 and FMA");
#endif
    AttentionModel model;
    model.ordered_ = std::move(ordered);
    model.prepare();
    return model;
}

void AttentionModel::prepare() {
    const float* src = ordered_.data();
    const auto take = [&](std::size_t count) {
        const float* start = src;
        src += count;
        return start;
    };
    const float* embedding = take(static_cast<std::size_t>(kVocab) * kCardEmbed);
    const float* token_w = take(kModel * kTokenInput);
    const float* token_b = take(kModel);
    const float* global_w = take(kModel * kGlobalDim);
    const float* global_b = take(kModel);
    const float* cls = take(kModel);

    packed_.clear();
    const auto reserve = [&](std::size_t count) {
        const auto offset = packed_.size();
        packed_.resize(offset + count);
        return offset;
    };
    // Transpose an output-major [out][in] PyTorch matrix (columns [first, first+in) of a wider row) to input-major.
    const auto transpose = [&](const float* w, int out, int row_width, int first, int in) {
        const auto offset = reserve(static_cast<std::size_t>(out) * in);
        for (int o = 0; o < out; ++o) {
            for (int i = 0; i < in; ++i) packed_[offset + static_cast<std::size_t>(i) * out + o] = w[o * row_width + first + i];
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
            for (int i = 0; i < kCardEmbed; ++i) {
                sum += static_cast<double>(token_w[o * kTokenInput + i]) * embedding[v * kCardEmbed + i];
            }
            packed_[offsets_.token_table + static_cast<std::size_t>(v) * kModel + o] = static_cast<float>(sum);
        }
    }
    offsets_.attr_weight = transpose(token_w, kModel, kTokenInput, kCardEmbed, kAttrDim);
    offsets_.global_weight = transpose(global_w, kModel, kGlobalDim, 0, kGlobalDim);
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
    const float* policy1_w = take(kPolicyHidden * kPolicyInput);
    offsets_.policy_cls_w = transpose(policy1_w, kPolicyHidden, kPolicyInput, 0, kModel);
    offsets_.policy_candidate_w = transpose(policy1_w, kPolicyHidden, kPolicyInput, kModel, kCandidateDim);
    offsets_.policy1_b = copy(take(kPolicyHidden), kPolicyHidden);
    offsets_.policy2_w = copy(take(kPolicyHidden), kPolicyHidden);
    offsets_.policy2_b = copy(take(1), 1);
    if (src != ordered_.data() + ordered_.size()) throw std::logic_error("Attention parameter layout mismatch");
}

AttentionModel AttentionModel::load(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open attention weights: " + file.string());
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const std::size_t expected = kHeaderBytes + 4 * kParameterCount;
    if (bytes.size() != expected) {
        throw std::runtime_error("attn_weights.bin must contain exactly " + std::to_string(expected) + " bytes: " + file.string());
    }
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        throw std::runtime_error("attn_weights.bin has a wrong magic number: " + file.string());
    }
    const auto* payload = bytes.data() + kHeaderBytes;
    const auto crc = crc32(payload, 4 * kParameterCount);
    const auto expected_words = header_words(crc);
    for (std::uint32_t i = 0; i < kHeaderWords; ++i) {
        const auto word = read_u32(bytes.data() + sizeof(kMagic) + 4 * i);
        if (word != expected_words[i]) {
            if (i == 0) throw std::runtime_error("Unsupported attn_weights.bin version: " + std::to_string(word));
            if (i == 16) throw std::runtime_error("attn_weights.bin checksum mismatch: " + file.string());
            throw std::runtime_error("attn_weights.bin architecture field " + std::to_string(i) + " mismatch: " + file.string());
        }
    }
    std::vector<float> ordered(kParameterCount);
    for (std::size_t i = 0; i < kParameterCount; ++i) ordered[i] = std::bit_cast<float>(read_u32(payload + 4 * i));
    return from_parameters(std::move(ordered));
}

void AttentionModel::save(const std::filesystem::path& file) const {
    const auto payload = little_endian_payload(ordered_);
    std::ofstream out;
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.open(file, std::ios::binary | std::ios::trunc);
    out.write(kMagic, sizeof(kMagic));
    for (const auto word : header_words(crc32(payload.data(), payload.size()))) write_u32(out, word);
    out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
}

AttentionModel AttentionModel::random(std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<float> ordered;
    ordered.reserve(kParameterCount);
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
    linear_layer(kTokenInput, kModel);
    linear_layer(kGlobalDim, kModel);
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
    linear_layer(kPolicyInput, kPolicyHidden);
    linear_layer(kPolicyHidden, 1);
    return from_parameters(std::move(ordered));
}

float AttentionModel::encode(const Tokens& tokens, float* cls) const {
    auto& ws = workspace;
    const float* p = packed_.data();
    // Gather the occupied slots; empty slots are excluded exactly like PyTorch's key padding mask.
    int count = 0;
    std::array<std::uint16_t, kCardTokens> vocab{};
    for (int slot = 0; slot < kCardTokens; ++slot) {
        const auto id = tokens.vocab[slot];
        if (id == 0) continue;
        if (id < 0 || id >= kVocab) throw std::out_of_range("Attention token vocabulary index out of range");
        vocab[count] = static_cast<std::uint16_t>(id);
        std::copy_n(tokens.attrs.data() + slot * kAttrDim, kAttrDim, ws.att + count * kAttrDim);
        ++count;
    }
    const int total = count + 1;
    float* x = ws.x;
    linear<false>(tokens.global.data(), 1, kGlobalDim, kGlobalDim, p + offsets_.global_weight, kModel, kModel,
                  p + offsets_.global_bias, x, kModel);
    for (int t = 0; t < count; ++t) {
        std::copy_n(p + offsets_.token_table + static_cast<std::size_t>(vocab[t]) * kModel, kModel, x + (t + 1) * kModel);
    }
    linear<true>(ws.att, count, kAttrDim, kAttrDim, p + offsets_.attr_weight, kModel, kModel, nullptr, x + kModel, kModel);

    for (int l = 0; l < kLayers; ++l) {
        const auto& layer = offsets_.layers[l];
        const bool last = l + 1 == kLayers;
        const int queries = last ? 1 : total; // Only the CLS row is read after the last layer.
        layer_norm(x, total, p + layer.norm1_w, p + layer.norm1_b, ws.h);
        // Queries for the rows we keep, keys and values for every token.
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

float AttentionModel::policy_logit(const RootContext& context, const float* candidate) const {
    const float* p = packed_.data();
#if SVWB_ATTN_AVX2
    const float* base = context.policy_base.data();
    const float* w1 = p + offsets_.policy_candidate_w;
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
        const float* wc = w1 + c;
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
    linear<true, true>(candidate, 1, kCandidateDim, kCandidateDim, p + offsets_.policy_candidate_w, kPolicyHidden,
                       kPolicyHidden, nullptr, ws.hidden, kPolicyHidden);
    const float logit = dot(ws.hidden, p + offsets_.policy2_w, kPolicyHidden) + p[offsets_.policy2_b];
#endif
    check_finite(logit);
    return logit;
}

} // namespace svwb::attn
