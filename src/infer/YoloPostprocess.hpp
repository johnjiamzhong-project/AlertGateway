#pragma once
#include <vector>
#include <string>
#include <unordered_set>
#include "common/Frame.hpp"

// COCO 80-class labels, indexed 0-79 matching YOLOv8 model output
static constexpr const char* COCO_LABELS[80] = {
    "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat",
    "traffic light","fire hydrant","stop sign","parking meter","bench","bird",
    "cat","dog","horse","sheep","cow","elephant","bear","zebra","giraffe",
    "backpack","umbrella","handbag","tie","suitcase","frisbee","skis",
    "snowboard","sports ball","kite","baseball bat","baseball glove","skateboard",
    "surfboard","tennis racket","bottle","wine glass","cup","fork","knife",
    "spoon","bowl","banana","apple","sandwich","orange","broccoli","carrot",
    "hot dog","pizza","donut","cake","chair","couch","potted plant","bed",
    "dining table","toilet","tv","laptop","mouse","remote","keyboard",
    "cell phone","microwave","oven","toaster","sink","refrigerator","book",
    "clock","vase","scissors","teddy bear","hair drier","toothbrush"
};

struct YoloConfig {
    float conf_threshold   = 0.25f;
    float iou_threshold    = 0.45f;
    int   model_input_size = 640;
    std::vector<std::string> target_classes;
};

class YoloPostprocess {
public:
    explicit YoloPostprocess(const YoloConfig& cfg);

    // Box and class are separate RKNN outputs (split before the final concat at
    // export time so each gets its own quantization scale -- a single shared scale
    // across box pixel coords (0-640) and class probabilities (0-1) destroys the
    // class precision). Both are row-major float32 [channels][n_anchors]:
    //   box[0..3][a]  : cx, cy, w, h in model_input_size pixel space
    //   cls[0..79][a] : class probabilities (already past Sigmoid in the ONNX graph)
    // Returns detections in original frame coordinate space.
    std::vector<Detection> process(const float* box, const float* cls, int n_anchors,
                                   int orig_w, int orig_h) const;

private:
    float iou(const Detection& a, const Detection& b) const;
    std::vector<Detection> nms(std::vector<Detection> dets) const;

    YoloConfig cfg_;
    std::unordered_set<std::string> target_set_;
};
