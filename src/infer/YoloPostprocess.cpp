#include "infer/YoloPostprocess.hpp"
#include <algorithm>
#include <cmath>

YoloPostprocess::YoloPostprocess(const YoloConfig& cfg) : cfg_(cfg) {
    for (const auto& c : cfg_.target_classes)
        target_set_.insert(c);
}

std::vector<Detection> YoloPostprocess::process(const float* box, const float* cls, int n_anchors,
                                                  int orig_w, int orig_h) const {
    constexpr int NUM_CLASSES = 80;
    float sx = static_cast<float>(orig_w) / cfg_.model_input_size;
    float sy = static_cast<float>(orig_h) / cfg_.model_input_size;

    std::vector<Detection> candidates;
    candidates.reserve(64);

    for (int a = 0; a < n_anchors; a++) {
        // cls is already past Sigmoid in the exported ONNX graph -- these are
        // probabilities in [0,1] already, do not apply sigmoid again.
        float best_score = -1.0f;
        int   best_class = -1;
        for (int c = 0; c < NUM_CLASSES; c++) {
            float score = cls[c * n_anchors + a];
            if (score > best_score) { best_score = score; best_class = c; }
        }

        if (best_score < cfg_.conf_threshold) continue;

        const char* label = COCO_LABELS[best_class];
        if (!target_set_.empty() && target_set_.find(label) == target_set_.end()) continue;

        float cx = box[0 * n_anchors + a];
        float cy = box[1 * n_anchors + a];
        float w  = box[2 * n_anchors + a];
        float h  = box[3 * n_anchors + a];

        Detection det;
        det.x1       = (cx - w * 0.5f) * sx;
        det.y1       = (cy - h * 0.5f) * sy;
        det.x2       = (cx + w * 0.5f) * sx;
        det.y2       = (cy + h * 0.5f) * sy;
        det.class_id = best_class;
        det.label    = label;
        det.score    = best_score;
        candidates.push_back(det);
    }

    return nms(std::move(candidates));
}

float YoloPostprocess::iou(const Detection& a, const Detection& b) const {
    float ix1 = std::max(a.x1, b.x1);
    float iy1 = std::max(a.y1, b.y1);
    float ix2 = std::min(a.x2, b.x2);
    float iy2 = std::min(a.y2, b.y2);
    float inter = std::max(0.0f, ix2 - ix1) * std::max(0.0f, iy2 - iy1);
    if (inter == 0.0f) return 0.0f;
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    return inter / (area_a + area_b - inter + 1e-6f);
}

std::vector<Detection> YoloPostprocess::nms(std::vector<Detection> dets) const {
    std::sort(dets.begin(), dets.end(),
              [](const Detection& a, const Detection& b) { return a.score > b.score; });

    std::vector<bool>      suppressed(dets.size(), false);
    std::vector<Detection> result;

    for (size_t i = 0; i < dets.size(); i++) {
        if (suppressed[i]) continue;
        result.push_back(dets[i]);
        for (size_t j = i + 1; j < dets.size(); j++) {
            if (!suppressed[j] &&
                dets[i].class_id == dets[j].class_id &&
                iou(dets[i], dets[j]) > cfg_.iou_threshold)
                suppressed[j] = true;
        }
    }
    return result;
}
