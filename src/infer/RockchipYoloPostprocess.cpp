#include "infer/RockchipYoloPostprocess.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace {

constexpr int kClasses = 80;
constexpr int kBranches = 3;
constexpr int kOutputsPerBranch = 3;
constexpr int kExpectedOutputs = kBranches * kOutputsPerBranch;
constexpr int kMaxDflValues = 64;

float dequantize(int8_t value, const rknn_tensor_attr& attr) {
    return (static_cast<int32_t>(value) - attr.zp) * attr.scale;
}

int8_t quantize_i8(float value, const rknn_tensor_attr& attr) {
    float quantized = value / attr.scale + attr.zp;
    quantized = std::max(-128.0f, std::min(127.0f, quantized));
    return static_cast<int8_t>(quantized);
}

void compute_dfl(const float* logits, int dfl_len, float distances[4]) {
    for (int side = 0; side < 4; ++side) {
        const float* side_logits = logits + side * dfl_len;
        float max_logit = *std::max_element(side_logits, side_logits + dfl_len);
        float exp_sum = 0.0f;
        float weighted_sum = 0.0f;
        for (int i = 0; i < dfl_len; ++i) {
            float value = std::exp(side_logits[i] - max_logit);
            exp_sum += value;
            weighted_sum += value * i;
        }
        distances[side] = weighted_sum / exp_sum;
    }
}

bool fail_validation(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

}  // namespace

RockchipYoloPostprocess::RockchipYoloPostprocess(const YoloConfig& cfg) : cfg_(cfg) {
    for (const auto& target : cfg_.target_classes)
        target_set_.insert(target);
}

bool RockchipYoloPostprocess::validate(
    const std::vector<rknn_tensor_attr>& attrs,
    int model_width,
    int model_height,
    std::string* error) const {
    if (attrs.size() != kExpectedOutputs)
        return fail_validation(error, "expected 9 outputs, got " +
                                          std::to_string(attrs.size()));
    if (model_width <= 0 || model_height <= 0)
        return fail_validation(error, "invalid model input dimensions");

    for (int branch = 0; branch < kBranches; ++branch) {
        int box_index = branch * kOutputsPerBranch;
        const auto& box = attrs[box_index];
        const auto& score = attrs[box_index + 1];
        const auto& sum = attrs[box_index + 2];

        if (box.type != RKNN_TENSOR_INT8 || score.type != RKNN_TENSOR_INT8 ||
            sum.type != RKNN_TENSOR_INT8 ||
            box.qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC ||
            score.qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC ||
            sum.qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC)
            return fail_validation(error, "branch " + std::to_string(branch) +
                                              " is not affine INT8");
        if (box.n_dims != 4 || score.n_dims != 4 || sum.n_dims != 4)
            return fail_validation(error, "branch " + std::to_string(branch) +
                                              " is not four-dimensional");

        int grid_h = static_cast<int>(box.dims[2]);
        int grid_w = static_cast<int>(box.dims[3]);
        int dfl_values = static_cast<int>(box.dims[1]);
        if (grid_h <= 0 || grid_w <= 0 || dfl_values <= 0 ||
            dfl_values % 4 != 0 || dfl_values > kMaxDflValues)
            return fail_validation(error, "branch " + std::to_string(branch) +
                                              " has invalid DFL shape");
        if (score.dims[1] != kClasses || score.dims[2] != box.dims[2] ||
            score.dims[3] != box.dims[3] || sum.dims[1] != 1 ||
            sum.dims[2] != box.dims[2] || sum.dims[3] != box.dims[3])
            return fail_validation(error, "branch " + std::to_string(branch) +
                                              " tensor shapes do not match");
        if (model_height % grid_h != 0 || model_width % grid_w != 0 ||
            model_height / grid_h != model_width / grid_w)
            return fail_validation(error, "branch " + std::to_string(branch) +
                                              " has invalid stride");
    }
    return true;
}

