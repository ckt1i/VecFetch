#include "vdb/simd/ip_exrabitq.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#if defined(VDB_USE_AVX512) || defined(VDB_USE_AVX2)
#include <immintrin.h>
#endif

namespace vdb {
namespace simd {

namespace {

VDB_FORCE_INLINE float IPExRaBitQReference(const float* VDB_RESTRICT query,
                                           const uint8_t* VDB_RESTRICT code_abs,
                                           const uint8_t* VDB_RESTRICT sign, bool sign_packed,
                                           Dim dim) {
    float sum = 0.0f;
    for (uint32_t i = 0; i < dim; ++i) {
        const bool positive = sign_packed ? ((sign[i / 8] >> (i % 8)) & 1u) != 0 : sign[i] != 0;
        const float signed_q = positive ? query[i] : -query[i];
        sum += signed_q * (static_cast<float>(code_abs[i]) + 0.5f);
    }
    return sum;
}

	VDB_FORCE_INLINE float IPOfficialRaBitQExDataReference(const float* VDB_RESTRICT query,
	                                                       const uint8_t* VDB_RESTRICT ex_code,
	                                                       Dim dim) {
	    float sum = 0.0f;
	    for (uint32_t i = 0; i < dim; ++i) {
	        sum += query[i] * static_cast<float>(ex_code[i]);
	    }
	    return sum;
	}

	constexpr uint32_t kOfficialDirect3DimBlock = 64;
	constexpr uint32_t kOfficialDirect3BytesPerLane = 24;

	VDB_FORCE_INLINE float IPOfficialDirectBitplanesReference(
	    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact, uint8_t bits,
	    Dim dim) {
	    if (bits == 0 || bits > 3) {
	        return 0.0f;
	    }
	    float sum = 0.0f;
	    const uint32_t bytes_per_lane = ExRaBitQPackedMagnitudeBytes(kOfficialDirect3DimBlock, bits);
	    uint32_t d = 0;
	    uint32_t chunk = 0;
	    while (d < dim) {
	        const uint8_t* planes = compact + static_cast<size_t>(chunk) * bytes_per_lane;
	        const uint32_t remaining = std::min<uint32_t>(kOfficialDirect3DimBlock, dim - d);
	        for (uint32_t i = 0; i < remaining; ++i) {
	            uint8_t v = 0;
	            for (uint8_t bit = 0; bit < bits; ++bit) {
	                const uint8_t plane_bit = static_cast<uint8_t>(
	                    (planes[static_cast<uint32_t>(bit) * 8u + i / 8u] >> (i % 8u)) & 0x01u);
	                v = static_cast<uint8_t>(v | (plane_bit << bit));
	            }
	            sum += query[d + i] * static_cast<float>(v);
	        }
	        d += remaining;
	        ++chunk;
	    }
	    return sum;
	}

	VDB_FORCE_INLINE float IPOfficialDirect3TwoPlusOneReference(
	    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact, Dim dim) {
	    float sum = 0.0f;
	    uint32_t d = 0;
	    uint32_t chunk = 0;
	    while (d < dim) {
	        const uint8_t* low2 = compact + static_cast<size_t>(chunk) * kOfficialDirect3BytesPerLane;
	        const uint8_t* high1 = low2 + 16;
	        const uint32_t remaining = std::min<uint32_t>(kOfficialDirect3DimBlock, dim - d);
	        for (uint32_t i = 0; i < remaining; ++i) {
	            const uint8_t low = static_cast<uint8_t>((low2[i / 4u] >> ((i % 4u) * 2u)) & 0x03u);
	            const uint8_t high =
	                static_cast<uint8_t>((high1[i / 8u] >> (i % 8u)) & 0x01u);
	            sum += query[d + i] * static_cast<float>(low | (high << 2u));
	        }
	        d += remaining;
	        ++chunk;
	    }
	    return sum;
	}

	VDB_FORCE_INLINE float IPOfficialDirect3BitplanesReference(
	    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact, Dim dim) {
	    return IPOfficialDirectBitplanesReference(query, compact, 3, dim);
	}

	VDB_FORCE_INLINE float IPOfficialDirect3Reference(const float* VDB_RESTRICT query,
	                                                  const uint8_t* VDB_RESTRICT compact,
	                                                  RaBitQExDataLayout layout, Dim dim) {
	    const RaBitQExDataLayout effective = RaBitQResolveSelectedExDataLayout(layout);
	    if (effective == RaBitQExDataLayout::kSplit3Bitplanes) {
	        return IPOfficialDirect3BitplanesReference(query, compact, dim);
	    }
	    return IPOfficialDirect3TwoPlusOneReference(query, compact, dim);
	}

	#if defined(VDB_USE_AVX512)
VDB_FORCE_INLINE uint64_t LoadPackedSignChunk64(const uint8_t* VDB_RESTRICT packed_sign,
                                                uint32_t bit_offset, Dim dim) {
    const uint32_t total_bytes = (dim + 7) / 8;
    const uint32_t byte_idx = bit_offset / 8;
    uint64_t bits = 0;
    if (byte_idx < total_bytes) {
        const uint32_t remaining = total_bytes - byte_idx;
        const uint32_t copy_bytes = remaining >= 8 ? 8 : remaining;
        std::memcpy(&bits, packed_sign + byte_idx, copy_bytes);
    }
    return bits;
}

VDB_FORCE_INLINE __m512 FlipQuery16PackedChunkAvx512(__m512 query_block, uint64_t sign_chunk,
                                                     uint32_t chunk_lane_idx) {
    const uint32_t shift = chunk_lane_idx * 16u;
    const __mmask16 pos_mask = static_cast<__mmask16>((sign_chunk >> shift) & 0xFFFFu);
    const __mmask16 neg_mask = static_cast<__mmask16>((~pos_mask) & 0xFFFFu);
    const __m512i sign_bit = _mm512_set1_epi32(static_cast<int>(0x80000000u));
    const __m512i sign_flip = _mm512_maskz_mov_epi32(neg_mask, sign_bit);
    return _mm512_castsi512_ps(_mm512_xor_si512(_mm512_castps_si512(query_block), sign_flip));
}

VDB_FORCE_INLINE __m512 LoadSignedQuery16PackedChunkAvx512(const float* VDB_RESTRICT query,
                                                           uint64_t sign_chunk,
                                                           uint32_t chunk_lane_idx) {
    return FlipQuery16PackedChunkAvx512(_mm512_loadu_ps(query), sign_chunk, chunk_lane_idx);
}

VDB_FORCE_INLINE __m512 LoadAbsMagnitude16Avx512(const uint8_t* VDB_RESTRICT code_abs) {
    const __m128i codes_16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(code_abs));
    const __m512i codes_32 = _mm512_cvtepu8_epi32(codes_16);
    return _mm512_cvtepi32_ps(codes_32);
}

VDB_FORCE_INLINE float IPExRaBitQPackedSignAvx512(const float* VDB_RESTRICT query,
                                                  const uint8_t* VDB_RESTRICT code_abs,
                                                  const uint8_t* VDB_RESTRICT packed_sign,
                                                  Dim dim) {
    __m512 dot = _mm512_setzero_ps();
    __m512 bias = _mm512_setzero_ps();
    uint32_t i = 0;

    for (; i + 64 <= dim; i += 64) {
        const uint64_t sign_chunk = LoadPackedSignChunk64(packed_sign, i, dim);
        for (uint32_t block = 0; block < 4; ++block) {
            const __m512 q =
                LoadSignedQuery16PackedChunkAvx512(query + i + block * 16u, sign_chunk, block);
            const __m512 a = LoadAbsMagnitude16Avx512(code_abs + i + block * 16u);
            dot = _mm512_fmadd_ps(q, a, dot);
            bias = _mm512_add_ps(bias, q);
        }
    }

    for (; i + 32 <= dim; i += 32) {
        const uint64_t sign_chunk = LoadPackedSignChunk64(packed_sign, i, dim);
        for (uint32_t block = 0; block < 2; ++block) {
            const __m512 q =
                LoadSignedQuery16PackedChunkAvx512(query + i + block * 16u, sign_chunk, block);
            const __m512 a = LoadAbsMagnitude16Avx512(code_abs + i + block * 16u);
            dot = _mm512_fmadd_ps(q, a, dot);
            bias = _mm512_add_ps(bias, q);
        }
    }

    for (; i + 16 <= dim; i += 16) {
        const uint64_t sign_chunk = LoadPackedSignChunk64(packed_sign, i, dim);
        const __m512 q = LoadSignedQuery16PackedChunkAvx512(query + i, sign_chunk, 0);
        const __m512 a = LoadAbsMagnitude16Avx512(code_abs + i);
        dot = _mm512_fmadd_ps(q, a, dot);
        bias = _mm512_add_ps(bias, q);
    }

    float result = _mm512_reduce_add_ps(dot) + 0.5f * _mm512_reduce_add_ps(bias);
    for (; i < dim; ++i) {
        const bool positive = ((packed_sign[i / 8] >> (i % 8)) & 1u) != 0;
        const float signed_q = positive ? query[i] : -query[i];
        result += signed_q * static_cast<float>(code_abs[i]) + 0.5f * signed_q;
    }
    return result;
}

