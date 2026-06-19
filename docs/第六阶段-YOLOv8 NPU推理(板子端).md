# 第六阶段：YOLOv8 NPU 推理（板子端）

**平台**：Firefly ROC-RK3588S-PC（Ubuntu 22.04 aarch64）  
**完成日期**：2026-06-14

---

## 环境确认

板子上预装了 RKNN runtime，无需额外安装：

```bash
strings /usr/lib/librknnrt.so | grep "librknnrt version"
# librknnrt version: 2.3.0
```

NPU 设备通过 DRI 接口暴露：

```bash
find /dev -name "*npu*"
# /dev/dri/by-path/platform-fdab0000.npu-render
# /dev/dri/by-path/platform-fdab0000.npu-card
```

---

## 一、安装依赖

```bash
pip3 install rknn-toolkit-lite2
pip3 install opencv-python-headless
```

验证：

```bash
python3 -c "from rknnlite.api import RKNNLite; print('ok')"
```

---

## 二、准备模型

从 WSL2 传输转换好的 rknn 模型到板子：

```bash
# WSL2 上执行
scp /home/rambos/yolov8n.rknn firefly@192.168.1.200:~/rknn_yolov8/
```

建议统一放在 `~/rknn_yolov8/` 目录下：

```bash
mkdir -p ~/rknn_yolov8
```

---

## 三、推理脚本

`~/rknn_yolov8/test_yolov8.py`：

```python
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
    pred = output[0]
    pred = pred[0].T          # (8400, 84)

    boxes = pred[:, :4]
    scores = pred[:, 4:]

    class_ids = np.argmax(scores, axis=1)
    confidences = scores[np.arange(len(scores)), class_ids]

    mask = confidences > conf_thres
    boxes = boxes[mask]
    confidences = confidences[mask]
    class_ids = class_ids[mask]

    # xywh -> xyxy
    x, y, w, h = boxes[:, 0], boxes[:, 1], boxes[:, 2], boxes[:, 3]
    boxes_xyxy = np.stack([x - w/2, y - h/2, x + w/2, y + h/2], axis=1)

    # NMS
    indices = cv2.dnn.NMSBoxes(
        boxes_xyxy.tolist(), confidences.tolist(), conf_thres, iou_thres)

    results = []
    if len(indices) > 0:
        for i in indices.flatten():
            results.append({
                'box': boxes_xyxy[i].astype(int).tolist(),
                'confidence': float(confidences[i]),
                'class_id': int(class_ids[i]),
                'class_name': CLASSES[int(class_ids[i])]
            })
    return results


# 初始化
rknn = RKNNLite(verbose=False)
rknn.load_rknn('/home/firefly/rknn_yolov8/yolov8n.rknn')
rknn.init_runtime()

# 读图
img_path = '/tmp/1.png'
img = cv2.imread(img_path)
h, w = img.shape[:2]

inp = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
inp = cv2.resize(inp, (640, 640))
inp = np.expand_dims(inp, axis=0)

# 推理
outputs = rknn.inference(inputs=[inp])

# 后处理（坐标还原到原图尺寸）
results = postprocess(outputs)
scale_x, scale_y = w / 640, h / 640
for r in results:
    b = r['box']
    b[0] = int(b[0] * scale_x)
    b[1] = int(b[1] * scale_y)
    b[2] = int(b[2] * scale_x)
    b[3] = int(b[3] * scale_y)

# 画框
for r in results:
    x1, y1, x2, y2 = r['box']
    label = f"{r['class_name']} {r['confidence']:.2f}"
    cv2.rectangle(img, (x1, y1), (x2, y2), (0, 255, 0), 2)
    cv2.putText(img, label, (x1, y1 - 5),
                cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1)

cv2.imwrite('/tmp/result.png', img)
print(f'检测到 {len(results)} 个目标:')
for r in results:
    print(f"  {r['class_name']}: {r['confidence']:.2f} {r['box']}")

rknn.release()
```

运行：

```bash
python3 ~/rknn_yolov8/test_yolov8.py
```

---

## 四、验证结果

### yolov8n 测试（俯视猫图）



```
检测到 1 个目标:
  dog: 0.89 [50, 215, 187, 309]
```

分类误判（cat→dog），原因：
- yolov8n 是最小版本，精度最低
- 俯视角度与训练集分布差异大

### yolov8s 测试（正面猫图）

```
检测到 2 个目标:
  cat: 0.86 [44, 159, 421, 732]
  bench: 0.26 [3, 2, 586, 674]
```

猫识别正确，bench 是猫坐的椅子，也属于正确检测。

### yolov8s 测试（狗图）

```
检测到 1 个目标:
  dog: 0.82 [132, 123, 339, 573]
```

识别正确。

结果图保存在 `/tmp/result.png`。

### 模型对比

| 模型 | 大小 | 精度 | 备注 |
|------|------|------|------|
| yolov8n | 8MB | 低 | 俯视角度容易误判 |
| yolov8s | 24MB | 中 | 正面识别准确 |

---

## 五、输出说明

| 字段 | 说明 |
|------|------|
| 模型输出 shape | `(1, 84, 8400)` |
| 84 | 4个坐标(xywh) + 80个COCO类别得分 |
| 8400 | 候选检测框数量 |
| 后处理 | xywh→xyxy + 置信度过滤 + NMS |

---

## 注意事项

- 板子上用 `RKNNLite`，不是 `RKNN`（后者是 PC 端转换用的）
- warning `query RKNN_QUERY_INPUT_DYNAMIC_RANGE error` 可忽略，静态 shape 模型的正常提示
- 精度提升方案：换 `yolov8s/m` 或开启 INT8 量化（需要校准图片集）

---

## 完成情况

| 目标 | 状态 |
|------|------|
| RKNN runtime 验证 | ✅ |
| rknn-toolkit-lite2 安装 | ✅ |
| YOLOv8n 模型加载 | ✅ |
| NPU 推理跑通 | ✅ |
| 后处理 + 画框 | ✅ |
| 检测结果输出 | ✅ |
| yolov8s 精度验证 | ✅ |
