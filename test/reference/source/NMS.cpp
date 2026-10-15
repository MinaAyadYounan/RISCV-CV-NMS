#include "NMS.hpp"

#include <algorithm>
#include <cstdint>

static int score_filter(const float* score_box, int score_box_size, const float* score_threshold, int* box_idx) {
    int j = 0;
    for (int i = 0; i < score_box_size; i++) {
        if (score_threshold == nullptr || score_box[i] >= *score_threshold) {
            box_idx[j] = i;
            j++;
        }
    }
    return j;
}

static void even_odd_sort(float* score_box, int* box_idx, int score_box_size) {
    if (score_box_size <= 1) return;
    bool odd_flag = true;
    bool even_flag = true;
    int i = 0;
    do {
        if (i % 2 == 0) {
            even_flag = false;
            for (int j = 0; j < score_box_size - 1; j += 2) {
                if (score_box[j] < score_box[j + 1]) {
                    even_flag = true;
                    int temp_idx = box_idx[j];
                    box_idx[j] = box_idx[j + 1];
                    box_idx[j + 1] = temp_idx;

                    float temp = score_box[j];
                    score_box[j] = score_box[j + 1];
                    score_box[j + 1] = temp;
                }
            }
        } else {
            odd_flag = false;
            for (int j = 1; j < score_box_size - 1; j += 2) {
                if (score_box[j] < score_box[j + 1]) {
                    odd_flag = true;
                    int temp_idx = box_idx[j];
                    box_idx[j] = box_idx[j + 1];
                    box_idx[j + 1] = temp_idx;

                    float temp = score_box[j];
                    score_box[j] = score_box[j + 1];
                    score_box[j + 1] = temp;
                }
            }
        }
        i++;
    } while (even_flag || odd_flag);
}

static float iou_calc(const float* box1, const float* box2) {
    float y1a = box1[0]; float y1b = box2[0];
    float x1a = box1[1]; float x1b = box2[1];
    float y2a = box1[2]; float y2b = box2[2];
    float x2a = box1[3]; float x2b = box2[3];
    float overlap_y = std::max(0.0f, std::min(y2a, y2b) - std::max(y1a, y1b));
    float overlap_x = std::max(0.0f, std::min(x2a, x2b) - std::max(x1a, x1b));
    float intersection = overlap_y * overlap_x;
    float area_a = (y2a - y1a) * (x2a - x1a);
    float area_b = (y2b - y1b) * (x2b - x1b);
    float union_area = area_a + area_b - intersection;
    if (union_area <= 0.0f) return 0.0f;
    float Iou = intersection / union_area;
    return Iou;
}

static void suppress(const float* box, const int* box_idx, float iou_threshold, int box_size, int* supp_box_idx) {
    for (int i = 0; i < box_size; i++) {
        supp_box_idx[i] = 0;
    }
    for (int i = 0; i < box_size; i++) {
        if (supp_box_idx[i] == 1) {
            continue;
        }
        for (int j = i + 1; j < box_size; j++) {
            if (supp_box_idx[j] == 1) {
                continue;
            }
            float iou = iou_calc(box + 4 * box_idx[i], box + 4 * box_idx[j]);
            if (iou > iou_threshold) {
                supp_box_idx[j] = 1;
            }
        }
    }
}

int ref::NonMaxSuppression(const float* boxes,
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
    float filtered_score[MAX_BOXES];
    int supp_box_idx[MAX_BOXES];
    int total_selected = 0;

    for (int b = 0; b < num_batches; b++) {
        const float* batch_boxes = boxes + b * (spatial_dim * 4);
        const float* batch_scores = scores + b * (num_classes * spatial_dim);

        for (int k = 0; k < num_classes; k++) {
            const float* class_scores = batch_scores + (k * spatial_dim);
            int filtered_box_len = score_filter(class_scores, spatial_dim, score_threshold, box_idx);

            for (int i = 0; i < filtered_box_len; i++) {
                filtered_score[i] = class_scores[box_idx[i]];
            }

            even_odd_sort(filtered_score, box_idx, filtered_box_len);
            suppress(batch_boxes, box_idx, iou_threshold, filtered_box_len, supp_box_idx);

            int64_t kept_in_class = 0;
            for (int i = 0; i < filtered_box_len; i++) {
                if (supp_box_idx[i] == 0) {
                    selected_indices[total_selected * 3 + 0] = b;
                    selected_indices[total_selected * 3 + 1] = k;
                    selected_indices[total_selected * 3 + 2] = box_idx[i];
                    total_selected++;
                    kept_in_class++;
                    if (kept_in_class == max_output_boxes_per_class) {
                        break;
                    }
                }
            }
        }
    }

    return total_selected;
}