	VDB_FORCE_INLINE float IPOfficialRaBitQExDataAvx512(const float* VDB_RESTRICT query,
	                                                    const uint8_t* VDB_RESTRICT ex_code, Dim dim) {
	    __m512 dot = _mm512_setzero_ps();
	    uint32_t i = 0;
	    for (; i + 64 <= dim; i += 64) {
	        dot =
	            _mm512_fmadd_ps(_mm512_loadu_ps(query + i), LoadAbsMagnitude16Avx512(ex_code + i), dot);
        dot = _mm512_fmadd_ps(_mm512_loadu_ps(query + i + 16),
                              LoadAbsMagnitude16Avx512(ex_code + i + 16), dot);
        dot = _mm512_fmadd_ps(_mm512_loadu_ps(query + i + 32),
                              LoadAbsMagnitude16Avx512(ex_code + i + 32), dot);
	        dot = _mm512_fmadd_ps(_mm512_loadu_ps(query + i + 48),
	                              LoadAbsMagnitude16Avx512(ex_code + i + 48), dot);
	    }
	    for (; i + 16 <= dim; i += 16) {
	        dot =
	            _mm512_fmadd_ps(_mm512_loadu_ps(query + i), LoadAbsMagnitude16Avx512(ex_code + i), dot);
    }
    float result = _mm512_reduce_add_ps(dot);
    for (; i < dim; ++i) {
        result += query[i] * static_cast<float>(ex_code[i]);
	    }
	    return result;
	}

	VDB_FORCE_INLINE __m512 LoadTwoBit16Avx512(const uint8_t* VDB_RESTRICT low2) {
	    alignas(16) uint8_t unpacked[16];
	    uint32_t word = 0;
	    std::memcpy(&word, low2, sizeof(word));
	    for (uint32_t i = 0; i < 16; ++i) {
	        unpacked[i] = static_cast<uint8_t>((word >> (2u * i)) & 0x03u);
	    }
	    const __m128i codes_16 = _mm_load_si128(reinterpret_cast<const __m128i*>(unpacked));
	    return _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(codes_16));
	}

	VDB_FORCE_INLINE __m512 LoadBitAsFloat16Avx512(uint64_t bits, uint32_t sub) {
	    const __m512 ones = _mm512_set1_ps(1.0f);
	    const __mmask16 mask =
	        static_cast<__mmask16>((bits >> (sub * 16u)) & 0xFFFFu);
	    return _mm512_maskz_mov_ps(mask, ones);
	}

	VDB_FORCE_INLINE float IPOfficialDirect3TwoPlusOneAvx512(
	    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact, Dim dim) {
	    __m512 dot = _mm512_setzero_ps();
	    uint32_t d = 0;
	    uint32_t chunk = 0;
	    while (d + kOfficialDirect3DimBlock <= dim) {
	        const uint8_t* low2 = compact + static_cast<size_t>(chunk) * kOfficialDirect3BytesPerLane;
	        const uint8_t* high1 = low2 + 16;
	        uint64_t high_bits = 0;
	        std::memcpy(&high_bits, high1, sizeof(high_bits));
	        for (uint32_t sub = 0; sub < 4; ++sub) {
	            const __m512 q = _mm512_loadu_ps(query + d + sub * 16u);
	            const __m512 low = LoadTwoBit16Avx512(low2 + sub * 4u);
	            const __m512 high = _mm512_mul_ps(
	                LoadBitAsFloat16Avx512(high_bits, sub), _mm512_set1_ps(4.0f));
	            dot = _mm512_fmadd_ps(q, _mm512_add_ps(low, high), dot);
	        }
	        d += kOfficialDirect3DimBlock;
	        ++chunk;
	    }
	    float result = _mm512_reduce_add_ps(dot);
	    if (d < dim) {
	        result += IPOfficialDirect3TwoPlusOneReference(
	            query + d, compact + static_cast<size_t>(chunk) * kOfficialDirect3BytesPerLane,
	            dim - d);
	    }
	    return result;
	}

	VDB_FORCE_INLINE float IPOfficialDirect3BitplanesAvx512(
	    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact, Dim dim) {
	    return IPOfficialDirectBitplanesReference(query, compact, 3, dim);
	}

	VDB_FORCE_INLINE float IPOfficialDirectBitplanesAvx512(
	    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact, uint8_t bits,
	    Dim dim) {
	    if (bits == 0 || bits > 3) {
	        return 0.0f;
	    }
	    const uint32_t bytes_per_lane = ExRaBitQPackedMagnitudeBytes(kOfficialDirect3DimBlock, bits);
	    __m512 dot0 = _mm512_setzero_ps();
	    __m512 dot1 = _mm512_setzero_ps();
	    __m512 dot2 = _mm512_setzero_ps();
	    uint32_t d = 0;
	    uint32_t chunk = 0;
	    while (d + kOfficialDirect3DimBlock <= dim) {
	        const uint8_t* planes = compact + static_cast<size_t>(chunk) * bytes_per_lane;
	        uint64_t bits0 = 0;
	        uint64_t bits1 = 0;
	        uint64_t bits2 = 0;
	        std::memcpy(&bits0, planes, sizeof(bits0));
	        if (bits > 1) {
	            std::memcpy(&bits1, planes + 8, sizeof(bits1));
	        }
	        if (bits > 2) {
	            std::memcpy(&bits2, planes + 16, sizeof(bits2));
	        }
	        for (uint32_t sub = 0; sub < 4; ++sub) {
	            const __m512 q = _mm512_loadu_ps(query + d + sub * 16u);
	            dot0 = _mm512_add_ps(dot0, _mm512_maskz_mov_ps(
	                                           static_cast<__mmask16>((bits0 >> (sub * 16u)) & 0xFFFFu),
	                                           q));
	            if (bits > 1) {
	                dot1 = _mm512_add_ps(
	                    dot1, _mm512_maskz_mov_ps(
	                              static_cast<__mmask16>((bits1 >> (sub * 16u)) & 0xFFFFu), q));
	            }
	            if (bits > 2) {
	                dot2 = _mm512_add_ps(
	                    dot2, _mm512_maskz_mov_ps(
	                              static_cast<__mmask16>((bits2 >> (sub * 16u)) & 0xFFFFu), q));
	            }
	        }
	        d += kOfficialDirect3DimBlock;
	        ++chunk;
	    }
	    float result = _mm512_reduce_add_ps(dot0);
	    if (bits > 1) {
	        result += 2.0f * _mm512_reduce_add_ps(dot1);
	    }
	    if (bits > 2) {
	        result += 4.0f * _mm512_reduce_add_ps(dot2);
	    }
	    if (d < dim) {
	        result += IPOfficialDirectBitplanesReference(
	            query + d, compact + static_cast<size_t>(chunk) * bytes_per_lane, bits, dim - d);
	    }
	    return result;
	}

