# Baseline deployment verification report

Date: 2026-07-11

## Scope

Fixed six-class YOLOv8s baseline after board ISP recovery. MQTT was intentionally excluded.

## Accuracy

- Fixed test split: 29 images / 130 boxes.
- Test mAP50: 77.18%; mAP50-95: 59.29%.
- Baseline remains the accuracy and deployment reference.
- 10% pruned, low-ratio, ReLU6, and distillation candidates were rejected and not deployed.

## Board Smoke

- Board: firefly@192.168.0.200.
- Camera: /dev/video20, YUYV 640x480, negotiated 15 FPS.
- RKNN API 2.3.0, driver 0.9.8, rockchip_dfl, 9 outputs, zero-copy input.
- 60-second run loaded the model, entered AlertGateway running, encoded, and exited cleanly with Done.
- FLV verified: H.264, 640x480, 15 FPS, 57.667 seconds; encoder about 15.1 FPS.
- Positive-scene repeat produced objs:1-2.

## Performance

- InferThread npu is the wall time around rknn_run; total includes preprocessing, copy, conversion, and postprocess.
- Interactive governor: npu varied about 30-45 ms.
- Temporary performance governor: npu mostly 27-31 ms; total mostly 31-37 ms.
- Governor was restored to interactive after the comparison.

## Final decision

Baseline board validation is complete. Keep the baseline deployment artifact as reference. Future optimization must pass the fixed test mAP gate before another export and Smoke cycle.

## Persistent performance policy follow-up

After the initial temporary governor comparison, the existing ockchip-performance.service was deployed and enabled on the board. It locks CPU policies 0/4/6, NPU, and DDR to performance and disables CPUIdle state1. A 15-second production-path Smoke then measured knn_run at about 26.7 ms and total inference at about 33.9 ms with clean shutdown. The baseline model and application source were unchanged.

## Long-run result

A 120-second production-path run under the persistent performance service exited cleanly. Final samples remained about 26.7 ms for knn_run and 33.9-34.5 ms total inference. Post-run thermal readings were approximately 37.0-37.9 C across the reported zones, with no observed throttling or process failure. The performance policy is accepted as the current board baseline, subject to its higher power-consumption tradeoff.

## Five-minute stability result

A 300-second production-path run under ockchip-performance.service completed on schedule with clean shutdown and detections observed. knn_run remained about 26.7 ms and total inference about 30-35 ms. Sampled thermal readings peaked around 37.9 C and returned to about 35-36 C after shutdown, with no observed throttling. No board power sensor was available, so absolute power consumption was not measured.
