#!/usr/bin/env python3
"""Build a source-balanced YOLO dataset for the 4K refinement candidate.

The generated files are hard links.  Source annotations are never modified.
The previously inspected 4K set is deliberately named ``eval_4k_dev`` rather
than test: it has already been used to compare candidates and is no longer a
pristine final holdout.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import os
from collections import Counter
from pathlib import Path


NAMES = ("cell phone", "cup", "keyboard", "mouse", "laptop", "book")
IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        default="/home/rambos/datasets/alertgateway_4k_refine/weighted_candidate_20260717",
    )
    parser.add_argument(
        "--history-root",
        default="/home/rambos/datasets/alertgateway_desktop6_final",
    )
    parser.add_argument(
        "--primary-4k",
        default="/home/rambos/datasets/alertgateway_4k_refine/annotation",
    )
    parser.add_argument(
        "--supplement-4k",
        default="/home/rambos/datasets/alertgateway_4k_refine/supplement_annotation",
    )
    parser.add_argument(
        "--dev-4k",
        default="/home/rambos/datasets/alertgateway_4k_refine/test_annotation",
    )
    parser.add_argument("--repeat-4k", type=int, default=3)
    return parser.parse_args()


def images_in(root: Path) -> list[Path]:
    image_dir = root / "images"
    images = sorted(
        path for path in image_dir.iterdir() if path.is_file() and path.suffix.lower() in IMAGE_SUFFIXES
    )
    if not images:
        raise ValueError(f"no images found in {image_dir}")
    return images


def validate_pair(image: Path, label: Path) -> Counter[int]:
    if not label.is_file():
        raise FileNotFoundError(f"missing label for {image}: {label}")
    counts: Counter[int] = Counter()
    for line_number, raw in enumerate(label.read_text(encoding="utf-8").splitlines(), 1):
        fields = raw.split()
        if len(fields) != 5:
            raise ValueError(f"{label}:{line_number}: expected 5 fields")
        class_id = int(fields[0])
        coords = tuple(float(value) for value in fields[1:])
        if class_id not in range(len(NAMES)):
            raise ValueError(f"{label}:{line_number}: invalid class {class_id}")
        if not all(0.0 <= value <= 1.0 for value in coords) or coords[2] <= 0 or coords[3] <= 0:
            raise ValueError(f"{label}:{line_number}: invalid normalized box")
        counts[class_id] += 1
    return counts


def link_sample(
    image: Path,
    label: Path,
    destination: Path,
    output_stem: str,
) -> None:
    image_target = destination / "images" / f"{output_stem}{image.suffix.lower()}"
    label_target = destination / "labels" / f"{output_stem}.txt"
    os.link(image, image_target)
    os.link(label, label_target)


def add_source(
    source_root: Path,
    destination: Path,
    prefix: str,
    domain: str,
    repeats: int,
    manifest_rows: list[dict[str, str | int]],
) -> tuple[int, Counter[int]]:
    total = 0
    boxes: Counter[int] = Counter()
    for repeat in range(repeats):
        for image in images_in(source_root):
            label = source_root / "labels" / f"{image.stem}.txt"
            sample_boxes = validate_pair(image, label)
            output_stem = f"{prefix}_r{repeat + 1}_{image.stem}"
            link_sample(image, label, destination, output_stem)
            boxes.update(sample_boxes)
            manifest_rows.append(
                {
                    "split": destination.name,
                    "domain": domain,
                    "source_image": str(image),
                    "output_stem": output_stem,
                    "repeat": repeat + 1,
                }
            )
            total += 1
    return total, boxes


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_yaml(path: Path, dataset_root: Path, val_dir: str) -> None:
    names = "\n".join(f"  {index}: {name}" for index, name in enumerate(NAMES))
    path.write_text(
        f"path: {dataset_root}\n"
        "train: train/images\n"
        f"val: {val_dir}/images\n"
        f"test: {val_dir}/images\n"
        "names:\n"
        f"{names}\n",
        encoding="utf-8",
    )


def main() -> int:
    args = parse_args()
    if args.repeat_4k < 1:
        raise ValueError("--repeat-4k must be positive")

    output = Path(args.output).resolve()
    if output.exists():
        raise FileExistsError(f"refusing to overwrite existing output: {output}")

    splits = ("train", "val", "eval_4k_dev", "eval_history_test")
    for split in splits:
        (output / split / "images").mkdir(parents=True)
        (output / split / "labels").mkdir(parents=True)

    manifest_rows: list[dict[str, str | int]] = []
    summaries: list[tuple[str, int, Counter[int]]] = []
    history_root = Path(args.history_root).resolve()

    summaries.append(
        (
            "history_train",
            *add_source(
                history_root / "train",
                output / "train",
                "hist",
                "history",
                1,
                manifest_rows,
            ),
        )
    )
    for label, source, prefix in (
        ("primary_4k", Path(args.primary_4k).resolve(), "k4_primary"),
        ("supplement_4k", Path(args.supplement_4k).resolve(), "k4_supplement"),
    ):
        summaries.append(
            (
                label,
                *add_source(
                    source,
                    output / "train",
                    prefix,
                    "4k",
                    args.repeat_4k,
                    manifest_rows,
                ),
            )
        )

    summaries.append(
        (
            "dev_4k",
            *add_source(
                Path(args.dev_4k).resolve(),
                output / "eval_4k_dev",
                "dev4k",
                "4k_dev",
                1,
                manifest_rows,
            ),
        )
    )
    summaries.append(
        (
            "history_test",
            *add_source(
                history_root / "test",
                output / "eval_history_test",
                "histtest",
                "history_dev",
                1,
                manifest_rows,
            ),
        )
    )

    # The composite validation split chooses checkpoints across both domains.
    for source_name in ("eval_4k_dev", "eval_history_test"):
        for image in images_in(output / source_name):
            label = output / source_name / "labels" / f"{image.stem}.txt"
            link_sample(image, label, output / "val", image.stem)

    train_hashes = {sha256(path) for path in images_in(output / "train")}
    val_hashes = {sha256(path) for path in images_in(output / "val")}
    overlap = train_hashes & val_hashes
    if overlap:
        raise ValueError(f"train/val image leakage detected: {len(overlap)} hashes")

    with (output / "manifest.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=("split", "domain", "source_image", "output_stem", "repeat"),
        )
        writer.writeheader()
        writer.writerows(manifest_rows)

    (output / "classes.txt").write_text("\n".join(NAMES) + "\n", encoding="utf-8")
    write_yaml(output / "data.yaml", output, "val")
    write_yaml(output / "data_4k_dev.yaml", output, "eval_4k_dev")
    write_yaml(output / "data_history_dev.yaml", output, "eval_history_test")

    print(f"output={output}")
    for name, image_count, boxes in summaries:
        counts = " ".join(str(boxes[index]) for index in range(len(NAMES)))
        print(f"source={name} images={image_count} boxes=[{counts}]")
    print(f"train_images={len(images_in(output / 'train'))}")
    print(f"val_images={len(images_in(output / 'val'))}")
    print(f"unique_train_hashes={len(train_hashes)} unique_val_hashes={len(val_hashes)} overlap=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