std::vector<Detection> RockchipYoloPostprocess::process(
    const std::vector<rknn_output>& outputs,
    const std::vector<rknn_tensor_attr>& attrs,
    int model_width,
    int model_height,
    int orig_width,
    int orig_height) const {
    std::string error;
    if (outputs.size() != attrs.size())
        throw std::runtime_error("Rockchip postprocess output/attribute count mismatch");
    if (!validate(attrs, model_width, model_height, &error))
        throw std::runtime_error("Rockchip postprocess: " + error);

    float scale_x = static_cast<float>(orig_width) / model_width;
    float scale_y = static_cast<float>(orig_height) / model_height;
    std::vector<Detection> candidates;
    candidates.reserve(64);

    for (int branch = 0; branch < kBranches; ++branch) {
        int box_index = branch * kOutputsPerBranch;
        int score_index = box_index + 1;
        int sum_index = box_index + 2;
        const auto& box_attr = attrs[box_index];
        const auto& score_attr = attrs[score_index];
        const auto& sum_attr = attrs[sum_index];

        int grid_h = static_cast<int>(box_attr.dims[2]);
        int grid_w = static_cast<int>(box_attr.dims[3]);
        int grid_len = grid_h * grid_w;
        int dfl_len = static_cast<int>(box_attr.dims[1]) / 4;
        int stride = model_height / grid_h;
        const int8_t* box_tensor =
            static_cast<const int8_t*>(outputs[box_index].buf);
        const int8_t* score_tensor =
            static_cast<const int8_t*>(outputs[score_index].buf);
        const int8_t* sum_tensor =
            static_cast<const int8_t*>(outputs[sum_index].buf);
        int8_t score_threshold = quantize_i8(cfg_.conf_threshold, score_attr);
        int8_t sum_threshold = quantize_i8(cfg_.conf_threshold, sum_attr);

        for (int row = 0; row < grid_h; ++row) {
            for (int col = 0; col < grid_w; ++col) {
                int cell = row * grid_w + col;
                if (sum_tensor[cell] < sum_threshold) continue;

                int best_class = -1;
                int8_t best_score = -128;
                for (int class_id = 0; class_id < kClasses; ++class_id) {
                    int8_t score = score_tensor[class_id * grid_len + cell];
                    if (score > score_threshold && score > best_score) {
                        best_score = score;
                        best_class = class_id;
                    }
                }
                if (best_class < 0) continue;

                const char* label = COCO_LABELS[best_class];
                if (!target_set_.empty() &&
                    target_set_.find(label) == target_set_.end())
                    continue;

                std::array<float, kMaxDflValues> dfl_logits{};
                for (int i = 0; i < 4 * dfl_len; ++i)
                    dfl_logits[i] =
                        dequantize(box_tensor[i * grid_len + cell], box_attr);
                float distances[4];
                compute_dfl(dfl_logits.data(), dfl_len, distances);

                Detection det;
                det.x1 = (col + 0.5f - distances[0]) * stride * scale_x;
                det.y1 = (row + 0.5f - distances[1]) * stride * scale_y;
                det.x2 = (col + 0.5f + distances[2]) * stride * scale_x;
                det.y2 = (row + 0.5f + distances[3]) * stride * scale_y;
                det.class_id = best_class;
                det.label = label;
                det.score = dequantize(best_score, score_attr);
                candidates.push_back(std::move(det));
            }
        }
    }

    return nms(std::move(candidates));
}

float RockchipYoloPostprocess::iou(const Detection& a, const Detection& b) const {
    float x1 = std::max(a.x1, b.x1);
    float y1 = std::max(a.y1, b.y1);
    float x2 = std::min(a.x2, b.x2);
    float y2 = std::min(a.y2, b.y2);
    float intersection = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    if (intersection <= 0.0f) return 0.0f;
    float area_a = std::max(0.0f, a.x2 - a.x1) * std::max(0.0f, a.y2 - a.y1);
    float area_b = std::max(0.0f, b.x2 - b.x1) * std::max(0.0f, b.y2 - b.y1);
    return intersection / (area_a + area_b - intersection + 1e-6f);
}

std::vector<Detection> RockchipYoloPostprocess::nms(
    std::vector<Detection> candidates) const {
    std::sort(candidates.begin(), candidates.end(),
              [](const Detection& a, const Detection& b) {
                  return a.score > b.score;
              });
    std::vector<bool> suppressed(candidates.size(), false);
    std::vector<Detection> result;
    result.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (suppressed[i]) continue;
        result.push_back(candidates[i]);
        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (!suppressed[j] &&
                candidates[i].class_id == candidates[j].class_id &&
                iou(candidates[i], candidates[j]) > cfg_.iou_threshold)
                suppressed[j] = true;
        }
    }
    return result;
}