	VDB_FORCE_INLINE float IPOfficialDirect3Avx512(const float* VDB_RESTRICT query,
	                                               const uint8_t* VDB_RESTRICT compact,
	                                               RaBitQExDataLayout layout, Dim dim) {
	    const RaBitQExDataLayout effective = RaBitQResolveSelectedExDataLayout(layout);
	    if (effective == RaBitQExDataLayout::kSplit3Bitplanes) {
	        return IPOfficialDirectBitplanesAvx512(query, compact, 3, dim);
	    }
	    return IPOfficialDirect3TwoPlusOneAvx512(query, compact, dim);
	}

VDB_FORCE_INLINE void
IPExRaBitQBatchPackedSignAvx512(const float* VDB_RESTRICT query,
                                const uint8_t* const* VDB_RESTRICT code_abs_ptrs,
                                const uint8_t* const* VDB_RESTRICT packed_sign_ptrs, uint32_t count,
                                Dim dim, float* VDB_RESTRICT out_ip_raw) {
    __m512 dot[8];
    __m512 bias[8];
    for (uint32_t c = 0; c < count; ++c) {
        dot[c] = _mm512_setzero_ps();
        bias[c] = _mm512_setzero_ps();
    }

    uint32_t i = 0;
    for (; i + 64 <= dim; i += 64) {
        const __m512 q0 = _mm512_loadu_ps(query + i);
        const __m512 q1 = _mm512_loadu_ps(query + i + 16);
        const __m512 q2 = _mm512_loadu_ps(query + i + 32);
        const __m512 q3 = _mm512_loadu_ps(query + i + 48);
        for (uint32_t c = 0; c < count; ++c) {
            const uint64_t sign_chunk = LoadPackedSignChunk64(packed_sign_ptrs[c], i, dim);
            const __m512 sq0 = FlipQuery16PackedChunkAvx512(q0, sign_chunk, 0);
            const __m512 sq1 = FlipQuery16PackedChunkAvx512(q1, sign_chunk, 1);
            const __m512 sq2 = FlipQuery16PackedChunkAvx512(q2, sign_chunk, 2);
            const __m512 sq3 = FlipQuery16PackedChunkAvx512(q3, sign_chunk, 3);
            const uint8_t* const code_abs = code_abs_ptrs[c] + i;
            dot[c] = _mm512_fmadd_ps(sq0, LoadAbsMagnitude16Avx512(code_abs), dot[c]);
            dot[c] = _mm512_fmadd_ps(sq1, LoadAbsMagnitude16Avx512(code_abs + 16), dot[c]);
            dot[c] = _mm512_fmadd_ps(sq2, LoadAbsMagnitude16Avx512(code_abs + 32), dot[c]);
            dot[c] = _mm512_fmadd_ps(sq3, LoadAbsMagnitude16Avx512(code_abs + 48), dot[c]);
            bias[c] = _mm512_add_ps(bias[c], sq0);
            bias[c] = _mm512_add_ps(bias[c], sq1);
            bias[c] = _mm512_add_ps(bias[c], sq2);
            bias[c] = _mm512_add_ps(bias[c], sq3);
        }
    }

    for (; i + 32 <= dim; i += 32) {
        const __m512 q0 = _mm512_loadu_ps(query + i);
        const __m512 q1 = _mm512_loadu_ps(query + i + 16);
        for (uint32_t c = 0; c < count; ++c) {
            const uint64_t sign_chunk = LoadPackedSignChunk64(packed_sign_ptrs[c], i, dim);
            const __m512 sq0 = FlipQuery16PackedChunkAvx512(q0, sign_chunk, 0);
            const __m512 sq1 = FlipQuery16PackedChunkAvx512(q1, sign_chunk, 1);
            const uint8_t* const code_abs = code_abs_ptrs[c] + i;
            dot[c] = _mm512_fmadd_ps(sq0, LoadAbsMagnitude16Avx512(code_abs), dot[c]);
            dot[c] = _mm512_fmadd_ps(sq1, LoadAbsMagnitude16Avx512(code_abs + 16), dot[c]);
            bias[c] = _mm512_add_ps(bias[c], sq0);
            bias[c] = _mm512_add_ps(bias[c], sq1);
        }
    }

    for (; i + 16 <= dim; i += 16) {
        const __m512 q0 = _mm512_loadu_ps(query + i);
        for (uint32_t c = 0; c < count; ++c) {
            const uint64_t sign_chunk = LoadPackedSignChunk64(packed_sign_ptrs[c], i, dim);
            const __m512 sq0 = FlipQuery16PackedChunkAvx512(q0, sign_chunk, 0);
            dot[c] = _mm512_fmadd_ps(sq0, LoadAbsMagnitude16Avx512(code_abs_ptrs[c] + i), dot[c]);
            bias[c] = _mm512_add_ps(bias[c], sq0);
        }
    }

    for (uint32_t c = 0; c < count; ++c) {
        float result = _mm512_reduce_add_ps(dot[c]) + 0.5f * _mm512_reduce_add_ps(bias[c]);
        for (uint32_t t = i; t < dim; ++t) {
            const bool positive = ((packed_sign_ptrs[c][t / 8] >> (t % 8)) & 1u) != 0;
            const float signed_q = positive ? query[t] : -query[t];
            result += signed_q * static_cast<float>(code_abs_ptrs[c][t]) + 0.5f * signed_q;
        }
        out_ip_raw[c] = result;
    }
}
#endif

#if defined(VDB_USE_AVX2)
alignas(32) const __m256i* PackedSignFlipLutAvx2() {
    alignas(32) static __m256i lut[256];
    static bool initialized = false;
    if (!initialized) {
        const uint32_t sign_bit = 0x80000000u;
        for (uint32_t bits = 0; bits < 256; ++bits) {
            lut[bits] = _mm256_setr_epi32((bits & (1u << 0)) ? 0 : static_cast<int>(sign_bit),
                                          (bits & (1u << 1)) ? 0 : static_cast<int>(sign_bit),
                                          (bits & (1u << 2)) ? 0 : static_cast<int>(sign_bit),
                                          (bits & (1u << 3)) ? 0 : static_cast<int>(sign_bit),
                                          (bits & (1u << 4)) ? 0 : static_cast<int>(sign_bit),
                                          (bits & (1u << 5)) ? 0 : static_cast<int>(sign_bit),
                                          (bits & (1u << 6)) ? 0 : static_cast<int>(sign_bit),
                                          (bits & (1u << 7)) ? 0 : static_cast<int>(sign_bit));
        }
        initialized = true;
    }
    return lut;
}

VDB_FORCE_INLINE __m256 LoadAbsMagnitude8Avx2(const uint8_t* VDB_RESTRICT code_abs) {
    const __m128i codes_8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(code_abs));
    const __m256i codes_32 = _mm256_cvtepu8_epi32(codes_8);
    return _mm256_cvtepi32_ps(codes_32);
}

VDB_FORCE_INLINE __m256 LoadSignedQuery8Avx2(const float* VDB_RESTRICT query,
                                             const uint8_t* VDB_RESTRICT sign) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i sign_bit = _mm256_set1_epi32(static_cast<int>(0x80000000u));
    const __m256 q = _mm256_loadu_ps(query);

    const __m128i sign_8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(sign));
    const __m256i sign_32 = _mm256_cvtepu8_epi32(sign_8);
    const __m256i neg_mask = _mm256_cmpeq_epi32(sign_32, zero);
    const __m256i sign_flip = _mm256_and_si256(neg_mask, sign_bit);
    return _mm256_xor_ps(q, _mm256_castsi256_ps(sign_flip));
}

VDB_FORCE_INLINE __m256 LoadSignedQuery8Avx2Packed(const float* VDB_RESTRICT query,
                                                   const uint8_t* VDB_RESTRICT packed_sign,
                                                   uint32_t bit_offset) {
    const __m256 q = _mm256_loadu_ps(query);
    const uint32_t byte_idx = bit_offset / 8;
    const __m256i sign_flip = PackedSignFlipLutAvx2()[packed_sign[byte_idx]];
    return _mm256_xor_ps(q, _mm256_castsi256_ps(sign_flip));
}

VDB_FORCE_INLINE float HorizAdd(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_hadd_ps(lo, lo);
    lo = _mm_hadd_ps(lo, lo);
    return _mm_cvtss_f32(lo);
}

VDB_FORCE_INLINE float IPOfficialRaBitQExDataAvx2(const float* VDB_RESTRICT query,
                                                  const uint8_t* VDB_RESTRICT ex_code, Dim dim) {
    __m256 dot0 = _mm256_setzero_ps();
    __m256 dot1 = _mm256_setzero_ps();
    uint32_t i = 0;
    for (; i + 32 <= dim; i += 32) {
        dot0 =
            _mm256_fmadd_ps(_mm256_loadu_ps(query + i), LoadAbsMagnitude8Avx2(ex_code + i), dot0);
        dot1 = _mm256_fmadd_ps(_mm256_loadu_ps(query + i + 8),
                               LoadAbsMagnitude8Avx2(ex_code + i + 8), dot1);
        dot0 = _mm256_fmadd_ps(_mm256_loadu_ps(query + i + 16),
                               LoadAbsMagnitude8Avx2(ex_code + i + 16), dot0);
        dot1 = _mm256_fmadd_ps(_mm256_loadu_ps(query + i + 24),
                               LoadAbsMagnitude8Avx2(ex_code + i + 24), dot1);
    }
    for (; i + 8 <= dim; i += 8) {
        dot0 =
            _mm256_fmadd_ps(_mm256_loadu_ps(query + i), LoadAbsMagnitude8Avx2(ex_code + i), dot0);
    }
    float result = HorizAdd(_mm256_add_ps(dot0, dot1));
    for (; i < dim; ++i) {
        result += query[i] * static_cast<float>(ex_code[i]);
    }
    return result;
}

