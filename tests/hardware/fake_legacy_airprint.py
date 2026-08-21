#!/usr/bin/env python3
"""Expose a compatibility-lab fixture as a legacy AirPrint printer on the LAN."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import signal
import subprocess
import sys
import threading
from pathlib import Path


OPERATION_NAMES = {
    0x0002: "Print-Job",
    0x0004: "Validate-Job",
    0x0005: "Create-Job",
    0x0006: "Send-Document",
    0x0008: "Cancel-Job",
    0x0009: "Get-Job-Attributes",
    0x000A: "Get-Jobs",
    0x000B: "Get-Printer-Attributes",
}


def load_compat_lab(root: Path):
    source = root / "tests" / "compat" / "compat_lab.py"
    spec = importlib.util.spec_from_file_location("espresso_compat_lab", source)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {source}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    default_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=default_root)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument(
        "--fixture",
        type=Path,
        default=default_root / "tests/compat/fixtures/ipp11-fallback.json",
    )
    parser.add_argument("--port", type=int, default=18632)
    parser.add_argument("--advertise-ip", required=True)
    parser.add_argument("--advertise-host", default="espresso-legacy-test.local")
    parser.add_argument("--service-name", default="ESPresso Legacy AirPrint Test")
    parser.add_argument("--capture-dir", type=Path, required=True)
    args = parser.parse_args()

    root = args.root.resolve()
    compat = load_compat_lab(root)
    fixture = json.loads(args.fixture.resolve().read_text())
    admin_url = f"http://{args.advertise_ip}:{args.port}/"
    fixture.setdefault("ipp", {})["moreInfo"] = admin_url
    codec = compat.Codec(args.library.resolve(), fixture)
    state = compat.LabState(codec, fixture)
    args.capture_dir.mkdir(parents=True, exist_ok=True)

    capture_lock = threading.Lock()
    original_response = state.legacy_response

    def logged_response(request: bytes) -> bytes:
        info = codec.inspect(request)
        operation = OPERATION_NAMES.get(info.operation_id, f"0x{info.operation_id:04x}")
        document_bytes = len(request) - info.attributes_length
        document_format = compat.string_attribute(request, "document-format") or ""
        print(
            f"request: IPP {info.major}.{info.minor} {operation}; "
            f"body={len(request)} attributes={info.attributes_length} "
            f"document={document_bytes} format={document_format or 'unspecified'}",
            flush=True,
        )
        with capture_lock:
            before = len(state.captured_documents)
            response = original_response(request)
            for document in state.captured_documents[before:]:
                extension = {
                    "application/pdf": "pdf",
                    "image/urf": "urf",
                }.get(document_format, "document")
                number = len(list(args.capture_dir.glob("job-*"))) + 1
                destination = args.capture_dir / f"job-{number:03d}.{extension}"
                destination.write_bytes(document)
                digest = hashlib.sha256(document).hexdigest()
                print(
                    f"captured: {destination} bytes={len(document)} sha256={digest}",
                    flush=True,
                )
        return response

    state.legacy_response = logged_response
    legacy_handler = compat.make_legacy_handler(state)

    def printer_home(handler):
        body = (
            "<!doctype html><meta name=viewport content='width=device-width'>"
            "<title>Legacy AirPrint Test Printer</title>"
            "<h1>Legacy AirPrint Test Printer</h1>"
            "<p>This is the physical-printer website advertised to ESPresso.</p>"
        ).encode()
        handler.send_response(200)
        handler.send_header("Content-Type", "text/html; charset=utf-8")
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)

    legacy_handler.do_GET = printer_home
    server = compat.ThreadingHTTPServer(("0.0.0.0", args.port), legacy_handler)
    server.timeout = 0.5

    txt = [
        "txtvers=1",
        "qtotal=1",
        "rp=ipp/print",
        f"ty={fixture['name']}",
        "product=(Legacy AirPrint test fixture)",
        "note=Legacy AirPrint test printer",
        f"adminurl={admin_url}",
        "pdl=image/urf,application/pdf",
        "URF=W8,SRGB24,CP1,IS1-4-5-19,MT1-2-3-4-5-6,RS300-600,V1.4,DM1",
        "Color=T",
        "Duplex=T",
        "Copies=T",
        "Collate=T",
        "UUID=2f097cdc-42d3-4ca9-86ef-73f31758df45",
    ]
    advertisement = subprocess.Popen(
        [
            "dns-sd",
            "-P",
            args.service_name,
            "_ipp._tcp",
            "local",
            str(args.port),
            args.advertise_host,
            args.advertise_ip,
            *txt,
        ]
    )

    stopping = False

    def stop(_signum, _frame):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    print(
        f"listening: ipp://{args.advertise_host}:{args.port}/ipp/print "
        f"({args.advertise_ip})",
        flush=True,
    )
    print("advertising: base _ipp._tcp only (legacy-style AirPrint)", flush=True)

    try:
        while not stopping:
            if advertisement.poll() is not None:
                raise RuntimeError(
                    f"dns-sd exited unexpectedly with status {advertisement.returncode}"
                )
            server.handle_request()
    finally:
        advertisement.terminate()
        try:
            advertisement.wait(timeout=3)
        except subprocess.TimeoutExpired:
            advertisement.kill()
        server.server_close()
        codec.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
