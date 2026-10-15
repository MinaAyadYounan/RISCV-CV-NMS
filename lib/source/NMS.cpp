#include "NMS.hpp"
#include "riscv_wrappers.hpp"

#include <cstddef>
#include <cstdint>

inline int score_filter(const float *score_box, size_t score_box_size, 
                        const float *score_threshold, int *box_idx, float *packed_scores) {
    int candidate_count = 0;
    size_t remaining = score_box_size;
    size_t offset = 0;

    while (remaining > 0) {
        const size_t vl = rvv::SetVlF32M4(remaining);
        rvv::U32M4 iota = rvv::IotaU32M4(vl);
        rvv::U32M4 indices = rvv::AddU32M4(iota, static_cast<uint32_t>(offset), vl);
        rvv::F32M4 scores = rvv::LoadF32M4(score_box + offset, vl);

        if (score_threshold != nullptr) {
            rvv::MaskF32M4 keep = rvv::MaskF32GreaterEqualScalarM4(scores, *score_threshold, vl);
            rvv::U32M4 packed_ids = rvv::CompressU32M4(indices, keep, vl);
            rvv::F32M4 packed = rvv::CompressF32M4(scores, keep, vl);
            const size_t kept = rvv::CountTrueF32M4(keep, vl);

            rvv::StoreI32M4(box_idx + candidate_count, rvv::BitcastU32ToI32M4(packed_ids), kept);
            rvv::StoreF32M4(packed_scores + candidate_count, packed, kept);
            candidate_count += static_cast<int>(kept);
        } else {
            rvv::StoreI32M4(box_idx + candidate_count, rvv::BitcastU32ToI32M4(indices), vl);
            rvv::StoreF32M4(packed_scores + candidate_count, scores, vl);
            candidate_count += static_cast<int>(vl);
        }

        offset += vl;
        remaining -= vl;
    }
    return candidate_count;
}

inline void even_odd_sort(int* box_idx, float* packed_scores, int K) {
    if (K <= 1) return;

    constexpr int HALF_MAX = 128 + 32;
    float even_scores[HALF_MAX];
    float odd_scores[HALF_MAX];
    int   even_idx[HALF_MAX];
    int   odd_idx[HALF_MAX];

    const int half_k = K / 2;
    size_t split_remaining = static_cast<size_t>(half_k);
    size_t split_offset = 0;
    while (split_remaining > 0) {
        const size_t vl = rvv::SetVlF32M4(split_remaining);
        rvv::DeinterleaveF32M4(packed_scores + 2 * split_offset,
                               even_scores + split_offset,
                               odd_scores + split_offset, vl);
        rvv::DeinterleaveI32M4(box_idx + 2 * split_offset,
                               even_idx + split_offset,
                               odd_idx + split_offset, vl);
        split_offset += vl;
        split_remaining -= vl;
    }
    if (K & 1) {
        even_scores[half_k] = packed_scores[K - 1];
        even_idx[half_k]    = box_idx[K - 1];
    }

    bool any_swapped = true;

    for (int phase = 0; phase < K; ++phase) {
        const int start = phase & 1; // 0 for even phase, 1 for odd phase
        size_t remaining_pairs = static_cast<size_t>(K - start) / 2;
        size_t pair_offset = 0;
        size_t phase_swaps = 0;

        float* left_scores  = (start == 0) ? even_scores : odd_scores;
        float* right_scores = (start == 0) ? odd_scores  : (even_scores + 1);
        int*   left_idx     = (start == 0) ? even_idx    : odd_idx;
        int*   right_idx    = (start == 0) ? odd_idx     : (even_idx + 1);

        while (remaining_pairs > 0) {
            const size_t vl = rvv::SetVlF32M4(remaining_pairs);

            rvv::F32M4 left_score  = rvv::LoadF32M4(left_scores  + pair_offset, vl);
            rvv::F32M4 right_score = rvv::LoadF32M4(right_scores + pair_offset, vl);
            rvv::I32M4 left_index  = rvv::LoadI32M4(left_idx     + pair_offset, vl);
            rvv::I32M4 right_index = rvv::LoadI32M4(right_idx    + pair_offset, vl);

            rvv::MaskF32M4 should_swap = rvv::MaskF32LessThanM4(left_score, right_score, vl);
            const size_t swaps_in_strip = rvv::CountTrueF32M4(should_swap, vl);

            if (swaps_in_strip > 0) {
                phase_swaps += swaps_in_strip;

                rvv::F32M4 new_left_score  = rvv::MergeF32M4(left_score,  right_score, should_swap, vl);
                rvv::F32M4 new_right_score = rvv::MergeF32M4(right_score, left_score,  should_swap, vl);
                rvv::I32M4 new_left_index  = rvv::MergeI32M4(left_index,  right_index, should_swap, vl);
                rvv::I32M4 new_right_index = rvv::MergeI32M4(right_index, left_index,  should_swap, vl);

                rvv::StoreF32M4(left_scores  + pair_offset, new_left_score,  vl);
                rvv::StoreF32M4(right_scores + pair_offset, new_right_score, vl);
                rvv::StoreI32M4(left_idx     + pair_offset, new_left_index,  vl);
                rvv::StoreI32M4(right_idx    + pair_offset, new_right_index, vl);
            }

            pair_offset += vl;
            remaining_pairs -= vl;
        }

        if (start == 0) {
            any_swapped = (phase_swaps > 0);
        } else {
            if (!any_swapped && phase_swaps == 0) {
                break;
            }
        }
    }

    size_t merge_remaining = static_cast<size_t>(half_k);
    size_t merge_offset = 0;
    while (merge_remaining > 0) {
        const size_t vl = rvv::SetVlF32M4(merge_remaining);
        rvv::InterleaveF32M4(packed_scores + 2 * merge_offset,
                             even_scores + merge_offset,
                             odd_scores + merge_offset, vl);
        rvv::InterleaveI32M4(box_idx + 2 * merge_offset,
                             even_idx + merge_offset,
                             odd_idx + merge_offset, vl);
        merge_offset += vl;
        merge_remaining -= vl;
    }
    if (K & 1) {
        box_idx[K - 1]       = even_idx[half_k];
        packed_scores[K - 1] = even_scores[half_k];
    }
}

