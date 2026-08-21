# Compatibility lab

The compatibility lab tests ESPresso without an ESP32 or physical printer. It runs a
configurable legacy IPP printer and an ESPresso facade on the host, with the facade
calling the same `ipp_codec.c` implementation compiled into the firmware.

```text
CUPS ipptool
    |
    v
host ESPresso facade :18631
    |
    v
legacy printer fixture :18632
```

Run the complete hardware-free protocol suite with:

```sh
make test
make test-cups
make test-compat
make test-sanitize
make test-roadmap
make test-conformance-report
```

`test-compat` requires `ipptool` and `cups-config`. On Debian/Ubuntu these are
provided by `cups-ipp-utils` and `libcups2-dev`.

## What the gate proves

For each JSON fixture the lab:

1. starts a stateful legacy-printer emulator;
2. sends an IPP/2.0 capability query and performs an IPP/1.1 fallback when required;
3. merges generic and document-format-specific capabilities into the real compact
   ESPresso profile;
4. starts the host facade and queries both endpoints through CUPS `ipptool`;
5. canonicalizes CUPS plist output and compares relay-exact and normalized attributes;
6. exercises unique job IDs, document attachment, completion, cancellation,
   unknown-job errors, the supported job lifecycle, and local error responses;
7. verifies small and 1 MiB URF documents arrive byte-for-byte unchanged;
8. runs selected RFC 8011 malformed-request tests; and
9. requires the stock CUPS `get-printer-attributes.test` to accept the facade;
10. executes the same transport-independent request policy and short-I/O stream pump
    compiled into the firmware; and
11. gates the complete CUPS IPP/1.1 suite.

Current fixtures cover a normal IPP/2.0 AirPrint printer, IPP/2.0 rejection with 1.1
fallback, old `output-mode-*` color attributes, URF recovery from DNS-SD, fragmented
chunked upstream HTTP responses, and a truncated IPP response that must be rejected.

## Red–green contract

[`tests/feature-matrix.json`](../tests/feature-matrix.json) is the scope ledger. Every
feature is `supported`, `expected-fail`, `hardware-required`, or `out-of-scope`.
Supported entries name their executable evidence. Expected failures name their future
target and acceptance criteria.

`make test-roadmap` executes the nearest expected-red behavior while keeping the main
CI result green: incoming chunked client requests and RFC requested-attribute group
selectors must still fail in their known way, while format-conditioned media queries
must pass. An unexpected pass or regression fails the runner so the matrix cannot
silently become stale.

`make test-conformance-report` runs CUPS `ipp-1.1.test`, `ipp-2.0.test`, and
`ipp-everywhere.test` with continue-on-error reporting. IPP/1.1 is a required green
gate. IPP/2.0 and IPP Everywhere remain expected red because their mandatory
description attributes, formats, and operations extend beyond the truthful Phase 1
relay contract. CI uploads the JSON reports for both Linux and macOS CUPS.

## Fixture contract

Fixtures live in `tests/compat/fixtures`. Each declares:

- `dns`: capabilities learned from DNS-SD TXT records;
- `ipp`: the legacy printer's IPP version, operations, formats and feature values;
- `faults`: optional transport or malformed-response behavior;
- `relayExact`: attributes whose CUPS-parsed semantic values must remain unchanged;
- `expected`: normalized facade values; and
- `expectedProbeFailure`: negative fixtures that must not produce a usable profile.

Comparisons intentionally ignore request IDs, bridge UUIDs, URI authorities,
attribute ordering, uptime values and other instance-specific data. CUPS scheduler
features such as filtering, rendering, subscriptions and durable spooling are not part
of the parity target.

## Adding a captured printer

Add a JSON fixture containing its DNS-SD and IPP behavior, preferably with secrets,
hostnames and serial numbers removed. Binary captured IPP responses can be added later
when a behavior cannot be represented by the emulator. Every new compatibility rule
should arrive with a fixture that fails before the rule and passes afterward.

The lab cannot replace tests involving real iOS discovery, multicast behavior across
consumer routers, ESP32 memory pressure, or physical-printer firmware. Those form the
next integration layer; a physical printer is not needed to test firmware against the
host emulator.
