#!/usr/bin/env python3
"""Executable expected failures for the next ESPresso compatibility phase."""

from __future__ import annotations

import argparse
import http.client
import json
import struct
import sys
import threading
from http.server import ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "compat"))
from compat_lab import (Codec, LabState, ipp_attr, make_legacy_handler,
                        make_proxy_handler, parse_ipp_attributes, post_ipp, probe)


def request_message(extra: list[bytes]) -> bytes:
    result = bytearray(struct.pack("!BBHI", 2, 0, 0x000B, 4001))
    result.append(0x01)
    result.extend(ipp_attr(0x47, "attributes-charset", "utf-8"))
    result.extend(ipp_attr(0x48, "attributes-natural-language", "en"))
    result.extend(ipp_attr(0x45, "printer-uri",
                           "ipp://127.0.0.1:18631/ipp/print"))
    for attribute in extra:
        result.extend(attribute)
    result.append(0x03)
    return bytes(result)


def printer_values(message: bytes, name: str) -> list[str]:
    return [value.decode("utf-8", errors="replace")
            for group, _tag, attribute_name, value in parse_ipp_attributes(message)
            if group == 0x04 and attribute_name == name]


def chunked_request_is_supported(body: bytes) -> bool:
    connection = http.client.HTTPConnection("127.0.0.1", 18631, timeout=5)
    try:
        connection.request(
            "POST", "/ipp/print", body=[body],
            headers={"Content-Type": "application/ipp"}, encode_chunked=True)
        response = connection.getresponse()
        response.read()
        return response.status == 200 and \
            response.getheader("Content-Type", "").split(";")[0] == "application/ipp"
    except (ConnectionError, OSError):
        return False
    finally:
        connection.close()


def requested_group_is_supported() -> bool:
    response = post_ipp(18631, request_message([
        ipp_attr(0x44, "requested-attributes", "printer-description")]))
    names = {name for group, _tag, name, _value in parse_ipp_attributes(response)
             if group == 0x04}
    return {"printer-name", "printer-state", "printer-uri-supported"} <= names


def conditional_profiles_are_supported() -> bool:
    response = post_ipp(18631, request_message([
        ipp_attr(0x49, "document-format", "image/urf"),
        ipp_attr(0x44, "requested-attributes", "media-supported")]))
    return printer_values(response, "media-supported") == ["iso_a4_210x297mm"]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    fixture = json.loads(args.fixture.read_text())
    codec = Codec(args.library.resolve(), fixture)
    state = LabState(codec, fixture)
    legacy = ThreadingHTTPServer(("127.0.0.1", 18632), make_legacy_handler(state))
    proxy = None
    threading.Thread(target=legacy.serve_forever, daemon=True).start()
    try:
        probe(state)
        proxy = ThreadingHTTPServer(("127.0.0.1", 18631), make_proxy_handler(state))
        threading.Thread(target=proxy.serve_forever, daemon=True).start()
        probes = [
            ("incoming-chunked-http",
             lambda: chunked_request_is_supported(request_message([])), True),
            ("requested-attribute-groups", requested_group_is_supported, True),
            ("format-specific-capabilities", conditional_profiles_are_supported,
             True),
        ]
        results = []
        unexpected = []
        for feature, probe_function, expected_pass in probes:
            passed = probe_function()
            if passed:
                outcome = "pass" if expected_pass else "unexpected-pass"
            else:
                outcome = "unexpected-fail" if expected_pass else "expected-fail"
            results.append({"feature": feature, "expected":
                            "pass" if expected_pass else "fail",
                            "outcome": outcome})
            print(f"{feature}: {outcome}")
            if passed != expected_pass:
                unexpected.append(feature)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({"schema": 1, "probes": results},
                                          indent=2) + "\n")
        if unexpected:
            raise SystemExit(
                "roadmap outcomes changed; update the feature matrix: " +
                ", ".join(unexpected))
    finally:
        if proxy:
            proxy.shutdown()
            proxy.server_close()
        legacy.shutdown()
        legacy.server_close()
        codec.close()


if __name__ == "__main__":
    main()
