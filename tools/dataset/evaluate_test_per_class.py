#!/usr/bin/env python3
"""Evaluate a YOLO checkpoint on the fixed split and print per-class AP50."""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from ultralytics import YOLO
# Register the repository's namespace explicitly; a third-party package named
# "tools" may exist in the environment after Ultralytics auto-install.
import importlib.util
import types
_root = Path(__file__).resolve().parents[2]
_tools = sys.modules.setdefault("tools", types.ModuleType("tools"))
_tools.__path__ = [str(_root / "tools")]
_dataset = sys.modules.setdefault("tools.dataset", types.ModuleType("tools.dataset"))
_dataset.__path__ = [str(_root / "tools" / "dataset")]
_spec = importlib.util.spec_from_file_location(
    "tools.dataset.distill_model", _root / "tools" / "dataset" / "distill_model.py"
)
_distill_model = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = _distill_model
_spec.loader.exec_module(_distill_model)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--model", required=True)
    p.add_argument("--data", default="/home/rambos/datasets/alertgateway_desktop6_final/data.yaml")
    p.add_argument("--split", default="test", choices=("train", "val", "test"))
    p.add_argument("--imgsz", type=int, default=640)
    p.add_argument("--batch", type=int, default=8)
    p.add_argument("--device", default="0")
    p.add_argument("--workers", type=int, default=2)
    args = p.parse_args()

    model = YOLO(args.model)
    result = model.val(
        data=args.data,
        split=args.split,
        imgsz=args.imgsz,
        batch=args.batch,
        device=args.device,
        workers=args.workers,
        plots=False,
        verbose=False,
    )
    names = model.names
    print(f"model={args.model}")
    print(f"split={args.split} mAP50={result.box.map50 * 100:.2f}% mAP50-95={result.box.map * 100:.2f}%")
    class_ids = (
        result.box.ap_class_index
        if hasattr(result.box, "ap_class_index")
        else result.box.ap_class
    )
    for class_id, ap50 in zip(class_ids, result.box.ap50):
        class_id = int(class_id)
        print(f"class={class_id} name={names[class_id]} AP50={ap50 * 100:.2f}%")


if __name__ == "__main__":
    raise SystemExit(main())
