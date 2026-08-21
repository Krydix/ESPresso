#!/usr/bin/env python3
"""Capture iOS AirPrint traffic while proxying Apple's IPP Everywhere sample."""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import signal
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


OPERATIONS = {
    0x0002: "Print-Job",
    0x0004: "Validate-Job",
    0x0005: "Create-Job",
    0x0006: "Send-Document",
    0x0008: "Cancel-Job",
    0x0009: "Get-Job-Attributes",
    0x000A: "Get-Jobs",
    0x000B: "Get-Printer-Attributes",
}


def attributes_length(message: bytes) -> int:
    """Return the offset immediately following IPP end-of-attributes."""
    if len(message) < 8:
        raise ValueError("IPP message is shorter than its header")
    offset = 8
    while offset < len(message):
        tag = message[offset]
        offset += 1
        if tag == 0x03:
            return offset
        if tag <= 0x0F:
            continue
        if offset + 2 > len(message):
            break
        name_length = int.from_bytes(message[offset:offset + 2], "big")
        offset += 2 + name_length
        if offset + 2 > len(message):
            break
        value_length = int.from_bytes(message[offset:offset + 2], "big")
        offset += 2 + value_length
    raise ValueError("IPP end-of-attributes tag is missing")


def rewrite_attribute_values(message: bytes, replacements: list[tuple[bytes, bytes]]) -> bytes:
    """Rewrite strings inside IPP attribute values without touching document data."""
    end = attributes_length(message)
    output = bytearray(message[:8])
    offset = 8
    while offset < end:
        tag = message[offset]
        output.append(tag)
        offset += 1
        if tag == 0x03:
            break
        if tag <= 0x0F:
            continue
        name_length = int.from_bytes(message[offset:offset + 2], "big")
        name_end = offset + 2 + name_length
        output.extend(message[offset:name_end])
        offset = name_end
        value_length = int.from_bytes(message[offset:offset + 2], "big")
        value_start = offset + 2
        value_end = value_start + value_length
        value = message[value_start:value_end]
        for old, new in replacements:
            value = value.replace(old, new)
        output.extend(len(value).to_bytes(2, "big"))
        output.extend(value)
        offset = value_end
    output.extend(message[end:])
    return bytes(output)


def read_body(handler: BaseHTTPRequestHandler) -> tuple[bytes, str]:
    transfer_encoding = handler.headers.get("Transfer-Encoding", "")
    if transfer_encoding:
        if transfer_encoding.lower() != "chunked" or handler.headers.get("Content-Length"):
            raise ValueError("ambiguous or unsupported HTTP request framing")
        body = bytearray()
        while True:
            size_line = handler.rfile.readline(257)
            if not size_line.endswith(b"\r\n"):
                raise ValueError("malformed HTTP chunk size")
            chunk_size = int(size_line[:-2].split(b";", 1)[0].strip(), 16)
            if chunk_size == 0:
                while handler.rfile.readline(4097) != b"\r\n":
                    pass
                break
            chunk = handler.rfile.read(chunk_size)
            if len(chunk) != chunk_size or handler.rfile.read(2) != b"\r\n":
                raise ValueError("truncated HTTP chunk")
            body.extend(chunk)
        return bytes(body), "chunked"
    length = int(handler.headers.get("Content-Length", "0"))
    if length < 8:
        raise ValueError("missing IPP request body")
    body = handler.rfile.read(length)
    if len(body) != length:
        raise ValueError("truncated HTTP request body")
    return body, "content-length"


