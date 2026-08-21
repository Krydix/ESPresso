#!/usr/bin/env python3
"""Stage an ESP Web Tools site from ESP-IDF's authoritative flash map."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
from datetime import datetime, timezone
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    setup_suffix = os.environ.get("ESPRESSO_SETUP_SUFFIX", "DEV0").upper()
    if not re.fullmatch(r"[A-Z0-9]{4}", setup_suffix):
        raise SystemExit("ESPRESSO_SETUP_SUFFIX must contain exactly four letters or digits")
    setup_ssid = f"ESPresso-{setup_suffix}"
    flash_args_path = args.build_dir / "flasher_args.json"
    if not flash_args_path.is_file():
        raise SystemExit(f"missing {flash_args_path}; run 'make build' first")

    flash_args = json.loads(flash_args_path.read_text(encoding="utf-8"))
    flash_files = flash_args.get("flash_files", {})
    if not flash_files:
        raise SystemExit("flasher_args.json contains no flash_files")

    firmware_dir = args.output_dir / "firmware"
    firmware_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / "web-installer" / "index.html", args.output_dir / "index.html")
    shutil.copy2(root / "logo.png", args.output_dir / "logo.png")

    parts = []
    for offset, relative_name in flash_files.items():
        source = args.build_dir / relative_name
        if not source.is_file():
            raise SystemExit(f"missing build artifact: {source}")
        destination_name = relative_name.replace("/", "-")
        shutil.copy2(source, firmware_dir / destination_name)
        parts.append({"path": f"./firmware/{destination_name}", "offset": int(offset, 0)})

    parts.sort(key=lambda part: part["offset"])
    manifest = {
        "name": "ESPresso",
        "version": "main",
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32-S3", "parts": parts}],
    }
    (args.output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )

    try:
        version = subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "--short", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        version = "unknown"
    build_info = {
        "project": "ESPresso",
        "version": version,
        "builtAt": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "chipFamily": "ESP32-S3",
        "setupSsid": setup_ssid,
    }
    (args.output_dir / "build-info.json").write_text(
        json.dumps(build_info, indent=2) + "\n", encoding="utf-8"
    )
    (args.output_dir / ".nojekyll").touch()
    print(f"staged web installer in {args.output_dir}")


if __name__ == "__main__":
    main()
