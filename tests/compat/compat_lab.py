#!/usr/bin/env python3
"""Hardware-free ESPresso compatibility lab using CUPS as the IPP client."""

from __future__ import annotations

import argparse
import ctypes
import http.client
import json
import plistlib
import struct
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


IPP_PRINT_JOB = 0x0002
IPP_VALIDATE_JOB = 0x0004
IPP_CREATE_JOB = 0x0005
IPP_SEND_DOCUMENT = 0x0006
IPP_CANCEL_JOB = 0x0008
IPP_GET_JOB_ATTRIBUTES = 0x0009
IPP_GET_JOBS = 0x000A
IPP_GET_PRINTER_ATTRIBUTES = 0x000B


class RequestInfo(ctypes.Structure):
    _fields_ = [
        ("major", ctypes.c_uint8),
        ("minor", ctypes.c_uint8),
        ("operation_id", ctypes.c_uint16),
        ("request_id", ctypes.c_uint32),
        ("attributes_length", ctypes.c_size_t),
        ("has_document", ctypes.c_bool),
        ("has_attributes_charset", ctypes.c_bool),
        ("has_natural_language", ctypes.c_bool),
        ("has_target_uri", ctypes.c_bool),
        ("operation_attributes_valid", ctypes.c_bool),
        ("attributes_charset", ctypes.c_char * 32),
        ("document_format", ctypes.c_char * 64),
        ("requested_attributes", ctypes.c_char * 512),
    ]