def make_handler(args, capture_lock: threading.Lock, request_number: list[int]):
    proxy_base = f"ipp://{args.advertise_host}:{args.port}".encode()
    upstream_base = f"ipp://{args.upstream_host}:{args.upstream_port}".encode()
    request_replacements = [(proxy_base, upstream_base)]
    response_replacements = [
        (f"ipps://{args.upstream_host}:{args.upstream_port}".encode(), proxy_base),
        (upstream_base, proxy_base),
        (f"https://{args.upstream_host}:{args.upstream_port}".encode(),
         f"http://{args.advertise_host}:{args.port}".encode()),
    ]

    class CaptureHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, _format, *_values):
            pass

        def do_POST(self):
            try:
                request, framing = read_body(self)
                end = attributes_length(request)
                operation_id = int.from_bytes(request[2:4], "big")
                operation = OPERATIONS.get(operation_id, f"0x{operation_id:04x}")
                with capture_lock:
                    request_number[0] += 1
                    number = request_number[0]
                    stem = args.capture_dir / f"request-{number:03d}-{operation.lower()}"
                    stem.with_suffix(".ipp").write_bytes(request)
                    stem.with_suffix(".json").write_text(json.dumps({
                        "client": self.client_address[0],
                        "path": self.path,
                        "framing": framing,
                        "headers": dict(self.headers.items()),
                        "ipp_version": f"{request[0]}.{request[1]}",
                        "operation": operation,
                        "operation_id": operation_id,
                        "request_id": int.from_bytes(request[4:8], "big"),
                        "body_bytes": len(request),
                        "attribute_bytes": end,
                        "document_bytes": len(request) - end,
                        "sha256": hashlib.sha256(request).hexdigest(),
                    }, indent=2) + "\n")
                    if len(request) > end:
                        stem.with_suffix(".document").write_bytes(request[end:])
                print(
                    f"capture {number:03d}: {self.client_address[0]} "
                    f"IPP {request[0]}.{request[1]} {operation}; framing={framing} "
                    f"body={len(request)} attributes={end} document={len(request) - end}",
                    flush=True,
                )

                upstream_request = rewrite_attribute_values(request, request_replacements)
                connection = http.client.HTTPConnection(
                    args.upstream_address, args.upstream_port, timeout=30
                )
                connection.request("POST", self.path, upstream_request, {
                    "Content-Type": "application/ipp",
                    "Accept": "application/ipp",
                    "User-Agent": self.headers.get("User-Agent", "AirPrint capture proxy"),
                    "Host": f"{args.upstream_host}:{args.upstream_port}",
                })
                upstream = connection.getresponse()
                response = upstream.read()
                status = upstream.status
                content_type = upstream.getheader("Content-Type", "application/ipp")
                connection.close()
                if content_type.split(";", 1)[0].strip() == "application/ipp":
                    response = rewrite_attribute_values(response, response_replacements)

                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(response)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(response)
                if len(response) >= 4:
                    print(
                        f"response {number:03d}: HTTP {status}; "
                        f"IPP status=0x{int.from_bytes(response[2:4], 'big'):04x} "
                        f"bytes={len(response)}",
                        flush=True,
                    )
            except Exception as error:
                print(f"capture error: {type(error).__name__}: {error}", flush=True)
                self.send_error(500, str(error))

    return CaptureHandler


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=18634)
    parser.add_argument("--advertise-ip", required=True)
    parser.add_argument("--advertise-host", default="capture-air.local")
    parser.add_argument("--service-name", default="Reference AirPrint Capture")
    parser.add_argument("--upstream-address", default="127.0.0.1")
    parser.add_argument("--upstream-host", default="air.local.")
    parser.add_argument("--upstream-port", type=int, default=18633)
    parser.add_argument("--capture-dir", type=Path, required=True)
    args = parser.parse_args()
    args.capture_dir.mkdir(parents=True, exist_ok=True)

    server = ThreadingHTTPServer(
        ("0.0.0.0", args.port), make_handler(args, threading.Lock(), [0])
    )
    server.daemon_threads = True
    txt = [
        "txtvers=1", "qtotal=1", "rp=ipp/print", "ty=Example Printer",
        "pdl=application/pdf,image/urf", "Color=F", "Duplex=T",
        "UUID=eabf1f84-999a-48c7-8b55-a6c748843c65",
        "URF=CP1,IS1-4-5-19,MT1-2-3-4-5-6,RS600,V1.4,W8,DM1",
    ]
    advertisement = subprocess.Popen([
        "dns-sd", "-P", args.service_name, "_ipp._tcp,_universal", "local",
        str(args.port), args.advertise_host, args.advertise_ip, *txt,
    ])
    stopping = threading.Event()

    def stop(_signum, _frame):
        stopping.set()
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    print(
        f"listening: ipp://{args.advertise_host}:{args.port}/ipp/print; "
        f"forwarding to ipp://{args.upstream_host}:{args.upstream_port}/ipp/print",
        flush=True,
    )
    try:
        server.serve_forever(poll_interval=0.25)
    finally:
        advertisement.terminate()
        try:
            advertisement.wait(timeout=3)
        except subprocess.TimeoutExpired:
            advertisement.kill()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
