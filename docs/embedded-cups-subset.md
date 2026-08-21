# Embedded CUPS subset

ESPresso does not compile or run CUPS. It ports the parts of CUPS and the PWG sample
server that describe how a driverless printer is discovered and represented, while
using ESP-IDF for the actual network transport.

## Included

| Upstream behavior | ESPresso implementation |
| --- | --- |
| Browse and resolve an IPP DNS-SD service | ESP-IDF mDNS `_ipp._tcp` PTR query, SRV/TXT/address result |
| Read `rp`, `pdl`, `URF`, `ty`, `UUID`, feature flags | Compact `printer_target_t` profile |
| Query `all,media-col-database` with IPP 2.0 | Bounded HTTP/IPP discovery request |
| Fall back to IPP 1.1 `all` for old firmware | Automatic second discovery request |
| Query capabilities in a document-format context | Bounded per-format probes, URF first |
| Re-resolve a saved DNS-SD hostname after reboot | ESP-IDF mDNS A query before advertisement |
| Derive formats, URF, media, color, duplex, copies and operations | Allocation-bounded IPP parser |
| Advertise `_ipp._tcp,_universal` using real capabilities | ESP-IDF mDNS service and TXT record |
| Present a modern queue identity | ESP-specific UUID, URI and IPP 1.1/2.0 facade |
| Relay printer and job operations | Safe operation allowlist, validation, IPP errors, and version/URI translation |
| Relay document data | Unchanged 4 KiB streaming path with host-tested short-read/short-write handling |
| Track live state | Refresh state and accepting-jobs metadata from capability responses |

Request policy, streaming, and DNS-SD TXT generation are platform-neutral C modules.
The firmware and host CUPS lab compile the same implementations, preventing the test
facade from drifting into a second interpretation of the protocol.

The selected profile is persisted in NVS. Its schema is versioned so firmware updates
discard incompatible cached records instead of interpreting an old C structure.

## Synthesized metadata

For successful `Get-Printer-Attributes` responses ESPresso can safely own and normalize:

- `ipp-versions-supported`: `1.1`, `2.0`;
- `printer-uri-supported` and printer/job URIs pointing at ESPresso;
- `printer-uuid`, using the bridge identity rather than the physical printer UUID;
- `uri-authentication-supported=none` and `uri-security-supported=none`, matching the
  current local endpoint;
- missing names and make/model text;
- missing formats, URF modes, media names, color, sides, copies and operations only when
  they were learned from the target profile;
- basic `media-col-database`/`media-col-default` size collections derived from PWG
  self-describing media names (plus a small set of CUPS-compatible legacy aliases);
- modern color-mode metadata derived from older AirPrint color/URF attributes and
  printer resolutions derived from URF `RS` values;
- mandatory charset/language, printer information, uptime, queue-count and PDL
  behavior metadata missing from some legacy responses;
- conservative `compression-supported=none` and
  `multiple-document-jobs-supported=false` values.

Explicit `requested-attributes` values are honored after normalization, preventing a
legacy-safe upstream `all` query from leaking unrequested attributes back to the
client. RFC `printer-description` and `job-template` selectors expand to their
defined sets; exact names, unions, duplicate values, unknown selectors and `all` are
handled without dropping required operation attributes or splitting collections.
As in CUPS, `media-col-database` is excluded from implicit/`all` and group requests
unless the client explicitly names it, avoiding an unexpectedly large capability
response. Upstream defaults remain relayed rather than fabricated.
The classifier distinguishes Printer Description, Job Description and Job Template
attributes so it can also be reused when job-response filtering is added. The first
two operation attributes are validated in their RFC-required charset/language order.

Jobs, unrecognized media collections, margins, finishings and vendor attributes are
forwarded from the old printer. Live state is forwarded and cached for DNS-SD/UI use;
ESPresso does not fabricate it.

## Excluded

These CUPS components are deliberately outside the embedded target:

- rasterization or conversion between PDF, Apple Raster, PWG Raster, PCL or PostScript;
- Ghostscript, MuPDF, Poppler, cups-filters and printer-specific filters;
- PPD parsing/generation and legacy printer drivers;
- the complete CUPS media-name/PPD database (self-describing PWG names need no table);
- local spooling, accounting, subscriptions, job history or persistent document storage;
- IPPS in the current firmware;
- USB, parallel, proprietary backend and Printer Application support.

The compatibility rule is simple: ESPresso may advertise and relay a document format
only when the physical printer already reports that it accepts that exact format.

## Upstream reference paths

- OpenPrinting CUPS `backend/dnssd.c`: discovery types, resolution and deduplication
- OpenPrinting CUPS `scheduler/ipp.c`: `Get-Printer-Attributes` 2.0 → 1.1 fallback
- OpenPrinting CUPS `scheduler/dirsvc.c`: DNS-SD TXT generation
- OpenPrinting CUPS `cups/ppd-cache.c`: URF and capability interpretation
- OpenPrinting cups-browsed `daemon/cups-browsed.c`: alternate capability forms
- PWG ippsample `server/printer.c`: modern IPP server DNS-SD advertisement

All upstream references are Apache-2.0-compatible. ESPresso's implementation is a
small, independently structured embedded adaptation rather than copied libcups code.
