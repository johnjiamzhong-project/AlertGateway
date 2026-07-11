#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "common/Frame.hpp"
#include "infer/YoloPostprocess.hpp"
#include "rknn_api.h"



// Rockchip Model Zoo YOLOv8 optimized-head postprocess.
// The model exposes three branches, each containing DFL box logits, 80 class
// scores, and a one-channel class-score sum used for early rejection.
class RockchipYoloPostprocess {
public:
    explicit RockchipYoloPostprocess(const YoloConfig& cfg);

    bool validate(const std::vector<rknn_tensor_attr>& attrs,
                  int model_width,
                  int model_height,
                  std::string* error = nullptr) const;

    std::vector<Detection> process(
        const std::vector<rknn_output>& outputs,
        const std::vector<rknn_tensor_attr>& attrs,
        int model_width,
        int model_height,
        int orig_width,
        int orig_height) const;

private:
    float iou(const Detection& a, const Detection& b) const;
    std::vector<Detection> nms(std::vector<Detection> candidates) const;

    YoloConfig cfg_;
    std::unordered_set<std::string> target_set_;
};