VDB_FORCE_INLINE float IPExRaBitQGenericAvx2(const float* VDB_RESTRICT query,
                                             const uint8_t* VDB_RESTRICT code_abs,
                                             const uint8_t* VDB_RESTRICT sign, bool sign_packed,
                                             Dim dim) {
    __m256 dot0 = _mm256_setzero_ps();
    __m256 dot1 = _mm256_setzero_ps();
    __m256 bias0 = _mm256_setzero_ps();
    __m256 bias1 = _mm256_setzero_ps();
    uint32_t i = 0;

    for (; i + 32 <= dim; i += 32) {
        const __m256 q0 = sign_packed ? LoadSignedQuery8Avx2Packed(query + i, sign, i)
                                      : LoadSignedQuery8Avx2(query + i, sign + i);
        const __m256 q1 = sign_packed ? LoadSignedQuery8Avx2Packed(query + i + 8, sign, i + 8)
                                      : LoadSignedQuery8Avx2(query + i + 8, sign + i + 8);
        const __m256 q2 = sign_packed ? LoadSignedQuery8Avx2Packed(query + i + 16, sign, i + 16)
                                      : LoadSignedQuery8Avx2(query + i + 16, sign + i + 16);
        const __m256 q3 = sign_packed ? LoadSignedQuery8Avx2Packed(query + i + 24, sign, i + 24)
                                      : LoadSignedQuery8Avx2(query + i + 24, sign + i + 24);
        const __m256 a0 = LoadAbsMagnitude8Avx2(code_abs + i);
        const __m256 a1 = LoadAbsMagnitude8Avx2(code_abs + i + 8);
        const __m256 a2 = LoadAbsMagnitude8Avx2(code_abs + i + 16);
        const __m256 a3 = LoadAbsMagnitude8Avx2(code_abs + i + 24);
        dot0 = _mm256_fmadd_ps(q0, a0, dot0);
        dot1 = _mm256_fmadd_ps(q1, a1, dot1);
        dot0 = _mm256_fmadd_ps(q2, a2, dot0);
        dot1 = _mm256_fmadd_ps(q3, a3, dot1);
        bias0 = _mm256_add_ps(bias0, q0);
        bias1 = _mm256_add_ps(bias1, q1);
        bias0 = _mm256_add_ps(bias0, q2);
        bias1 = _mm256_add_ps(bias1, q3);
    }

    for (; i + 16 <= dim; i += 16) {
        const __m256 q0 = sign_packed ? LoadSignedQuery8Avx2Packed(query + i, sign, i)
                                      : LoadSignedQuery8Avx2(query + i, sign + i);
        const __m256 q1 = sign_packed ? LoadSignedQuery8Avx2Packed(query + i + 8, sign, i + 8)
                                      : LoadSignedQuery8Avx2(query + i + 8, sign + i + 8);
        const __m256 a0 = LoadAbsMagnitude8Avx2(code_abs + i);
        const __m256 a1 = LoadAbsMagnitude8Avx2(code_abs + i + 8);
        dot0 = _mm256_fmadd_ps(q0, a0, dot0);
        dot1 = _mm256_fmadd_ps(q1, a1, dot1);
        bias0 = _mm256_add_ps(bias0, q0);
        bias1 = _mm256_add_ps(bias1, q1);
    }

    for (; i + 8 <= dim; i += 8) {
        const __m256 q = sign_packed ? LoadSignedQuery8Avx2Packed(query + i, sign, i)
                                     : LoadSignedQuery8Avx2(query + i, sign + i);
        const __m256 a = LoadAbsMagnitude8Avx2(code_abs + i);
        dot0 = _mm256_fmadd_ps(q, a, dot0);
        bias0 = _mm256_add_ps(bias0, q);
    }

    float result = HorizAdd(_mm256_add_ps(dot0, dot1));
    result += 0.5f * HorizAdd(_mm256_add_ps(bias0, bias1));
    for (; i < dim; ++i) {
        const bool positive = sign_packed ? ((sign[i / 8] >> (i % 8)) & 1u) != 0 : sign[i] != 0;
        const float signed_q = positive ? query[i] : -query[i];
        result += signed_q * static_cast<float>(code_abs[i]) + 0.5f * signed_q;
    }
    return result;
}
#endif

} // namespace

uint32_t ExRaBitQPackedMagnitudeBytes(uint32_t dim_block, uint8_t bits) {
    if (bits < 1 || bits > 4) {
        return 0;
    }
    return (dim_block * static_cast<uint32_t>(bits) + 7u) / 8u;
}

bool ExRaBitQPackMagnitudes(const uint8_t* VDB_RESTRICT decoded, uint32_t count, uint8_t bits,
                            uint8_t* VDB_RESTRICT packed, uint32_t packed_bytes) {
    if (decoded == nullptr || packed == nullptr) {
        return false;
    }
    const uint32_t expected_bytes = ExRaBitQPackedMagnitudeBytes(count, bits);
    if (expected_bytes == 0 || packed_bytes < expected_bytes) {
        return false;
    }
    std::memset(packed, 0, packed_bytes);
    if (bits == 2) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t v = decoded[i];
            if (v > 3u)
                return false;
            packed[i / 4u] |= static_cast<uint8_t>(v << ((i % 4u) * 2u));
        }
        return true;
    }
    const uint8_t max_value = static_cast<uint8_t>((1u << bits) - 1u);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t v = decoded[i];
        if (v > max_value)
            return false;
        const uint32_t bit_pos = i * static_cast<uint32_t>(bits);
        const uint32_t byte_pos = bit_pos / 8u;
        const uint32_t bit_off = bit_pos % 8u;
        uint16_t shifted = static_cast<uint16_t>(v) << bit_off;
        packed[byte_pos] |= static_cast<uint8_t>(shifted & 0xFFu);
        if (bit_off + bits > 8u) {
            packed[byte_pos + 1u] |= static_cast<uint8_t>(shifted >> 8u);
        }
    }
    return true;
}

namespace {

inline void UnpackMagnitudesScalar(const uint8_t* VDB_RESTRICT packed, uint32_t start,
                                   uint32_t count, uint8_t bits, uint8_t* VDB_RESTRICT decoded) {
    for (uint32_t i = start; i < count; ++i) {
        if (bits == 2) {
            decoded[i] = static_cast<uint8_t>((packed[i / 4u] >> ((i % 4u) * 2u)) & 0x03u);
            continue;
        }
        if (bits == 4) {
            decoded[i] = static_cast<uint8_t>((packed[i / 2u] >> ((i % 2u) * 4u)) & 0x0Fu);
            continue;
        }
        const uint32_t bit_pos = i * static_cast<uint32_t>(bits);
        const uint32_t byte_pos = bit_pos / 8u;
        const uint32_t bit_off = bit_pos % 8u;
        uint16_t window = packed[byte_pos];
        if (bit_off + bits > 8u) {
            window |= static_cast<uint16_t>(packed[byte_pos + 1u]) << 8u;
        }
        decoded[i] = static_cast<uint8_t>((window >> bit_off) & ((1u << bits) - 1u));
    }
}

#if defined(VDB_USE_AVX512) || defined(VDB_USE_AVX2)
inline uint32_t UnpackBits4Simd(const uint8_t* VDB_RESTRICT packed, uint32_t count,
                                uint8_t* VDB_RESTRICT decoded) {
    const __m128i mask = _mm_set1_epi8(0x0F);
    uint32_t out = 0;
    uint32_t in = 0;
    for (; out + 32u <= count; out += 32u, in += 16u) {
        const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(packed + in));
        const __m128i lo = _mm_and_si128(bytes, mask);
        const __m128i hi = _mm_and_si128(_mm_srli_epi16(bytes, 4), mask);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(decoded + out), _mm_unpacklo_epi8(lo, hi));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(decoded + out + 16), _mm_unpackhi_epi8(lo, hi));
    }
    return out;
}

inline uint32_t UnpackBits2Simd(const uint8_t* VDB_RESTRICT packed, uint32_t count,
                                uint8_t* VDB_RESTRICT decoded) {
    const __m128i mask = _mm_set1_epi8(0x03);
    uint32_t out = 0;
    uint32_t in = 0;
    for (; out + 64u <= count; out += 64u, in += 16u) {
        const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(packed + in));
        const __m128i v0 = _mm_and_si128(bytes, mask);
        const __m128i v1 = _mm_and_si128(_mm_srli_epi16(bytes, 2), mask);
        const __m128i v2 = _mm_and_si128(_mm_srli_epi16(bytes, 4), mask);
        const __m128i v3 = _mm_and_si128(_mm_srli_epi16(bytes, 6), mask);

        const __m128i a01_lo = _mm_unpacklo_epi8(v0, v1);
        const __m128i a01_hi = _mm_unpackhi_epi8(v0, v1);
        const __m128i a23_lo = _mm_unpacklo_epi8(v2, v3);
        const __m128i a23_hi = _mm_unpackhi_epi8(v2, v3);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(decoded + out),
                         _mm_unpacklo_epi16(a01_lo, a23_lo));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(decoded + out + 16),
                         _mm_unpackhi_epi16(a01_lo, a23_lo));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(decoded + out + 32),
                         _mm_unpacklo_epi16(a01_hi, a23_hi));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(decoded + out + 48),
                         _mm_unpackhi_epi16(a01_hi, a23_hi));
    }
    return out;
}
#endif

} // namespace

bool ExRaBitQUnpackMagnitudes(const uint8_t* VDB_RESTRICT packed, uint32_t count, uint8_t bits,
                              uint8_t* VDB_RESTRICT decoded) {
    if (packed == nullptr || decoded == nullptr || bits < 1 || bits > 4) {
        return false;
    }
    uint32_t decoded_count = 0;
#if defined(VDB_USE_AVX512) || defined(VDB_USE_AVX2)
    if (bits == 4) {
        decoded_count = UnpackBits4Simd(packed, count, decoded);
    } else if (bits == 2) {
        decoded_count = UnpackBits2Simd(packed, count, decoded);
    }
#endif
    if (decoded_count < count) {
        UnpackMagnitudesScalar(packed, decoded_count, count, bits, decoded);
    }
    return true;
}

