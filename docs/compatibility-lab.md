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
make test-fuzz-smoke
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
7. verifies small and 1 MiB URF documents arrive byte-for-byte unchanged using
   both Content-Length and CUPS `ipptool -C` chunked requests;
8. runs selected RFC 8011 malformed-request tests; and
9. requires the stock CUPS `get-printer-attributes.test`,
   `get-printer-description-attributes.test`, and
   `get-job-template-attributes.test` suites to accept the facade;
10. executes the same transport-independent request policy and short-I/O stream pump
    compiled into the firmware; and
11. verifies job-response filtering, facade-owned defaults and legacy color-mode
    request translation; and
12. gates the complete CUPS IPP/1.1 and IPP/2.0 suites.

Current fixtures cover a normal IPP/2.0 AirPrint printer, IPP/2.0 rejection with 1.1
fallback, old `output-mode-*` color attributes, URF recovery from DNS-SD, chunked
client requests and fragmented chunked upstream HTTP responses, and a truncated IPP
response that must be rejected. The job suites additionally force over-reporting
legacy responses, reject leaked facade-only defaults and require modern color-mode
requests to translate to the older spelling.

## Red–green contract

[`tests/feature-matrix.json`](../tests/feature-matrix.json) is the scope ledger. Every
feature is `supported`, `expected-fail`, `hardware-required`, or `out-of-scope`.
Supported entries name their executable evidence. Expected failures name their future
target and acceptance criteria.

`make test-roadmap` executes the nearest expected-red behavior while keeping the main
CI result green. Chunked client requests, format-conditioned media queries and RFC
requested-attribute group selectors must pass. An unexpected pass or regression
fails the runner so the matrix cannot silently become stale.

`make test-compat` also replays the readiness query, full capability query and
chunked `Print-Job` captured from iOS against every usable legacy-printer fixture.
It requires the document to arrive byte-for-byte unchanged and requires
`printer-more-info` to keep pointing to `http://espresso.local/`. Discovery and
selection on an actual iPhone remain a hardware/LAN test because host CI cannot
emulate Apple's multicast browser.

`make test-conformance-report` runs CUPS `ipp-1.1.test`, `ipp-2.0.test`, and
`ipp-everywhere.test` with continue-on-error reporting. All three are required green
gates. The Everywhere suite uses a target fixture that truthfully supplies mandatory
JPEG, overrides, page ranges, and advanced operations; the facade adds its tested PWG
Raster conversion and modern metadata. A simpler legacy target does not falsely gain
those non-convertible capabilities. CI uploads the JSON reports for Linux and macOS CUPS.

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