class Codec:
    def __init__(self, library: Path, fixture: dict):
        self.lib = ctypes.CDLL(str(library))
        self._declare()
        dns = fixture.get("dns", {})
        self.handle = self.lib.espresso_bridge_new(
            fixture["name"].encode(), dns.get("pdl", "").encode(),
            dns.get("urf", "").encode(), dns.get("media", "").encode())
        if not self.handle:
            raise MemoryError("could not allocate compatibility profile")
        self.lock = threading.Lock()

    def _declare(self):
        u8p = ctypes.POINTER(ctypes.c_uint8)
        voidpp = ctypes.POINTER(ctypes.c_void_p)
        sizep = ctypes.POINTER(ctypes.c_size_t)
        self.lib.espresso_bridge_new.argtypes = [ctypes.c_char_p] * 4
        self.lib.espresso_bridge_new.restype = ctypes.c_void_p
        self.lib.espresso_bridge_delete.argtypes = [ctypes.c_void_p]
        self.lib.espresso_bridge_free.argtypes = [ctypes.c_void_p]
        self.lib.espresso_bridge_apply.argtypes = [ctypes.c_void_p, u8p,
                                                    ctypes.c_size_t]
        self.lib.espresso_bridge_apply.restype = ctypes.c_int
        self.lib.espresso_bridge_query.argtypes = [ctypes.c_uint8, ctypes.c_uint8,
                                                    ctypes.c_uint32, ctypes.c_char_p,
                                                    voidpp, sizep]
        self.lib.espresso_bridge_query.restype = ctypes.c_int
        self.lib.espresso_bridge_inspect.argtypes = [u8p, ctypes.c_size_t,
                                                      ctypes.POINTER(RequestInfo)]
        self.lib.espresso_bridge_inspect.restype = ctypes.c_int
        self.lib.espresso_bridge_rewrite.argtypes = [u8p, ctypes.c_size_t,
                                                      ctypes.c_char_p, ctypes.c_char_p,
                                                      voidpp, sizep, sizep]
        self.lib.espresso_bridge_rewrite.restype = ctypes.c_int
        self.lib.espresso_bridge_normalize.argtypes = [ctypes.c_void_p, u8p,
                                                        ctypes.c_size_t,
                                                        ctypes.c_char_p, voidpp,
                                                        sizep, sizep]
        self.lib.espresso_bridge_normalize.restype = ctypes.c_int
        self.lib.espresso_bridge_status.argtypes = [ctypes.c_uint8, ctypes.c_uint8,
                                                     ctypes.c_uint16, ctypes.c_uint32,
                                                     ctypes.c_char_p, voidpp, sizep]
        self.lib.espresso_bridge_status.restype = ctypes.c_int
        self.lib.espresso_bridge_operations.argtypes = [ctypes.c_void_p]
        self.lib.espresso_bridge_operations.restype = ctypes.c_uint64
        self.lib.espresso_bridge_format_supported.argtypes = [ctypes.c_void_p,
                                                               ctypes.c_char_p]
        self.lib.espresso_bridge_format_supported.restype = ctypes.c_int
        self.lib.espresso_bridge_upstream_major.argtypes = [ctypes.c_void_p]
        self.lib.espresso_bridge_upstream_major.restype = ctypes.c_uint8
        self.lib.espresso_bridge_upstream_minor.argtypes = [ctypes.c_void_p]
        self.lib.espresso_bridge_upstream_minor.restype = ctypes.c_uint8

    @staticmethod
    def _input(data: bytes):
        return (ctypes.c_uint8 * len(data)).from_buffer_copy(data)

    def _output(self, call) -> bytes:
        pointer = ctypes.c_void_p()
        length = ctypes.c_size_t()
        if not call(ctypes.byref(pointer), ctypes.byref(length)):
            raise ValueError("IPP codec rejected compatibility-lab message")
        try:
            return ctypes.string_at(pointer, length.value)
        finally:
            self.lib.espresso_bridge_free(pointer)

    def query(self, major: int, minor: int, request_id: int,
              document_format: str = "") -> bytes:
        return self._output(lambda out, length: self.lib.espresso_bridge_query(
            major, minor, request_id, document_format.encode(), out, length))

    def inspect(self, data: bytes) -> RequestInfo:
        info = RequestInfo()
        if not self.lib.espresso_bridge_inspect(self._input(data), len(data),
                                                 ctypes.byref(info)):
            raise ValueError("malformed IPP message")
        return info

    def apply(self, data: bytes):
        with self.lock:
            if not self.lib.espresso_bridge_apply(self.handle, self._input(data),
                                                   len(data)):
                raise ValueError("could not apply printer capabilities")

    def rewrite(self, data: bytes, printer_uri: str, authority: str) -> bytes:
        source = self._input(data)
        attributes_length = ctypes.c_size_t()
        return self._output(lambda out, length: self.lib.espresso_bridge_rewrite(
            source, len(data), printer_uri.encode(), authority.encode(), out,
            length, ctypes.byref(attributes_length)))

    def normalize(self, data: bytes, requested_attributes: str = "") -> bytes:
        source = self._input(data)
        attributes_length = ctypes.c_size_t()
        with self.lock:
            return self._output(lambda out, length:
                self.lib.espresso_bridge_normalize(
                    self.handle, source, len(data), requested_attributes.encode(),
                    out, length,
                    ctypes.byref(attributes_length)))

    def status(self, major: int, minor: int, status: int, request_id: int,
               message: str) -> bytes:
        return self._output(lambda out, length: self.lib.espresso_bridge_status(
            major, minor, status, request_id, message.encode(), out, length))

    def operations(self) -> int:
        with self.lock:
            return int(self.lib.espresso_bridge_operations(self.handle))

    def supports_format(self, document_format: str) -> bool:
        with self.lock:
            return bool(self.lib.espresso_bridge_format_supported(
                self.handle, document_format.encode()))

    def upstream_version(self) -> tuple[int, int]:
        with self.lock:
            return (int(self.lib.espresso_bridge_upstream_major(self.handle)),
                    int(self.lib.espresso_bridge_upstream_minor(self.handle)))

    def close(self):
        if self.handle:
            self.lib.espresso_bridge_delete(self.handle)
            self.handle = None


def ipp_attr(tag: int, name: str | None, value) -> bytes:
    encoded_name = name.encode() if name else b""
    if isinstance(value, str):
        value = value.encode()
    return bytes([tag]) + struct.pack("!H", len(encoded_name)) + encoded_name + \
        struct.pack("!H", len(value)) + value


