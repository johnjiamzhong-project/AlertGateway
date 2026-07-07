# 话题:INT8 量化迁移全流程(BUG-007)

记录格式约定:核心一句话 / 要点N / 注意事项 / 代码位置参考

## 核心一句话
推理总耗时150ms(6.7 FPS)排查到底,根因是模型从一开始就没做过INT8量化——`rknn-toolkit2`导出ONNX时如果不显式开`do_quantization=True`+校准数据集,默认产物就是FP16,而FP16在RK3588S NPU上运算量是INT8的两倍;补上"采集校准图→重新量化转换→替换板上模型"这一整套流程后,总耗时降到48ms(约20 FPS),已经超过摄像头自身14.6fps的上限。

## 要点1:定位根因——用 strings 查 dtype,不要猜
加计时后发现`rknn_run`(纯NPU计算)单项占75ms,是YOLOv8s在RK3588S上参考值(30-40ms)的整整一倍。第一嫌疑是量化类型,直接查模型文件而不是翻配置猜测:
```bash
strings ~/AlertGateway/model/yolov8s.rknn | grep -E 'dtype|quant|int8|float'
```
输出`"dtype": "float16"`,量化参数段为空,坐实模型从未量化过。
- 追问:为什么不先怀疑多核/SRAM之类的NPU优化开关?——已经测过`rknn_set_core_mask(RKNN_NPU_CORE_0_1_2)`三核并行,`rknn_run`时间无变化(YOLOv8s算子依赖关系限制了多核收益),这条路先排除了才聚焦到量化类型上。

## 要点2:校准数据采集——`tools/collect_calibration.py`
INT8量化靠校准数据统计各层激活值分布,需要真实场景图,不能用随机噪声代替。脚本从`/dev/video20`实时采集,带OpenCV预览窗口,空格采集/D撤销/Q退出,直接存成640×640 RGB PNG(与推理预处理`yuyv_to_rgb→resize_rgb`的尺寸保持一致)。
- 通过Moonlight+Sunshine远程桌面在板子Xfce桌面上操作预览——板子本身没有显示器,这是当时能拿到图形界面最直接的办法。
- 共采集150张,要有代表性(真实场景、多角度、多光线),图片不足或质量差会导致量化后精度下降。

## 要点3:重新转换踩的两个环境坑——`tools/convert_int8.py`
`rknn-toolkit2`只能在x86_64 Linux上跑(WSL可用),不能在板子(aarch64)本身上转换,所以这一步在PC侧完成:
1. **onnx版本冲突**:系统装的`onnx==1.22.0`移除了`onnx.mapping`,而`rknn-toolkit2`依赖这个接口,转换直接报错。解决:降级到`onnx==1.13.1`。
2. **dataset参数类型**:`rknn.build(dataset=...)`只接受**文件路径字符串**,不接受numpy数组列表——一开始直接传读图后的数组列表会报错。解决:把图片路径逐行写入`dataset.txt`,传这个txt文件路径(`convert_int8.py:51-59`)。

## 要点4:双输出独立scale——量化配置和后处理代码要对上(详见 infer_thread.md要点3)
`load_onnx`时显式把box坐标和class概率拆成两路独立输出(`convert_int8.py:82-83`),原因写在脚本注释里:原始单一84通道合并输出共享一组量化scale,box坐标(0-640)和class概率(0-1)数量级悬殊,共用scale会把class概率的精度完全压没(反量化回来全部变成0)。
- 这是**转换脚本配置**和**`InferThread`运行时按输出元素个数识别哪路是哪路**(详见`infer_thread.md`要点3)两处配合的同一个问题的两个环节——量化阶段先把两路拆开校准,运行时再按特征识别避免假设固定顺序,缺一不可。

## 要点5:效果数据
| 阶段 | FP16(优化前) | INT8(优化后) | 说明 |
|---|---|---|---|
| rknn_run | 75ms | 38ms | INT8运算量减半 |
| inputs_set | 37ms | 0.4ms | INT8数据体积约1/2,搬运快 |
| outputs_get | 37ms | 1-3ms | 同上 |
| **total** | **~150ms** | **~48ms** | **3x提速** |

推理能力从6.7 FPS提升到约20 FPS,超过摄像头实际帧率(14.6fps)上限,代码本身无需改动(`outputs[i].want_float=1`让runtime自动反量化为float,后处理`YoloPostprocess`照常工作)。

## 注意事项
1. 这是完整的"现象→排查→处理→验证效果"debug故事,讲的时候按这个顺序展开,先给"模型一直是FP16从没量化过"这个根因结论,再展开排查过程和踩的坑。
2. 容易被追问的边界:INT8不是免费的精度,要承认量化会有精度损失,校准数据的代表性直接决定精度损失大小——这次没有专门测mAP掉了多少,只验证了功能正常(检测结果没有明显异常),如果被问"精度测过吗"要诚实说没有定量测过。
3. 与`npu_optimization_results.md`衔接:INT8切换后`rknn_run`降到38-40ms这个数字,后续又被进一步发现"受CPU主频影响、不是纯NPU算力下限"——如果聊到这条优化链路的下一步,可以接到那条memory。

## 代码位置参考
`tools/collect_calibration.py` — 校准图采集,预览模式按键采集 (line 79-122)
`tools/convert_int8.py`
- 量化配置 quantized_dtype/quantized_algorithm (line 67-74)
- 双输出拆分加载 (line 82-83)
- dataset.txt 生成,只接受路径字符串 (line 51-59)
- build 量化构建 (line 90)
`BUGS.md` BUG-007 — 排查过程、踩坑、效果数据原始记录 (line 368-462)
