# 第六阶段：YOLOv8 模型转换为 RKNN 格式

**平台**：WSL2 Ubuntu 22.04 x86_64  
**完成日期**：2026-06-14

---

## 环境说明

- 模型转换在 **WSL2** 上完成，不在板子上
- 板子只需要 `librknnrt.so`（已预装），跑转换好的 `.rknn` 模型
- 使用 Python 虚拟环境隔离依赖

---

## 一、创建 Python 虚拟环境

```bash
sudo apt install -y python3-pip python3-venv
python3 -m venv ~/rknn-env
source ~/rknn-env/bin/activate
```

每次使用前需要激活：

```bash
source ~/rknn-env/bin/activate
# 命令行前缀显示 (rknn-env) 表示已激活
```

---

## 二、安装依赖

```bash
# 安装 rknn-toolkit2（会自动拉取 torch、onnx 等依赖，约 3-4GB）
pip install rknn-toolkit2

# 降级 torch（rknn-toolkit2 2.3.2 要求 torch<=2.4.0）
pip install torch==2.4.0

# 安装 ultralytics（用于导出 YOLOv8 模型）
pip install ultralytics

# 降级 onnx（rknn-toolkit2 与 onnx>=1.17 不兼容，onnx.mapping 已移除）
pip install onnx==1.16.0
```

**版本确认：**

```bash
python3 -c "from rknn.api import RKNN; print('rknn ok')"
python3 -c "import ultralytics; print(ultralytics.__version__)"
```

---

## 三、导出 YOLOv8n 为 ONNX

```bash
python3 << 'PYEOF'
from ultralytics import YOLO

# 自动下载预训练权重 yolov8n.pt（约 6MB）
model = YOLO('yolov8n.pt')
model.export(format='onnx', opset=12, simplify=True, imgsz=640)
print('ONNX 导出完成')
PYEOF
```

输出文件：`~/yolov8n.onnx`（约 12MB）

---

## 四、ONNX 转换为 RKNN

```bash
python3 << 'PYEOF'
from rknn.api import RKNN

rknn = RKNN(verbose=False)

# 配置目标平台
rknn.config(
    mean_values=[[0, 0, 0]],
    std_values=[[255, 255, 255]],
    target_platform='rk3588'
)

# 加载 ONNX
ret = rknn.load_onnx(model='/home/rambos/yolov8n.onnx')
if ret != 0:
    print(f'load onnx failed: {ret}')
    exit(ret)

# 构建（不量化）
ret = rknn.build(do_quantization=False)
if ret != 0:
    print(f'build failed: {ret}')
    exit(ret)

# 导出 rknn
ret = rknn.export_rknn('/home/rambos/yolov8n.rknn')
if ret != 0:
    print(f'export failed: {ret}')
    exit(ret)

print('转换完成: yolov8n.rknn')
rknn.release()
PYEOF
```

输出文件：`~/yolov8n.rknn`

---

## 五、传输到板子

```bash
scp /home/rambos/yolov8n.rknn firefly@192.168.1.200:~/
```

---

## 注意事项

- `rknn-toolkit2` 只用于 PC 端模型转换，板子上不需要装
- `onnx 1.21` 移除了 `onnx.mapping`，必须降到 `1.16.0`
- `torch` 必须 `<=2.4.0`，否则 rknn-toolkit2 报版本冲突
- `do_quantization=False` 跳过量化，精度更高但推理速度略慢于 INT8 量化版本
- `target_platform` 必须指定 `rk3588`，不能用 `rk3588s`

---

## 版本汇总

| 包 | 版本 |
|----|------|
| rknn-toolkit2 | 2.3.2 |
| onnx | 1.16.0 |
| torch | 2.4.0 |
| ultralytics | 8.4.67 |
| Python | 3.10.12 |