def ipp_integer(tag: int, name: str | None, value: int) -> bytes:
    return ipp_attr(tag, name, struct.pack("!I", value))


def ipp_response(version: tuple[int, int], status: int, request_id: int,
                 operation_attrs: list[bytes], groups: list[tuple[int, list[bytes]]]) -> bytes:
    result = bytearray(struct.pack("!BBHI", version[0], version[1], status,
                                   request_id))
    result.append(0x01)
    for attribute in operation_attrs:
        result.extend(attribute)
    for group, attributes in groups:
        result.append(group)
        for attribute in attributes:
            result.extend(attribute)
    result.append(0x03)
    return bytes(result)


def operation_attributes() -> list[bytes]:
    return [ipp_attr(0x47, "attributes-charset", "utf-8"),
            ipp_attr(0x48, "attributes-natural-language", "en")]


class LabState:
    def __init__(self, codec: Codec, fixture: dict):
        self.codec = codec
        self.fixture = fixture
        self.captured_documents: list[bytes] = []

    def legacy_response(self, request: bytes) -> bytes:
        info = self.codec.inspect(request)
        spec = self.fixture["ipp"]
        legacy_version = tuple(int(part) for part in spec.get("version", "1.1").split("."))
        if spec.get("rejectIpp20") and info.major >= 2:
            return ipp_response(legacy_version, 0x0503, info.request_id,
                                operation_attributes(), [])
        if info.operation_id == IPP_GET_PRINTER_ATTRIBUTES:
            attrs: list[bytes] = []
            attrs.append(ipp_attr(0x44, "ipp-versions-supported",
                                  spec.get("version", "1.1")))
            attrs.append(ipp_attr(0x45, "printer-uri-supported",
                                  "ipp://legacy.local:631/ipp/print"))
            attrs.append(ipp_attr(0x45, "printer-uuid",
                                  "urn:uuid:physical-fixture"))
            attrs.append(ipp_attr(0x44, "uri-security-supported", "none"))
            formats = spec.get("pdl", [])
            for index, value in enumerate(formats):
                attrs.append(ipp_attr(0x49,
                                      "document-format-supported" if index == 0 else None,
                                      value))
            for index, value in enumerate(spec.get("urf", [])):
                attrs.append(ipp_attr(0x44, "urf-supported" if index == 0 else None,
                                      value))
            for index, value in enumerate(spec.get("media", [])):
                attrs.append(ipp_attr(0x44, "media-supported" if index == 0 else None,
                                      value))
            if spec.get("mediaDefault"):
                attrs.append(ipp_attr(0x44, "media-default", spec["mediaDefault"]))
            color_name = "output-mode-supported" if spec.get("oldOutputMode") else \
                         "print-color-mode-supported"
            modes = ["monochrome", "color"] if spec.get("color", True) else ["monochrome"]
            for index, value in enumerate(modes):
                attrs.append(ipp_attr(0x44, color_name if index == 0 else None, value))
            default_name = "output-mode-default" if spec.get("oldOutputMode") else \
                           "print-color-mode-default"
            attrs.append(ipp_attr(0x44, default_name,
                                  "color" if spec.get("color", True) else "monochrome"))
            attrs.append(ipp_attr(0x22, "color-supported",
                                  bytes([1 if spec.get("color", True) else 0])))
            sides = ["one-sided"] + (["two-sided-long-edge", "two-sided-short-edge"]
                                      if spec.get("duplex", True) else [])
            for index, value in enumerate(sides):
                attrs.append(ipp_attr(0x44, "sides-supported" if index == 0 else None,
                                      value))
            attrs.append(ipp_attr(0x33, "copies-supported",
                                  struct.pack("!II", 1, spec.get("copies", 99))))
            for index, value in enumerate(spec.get("operations", [2, 4, 5, 6, 8, 9, 10, 11])):
                attrs.append(ipp_integer(0x23,
                                         "operations-supported" if index == 0 else None,
                                         value))
            attrs.append(ipp_integer(0x23, "printer-state", spec.get("state", 3)))
            attrs.append(ipp_attr(0x22, "printer-is-accepting-jobs",
                                  bytes([1 if spec.get("accepting", True) else 0])))
            attrs.append(ipp_attr(0x44, "printer-state-reasons",
                                  spec.get("stateReason", "none")))
            attrs.append(ipp_attr(0x41, "printer-make-and-model", self.fixture["name"]))
            if spec.get("location"):
                attrs.append(ipp_attr(0x41, "printer-location", spec["location"]))
            return ipp_response(legacy_version, 0, info.request_id,
                                operation_attributes(), [(0x04, attrs)])

        if info.operation_id in (IPP_PRINT_JOB, IPP_CREATE_JOB,
                                  IPP_GET_JOB_ATTRIBUTES, IPP_GET_JOBS):
            if info.operation_id == IPP_PRINT_JOB:
                self.captured_documents.append(request[info.attributes_length:])
            job = [ipp_integer(0x21, "job-id", 42),
                   ipp_attr(0x45, "job-uri", "ipp://legacy.local:631/jobs/42"),
                   ipp_integer(0x23, "job-state", 3),
                   ipp_attr(0x44, "job-state-reasons", "none")]
            return ipp_response(legacy_version, 0, info.request_id,
                                operation_attributes(), [(0x02, job)])
        if info.operation_id == IPP_SEND_DOCUMENT:
            self.captured_documents.append(request[info.attributes_length:])
        return ipp_response(legacy_version, 0, info.request_id,
                            operation_attributes(), [])


class QuietHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, _format, *_args):
        pass

    def read_ipp(self) -> bytes:
        length = int(self.headers.get("Content-Length", "0"))
        if length < 8:
            raise ValueError("missing IPP request body")
        return self.rfile.read(length)

    def send_ipp(self, body: bytes, chunked: bool = False, piece_size: int = 0):
        self.send_response(200)
        self.send_header("Content-Type", "application/ipp")
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if chunked:
            piece_size = piece_size or 37
            for offset in range(0, len(body), piece_size):
                piece = body[offset:offset + piece_size]
                self.wfile.write(f"{len(piece):x}\r\n".encode())
                self.wfile.write(piece + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif piece_size:
            for offset in range(0, len(body), piece_size):
                self.wfile.write(body[offset:offset + piece_size])
                self.wfile.flush()
        else:
            self.wfile.write(body)


def make_legacy_handler(state: LabState):
    class LegacyHandler(QuietHandler):
        def do_POST(self):
            try:
                request = self.read_ipp()
                delay = state.fixture.get("faults", {}).get("delayMs", 0)
                if delay:
                    time.sleep(delay / 1000)
                faults = state.fixture.get("faults", {})
                response = state.legacy_response(request)
                if faults.get("truncateResponse"):
                    response = response[:-1]
                self.send_ipp(response, faults.get("chunkedResponse", False),
                              faults.get("pieceSize", 0))
            except Exception as error:
                self.send_error(500, str(error))
    return LegacyHandler


def post_ipp(port: int, body: bytes) -> bytes:
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    connection.request("POST", "/ipp/print", body,
                       {"Content-Type": "application/ipp",
                        "Accept": "application/ipp"})
    response = connection.getresponse()
    data = response.read()
    connection.close()
    if response.status != 200 or response.getheader("Content-Type", "").split(";")[0] != \
            "application/ipp":
        raise RuntimeError(f"upstream HTTP {response.status}")
    return data


def make_proxy_handler(state: LabState):
    class ProxyHandler(QuietHandler):
        def local_status(self, info: RequestInfo, status: int, message: str):
            self.send_ipp(state.codec.status(info.major, info.minor, status,
                                             info.request_id, message))

        def do_POST(self):
            try:
                request = self.read_ipp()
                info = state.codec.inspect(request)
                if (info.major, info.minor) not in ((1, 1), (2, 0)):
                    self.send_ipp(state.codec.status(2, 0, 0x0503,
                                                     info.request_id,
                                                     "IPP version not supported"))
                    return
                if not (info.request_id and info.has_attributes_charset and
                        info.has_natural_language and info.has_target_uri and
                        info.operation_attributes_valid):
                    self.local_status(info, 0x0400, "Missing operation attribute")
                    return
                operations = state.codec.operations()
                if info.operation_id >= 64 or not operations & (1 << info.operation_id):
                    self.local_status(info, 0x0501, "Operation is not safe to relay")
                    return
                document_format = bytes(info.document_format).split(b"\0", 1)[0].decode()
                if document_format and not state.codec.supports_format(document_format):
                    self.local_status(info, 0x040A, "Document format not supported")
                    return
                upstream_major, upstream_minor = state.codec.upstream_version()
                if info.operation_id == IPP_GET_PRINTER_ATTRIBUTES:
                    upstream = state.codec.query(upstream_major, upstream_minor,
                                                 info.request_id, document_format)
                else:
                    upstream = bytearray(state.codec.rewrite(
                        request, "ipp://127.0.0.1:18632/ipp/print",
                        "ipp://127.0.0.1:18632"))
                    upstream[0:2] = bytes([upstream_major, upstream_minor])
                    upstream = bytes(upstream)
                legacy = post_ipp(18632, upstream)
                if info.operation_id == IPP_GET_PRINTER_ATTRIBUTES:
                    state.codec.apply(legacy)
                    requested = bytes(info.requested_attributes).split(
                        b"\0", 1)[0].decode()
                    response = state.codec.normalize(legacy, requested)
                else:
                    response = state.codec.rewrite(
                        legacy, "ipp://127.0.0.1:18631/ipp/print",
                        "ipp://127.0.0.1:18631")
                response = bytearray(response)
                response[0:2] = bytes([info.major, info.minor])
                self.send_ipp(bytes(response))
            except Exception as error:
                self.send_error(502, str(error))
    return ProxyHandler


def probe(state: LabState):
    response = post_ipp(18632, state.codec.query(2, 0, 1))
    status = struct.unpack("!H", response[2:4])[0]
    if status >= 0x0400:
        response = post_ipp(18632, state.codec.query(1, 1, 2))
        status = struct.unpack("!H", response[2:4])[0]
    if status >= 0x0400:
        raise RuntimeError(f"fixture rejected capability probe: 0x{status:04x}")
    state.codec.apply(response)
    formats = state.fixture["ipp"].get("pdl", [])
    formats = sorted(formats, key=lambda value: value != "image/urf")[:6]
    for request_id, document_format in enumerate(formats, 10):
        response = post_ipp(18632, state.codec.query(
            *state.codec.upstream_version(), request_id, document_format))
        if struct.unpack("!H", response[2:4])[0] < 0x0400:
            state.codec.apply(response)


def flatten_response(plist_path: Path) -> dict:
    with plist_path.open("rb") as source:
        report = plistlib.load(source)
    tests = report["Tests"]
    if not tests or tests[0]["StatusCode"] != "successful-ok":
        raise AssertionError(f"CUPS query failed: {plist_path}")
    flattened: dict = {}
    for group in tests[0].get("ResponseAttributes", []):
        flattened.update(group)
    return flattened


def canonical(value):
    if isinstance(value, list):
        return sorted((canonical(item) for item in value), key=lambda item: repr(item))
    if isinstance(value, dict):
        return {key: canonical(value[key]) for key in sorted(value)}
    return value


def run_ipptool(root: Path, uri: str, test_file: Path, plist_path: Path | None = None,
                version: str | None = None, filename: Path | None = None):
    command = ["ipptool", "-L"]
    if plist_path:
        command += ["-P", str(plist_path)]
    elif test_file.name.startswith("ipp-"):
        command.append("-t")
    else:
        command.append("-q")
    if version:
        command += ["-V", version]
    if filename:
        command += ["-f", str(filename)]
    command += [uri, str(test_file)]
    subprocess.run(command, cwd=root, check=True)


def validate_fixture(root: Path, library: Path, fixture_path: Path):
    fixture = json.loads(fixture_path.read_text())
    codec = Codec(library, fixture)
    state = LabState(codec, fixture)
    legacy = ThreadingHTTPServer(("127.0.0.1", 18632), make_legacy_handler(state))
    legacy_thread = threading.Thread(target=legacy.serve_forever, daemon=True)
    legacy_thread.start()
    proxy = None
    try:
        try:
            probe(state)
        except Exception:
            if fixture.get("expectedProbeFailure"):
                print(f"{fixture_path.stem}: malformed capability rejection [PASS]")
                return
            raise
        if fixture.get("expectedProbeFailure"):
            raise AssertionError(f"{fixture_path.stem}: malformed fixture was accepted")
        proxy = ThreadingHTTPServer(("127.0.0.1", 18631), make_proxy_handler(state))
        proxy_thread = threading.Thread(target=proxy.serve_forever, daemon=True)
        proxy_thread.start()
        with tempfile.TemporaryDirectory(prefix="espresso-compat-") as directory:
            temporary = Path(directory)
            raw_plist = temporary / "legacy.plist"
            facade_plist = temporary / "facade.plist"
            query_test = root / "tests/compat/query.test"
            run_ipptool(root, "ipp://127.0.0.1:18632/ipp/print", query_test,
                        raw_plist, fixture["ipp"].get("version", "1.1"))
            run_ipptool(root, "ipp://127.0.0.1:18631/ipp/print", query_test,
                        facade_plist, "2.0")
            raw = flatten_response(raw_plist)
            facade = flatten_response(facade_plist)
            for attribute in fixture.get("relayExact", []):
                if canonical(raw.get(attribute)) != canonical(facade.get(attribute)):
                    raise AssertionError(
                        f"{fixture_path.stem}: {attribute} differs: "
                        f"legacy={raw.get(attribute)!r}, facade={facade.get(attribute)!r}")
            for attribute, expected in fixture["expected"].items():
                if canonical(facade.get(attribute)) != canonical(expected):
                    raise AssertionError(
                        f"{fixture_path.stem}: {attribute}: "
                        f"expected={expected!r}, got={facade.get(attribute)!r}")

            run_ipptool(root, "ipp://127.0.0.1:18631/ipp/print",
                        root / "tests/compat/job-flow.test")
            run_ipptool(root, "ipp://127.0.0.1:18631/ipp/print",
                        root / "tests/compat/rfc-core.test")
            cups_data = Path(subprocess.check_output(
                ["cups-config", "--datadir"], text=True).strip())
            run_ipptool(root, "ipp://127.0.0.1:18631/ipp/print",
                        cups_data / "ipptool/get-printer-attributes.test")
            document = (root / "tests/minimal.urf").read_bytes()
            if not state.captured_documents or document not in state.captured_documents:
                raise AssertionError(f"{fixture_path.stem}: document bytes changed in relay")
            large_document = b"UNIRAST\0" + bytes(range(256)) * 4096
            large_path = temporary / "large.urf"
            large_path.write_bytes(large_document)
            run_ipptool(root, "ipp://127.0.0.1:18631/ipp/print",
                        root / "tests/compat/large-print.test",
                        filename=large_path)
            if large_document not in state.captured_documents:
                raise AssertionError(f"{fixture_path.stem}: large document bytes changed")
        print(f"{fixture_path.stem}: CUPS differential + job flow [PASS]")
    finally:
        if proxy:
            proxy.shutdown()
            proxy.server_close()
        legacy.shutdown()
        legacy.server_close()
        codec.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("fixtures", nargs="+", type=Path)
    args = parser.parse_args()
    for fixture in args.fixtures:
        validate_fixture(args.root.resolve(), args.library.resolve(), fixture.resolve())
    print(f"Compatibility lab: {len(args.fixtures)} fixture(s) passed")


if __name__ == "__main__":
    main()
