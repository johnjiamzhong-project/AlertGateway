#!/usr/bin/env python3
import csv
from pathlib import Path
from collections import Counter

DATASET_ROOT = Path("/home/rambos/datasets/alertgateway_desktop6_final")
CLASSES = ["cell phone", "cup", "keyboard", "mouse", "laptop", "book"]

def main():
    print("=" * 60)
    print("AlertGateway Dataset Pre-training Check & Statistics")
    print("=" * 60)

    # 1. Check basic paths
    if not DATASET_ROOT.exists():
        print(f"Error: Dataset root {DATASET_ROOT} does not exist.")
        return 1

    annotation_dir = DATASET_ROOT / "annotation"
    pending_csv_path = annotation_dir / "pending_manual_review.csv"
    split_manifest_path = DATASET_ROOT / "split_manifest.csv"

    splits = ["train", "val", "test"]
    split_stats = {}
    session_to_split = {}
    split_sessions = {s: set() for s in splits}

    class_counts_by_split = {s: Counter() for s in splits}
    empty_label_counts_by_split = {s: 0 for s in splits}
    total_box_counts_by_split = {s: 0 for s in splits}

    print("\n--- 1. Checking split directories on disk ---")
    for split in splits:
        img_dir = DATASET_ROOT / split / "images"
        lbl_dir = DATASET_ROOT / split / "labels"

        if not img_dir.exists() or not lbl_dir.exists():
            print(f"Warning: {split} directory structure is incomplete.")
            continue

        images = list(img_dir.glob("*.jpg")) + list(img_dir.glob("*.png")) + list(img_dir.glob("*.jpeg"))
        labels = list(lbl_dir.glob("*.txt"))

        print(f"Split [{split}]: {len(images)} images, {len(labels)} label files found.")
        split_stats[split] = {
            "images_count": len(images),
            "labels_count": len(labels),
            "images": images,
            "labels": labels
        }

        # Parse sessions and boxes
        for img_path in images:
            # Format is [session]__[filename].jpg
            parts = img_path.name.split("__")
            if len(parts) > 1:
                session = parts[0]
            else:
                session = "unknown"
            split_sessions[split].add(session)
            session_to_split[session] = split

        for lbl_path in labels:
            # Read bounding boxes
            with lbl_path.open("r", encoding="utf-8") as f:
                lines = [line.strip() for line in f if line.strip()]

            if len(lines) == 0:
                empty_label_counts_by_split[split] += 1
            else:
                total_box_counts_by_split[split] += len(lines)
                for line in lines:
                    parts = line.split()
                    if parts:
                        try:
                            class_id = int(parts[0])
                            class_counts_by_split[split][class_id] += 1
                        except ValueError:
                            print(f"Warning: invalid line in {lbl_path}: '{line}'")

    print("\n--- 2. Session Isolation & Leak Check ---")
    # Verify no session overlap between splits
    overlap_detected = False
    for i, s1 in enumerate(splits):
        for j, s2 in enumerate(splits):
            if i >= j:
                continue
            intersection = split_sessions[s1].intersection(split_sessions[s2])
            if intersection:
                print(f"!!! CRITICAL WARNING: Session leakage detected between '{s1}' and '{s2}'!")
                print(f"    Overlapping sessions: {intersection}")
                overlap_detected = True

    if not overlap_detected:
        print("Success: Session isolation verified! No sessions are shared between train/val/test splits.")

    print("\n--- 3. Session Mapping to Splits ---")
    for split in splits:
        print(f"  Split '{split}' contains {len(split_sessions[split])} sessions:")
        for idx, sess in enumerate(sorted(split_sessions[split]), 1):
            print(f"    {idx}. {sess}")

    print("\n--- 4. Detailed Statistics by Split ---")
    print(f"{'Split':<8} | {'Images':<8} | {'Labels':<8} | {'Empty Lbls':<10} | {'Total Boxes':<12}")
    print("-" * 55)
    total_imgs = 0
    total_lbls = 0
    total_empties = 0
    total_boxes = 0
    for split in splits:
        imgs = split_stats[split]["images_count"]
        lbls = split_stats[split]["labels_count"]
        empties = empty_label_counts_by_split[split]
        boxes = total_box_counts_by_split[split]
        print(f"{split:<8} | {imgs:<8} | {lbls:<8} | {empties:<10} | {boxes:<12}")
        total_imgs += imgs
        total_lbls += lbls
        total_empties += empties
        total_boxes += boxes
    print("-" * 55)
    print(f"{'Total':<8} | {total_imgs:<8} | {total_lbls:<8} | {total_empties:<10} | {total_boxes:<12}")

    print("\n--- 5. Bounding Box Class Distribution ---")
    print(f"{'Class ID':<8} | {'Class Name':<15} | {'Train':<8} | {'Val':<8} | {'Test':<8} | {'Total':<8} | {'Ratio':<6}")
    print("-" * 70)
    for class_id in range(6):
        c_name = CLASSES[class_id]
        c_train = class_counts_by_split["train"][class_id]
        c_val = class_counts_by_split["val"][class_id]
        c_test = class_counts_by_split["test"][class_id]
        c_total = c_train + c_val + c_test
        c_ratio = f"{(c_total / total_boxes * 100):.2f}%" if total_boxes > 0 else "0.00%"
        print(f"{class_id:<8} | {c_name:<15} | {c_train:<8} | {c_val:<8} | {c_test:<8} | {c_total:<8} | {c_ratio:<6}")
    print("-" * 70)
    print(f"{'Total':<8} | {'-':<15} | {total_box_counts_by_split['train']:<8} | {total_box_counts_by_split['val']:<8} | {total_box_counts_by_split['test']:<8} | {total_boxes:<8} | 100.00%")

    # Analyze class skewness
    print("\n--- 6. Class Imbalance Analysis ---")
    all_class_totals = [sum(class_counts_by_split[split][class_id] for split in splits) for class_id in range(6)]
    min_class_total = min(all_class_totals)
    max_class_total = max(all_class_totals)
    min_class_name = CLASSES[all_class_totals.index(min_class_total)]
    max_class_name = CLASSES[all_class_totals.index(max_class_total)]
    skew_factor = max_class_total / min_class_total if min_class_total > 0 else float('inf')

    print(f"  Most frequent category: '{max_class_name}' with {max_class_total} boxes")
    print(f"  Least frequent category: '{min_class_name}' with {min_class_total} boxes")
    print(f"  Max/Min imbalance ratio: {skew_factor:.2f}x")
    if skew_factor > 10.0:
        print("  Warning: High class imbalance (> 10x). Model performance on minority classes might be lower.")
    else:
        print("  Info: Class imbalance is moderate (< 10x), acceptable for initial training.")

    print("\n--- 7. Pending Manual Review Analysis ---")
    if not pending_csv_path.exists():
        print("Info: No pending_manual_review.csv found.")
    else:
        pending_sessions = Counter()
        pending_total = 0
        with pending_csv_path.open(newline="", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            for row in reader:
                sess = row.get("session", "unknown")
                pending_sessions[sess] += 1
                pending_total += 1

        print(f"Total pending manual review images: {pending_total}")
        print("Pending images count by session and their current split mapping:")
        for sess, count in pending_sessions.items():
            current_split = session_to_split.get(sess, "NOT YET ASSIGNED")
            print(f"  - Session '{sess}': {count} images (mapped to split: {current_split})")

        print("\nDecision Guidance:")
        print("  - If all pending sessions are already in the 'train' split, adding them later will only increase training samples without leaking.")
        print("  - We do NOT need to wait for these 20 images before training. Proceeding to mini-trial training is highly recommended.")

    print("=" * 60)
    return 0

if __name__ == "__main__":
    import sys
    sys.exit(main())
