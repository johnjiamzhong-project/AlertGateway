#!/usr/bin/env python3
import os
import argparse

# 桌面六类目标类别定义
CLASSES = [
    "cell phone",
    "cup",
    "keyboard",
    "mouse",
    "laptop",
    "book"
]

def main():
    parser = argparse.ArgumentParser(description="Prepare dataset directory structure and template config files for alertgateway_desktop6.")
    parser.add_argument("--output-dir", type=str, default="/home/rambos/datasets/alertgateway_desktop6",
                        help="Target root path for the dataset (outside git repo).")
    args = parser.parse_args()

    target_root = os.path.abspath(args.output_dir)
    print(f">> Initializing alertgateway_desktop6 dataset directories at: {target_root}")

    # 1. 创建子目录
    subdirs = [
        "train/images", "train/labels",
        "val/images", "val/labels",
        "test/images", "test/labels"
    ]
    for d in subdirs:
        dir_path = os.path.join(target_root, d)
        os.makedirs(dir_path, exist_ok=True)
        print(f"   Created directory: {dir_path}")

    # 2. 生成 data.yaml
    data_yaml_path = os.path.join(target_root, "data.yaml")
    data_yaml_content = f"""path: {target_root}
train: train/images
val: val/images
test: test/images

names:
"""
    for idx, cls_name in enumerate(CLASSES):
        data_yaml_content += f"  {idx}: {cls_name}\n"

    with open(data_yaml_path, "w") as f:
        f.write(data_yaml_content)
    print(f"   Created data.yaml template: {data_yaml_path}")

    # 3. 生成 classes.txt
    classes_txt_path = os.path.join(target_root, "classes.txt")
    classes_txt_content = "\n".join(CLASSES) + "\n"
    with open(classes_txt_path, "w") as f:
        f.write(classes_txt_content)
    print(f"   Created classes.txt template: {classes_txt_path}")

    print("\n>> Directory preparation completed successfully.")
    print(">> You can now start collecting, naming and splitting your images into these folders.")

if __name__ == "__main__":
    main()
