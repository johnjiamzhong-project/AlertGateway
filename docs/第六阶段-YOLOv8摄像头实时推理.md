# 第六阶段：YOLOv8 摄像头实时推理

**平台**：Firefly ROC-RK3588S-PC（Ubuntu 22.04 aarch64）  
**摄像头**：罗技 C310（USB UVC）  
**完成日期**：2026-06-14

---

## 一、摄像头确认

```bash
v4l2-ctl --list-devices
```

输出：
```
UVC Camera (046d:081b) (usb-fc800000.usb-1):
        /dev/video20
        /dev/video21
```

C310 对应 `/dev/video20`，640x480 @ 30fps，YUY2 格式。

抓帧测试：

```bash
ffmpeg -i /dev/video20 -frames:v 1 /tmp/cam_test.jpg
```

---

## 二、依赖

`opencv-python-headless` 不支持 `cv2.imshow`，需要完整版：

```bash
pip3 uninstall opencv-python-headless -y
pip3 install opencv-python
```

---

## 三、实时推理脚本

`~/rknn_yolov8/realtime.py`：

```python
import time
import cv2
import numpy as np
from rknnlite.api import RKNNLite

CLASSES = ['person','bicycle','car','motorcycle','airplane','bus','train','truck','boat',
           'traffic light','fire hydrant','stop sign','parking meter','bench','bird','cat',
           'dog','horse','sheep','cow','elephant','bear','zebra','giraffe','backpack',
           'umbrella','handbag','tie','suitcase','frisbee','skis','snowboard','sports ball',
           'kite','baseball bat','baseball glove','skateboard','surfboard','tennis racket',
           'bottle','wine glass','cup','fork','knife','spoon','bowl','banana','apple',
           'sandwich','orange','broccoli','carrot','hot dog','pizza','donut','cake','chair',
           'couch','potted plant','bed','dining table','toilet','tv','laptop','mouse',
           'remote','keyboard','cell phone','microwave','oven','toaster','sink','refrigerator',
           'book','clock','vase','scissors','teddy bear','hair drier','toothbrush']

def postprocess(output, conf_thres=0.25, iou_thres=0.45):
    pred = output[0][0].T
    boxes = pred[:, :4]
    scores = pred[:, 4:]
    class_ids = np.argmax(scores, axis=1)
    confidences = scores[np.arange(len(scores)), class_ids]
    mask = confidences > conf_thres
    boxes = boxes[mask]
    confidences = confidences[mask]
    class_ids = class_ids[mask]
    if len(boxes) == 0:
        return []
    x, y, w, h = boxes[:, 0], boxes[:, 1], boxes[:, 2], boxes[:, 3]
    boxes_xyxy = np.stack([x - w/2, y - h/2, x + w/2, y + h/2], axis=1)
    indices = cv2.dnn.NMSBoxes(
        boxes_xyxy.tolist(), confidences.tolist(), conf_thres, iou_thres)
    results = []
    if len(indices) > 0:
        for i in indices.flatten():
            results.append({
                'box': boxes_xyxy[i].astype(int).tolist(),
                'confidence': float(confidences[i]),
                'class_name': CLASSES[int(class_ids[i])]
            })
    return results


# 初始化 RKNN
rknn = RKNNLite(verbose=False)
rknn.load_rknn('/home/firefly/rknn_yolov8/yolov8s.rknn')
rknn.init_runtime()

# 打开摄像头
cap = cv2.VideoCapture('/dev/video20')
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)

print('按 q 退出')
while True:
    # 清空缓冲区积压帧，减少延迟
    for _ in range(3):
        cap.grab()
    ret, frame = cap.read()
    if not ret:
        break

    h, w = frame.shape[:2]
    inp = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
    inp = cv2.resize(inp, (640, 640))
    inp = np.expand_dims(inp, axis=0)

    t0 = time.time()
    outputs = rknn.inference(inputs=[inp])
    t1 = time.time()

    results = postprocess(outputs)

    scale_x, scale_y = w / 640, h / 640
    for r in results:
        b = r['box']
        x1 = int(b[0] * scale_x)
        y1 = int(b[1] * scale_y)
        x2 = int(b[2] * scale_x)
        y2 = int(b[3] * scale_y)
        label = f"{r['class_name']} {r['confidence']:.2f}"
        cv2.rectangle(frame, (x1, y1), (x2, y2), (0, 255, 0), 2)
        cv2.putText(frame, label, (x1, y1 - 5),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1)

    # 显示推理耗时
    cv2.putText(frame, f'{(t1-t0)*1000:.0f}ms', (10, 30),
                cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 0, 255), 2)

    cv2.imshow('YOLOv8 RK3588S NPU', frame)
    if cv2.waitKey(1) & 0xFF == ord('q'):
        break

cap.release()
cv2.destroyAllWindows()
rknn.release()
```

运行：

```bash
python3 ~/rknn_yolov8/realtime.py
```

---

## 四、性能数据

| 指标 | 数值 |
|------|------|
| 模型 | yolov8s（非量化） |
| 推理耗时 | 110~180ms |
| 实际帧率 | ~6-9 fps |
| 端到端延迟 | ~0.5s |
| 摄像头分辨率 | 640x480 |

---

## 五、延迟分析

| 来源 | 耗时 |
|------|------|
| NPU 推理 | ~130ms |
| 摄像头读取+预处理 | ~50ms |
| 显示渲染 | ~50ms |
| 缓冲区积压 | ~200ms（已用 grab 缓解） |

---

## 六、优化方向（待做）

| 方案 | 效果 |
|------|------|
| 换 yolov8n | 推理快一倍，精度略低 |
| INT8 量化 | 速度提升 2-3 倍，需要校准图片集 |
| 多线程解耦采集和推理 | 减少缓冲积压延迟 |

---

## 完成情况

| 目标 | 状态 |
|------|------|
| C310 摄像头识别 | ✅ |
| 摄像头抓帧验证 | ✅ |
| 实时推理脚本 | ✅ |
| 检测框实时显示 | ✅ |
| 推理耗时统计 | ✅ |