inline void SOA(const float* boxes, const int* box_idx,
                float* ymin, float* xmin, float* ymax, float* xmax,
                uint8_t* alive, size_t candidate_count) {
    size_t remaining = candidate_count;
    size_t offset = 0;
    while (remaining > 0) {
        const size_t vl = rvv::SetVlF32M4(remaining);
        rvv::I32M4 indices = rvv::LoadI32M4(box_idx + offset, vl);
        rvv::U32M4 byte_offsets = rvv::ShiftLeftU32M4(rvv::BitcastI32ToU32M4(indices), 4, vl);

        rvv::StoreF32M4(ymin + offset, rvv::GatherF32M4(boxes, byte_offsets, vl), vl);
        rvv::StoreF32M4(xmin + offset, rvv::GatherF32M4(boxes, rvv::AddU32M4(byte_offsets, 4, vl), vl), vl);
        rvv::StoreF32M4(ymax + offset, rvv::GatherF32M4(boxes, rvv::AddU32M4(byte_offsets, 8, vl), vl), vl);
        rvv::StoreF32M4(xmax + offset, rvv::GatherF32M4(boxes, rvv::AddU32M4(byte_offsets, 12, vl), vl), vl);

        offset += vl;
        remaining -= vl;
    }
    rvv::FillU8(alive, 1, candidate_count);
}

inline int next_alive_index(const uint8_t* alive, size_t start, size_t K) {
    size_t remaining = K - start;
    size_t offset = start;
    while (remaining > 0) {
        const size_t vl = rvv::SetVlF32M2(remaining);
        rvv::U8Mf2 flags = rvv::LoadU8Mf2(alive + offset, vl);
        rvv::MaskF32M2 is_alive = rvv::MaskU8NotEqualScalarMf2(flags, 0, vl);
        const int first = rvv::FirstTrueM2(is_alive, vl);
        if (first >= 0) {
            return static_cast<int>(offset + static_cast<size_t>(first));
        }
        offset += vl;
        remaining -= vl;
    }
    return -1;
}

