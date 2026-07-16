#!/usr/bin/env python3
import argparse
import csv
import shutil
from pathlib import Path


DEFAULT_MANIFEST = Path("/home/rambos/datasets/selected_desktop6/review/review_manifest.csv")
DEFAULT_OUTPUT_DIR = Path("/home/rambos/datasets/selected_desktop6/images")
KEEP_VALUES = {"1", "y", "yes", "true", "keep", "保留"}


def is_keep(value: str) -> bool:
    return value.strip().lower() in KEEP_VALUES


def selected_rows(manifest: Path):
    with manifest.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        required = {"image_path"}
        missing = required - set(reader.fieldnames or [])
        if missing:
            raise ValueError(f"manifest missing columns: {', '.join(sorted(missing))}")

        for row_number, row in enumerate(reader, start=2):
            if is_keep(row.get("keep", "")) or row.get("review_choice", "").strip().lower() == "keep":
                yield row_number, row


def output_name(row: dict) -> str:
    session = Path(row.get("session") or Path(row["image_path"]).parent.name).name
    image_name = Path(row["image_path"]).name
    return f"{session}__{image_name}"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Copy frames marked keep in review_manifest.csv into selected_desktop6/images."
    )
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument(
        "--exclude-session",
        action="append",
        default=[],
        help="Skip selected rows whose session matches this value. Can be passed multiple times.",
    )
    parser.add_argument("--dry-run", action="store_true", help="Print what would be copied without writing files.")
    args = parser.parse_args()

    excluded_sessions = set(args.exclude_session)
    selected = 0
    copied = 0
    skipped_existing = 0
    missing_source = 0
    skipped_excluded = 0

    if not args.manifest.exists():
        raise FileNotFoundError(args.manifest)

    if not args.dry_run:
        args.output_dir.mkdir(parents=True, exist_ok=True)

    for row_number, row in selected_rows(args.manifest):
        selected += 1
        session = row.get("session") or Path(row["image_path"]).parent.name
        if session in excluded_sessions:
            skipped_excluded += 1
            continue

        source = Path(row["image_path"])
        destination = args.output_dir / output_name(row)

        if not source.exists():
            missing_source += 1
            print(f"missing row {row_number}: {source}")
            continue

        if destination.exists():
            skipped_existing += 1
            continue

        if args.dry_run:
            print(f"copy {source} -> {destination}")
        else:
            shutil.copy2(source, destination)
        copied += 1

    mode = "dry-run" if args.dry_run else "copied"
    print(f"manifest: {args.manifest}")
    print(f"output_dir: {args.output_dir}")
    print(f"selected: {selected}")
    print(f"{mode}: {copied}")
    print(f"skipped_existing: {skipped_existing}")
    print(f"skipped_excluded: {skipped_excluded}")
    print(f"missing_source: {missing_source}")
    return 1 if missing_source else 0


if __name__ == "__main__":
    raise SystemExit(main())