bool ExRaBitQPackOfficialDirect3(const uint8_t* VDB_RESTRICT decoded, uint32_t count,
                                 RaBitQExDataLayout layout,
                                 uint8_t* VDB_RESTRICT packed, uint32_t packed_bytes) {
    const RaBitQExDataLayout effective = RaBitQResolveSelectedExDataLayout(layout);
    if (decoded == nullptr || packed == nullptr ||
        (effective != RaBitQExDataLayout::kSplit3TwoPlusOne &&
         effective != RaBitQExDataLayout::kSplit3Bitplanes)) {
        return false;
    }
    const uint32_t chunks = (count + kOfficialDirect3DimBlock - 1u) / kOfficialDirect3DimBlock;
    const uint32_t expected_bytes = chunks * kOfficialDirect3BytesPerLane;
    if (chunks == 0 || packed_bytes < expected_bytes) {
        return false;
    }
    std::memset(packed, 0, packed_bytes);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t v = decoded[i];
        if (v > 7u) {
            return false;
        }
        const uint32_t chunk = i / kOfficialDirect3DimBlock;
        const uint32_t in_chunk = i % kOfficialDirect3DimBlock;
        uint8_t* dst = packed + static_cast<size_t>(chunk) * kOfficialDirect3BytesPerLane;
        if (effective == RaBitQExDataLayout::kSplit3TwoPlusOne) {
            dst[in_chunk / 4u] |= static_cast<uint8_t>((v & 0x03u) << ((in_chunk % 4u) * 2u));
            if ((v & 0x04u) != 0) {
                dst[16u + in_chunk / 8u] |= static_cast<uint8_t>(1u << (in_chunk % 8u));
            }
        } else {
            if ((v & 0x01u) != 0) {
                dst[in_chunk / 8u] |= static_cast<uint8_t>(1u << (in_chunk % 8u));
            }
            if ((v & 0x02u) != 0) {
                dst[8u + in_chunk / 8u] |= static_cast<uint8_t>(1u << (in_chunk % 8u));
            }
            if ((v & 0x04u) != 0) {
                dst[16u + in_chunk / 8u] |= static_cast<uint8_t>(1u << (in_chunk % 8u));
            }
        }
    }
    return true;
}

bool ExRaBitQUnpackOfficialDirect3(const uint8_t* VDB_RESTRICT packed, uint32_t count,
                                   RaBitQExDataLayout layout,
                                   uint8_t* VDB_RESTRICT decoded) {
    const RaBitQExDataLayout effective = RaBitQResolveSelectedExDataLayout(layout);
    if (packed == nullptr || decoded == nullptr ||
        (effective != RaBitQExDataLayout::kSplit3TwoPlusOne &&
         effective != RaBitQExDataLayout::kSplit3Bitplanes)) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t chunk = i / kOfficialDirect3DimBlock;
        const uint32_t in_chunk = i % kOfficialDirect3DimBlock;
        const uint8_t* src = packed + static_cast<size_t>(chunk) * kOfficialDirect3BytesPerLane;
        if (effective == RaBitQExDataLayout::kSplit3TwoPlusOne) {
            const uint8_t low =
                static_cast<uint8_t>((src[in_chunk / 4u] >> ((in_chunk % 4u) * 2u)) & 0x03u);
            const uint8_t high =
                static_cast<uint8_t>((src[16u + in_chunk / 8u] >> (in_chunk % 8u)) & 0x01u);
            decoded[i] = static_cast<uint8_t>(low | (high << 2u));
        } else {
            const uint8_t bit0 =
                static_cast<uint8_t>((src[in_chunk / 8u] >> (in_chunk % 8u)) & 0x01u);
            const uint8_t bit1 =
                static_cast<uint8_t>((src[8u + in_chunk / 8u] >> (in_chunk % 8u)) & 0x01u);
            const uint8_t bit2 =
                static_cast<uint8_t>((src[16u + in_chunk / 8u] >> (in_chunk % 8u)) & 0x01u);
            decoded[i] = static_cast<uint8_t>(bit0 | (bit1 << 1u) | (bit2 << 2u));
        }
    }
    return true;
}

bool ExRaBitQPackOfficialDirectBitplanes(const uint8_t* VDB_RESTRICT decoded, uint32_t count,
                                         uint8_t bits, uint8_t* VDB_RESTRICT packed,
                                         uint32_t packed_bytes) {
    if (decoded == nullptr || packed == nullptr || bits < 1 || bits > 3) {
        return false;
    }
    const uint32_t chunks = (count + kOfficialDirect3DimBlock - 1u) / kOfficialDirect3DimBlock;
    const uint32_t bytes_per_lane = ExRaBitQPackedMagnitudeBytes(kOfficialDirect3DimBlock, bits);
    const uint32_t expected_bytes = chunks * bytes_per_lane;
    if (chunks == 0 || bytes_per_lane == 0 || packed_bytes < expected_bytes) {
        return false;
    }
    const uint8_t max_value = static_cast<uint8_t>((1u << bits) - 1u);
    std::memset(packed, 0, packed_bytes);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t v = decoded[i];
        if (v > max_value) {
            return false;
        }
        const uint32_t chunk = i / kOfficialDirect3DimBlock;
        const uint32_t in_chunk = i % kOfficialDirect3DimBlock;
        uint8_t* dst = packed + static_cast<size_t>(chunk) * bytes_per_lane;
        for (uint8_t bit = 0; bit < bits; ++bit) {
            if ((v & (1u << bit)) != 0) {
                dst[static_cast<uint32_t>(bit) * 8u + in_chunk / 8u] |=
                    static_cast<uint8_t>(1u << (in_chunk % 8u));
            }
        }
    }
    return true;
}

bool ExRaBitQUnpackOfficialDirectBitplanes(const uint8_t* VDB_RESTRICT packed, uint32_t count,
                                           uint8_t bits, uint8_t* VDB_RESTRICT decoded) {
    if (packed == nullptr || decoded == nullptr || bits < 1 || bits > 3) {
        return false;
    }
    const uint32_t bytes_per_lane = ExRaBitQPackedMagnitudeBytes(kOfficialDirect3DimBlock, bits);
    if (bytes_per_lane == 0) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t chunk = i / kOfficialDirect3DimBlock;
        const uint32_t in_chunk = i % kOfficialDirect3DimBlock;
        const uint8_t* src = packed + static_cast<size_t>(chunk) * bytes_per_lane;
        uint8_t v = 0;
        for (uint8_t bit = 0; bit < bits; ++bit) {
            const uint8_t plane_bit = static_cast<uint8_t>(
                (src[static_cast<uint32_t>(bit) * 8u + in_chunk / 8u] >> (in_chunk % 8u)) &
                0x01u);
            v = static_cast<uint8_t>(v | (plane_bit << bit));
        }
        decoded[i] = v;
    }
    return true;
}

bool ExRaBitQDecodePackedBatchBlockMagnitudes(const uint8_t* VDB_RESTRICT packed_abs_blocks,
                                              uint32_t num_dim_blocks, uint32_t batch_size,
                                              uint32_t dim_block,
                                              uint32_t abs_bytes_per_lane_dim_block, uint8_t bits,
                                              uint8_t* VDB_RESTRICT decoded_abs_blocks) {
    if (packed_abs_blocks == nullptr || decoded_abs_blocks == nullptr || num_dim_blocks == 0 ||
        batch_size == 0 || dim_block == 0) {
        return false;
    }
    if (ExRaBitQPackedMagnitudeBytes(dim_block, bits) != abs_bytes_per_lane_dim_block) {
        return false;
    }
    for (uint32_t db = 0; db < num_dim_blocks; ++db) {
        for (uint32_t lane = 0; lane < batch_size; ++lane) {
            const uint8_t* packed =
                packed_abs_blocks +
                (static_cast<size_t>(db) * batch_size + lane) * abs_bytes_per_lane_dim_block;
            uint8_t* decoded =
                decoded_abs_blocks + (static_cast<size_t>(db) * batch_size + lane) * dim_block;
            if (!ExRaBitQUnpackMagnitudes(packed, dim_block, bits, decoded)) {
                return false;
            }
        }
    }
    return true;
}

float IPExRaBitQPackedSign(const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT code_abs,
                           const uint8_t* VDB_RESTRICT packed_sign, Dim dim) {
#if defined(VDB_USE_AVX512)
    float result = IPExRaBitQPackedSignAvx512(query, code_abs, packed_sign, dim);
#elif defined(VDB_USE_AVX2)
    float result = IPExRaBitQGenericAvx2(query, code_abs, packed_sign, true, dim);
#else
    float result = IPExRaBitQReference(query, code_abs, packed_sign, true, dim);
#endif
#ifndef NDEBUG
    const float ref = IPExRaBitQReference(query, code_abs, packed_sign, true, dim);
    if (std::abs(ref - result) > 1e-3f) {
        __builtin_trap();
    }
#endif
    return result;
}

void IPExRaBitQBatchPackedSign(const float* VDB_RESTRICT query,
                               const uint8_t* const* VDB_RESTRICT code_abs_ptrs,
                               const uint8_t* const* VDB_RESTRICT packed_sign_ptrs, uint32_t count,
                               Dim dim, float* VDB_RESTRICT out_ip_raw) {
#if defined(VDB_USE_AVX512)
    IPExRaBitQBatchPackedSignAvx512(query, code_abs_ptrs, packed_sign_ptrs, count, dim, out_ip_raw);
#else
    for (uint32_t i = 0; i < count; ++i) {
        out_ip_raw[i] = IPExRaBitQPackedSign(query, code_abs_ptrs[i], packed_sign_ptrs[i], dim);
    }
#endif
#ifndef NDEBUG
    for (uint32_t i = 0; i < count; ++i) {
        const float fast = IPExRaBitQPackedSign(query, code_abs_ptrs[i], packed_sign_ptrs[i], dim);
        const float ref =
            IPExRaBitQReference(query, code_abs_ptrs[i], packed_sign_ptrs[i], true, dim);
        if (std::abs(fast - out_ip_raw[i]) > 1e-3f || std::abs(ref - out_ip_raw[i]) > 1e-3f) {
            __builtin_trap();
        }
    }
#endif
}

