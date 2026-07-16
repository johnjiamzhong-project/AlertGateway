#pragma once
#include <vector>
#include <string>
#include <cstdint>

// 帧像素格式：YUYV (V4L2 采集) 或 NV12 (FFmpeg 解码)
enum class PixelFormat {
    YUYV,
    NV12
};


// 单个目标的检测结果。由 InferThread 通过 YoloPostprocess 生成，
// 存入 Frame::detections 或 SharedDetections，供 EncodeThread 叠框使用。
struct Detection {
    float x1, y1, x2, y2;  // 检测框在原始帧分辨率下的像素坐标（左上/右下角）
    int   class_id;         // COCO 类别 ID（0~79）
    std::string label;      // 类别名称，如 "cup"、"cell phone"
    float score;            // 置信度（0~1）
};

// 一帧画面的完整载体，在 CaptureThread → InferThread / EncodeThread 之间流转。
// raw_data 由 V4L2 mmap 拷贝填入；rgb_data 由 EncodeThread 在 YUYV→NV12
// 转换过程中顺带生成，供调试或未来扩展使用（当前叠框直接在 NV12 上操作）。
struct Frame {
    std::vector<uint8_t> raw_data;      // V4L2 采集的原始 YUYV422 数据，或拉流解码后的 NV12 数据
    std::vector<uint8_t> rgb_data;      // 转换后的 RGB24 数据（原始分辨率，暂未使用）
    int     width        = 0;
    int     height       = 0;
    uint64_t frame_id    = 0;           // 源帧单调序号，同一画面送入编码/推理队列时保持一致
    int64_t pts_ms       = 0;           // 同一单调时钟基准的显示时间戳（ms），用于框与视频对齐
    int64_t timestamp_ms = 0;           // 兼容现有 ROI/MQTT 的采集时间戳（ms），等同 pts_ms
    std::vector<Detection> detections;  // 本帧的推理结果，由 InferThread 写入
    PixelFormat pixel_format = PixelFormat::YUYV; // 像素格式，默认 YUYV
};

// MPP 硬编后的 H.264 码流包，在 EncodeThread → StreamThread 之间流转。
// pts/dts 按帧号递增，StreamThread 据此写入 RTMP flv tag 的时间戳字段。
struct EncodedPacket {
    std::vector<uint8_t> data;
    bool    is_keyframe = false;  // IDR 帧标志，StreamThread 需先发 SPS/PPS extradata
    int64_t pts         = 0;     // 显示时间戳（ms）
    int64_t dts         = 0;     // 解码时间戳（ms），H.264 baseline 下与 pts 相同
};
