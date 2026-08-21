#!/usr/bin/env python3
"""Stage one ESP Web Tools site from ESP-IDF's target-specific flash maps."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
from datetime import datetime, timezone
from pathlib import Path


CHIP_FAMILIES = {
    "esp32": "ESP32",
    "esp32s2": "ESP32-S2",
    "esp32s3": "ESP32-S3",
}


def target_build(value: str) -> tuple[str, Path]:
    target, separator, build_dir = value.partition("=")
    if not separator or not build_dir:
        raise argparse.ArgumentTypeError("expected TARGET=BUILD_DIR")
    if target not in CHIP_FAMILIES:
        supported = ", ".join(CHIP_FAMILIES)
        raise argparse.ArgumentTypeError(
            f"unsupported target {target!r}; choose one of: {supported}"
        )
    return target, Path(build_dir)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--build",
        action="append",
        type=target_build,
        required=True,
        metavar="TARGET=BUILD_DIR",
        help="add an ESP-IDF build (repeat once per target)",
    )
    parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args()


def project_version(build_dir: Path, root: Path) -> str:
    try:
        description = json.loads(
            (build_dir / "project_description.json").read_text(encoding="utf-8")
        )
        return description["project_version"]
    except (OSError, KeyError, json.JSONDecodeError):
        try:
            return subprocess.check_output(
                ["git", "-C", str(root), "rev-parse", "--short", "HEAD"],
                text=True,
                stderr=subprocess.DEVNULL,
            ).strip()
        except (subprocess.CalledProcessError, FileNotFoundError):
            return "unknown"


def main() -> None:
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    setup_suffix = os.environ.get("ESPRESSO_SETUP_SUFFIX", "DEV0").upper()
    if not re.fullmatch(r"[A-Z0-9]{4}", setup_suffix):
        raise SystemExit("ESPRESSO_SETUP_SUFFIX must contain exactly four letters or digits")
    setup_ssid = f"ESPresso-{setup_suffix}"

    builds = dict(args.build)
    if len(builds) != len(args.build):
        raise SystemExit("each target may only be specified once")

    firmware_dir = args.output_dir / "firmware"
    if firmware_dir.exists():
        shutil.rmtree(firmware_dir)
    firmware_dir.mkdir(parents=True)
    shutil.copy2(root / "web-installer" / "index.html", args.output_dir / "index.html")
    shutil.copy2(root / "favicon.svg", args.output_dir / "favicon.svg")
    shutil.copy2(root / "logo.png", args.output_dir / "logo.png")

    manifest_builds = []
    target_info = []
    versions = set()
    for target in CHIP_FAMILIES:
        if target not in builds:
            continue
        build_dir = builds[target]
        flash_args_path = build_dir / "flasher_args.json"
        if not flash_args_path.is_file():
            raise SystemExit(f"missing {flash_args_path}; build {target} first")

        flash_args = json.loads(flash_args_path.read_text(encoding="utf-8"))
        flash_files = flash_args.get("flash_files", {})
        if not flash_files:
            raise SystemExit(f"{flash_args_path} contains no flash_files")
        built_target = flash_args.get("extra_esptool_args", {}).get("chip")
        if built_target and built_target != target:
            raise SystemExit(
                f"{flash_args_path} is for {built_target}, not requested target {target}"
            )

        target_dir = firmware_dir / target
        target_dir.mkdir()
        parts = []
        for offset, relative_name in flash_files.items():
            source = build_dir / relative_name
            if not source.is_file():
                raise SystemExit(f"missing build artifact: {source}")
            destination_name = relative_name.replace("/", "-")
            shutil.copy2(source, target_dir / destination_name)
            parts.append(
                {
                    "path": f"./firmware/{target}/{destination_name}",
                    "offset": int(offset, 0),
                }
            )
        parts.sort(key=lambda part: part["offset"])
        manifest_builds.append(
            {"chipFamily": CHIP_FAMILIES[target], "parts": parts}
        )

        version = project_version(build_dir, root)
        versions.add(version)
        info = {
            "target": target,
            "chipFamily": CHIP_FAMILIES[target],
        }
        app_file = flash_args.get("app", {}).get("file")
        if app_file:
            app_source = build_dir / app_file
            app_destination = app_file.replace("/", "-")
            info["ota"] = {
                "path": f"./firmware/{target}/{app_destination}",
                "size": app_source.stat().st_size,
                "sha256": hashlib.sha256(app_source.read_bytes()).hexdigest(),
            }
        target_info.append(info)

    if len(versions) != 1:
        raise SystemExit(
            "all target builds must have the same project version; got: "
            + ", ".join(sorted(versions))
        )
    version = versions.pop()
    manifest = {
        "name": "ESPresso",
        "version": version,
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 15,
        "builds": manifest_builds,
    }
    (args.output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )

    build_info = {
        "project": "ESPresso",
        "version": version,
        "builtAt": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "chipFamilies": [item["chipFamily"] for item in target_info],
        "setupSsid": setup_ssid,
        "targets": target_info,
    }
    (args.output_dir / "build-info.json").write_text(
        json.dumps(build_info, indent=2) + "\n", encoding="utf-8"
    )
    (args.output_dir / ".nojekyll").touch()
    targets = ", ".join(builds)
    print(f"staged web installer for {targets} in {args.output_dir}")


if __name__ == "__main__":
    main()