inline void suppress_overlaps(int kept_idx, size_t K,
                              const float* ymin, const float* xmin,
                              const float* ymax, const float* xmax,
                              uint8_t* alive, float iou_threshold) {
    const float kept_ymin = ymin[kept_idx];
    const float kept_xmin = xmin[kept_idx];
    const float kept_ymax = ymax[kept_idx];
    const float kept_xmax = xmax[kept_idx];
    const float kept_area = (kept_ymax - kept_ymin) * (kept_xmax - kept_xmin);

    size_t start = static_cast<size_t>(kept_idx + 1);
    size_t remaining = K - start;
    size_t offset = start;

    while (remaining > 0) {
        const size_t vl = rvv::SetVlF32M2(remaining);
        rvv::U8Mf2 flags = rvv::LoadU8Mf2(alive + offset, vl);
        rvv::MaskF32M2 is_alive = rvv::MaskU8NotEqualScalarMf2(flags, 0, vl);

        if (rvv::FirstTrueM2(is_alive, vl) >= 0) {
            rvv::F32M2 cand_ymin = rvv::LoadF32M2(ymin + offset, vl);
            rvv::F32M2 cand_xmin = rvv::LoadF32M2(xmin + offset, vl);
            rvv::F32M2 cand_ymax = rvv::LoadF32M2(ymax + offset, vl);
            rvv::F32M2 cand_xmax = rvv::LoadF32M2(xmax + offset, vl);

            rvv::F32M2 inter_ymin = rvv::MaxF32ScalarM2(cand_ymin, kept_ymin, vl);
            rvv::F32M2 inter_xmin = rvv::MaxF32ScalarM2(cand_xmin, kept_xmin, vl);
            rvv::F32M2 inter_ymax = rvv::MinF32ScalarM2(cand_ymax, kept_ymax, vl);
            rvv::F32M2 inter_xmax = rvv::MinF32ScalarM2(cand_xmax, kept_xmax, vl);

            rvv::F32M2 h = rvv::MaxF32ScalarM2(rvv::SubF32M2(inter_ymax, inter_ymin, vl), 0.0f, vl);
            rvv::F32M2 w = rvv::MaxF32ScalarM2(rvv::SubF32M2(inter_xmax, inter_xmin, vl), 0.0f, vl);
            rvv::F32M2 intersection = rvv::MulF32M2(h, w, vl);

            rvv::F32M2 cand_h = rvv::SubF32M2(cand_ymax, cand_ymin, vl);
            rvv::F32M2 cand_w = rvv::SubF32M2(cand_xmax, cand_xmin, vl);
            rvv::F32M2 cand_area = rvv::MulF32M2(cand_h, cand_w, vl);

            rvv::F32M2 total_area = rvv::AddF32ScalarM2(cand_area, kept_area, vl);
            rvv::F32M2 union_area = rvv::SubF32M2(total_area, intersection, vl);

            rvv::MaskF32M2 valid = rvv::MaskF32GreaterThanScalarM2(union_area, 0.0f, vl);
            rvv::F32M2 safe_union = rvv::MergeF32M2(rvv::SplatF32M2(1.0f, vl), union_area, valid, vl);
            rvv::F32M2 iou = rvv::DivF32M2(intersection, safe_union, vl);

            rvv::MaskF32M2 over_thresh = rvv::MaskF32GreaterThanScalarM2(iou, iou_threshold, vl);
            rvv::MaskF32M2 kill = rvv::AndMaskM2(is_alive, over_thresh, vl);

            rvv::StoreU8Mf2Masked(alive + offset, rvv::SplatU8Mf2(0, vl), kill, vl);
        }

        offset += vl;
        remaining -= vl;
    }
}

int vec::NonMaxSuppression(const float* boxes,
                           const float* scores,
                           int num_batches,
                           int num_classes,
                           int spatial_dim,
                           int64_t max_output_boxes_per_class,
                           float iou_threshold,
                           const float* score_threshold,
                           CenterPointBox center_point_box,
                           int64_t* selected_indices)
{
    (void)center_point_box;
    if (num_batches <= 0 || num_classes <= 0 || spatial_dim <= 0 || max_output_boxes_per_class <= 0) {
        return 0;
    }

    constexpr int MAX_BOXES = 256;
    int box_idx[MAX_BOXES];
    float packed_scores[MAX_BOXES];
    float ymin[MAX_BOXES];
    float xmin[MAX_BOXES];
    float ymax[MAX_BOXES];
    float xmax[MAX_BOXES];
    uint8_t alive[MAX_BOXES];
    int total_selected = 0;

    for (int b = 0; b < num_batches; b++) {
        const float* batch_boxes = boxes + b * (spatial_dim * 4);
        const float* batch_scores = scores + b * (num_classes * spatial_dim);

        for (int k = 0; k < num_classes; k++) {
            const float* class_scores = batch_scores + (k * spatial_dim);

            int candidate_count = score_filter(class_scores, static_cast<size_t>(spatial_dim),
                                               score_threshold, box_idx, packed_scores);
            if (candidate_count == 0) {
                continue;
            }

            even_odd_sort(box_idx, packed_scores, candidate_count);

            SOA(batch_boxes, box_idx, ymin, xmin, ymax, xmax, alive,
                static_cast<size_t>(candidate_count));

            int64_t kept_in_class = 0;
            size_t search_start = 0;

            while (kept_in_class < max_output_boxes_per_class) {
                int next_idx = next_alive_index(alive, search_start, static_cast<size_t>(candidate_count));
                if (next_idx < 0) {
                    break;
                }

                selected_indices[total_selected * 3 + 0] = b;
                selected_indices[total_selected * 3 + 1] = k;
                selected_indices[total_selected * 3 + 2] = box_idx[next_idx];
                total_selected++;
                kept_in_class++;

                alive[next_idx] = 0;

                if (kept_in_class == max_output_boxes_per_class) {
                    break;
                }

                suppress_overlaps(next_idx, static_cast<size_t>(candidate_count),
                                  ymin, xmin, ymax, xmax, alive, iou_threshold);

                search_start = static_cast<size_t>(next_idx + 1);
            }
        }
    }

    return total_selected;
}