void IPExRaBitQBatchPackedSignCompact(const float* VDB_RESTRICT query,
                                      const uint8_t* VDB_RESTRICT abs_blocks,
                                      const uint8_t* VDB_RESTRICT sign_blocks, uint32_t valid_count,
                                      Dim dim, uint32_t dim_block, float* VDB_RESTRICT out_ip_raw,
                                      IPExRaBitQBatchPackedSignCompactTiming* timing) {
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    const uint32_t sign_block_bytes = dim_block / 8;
#if defined(VDB_USE_AVX512)
    const bool measure = timing != nullptr;
    __m512 dot[8];
    __m512 bias[8];
    for (uint32_t lane = 0; lane < valid_count; ++lane) {
        dot[lane] = _mm512_setzero_ps();
        bias[lane] = _mm512_setzero_ps();
    }

    for (uint32_t db = 0; db < num_dim_blocks; ++db) {
        const uint32_t dim_start = db * dim_block;
        const uint32_t remaining = std::min(dim_block, dim - dim_start);
        const uint8_t* abs_base = abs_blocks + static_cast<size_t>(db) * 8 * dim_block;
        const uint8_t* sign_base = sign_blocks + static_cast<size_t>(db) * 8 * sign_block_bytes;

        uint32_t i = 0;
        for (; i + 64 <= remaining; i += 64) {
            const __m512 q0 = _mm512_loadu_ps(query + dim_start + i);
            const __m512 q1 = _mm512_loadu_ps(query + dim_start + i + 16);
            const __m512 q2 = _mm512_loadu_ps(query + dim_start + i + 32);
            const __m512 q3 = _mm512_loadu_ps(query + dim_start + i + 48);
            for (uint32_t lane = 0; lane < valid_count; ++lane) {
                const auto sign_start = measure ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
                uint64_t sign_chunk = 0;
                std::memcpy(&sign_chunk, sign_base + lane * sign_block_bytes + i / 8,
                            sizeof(uint64_t));
                const __m512 sq0 = FlipQuery16PackedChunkAvx512(q0, sign_chunk, 0);
                const __m512 sq1 = FlipQuery16PackedChunkAvx512(q1, sign_chunk, 1);
                const __m512 sq2 = FlipQuery16PackedChunkAvx512(q2, sign_chunk, 2);
                const __m512 sq3 = FlipQuery16PackedChunkAvx512(q3, sign_chunk, 3);
                if (measure) {
                    timing->sign_flip_ms += std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - sign_start)
                                                .count();
                }

                const auto abs_start = measure ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
                const uint8_t* lane_abs = abs_base + lane * dim_block + i;
                dot[lane] = _mm512_fmadd_ps(sq0, LoadAbsMagnitude16Avx512(lane_abs), dot[lane]);
                dot[lane] =
                    _mm512_fmadd_ps(sq1, LoadAbsMagnitude16Avx512(lane_abs + 16), dot[lane]);
                dot[lane] =
                    _mm512_fmadd_ps(sq2, LoadAbsMagnitude16Avx512(lane_abs + 32), dot[lane]);
                dot[lane] =
                    _mm512_fmadd_ps(sq3, LoadAbsMagnitude16Avx512(lane_abs + 48), dot[lane]);
                bias[lane] = _mm512_add_ps(bias[lane], sq0);
                bias[lane] = _mm512_add_ps(bias[lane], sq1);
                bias[lane] = _mm512_add_ps(bias[lane], sq2);
                bias[lane] = _mm512_add_ps(bias[lane], sq3);
                if (measure) {
                    timing->abs_fma_ms += std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - abs_start)
                                              .count();
                }
            }
        }
        for (; i + 32 <= remaining; i += 32) {
            const __m512 q0 = _mm512_loadu_ps(query + dim_start + i);
            const __m512 q1 = _mm512_loadu_ps(query + dim_start + i + 16);
            for (uint32_t lane = 0; lane < valid_count; ++lane) {
                const auto sign_start = measure ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
                uint64_t sign_chunk = 0;
                std::memcpy(&sign_chunk, sign_base + lane * sign_block_bytes + i / 8,
                            sizeof(uint32_t));
                const __m512 sq0 = FlipQuery16PackedChunkAvx512(q0, sign_chunk, 0);
                const __m512 sq1 = FlipQuery16PackedChunkAvx512(q1, sign_chunk, 1);
                if (measure) {
                    timing->sign_flip_ms += std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - sign_start)
                                                .count();
                }

                const auto abs_start = measure ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
                const uint8_t* lane_abs = abs_base + lane * dim_block + i;
                dot[lane] = _mm512_fmadd_ps(sq0, LoadAbsMagnitude16Avx512(lane_abs), dot[lane]);
                dot[lane] =
                    _mm512_fmadd_ps(sq1, LoadAbsMagnitude16Avx512(lane_abs + 16), dot[lane]);
                bias[lane] = _mm512_add_ps(bias[lane], sq0);
                bias[lane] = _mm512_add_ps(bias[lane], sq1);
                if (measure) {
                    timing->abs_fma_ms += std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - abs_start)
                                              .count();
                }
            }
        }
        for (; i + 16 <= remaining; i += 16) {
            const __m512 q0 = _mm512_loadu_ps(query + dim_start + i);
            for (uint32_t lane = 0; lane < valid_count; ++lane) {
                const auto sign_start = measure ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
                uint64_t sign_chunk = 0;
                std::memcpy(&sign_chunk, sign_base + lane * sign_block_bytes + i / 8,
                            sizeof(uint16_t));
                const __m512 sq0 = FlipQuery16PackedChunkAvx512(q0, sign_chunk, 0);
                if (measure) {
                    timing->sign_flip_ms += std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - sign_start)
                                                .count();
                }

                const auto abs_start = measure ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
                const uint8_t* lane_abs = abs_base + lane * dim_block + i;
                dot[lane] = _mm512_fmadd_ps(sq0, LoadAbsMagnitude16Avx512(lane_abs), dot[lane]);
                bias[lane] = _mm512_add_ps(bias[lane], sq0);
                if (measure) {
                    timing->abs_fma_ms += std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - abs_start)
                                              .count();
                }
            }
        }
        for (; i < remaining; ++i) {
            const auto tail_start = measure ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
            for (uint32_t lane = 0; lane < valid_count; ++lane) {
                const bool positive =
                    ((sign_base[lane * sign_block_bytes + i / 8] >> (i % 8)) & 1u) != 0;
                const float signed_q = positive ? query[dim_start + i] : -query[dim_start + i];
                out_ip_raw[lane] +=
                    signed_q * static_cast<float>(abs_base[lane * dim_block + i]) + 0.5f * signed_q;
            }
            if (measure) {
                timing->tail_ms += std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - tail_start)
                                       .count();
            }
        }
    }
    const auto reduce_start =
        measure ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (uint32_t lane = 0; lane < valid_count; ++lane) {
        out_ip_raw[lane] +=
            _mm512_reduce_add_ps(dot[lane]) + 0.5f * _mm512_reduce_add_ps(bias[lane]);
    }
    if (measure) {
        timing->reduce_ms += std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - reduce_start)
                                 .count();
    }
#else
    const uint8_t* code_abs_ptrs[8] = {};
    const uint8_t* packed_sign_ptrs[8] = {};
    alignas(64) uint8_t abs_scratch[8][1536] = {};
    alignas(64) uint8_t sign_scratch[8][192] = {};
    for (uint32_t lane = 0; lane < valid_count; ++lane) {
        for (uint32_t db = 0; db < num_dim_blocks; ++db) {
            const uint32_t dim_start = db * dim_block;
            const uint32_t copy = std::min(dim_block, dim - dim_start);
            const uint8_t* abs_ptr =
                abs_blocks + static_cast<size_t>(db) * 8 * dim_block + lane * dim_block;
            std::memcpy(abs_scratch[lane] + dim_start, abs_ptr, copy);
            const uint8_t* sign_ptr = sign_blocks + static_cast<size_t>(db) * 8 * sign_block_bytes +
                                      lane * sign_block_bytes;
            std::memcpy(sign_scratch[lane] + dim_start / 8, sign_ptr, sign_block_bytes);
        }
        code_abs_ptrs[lane] = abs_scratch[lane];
        packed_sign_ptrs[lane] = sign_scratch[lane];
    }
    IPExRaBitQBatchPackedSign(query, code_abs_ptrs, packed_sign_ptrs, valid_count, dim, out_ip_raw);
#endif
}

