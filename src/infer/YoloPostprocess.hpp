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

// YOLOv8 后处理：把NPU输出的原始 box/cls 张量解码成检测框，按置信度过滤、NMS去重，
// 坐标映射回原始帧尺寸。详细的张量布局和算法细节见 .cpp 各函数上方注释。
class YoloPostprocess {
public:
    explicit YoloPostprocess(const YoloConfig& cfg);

    // 解码+过滤+NMS，返回原始帧坐标系下的检测框列表，详见 .cpp
    std::vector<Detection> process(const float* box, const float* cls, int n_anchors,
                                   int orig_w, int orig_h) const;

private:
    float iou(const Detection& a, const Detection& b) const;          // 计算两个框的IoU，详见 .cpp
    std::vector<Detection> nms(std::vector<Detection> dets) const;    // 非极大值抑制去重叠框，详见 .cpp

    YoloConfig cfg_;
    std::unordered_set<std::string> target_set_;  // target_classes 转成set，方便process()里O(1)查找
};
