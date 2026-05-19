#include "vdb/simd/stage2_classify.h"

#include <algorithm>

#if defined(VDB_USE_AVX2) || defined(VDB_USE_AVX512)
#include <immintrin.h>
#endif

namespace vdb {
namespace simd {

Stage2ClassifyMasks Stage2ClassifyBatch(const float* VDB_RESTRICT ip_raw,
                                        const float* VDB_RESTRICT xipnorm,
                                        const float* VDB_RESTRICT norm_oc,
                                        const float* VDB_RESTRICT margin_s1,
                                        uint32_t active_mask,
                                        float norm_qc,
                                        float norm_qc_sq,
                                        float inv_margin_s2_divisor,
                                        float safein_dk,
                                        float dynamic_d_k) {
    Stage2ClassifyMasks masks{};
#if defined(VDB_USE_AVX2)
    const __m256 v_ip_raw = _mm256_load_ps(ip_raw);
    const __m256 v_xipnorm = _mm256_load_ps(xipnorm);
    const __m256 v_norm_oc = _mm256_load_ps(norm_oc);
    const __m256 v_margin_s1 = _mm256_load_ps(margin_s1);
    const __m256 v_norm_qc_sq = _mm256_set1_ps(norm_qc_sq);
    const __m256 v_two_norm_qc = _mm256_set1_ps(2.0f * norm_qc);
    const __m256 v_zero = _mm256_setzero_ps();
    const __m256 v_margin_mul = _mm256_set1_ps(2.0f * inv_margin_s2_divisor);
    const __m256 v_dynamic_dk = _mm256_set1_ps(dynamic_d_k);
    const __m256 v_safein_dk = _mm256_set1_ps(safein_dk);

    const __m256 v_ip_est = _mm256_mul_ps(v_ip_raw, v_xipnorm);
    __m256 v_est_dist = _mm256_add_ps(_mm256_mul_ps(v_norm_oc, v_norm_oc), v_norm_qc_sq);
    v_est_dist = _mm256_sub_ps(
        v_est_dist,
        _mm256_mul_ps(_mm256_mul_ps(v_two_norm_qc, v_norm_oc), v_ip_est));
    v_est_dist = _mm256_max_ps(v_est_dist, v_zero);

    const __m256 v_margin_twice = _mm256_mul_ps(v_margin_s1, v_margin_mul);
    const __m256 v_safeout_th = _mm256_add_ps(v_dynamic_dk, v_margin_twice);
    const __m256 v_safein_th = _mm256_sub_ps(v_safein_dk, v_margin_twice);

    masks.safeout = static_cast<uint32_t>(_mm256_movemask_ps(
                        _mm256_cmp_ps(v_est_dist, v_safeout_th, _CMP_GT_OQ))) &
                    active_mask;
    masks.safein = static_cast<uint32_t>(_mm256_movemask_ps(
                       _mm256_cmp_ps(v_est_dist, v_safein_th, _CMP_LT_OQ))) &
                   active_mask & ~masks.safeout;
    masks.uncertain = active_mask & ~masks.safeout & ~masks.safein;
#else
    for (uint32_t lane = 0; lane < 8; ++lane) {
        const uint32_t lane_bit = 1u << lane;
        if ((active_mask & lane_bit) == 0) continue;
        const float ip_est = ip_raw[lane] * xipnorm[lane];
        float est_dist = norm_oc[lane] * norm_oc[lane] + norm_qc_sq -
                         2.0f * norm_oc[lane] * norm_qc * ip_est;
        est_dist = std::max(est_dist, 0.0f);
        const float margin_twice = 2.0f * margin_s1[lane] * inv_margin_s2_divisor;
        if (est_dist > dynamic_d_k + margin_twice) {
            masks.safeout |= lane_bit;
        } else if (est_dist < safein_dk - margin_twice) {
            masks.safein |= lane_bit;
        } else {
            masks.uncertain |= lane_bit;
        }
    }
#endif
    return masks;
}

}  // namespace simd
}  // namespace vdb