void IPExRaBitQBatchPackedSignParallelCompact(const float* VDB_RESTRICT query,
                                              const uint8_t* VDB_RESTRICT abs_slices,
                                              const uint16_t* VDB_RESTRICT sign_words,
                                              uint32_t valid_count, Dim dim, uint32_t dim_block,
                                              uint32_t slices_per_dim_block,
                                              float* VDB_RESTRICT out_ip_raw,
                                              IPExRaBitQBatchPackedSignCompactTiming* timing) {
#if defined(VDB_USE_AVX512)
    const bool measure = timing != nullptr;
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    constexpr uint32_t kLaneBatch = 4;
    __m512 dot[8];
    __m512 bias[8];
    for (uint32_t lane = 0; lane < valid_count; ++lane) {
        dot[lane] = _mm512_setzero_ps();
        bias[lane] = _mm512_setzero_ps();
    }

    for (uint32_t db = 0; db < num_dim_blocks; ++db) {
        const uint32_t dim_start = db * dim_block;
        const uint32_t remaining = std::min(dim_block, dim - dim_start);
        const uint32_t full_slices = remaining / 16;
        for (uint32_t sub = 0; sub < full_slices; ++sub) {
            const __m512 q = _mm512_loadu_ps(query + dim_start + sub * 16u);
            const uint16_t* sign_base =
                sign_words + (static_cast<size_t>(db) * slices_per_dim_block + sub) * 8;
            const uint8_t* abs_base =
                abs_slices + ((static_cast<size_t>(db) * slices_per_dim_block + sub) * 8) * 16;
            uint32_t lane = 0;
            for (; lane + kLaneBatch <= valid_count; lane += kLaneBatch) {
                const auto sign_start = measure ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
                const __m512 signed_q0 =
                    FlipQuery16PackedChunkAvx512(q, static_cast<uint64_t>(sign_base[lane + 0]), 0);
                const __m512 signed_q1 =
                    FlipQuery16PackedChunkAvx512(q, static_cast<uint64_t>(sign_base[lane + 1]), 0);
                const __m512 signed_q2 =
                    FlipQuery16PackedChunkAvx512(q, static_cast<uint64_t>(sign_base[lane + 2]), 0);
                const __m512 signed_q3 =
                    FlipQuery16PackedChunkAvx512(q, static_cast<uint64_t>(sign_base[lane + 3]), 0);
                if (measure) {
                    timing->sign_flip_ms += std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - sign_start)
                                                .count();
                }
                const auto abs_start = measure ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
                dot[lane + 0] = _mm512_fmadd_ps(
                    signed_q0, LoadAbsMagnitude16Avx512(abs_base + (lane + 0) * 16), dot[lane + 0]);
                dot[lane + 1] = _mm512_fmadd_ps(
                    signed_q1, LoadAbsMagnitude16Avx512(abs_base + (lane + 1) * 16), dot[lane + 1]);
                dot[lane + 2] = _mm512_fmadd_ps(
                    signed_q2, LoadAbsMagnitude16Avx512(abs_base + (lane + 2) * 16), dot[lane + 2]);
                dot[lane + 3] = _mm512_fmadd_ps(
                    signed_q3, LoadAbsMagnitude16Avx512(abs_base + (lane + 3) * 16), dot[lane + 3]);
                bias[lane + 0] = _mm512_add_ps(bias[lane + 0], signed_q0);
                bias[lane + 1] = _mm512_add_ps(bias[lane + 1], signed_q1);
                bias[lane + 2] = _mm512_add_ps(bias[lane + 2], signed_q2);
                bias[lane + 3] = _mm512_add_ps(bias[lane + 3], signed_q3);
                if (measure) {
                    timing->abs_fma_ms += std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - abs_start)
                                              .count();
                }
            }
            for (; lane < valid_count; ++lane) {
                const auto sign_start = measure ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
                const __m512 sq =
                    FlipQuery16PackedChunkAvx512(q, static_cast<uint64_t>(sign_base[lane]), 0);
                if (measure) {
                    timing->sign_flip_ms += std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - sign_start)
                                                .count();
                }

                const auto abs_start = measure ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
                dot[lane] =
                    _mm512_fmadd_ps(sq, LoadAbsMagnitude16Avx512(abs_base + lane * 16), dot[lane]);
                bias[lane] = _mm512_add_ps(bias[lane], sq);
                if (measure) {
                    timing->abs_fma_ms += std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - abs_start)
                                              .count();
                }
            }
        }

        const uint32_t tail_start = full_slices * 16;
        for (uint32_t t = tail_start; t < remaining; ++t) {
            const auto tail_ts = measure ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
            const uint32_t sub = t / 16;
            const uint32_t offset = t % 16;
            const uint16_t* sign_base =
                sign_words + (static_cast<size_t>(db) * slices_per_dim_block + sub) * 8;
            const uint8_t* abs_base =
                abs_slices + ((static_cast<size_t>(db) * slices_per_dim_block + sub) * 8) * 16;
            for (uint32_t lane = 0; lane < valid_count; ++lane) {
                const bool positive = ((sign_base[lane] >> offset) & 1u) != 0;
                const float signed_q = positive ? query[dim_start + t] : -query[dim_start + t];
                out_ip_raw[lane] +=
                    signed_q * static_cast<float>(abs_base[lane * 16 + offset]) + 0.5f * signed_q;
            }
            if (measure) {
                timing->tail_ms += std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - tail_ts)
                                       .count();
            }
        }
    }

    const auto reduce_start =
        measure ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (uint32_t lane = 0; lane < valid_count; ++lane) {
        out_ip_raw[lane] +=
            _mm512_reduce_add_ps(dot[lane]) + 0.5f * _mm512_reduce_add_ps(bias[lane]);
    }
    if (measure) {
        timing->reduce_ms += std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - reduce_start)
                                 .count();
    }
#else
    (void)query;
    (void)abs_slices;
    (void)sign_words;
    (void)valid_count;
    (void)dim;
    (void)dim_block;
    (void)slices_per_dim_block;
    (void)out_ip_raw;
    (void)timing;
#endif
}

void IPExRaBitQBatchPackedSignParallelCompactMasked(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT abs_slices,
    const uint16_t* VDB_RESTRICT sign_words, uint32_t lane_mask, uint32_t valid_count, Dim dim,
    uint32_t dim_block, uint32_t slices_per_dim_block, float* VDB_RESTRICT out_ip_raw,
    IPExRaBitQBatchPackedSignCompactTiming* timing) {
    const uint32_t valid_mask = valid_count >= 32 ? 0xFFFFFFFFu : ((1u << valid_count) - 1u);
    lane_mask &= valid_mask;
    if (lane_mask == 0) {
        return;
    }
    if (lane_mask == valid_mask) {
        IPExRaBitQBatchPackedSignParallelCompact(query, abs_slices, sign_words, valid_count, dim,
                                                 dim_block, slices_per_dim_block, out_ip_raw,
                                                 timing);
        return;
    }
#if defined(VDB_USE_AVX512)
    const bool measure = timing != nullptr;
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    uint32_t lanes[8];
    uint32_t lane_count = 0;
    uint32_t m = lane_mask;
    while (m != 0) {
        const uint32_t lane = static_cast<uint32_t>(__builtin_ctz(m));
        lanes[lane_count++] = lane;
        m &= (m - 1u);
    }

    __m512 dot[8];
    __m512 bias[8];
    for (uint32_t i = 0; i < lane_count; ++i) {
        dot[i] = _mm512_setzero_ps();
        bias[i] = _mm512_setzero_ps();
    }

    for (uint32_t db = 0; db < num_dim_blocks; ++db) {
        const uint32_t dim_start = db * dim_block;
        const uint32_t remaining = std::min(dim_block, dim - dim_start);
        const uint32_t full_slices = remaining / 16;
        for (uint32_t sub = 0; sub < full_slices; ++sub) {
            const __m512 q = _mm512_loadu_ps(query + dim_start + sub * 16u);
            const uint16_t* sign_base =
                sign_words + (static_cast<size_t>(db) * slices_per_dim_block + sub) * 8;
            const uint8_t* abs_base =
                abs_slices + ((static_cast<size_t>(db) * slices_per_dim_block + sub) * 8) * 16;
            for (uint32_t i = 0; i < lane_count; ++i) {
                const uint32_t lane = lanes[i];
                const auto sign_start = measure ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
                const __m512 sq =
                    FlipQuery16PackedChunkAvx512(q, static_cast<uint64_t>(sign_base[lane]), 0);
                if (measure) {
                    timing->sign_flip_ms += std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - sign_start)
                                                .count();
                }

                const auto abs_start = measure ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
                dot[i] =
                    _mm512_fmadd_ps(sq, LoadAbsMagnitude16Avx512(abs_base + lane * 16), dot[i]);
                bias[i] = _mm512_add_ps(bias[i], sq);
                if (measure) {
                    timing->abs_fma_ms += std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - abs_start)
                                              .count();
                }
            }
        }

        const uint32_t tail_start = full_slices * 16;
        for (uint32_t t = tail_start; t < remaining; ++t) {
            const auto tail_ts = measure ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
            const uint32_t sub = t / 16;
            const uint32_t offset = t % 16;
            const uint16_t* sign_base =
                sign_words + (static_cast<size_t>(db) * slices_per_dim_block + sub) * 8;
            const uint8_t* abs_base =
                abs_slices + ((static_cast<size_t>(db) * slices_per_dim_block + sub) * 8) * 16;
            for (uint32_t i = 0; i < lane_count; ++i) {
                const uint32_t lane = lanes[i];
                const bool positive = ((sign_base[lane] >> offset) & 1u) != 0;
                const float signed_q = positive ? query[dim_start + t] : -query[dim_start + t];
                out_ip_raw[lane] +=
                    signed_q * static_cast<float>(abs_base[lane * 16 + offset]) + 0.5f * signed_q;
            }
            if (measure) {
                timing->tail_ms += std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - tail_ts)
                                       .count();
            }
        }
    }

    const auto reduce_start =
        measure ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (uint32_t i = 0; i < lane_count; ++i) {
        const uint32_t lane = lanes[i];
        out_ip_raw[lane] += _mm512_reduce_add_ps(dot[i]) + 0.5f * _mm512_reduce_add_ps(bias[i]);
    }
    if (measure) {
        timing->reduce_ms += std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - reduce_start)
                                 .count();
    }
