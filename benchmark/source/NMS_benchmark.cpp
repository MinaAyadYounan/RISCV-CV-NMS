#include "benchmark.hpp"
#include "reference_cv.hpp"
#include "riscv_cv.hpp"

#include <cstdint>

int main()
{
#ifdef RISCV_BAREMETAL
    riscv_enable_vector();
#endif

    const int box_count = 256;
    const int num_batches = 2;
    const int num_classes = 2;
    const int64_t max_output_boxes_per_class = 20;
    const float iou_threshold = 0.5f;
    const float score_threshold = 0.2f;
    const int loop_count = 1;

    printf("NMS benchmark: %d boxes, %d batches, %d classes, %d iterations\n",
           box_count, num_batches, num_classes, loop_count);

    static float boxes[num_batches * box_count * 4];
    static float scores[num_batches * num_classes * box_count];

    // Synthetic clusters of overlapping boxes
    for (int b = 0; b < num_batches; b++)
    {
        for (int i = 0; i < box_count; i++)
        {
            int cluster = i / 10;
            float base_y = static_cast<float>(cluster * 20);
            float base_x = static_cast<float>(cluster * 20);
            float offset = static_cast<float>(i % 10) * 0.3f;
            int b_idx = (b * box_count + i) * 4;
            boxes[b_idx + 0] = base_y + offset;
            boxes[b_idx + 1] = base_x + offset;
            boxes[b_idx + 2] = base_y + offset + 10.0f;
            boxes[b_idx + 3] = base_x + offset + 10.0f;
        }

        for (int k = 0; k < num_classes; k++)
        {
            for (int i = 0; i < box_count; i++)
            {
                int s_idx = (b * num_classes + k) * box_count + i;
                scores[s_idx] = 1.0f - static_cast<float>(i % 10) * 0.08f;
            }
        }
    }

    constexpr size_t max_out_rows = static_cast<size_t>(num_batches * num_classes * max_output_boxes_per_class);
    static int64_t actual[max_out_rows * 3];
    static int64_t expected[max_out_rows * 3];

    uint64_t reference_cycles = 0, vectorized_cycles = 0;
    uint64_t reference_instrs = 0, vectorized_instrs = 0;
    bool all_correct = true;

    for (int i = 0; i < loop_count; ++i)
    {
        Timer timer_reference, timer_vectorized;

        timer_reference.Start();
        const int expected_count = ref::NonMaxSuppression(
            boxes, scores, num_batches, num_classes, box_count,
            max_output_boxes_per_class, iou_threshold, &score_threshold,
            CenterPointBox::Corners, expected);
        timer_reference.Stop();

        timer_vectorized.Start();
        const int actual_count = vec::NonMaxSuppression(
            boxes, scores, num_batches, num_classes, box_count,
            max_output_boxes_per_class, iou_threshold, &score_threshold,
            CenterPointBox::Corners, actual);
        timer_vectorized.Stop();

        reference_cycles += timer_reference.ElapsedCycles();
        reference_instrs += timer_reference.ElapsedInstructions();

        vectorized_cycles += timer_vectorized.ElapsedCycles();
        vectorized_instrs += timer_vectorized.ElapsedInstructions();

        if (actual_count != expected_count)
        {
            all_correct = false;
            break;
        }

        for (int j = 0; j < actual_count * 3; ++j)
        {
            if (actual[j] != expected[j])
            {
                all_correct = false;
                break;
            }
        }

        if (!all_correct)
        {
            break;
        }
    }

    char label[80];
    snprintf(label, sizeof(label), "NMS (%d boxes, %d batches, %d classes)",
             box_count, num_batches, num_classes);

    PrintTime(label,
              reference_cycles / loop_count, vectorized_cycles / loop_count,
              reference_instrs / loop_count, vectorized_instrs / loop_count);

    if (all_correct)
    {
        printf("Output is correct.\n");
        return 0;
    }
    else
    {
        printf("Output is wrong!\n");
        return 1;
    }
}
