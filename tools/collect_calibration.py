#!/usr/bin/env python3
"""
采集 INT8 量化校准图片，带实时预览。

从 V4L2 摄像头采集画面，缩放为模型输入尺寸 RGB 后保存为 PNG。
默认尺寸为 640×640，可用 --model-width/--model-height 采集 640×480 等实验尺寸。

操作说明：
    空格键  — 采集当前帧
    d 键    — 删除上一张（拍错了可以撤回）
    q / ESC — 退出

用法：
    python3 tools/collect_calibration.py
    python3 tools/collect_calibration.py --device /dev/video20 --count 150 --out ~/calibration
    python3 tools/collect_calibration.py --model-width 640 --model-height 480 --out ~/calibration_640x480

显示说明：
    需要桌面环境或 SSH X11 转发（ssh -X firefly@192.168.0.200）
    无显示环境时加 --no-preview 参数，退回定时自动采集模式
"""
import cv2
import argparse
import os
import time

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--device",     default="/dev/video20",                      help="V4L2 设备节点")
    p.add_argument("--count",      type=int, default=150,                       help="目标采集张数")
    p.add_argument("--out",        default=os.path.expanduser("~/calibration"), help="保存目录")
    p.add_argument("--interval",   type=float, default=0.5,                     help="无预览模式的采集间隔（秒）")
    p.add_argument("--no-preview", action="store_true",                         help="禁用预览，定时自动采集")
    p.add_argument("--model-width", type=int, default=640,                      help="保存图片宽度/模型输入宽度")
    p.add_argument("--model-height", type=int, default=640,                     help="保存图片高度/模型输入高度")
    return p.parse_args()

def draw_overlay(frame, saved, total):
    """在预览画面上叠加提示信息。"""
    h, w = frame.shape[:2]
    overlay = frame.copy()
    cv2.rectangle(overlay, (0, 0), (w, 36), (0, 0, 0), -1)
    cv2.addWeighted(overlay, 0.5, frame, 0.5, 0, frame)
    cv2.putText(frame, f"Saved: {saved}/{total}  |  SPACE=capture  D=delete last  Q=quit",
                (8, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 255, 0), 1, cv2.LINE_AA)
    return frame

def main():
    args = parse_args()
    if args.model_width <= 0 or args.model_height <= 0:
        raise ValueError("--model-width 和 --model-height 必须为正整数")

    os.makedirs(args.out, exist_ok=True)
    model_size = (args.model_width, args.model_height)

    cap = cv2.VideoCapture(args.device, cv2.CAP_V4L2)
    cap.set(cv2.CAP_PROP_FRAME_WIDTH,  640)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"YUYV"))

    if not cap.isOpened():
        print(f"无法打开设备 {args.device}")
        return

    saved = 0
    saved_paths = []

    if args.no_preview:
        # ── 无预览：定时自动采集 ──────────────────────────────────────────────
        print(f"自动采集模式，目标 {args.count} 张，间隔 {args.interval}s，保存尺寸 {args.model_width}x{args.model_height}")
        print(f"保存路径：{args.out}  |  Ctrl+C 提前结束\n")
        try:
            while saved < args.count:
                ret, frame = cap.read()
                if not ret:
                    continue
                img = cv2.resize(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB),
                                 model_size, interpolation=cv2.INTER_LINEAR)
                path = os.path.join(args.out, f"calib_{saved:04d}.png")
                cv2.imwrite(path, cv2.cvtColor(img, cv2.COLOR_RGB2BGR))
                saved += 1
                print(f"\r已采集 {saved}/{args.count}", end="", flush=True)
                time.sleep(args.interval)
        except KeyboardInterrupt:
            print(f"\n提前结束")
    else:
        # ── 预览模式：手动按空格采集 ─────────────────────────────────────────
        print(f"预览模式，目标 {args.count} 张，保存尺寸 {args.model_width}x{args.model_height}")
        print(f"保存路径：{args.out}")
        print("空格=采集  D=撤销上一张  Q/ESC=退出\n")

        cv2.namedWindow("Calibration Preview", cv2.WINDOW_NORMAL)
        cv2.resizeWindow("Calibration Preview", args.model_width, args.model_height)

        while saved < args.count:
            ret, frame = cap.read()
            if not ret:
                continue

            # 缩放到模型输入尺寸（与推理预处理一致）
            img_rgb = cv2.resize(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB),
                                 model_size, interpolation=cv2.INTER_LINEAR)
            img_bgr = cv2.cvtColor(img_rgb, cv2.COLOR_RGB2BGR)

            preview = draw_overlay(img_bgr.copy(), saved, args.count)
            cv2.imshow("Calibration Preview", preview)

            key = cv2.waitKey(30) & 0xFF

            if key == ord(' '):
                # 采集当前帧
                path = os.path.join(args.out, f"calib_{saved:04d}.png")
                cv2.imwrite(path, img_bgr)
                saved_paths.append(path)
                saved += 1
                print(f"已采集 {saved}/{args.count}  →  {path}")

            elif key == ord('d') and saved_paths:
                # 撤销上一张
                last = saved_paths.pop()
                os.remove(last)
                saved -= 1
                print(f"已删除 {last}，当前 {saved} 张")

            elif key in (ord('q'), ord('Q'), 27):
                print("退出")
                break

        cv2.destroyAllWindows()

    cap.release()
    print(f"\n完成，共采集 {saved} 张，保存在 {args.out}")

if __name__ == "__main__":
    main()