#else
    IPExRaBitQBatchPackedSignParallelCompact(query, abs_slices, sign_words, valid_count, dim,
                                             dim_block, slices_per_dim_block, out_ip_raw, timing);
#endif
}

float IPOfficialRaBitQExData(const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT ex_code,
                             Dim dim) {
#if defined(VDB_USE_AVX512)
    float result = IPOfficialRaBitQExDataAvx512(query, ex_code, dim);
#elif defined(VDB_USE_AVX2)
    float result = IPOfficialRaBitQExDataAvx2(query, ex_code, dim);
#else
    float result = IPOfficialRaBitQExDataReference(query, ex_code, dim);
#endif
#ifndef NDEBUG
    const float ref = IPOfficialRaBitQExDataReference(query, ex_code, dim);
    if (std::abs(ref - result) > 1e-3f) {
        __builtin_trap();
    }
#endif
    return result;
}

void IPOfficialRaBitQBatchCompact(const float* VDB_RESTRICT query,
                                  const uint8_t* VDB_RESTRICT ex_code_blocks, uint32_t valid_count,
                                  Dim dim, uint32_t dim_block, float* VDB_RESTRICT out_ip_ex,
                                  IPExRaBitQBatchPackedSignCompactTiming* timing) {
    const bool measure = timing != nullptr;
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    for (uint32_t db = 0; db < num_dim_blocks; ++db) {
        const uint32_t dim_start = db * dim_block;
        const uint32_t remaining = std::min(dim_block, dim - dim_start);
        const uint8_t* block_base = ex_code_blocks + static_cast<size_t>(db) * 8u * dim_block;
        for (uint32_t lane = 0; lane < valid_count; ++lane) {
            const auto abs_start = measure ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
            out_ip_ex[lane] +=
                IPOfficialRaBitQExData(query + dim_start, block_base + lane * dim_block, remaining);
            if (measure) {
                timing->abs_fma_ms += std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - abs_start)
                                          .count();
            }
        }
    }
}

void IPOfficialRaBitQBatchCompactMasked(const float* VDB_RESTRICT query,
                                        const uint8_t* VDB_RESTRICT ex_code_blocks,
                                        uint32_t lane_mask, uint32_t valid_count, Dim dim,
                                        uint32_t dim_block, float* VDB_RESTRICT out_ip_ex,
                                        IPExRaBitQBatchPackedSignCompactTiming* timing) {
    const uint32_t valid_mask = valid_count >= 32 ? 0xFFFFFFFFu : ((1u << valid_count) - 1u);
    lane_mask &= valid_mask;
    if (lane_mask == 0) {
        return;
    }
    if (lane_mask == valid_mask) {
        IPOfficialRaBitQBatchCompact(query, ex_code_blocks, valid_count, dim, dim_block, out_ip_ex,
                                     timing);
        return;
    }

    const bool measure = timing != nullptr;
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    for (uint32_t db = 0; db < num_dim_blocks; ++db) {
        const uint32_t dim_start = db * dim_block;
        const uint32_t remaining = std::min(dim_block, dim - dim_start);
        const uint8_t* block_base = ex_code_blocks + static_cast<size_t>(db) * 8u * dim_block;
        uint32_t m = lane_mask;
        while (m != 0) {
            const uint32_t lane = static_cast<uint32_t>(__builtin_ctz(m));
            const auto abs_start = measure ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
            out_ip_ex[lane] +=
                IPOfficialRaBitQExData(query + dim_start, block_base + lane * dim_block, remaining);
            if (measure) {
                timing->abs_fma_ms += std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - abs_start)
                                          .count();
            }
            m &= (m - 1u);
        }
    }
}

void IPOfficialRaBitQBatchCompactDirect3Masked(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact_blocks,
    RaBitQExDataLayout layout, uint32_t lane_mask, uint32_t valid_count, Dim dim,
    uint32_t dim_block, float* VDB_RESTRICT out_ip_ex,
    IPExRaBitQBatchPackedSignCompactTiming* timing) {
    const RaBitQExDataLayout effective = RaBitQResolveSelectedExDataLayout(layout);
    if (compact_blocks == nullptr || dim_block != kOfficialDirect3DimBlock ||
        (effective != RaBitQExDataLayout::kSplit3TwoPlusOne &&
         effective != RaBitQExDataLayout::kSplit3Bitplanes)) {
        return;
    }
    const uint32_t valid_mask = valid_count >= 32 ? 0xFFFFFFFFu : ((1u << valid_count) - 1u);
    lane_mask &= valid_mask;
    if (lane_mask == 0) {
        return;
    }
    const bool measure = timing != nullptr;
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    for (uint32_t db = 0; db < num_dim_blocks; ++db) {
        const uint32_t dim_start = db * dim_block;
        const uint32_t remaining = std::min(dim_block, dim - dim_start);
        const uint8_t* block_base =
            compact_blocks +
            static_cast<size_t>(db) * 8u * kOfficialDirect3BytesPerLane;
        uint32_t m = lane_mask;
        while (m != 0) {
            const uint32_t lane = static_cast<uint32_t>(__builtin_ctz(m));
            const auto abs_start = measure ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
            const uint8_t* lane_compact =
                block_base + static_cast<size_t>(lane) * kOfficialDirect3BytesPerLane;
        #if defined(VDB_USE_AVX512)
            out_ip_ex[lane] +=
                IPOfficialDirect3Avx512(query + dim_start, lane_compact, effective, remaining);
        #else
            out_ip_ex[lane] +=
                IPOfficialDirect3Reference(query + dim_start, lane_compact, effective, remaining);
        #endif
            if (measure) {
                timing->abs_fma_ms += std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - abs_start)
                                          .count();
            }
            m &= (m - 1u);
        }
    }
}

void IPOfficialRaBitQBatchCompactDirectBitplanesMasked(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact_blocks, uint8_t bits,
    uint32_t lane_mask, uint32_t valid_count, Dim dim, uint32_t dim_block,
    float* VDB_RESTRICT out_ip_ex, IPExRaBitQBatchPackedSignCompactTiming* timing) {
    if (compact_blocks == nullptr || dim_block != kOfficialDirect3DimBlock || bits < 1 ||
        bits > 3) {
        return;
    }
    const uint32_t valid_mask = valid_count >= 32 ? 0xFFFFFFFFu : ((1u << valid_count) - 1u);
    lane_mask &= valid_mask;
    if (lane_mask == 0) {
        return;
    }
    const bool measure = timing != nullptr;
    const uint32_t bytes_per_lane = ExRaBitQPackedMagnitudeBytes(dim_block, bits);
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    for (uint32_t db = 0; db < num_dim_blocks; ++db) {
        const uint32_t dim_start = db * dim_block;
        const uint32_t remaining = std::min(dim_block, dim - dim_start);
        const uint8_t* block_base =
            compact_blocks + static_cast<size_t>(db) * 8u * bytes_per_lane;
        uint32_t m = lane_mask;
        while (m != 0) {
            const uint32_t lane = static_cast<uint32_t>(__builtin_ctz(m));
            const auto abs_start = measure ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
            const uint8_t* lane_compact =
                block_base + static_cast<size_t>(lane) * bytes_per_lane;
        #if defined(VDB_USE_AVX512)
            out_ip_ex[lane] += IPOfficialDirectBitplanesAvx512(
                query + dim_start, lane_compact, bits, remaining);
        #else
            out_ip_ex[lane] += IPOfficialDirectBitplanesReference(
                query + dim_start, lane_compact, bits, remaining);
        #endif
            if (measure) {
                timing->abs_fma_ms += std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - abs_start)
                                          .count();
            }
            m &= (m - 1u);
        }
    }
}

float IPExRaBitQ(const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT code_abs,
                 const uint8_t* VDB_RESTRICT sign, bool sign_packed, Dim dim) {
    float result;
    if (sign_packed) {
        result = IPExRaBitQPackedSign(query, code_abs, sign, dim);
    } else {
#if defined(VDB_USE_AVX2)
        result = IPExRaBitQGenericAvx2(query, code_abs, sign, false, dim);
#else
        result = IPExRaBitQReference(query, code_abs, sign, false, dim);
#endif
    }
#ifndef NDEBUG
    const float ref = IPExRaBitQReference(query, code_abs, sign, sign_packed, dim);
    if (std::abs(ref - result) > 1e-3f) {
        __builtin_trap();
    }
#endif
    return result;
}

} // namespace simd
} // namespace vdb
