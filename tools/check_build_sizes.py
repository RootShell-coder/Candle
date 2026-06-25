#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
from pathlib import Path
import sys


def parse_size(raw: str) -> int:
    value = raw.strip().lower()
    if value.startswith("0x"):
        return int(value, 16)
    if value.endswith("k"):
        return int(value[:-1]) * 1024
    if value.endswith("m"):
        return int(value[:-1]) * 1024 * 1024
    return int(value)


def load_partitions(path: Path) -> dict[str, int]:
    partitions: dict[str, int] = {}
    with path.open(newline="", encoding="utf-8") as fh:
        for row in csv.reader(line for line in fh if line.strip() and not line.lstrip().startswith("#")):
            if len(row) < 5:
                continue
            name = row[0].strip()
            partitions[name] = parse_size(row[4])
    return partitions


def check_image(path: Path, partition_name: str, partition_size: int) -> bool:
    if not path.is_file():
        print(f"missing build artifact: {path}", file=sys.stderr)
        return False

    image_size = path.stat().st_size
    free = partition_size - image_size
    print(f"{path}: {image_size} bytes / {partition_size} bytes ({free} bytes free)")
    if image_size > partition_size:
        print(
            f"{path} is larger than {partition_name} partition by {-free} bytes",
            file=sys.stderr,
        )
        return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="Check PlatformIO build images against partitions.csv.")
    parser.add_argument("--partitions", default="partitions.csv", type=Path)
    parser.add_argument("--build-dir", default=".pio/build/esp32s3zero", type=Path)
    args = parser.parse_args()

    partitions = load_partitions(args.partitions)
    required = {"app0", "app1", "littlefs"}
    missing = sorted(required - partitions.keys())
    if missing:
        print(f"missing partitions in {args.partitions}: {', '.join(missing)}", file=sys.stderr)
        return 1

    app_slot_size = min(partitions["app0"], partitions["app1"])
    checks = [
        check_image(args.build_dir / "bootloader.bin", "bootloader", partitions["app0"]),
        check_image(args.build_dir / "firmware.bin", "OTA app", app_slot_size),
        check_image(args.build_dir / "littlefs.bin", "littlefs", partitions["littlefs"]),
        check_image(args.build_dir / "partitions.bin", "partition table", partitions["app0"]),
    ]
    return 0 if all(checks) else 1


if __name__ == "__main__":
    raise SystemExit(main())
