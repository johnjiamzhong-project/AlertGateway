#include "infer/YoloPostprocess.hpp"
#include <algorithm>
#include <cmath>

// 把构造时传入的 target_classes（vector<string>）转成 unordered_set，
// 这样 process() 里每个anchor过滤目标类别时是 O(1) 查找，不用每次线性扫vector。
// target_classes 为空表示不过滤，process() 里据此判断。
YoloPostprocess::YoloPostprocess(const YoloConfig& cfg) : cfg_(cfg) {
    for (const auto& c : cfg_.target_classes)
        target_set_.insert(c);
}

// 核心解码函数，InferThread::run() 每次 rknn_run 拿到原始输出后调用一次。
// box/cls 是 InferThread 里 zero-copy 取出的两路独立量化输出，row-major float32：
//   box[0..3][a]  : 第a个anchor的 cx, cy, w, h，单位是 model_input_size(640) 像素空间
//   cls[0..79][a] : 第a个anchor对80个COCO类别各自的概率（导出ONNX图里已经过Sigmoid）
// 处理流程：遍历每个anchor → 取最高分类别 → 按conf_threshold过滤 → 按target_classes过滤
//          → 把box坐标从640空间换算回原始帧(orig_w×orig_h)像素坐标 → 最后统一做NMS去重叠框。
std::vector<Detection> YoloPostprocess::process(const float* box, const float* cls, int n_anchors,
                                                  int orig_w, int orig_h) const {
    constexpr int NUM_CLASSES = 80;
    // sx/sy: 640模型输入空间 → 原始帧分辨率的换算比例（模型输入是整图拉伸缩放，不是letterbox，
    // 所以x/y两个方向的缩放比例可以不同，要分开算）
    float sx = static_cast<float>(orig_w) / cfg_.model_input_size;
    float sy = static_cast<float>(orig_h) / cfg_.model_input_size;

    std::vector<Detection> candidates;
    candidates.reserve(64);  // 经验值，预留空间减少前几次push_back的重新分配

    for (int a = 0; a < n_anchors; a++) {
        // cls 在导出的ONNX图里已经过 Sigmoid，这里直接是 [0,1] 概率，不能再套一次sigmoid
        float best_score = -1.0f;
        int   best_class = -1;
        for (int c = 0; c < NUM_CLASSES; c++) {
            float score = cls[c * n_anchors + a];
            if (score > best_score) { best_score = score; best_class = c; }
        }

        if (best_score < cfg_.conf_threshold) continue;

        const char* label = COCO_LABELS[best_class];
        if (!target_set_.empty() && target_set_.find(label) == target_set_.end()) continue;

        // box存的是中心点(cx,cy)+宽高(w,h)，下面转成左上/右下角坐标(x1,y1,x2,y2)，
        // 同时乘 sx/sy 把640模型空间坐标换算回原始帧像素坐标
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

// 计算两个检测框的IoU（交并比）：交集面积 / (a面积+b面积-交集面积)。
// 交集为0时直接返回0，跳过除法（同时也避免两个框都退化成0面积时除0）。
// 分母加 1e-6f 是防止极端情况下分母仍接近0导致数值不稳定。
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

// 非极大值抑制：对同一个物体，模型往往在相邻多个anchor上都给出高分检测框，
// 这里要把这些重叠的框去重，只保留分数最高的那个。
// 做法：按score从高到低排序 → 从最高分开始依次保留 → 同类别且IoU超过iou_threshold的
// 后续框标记为suppressed（认为是同一个物体的重复框，丢弃）→ 跳过被抑制的框继续找下一个保留项。
// 注意只对相同 class_id 的框互相抑制——不同类别框允许重叠（比如人骑车，人和车的框天然重叠）。
// 时间复杂度O(n^2)，n是过滤阈值后剩下的候选框数（一般几十个量级），够用。
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
