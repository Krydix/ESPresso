#!/usr/bin/env python3
"""Guard the management-page behavior that is easy to regress accidentally."""

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SOURCE = (ROOT / "frontend/index.html").read_text()


def require(fragment: str) -> None:
    if fragment not in SOURCE:
        raise AssertionError(f"frontend contract is missing {fragment!r}")


for element_id in (
    'id="printerWebsiteRow"',
    'id="printerWebsite"',
    'id="printerNameForm"',
    'id="activeJobs"',
    'id="recentJobs"',
):
    require(element_id)

refresh_start = SOURCE.index("async function refresh(){")
refresh_end = SOURCE.index("$('scanWifi').onclick", refresh_start)
refresh_body = SOURCE[refresh_start:refresh_end]
require("$('printerWebsite').href=s.printer.adminUrl")
require("Number(p.port)===Number(currentPrinter.port)")
require("api('/api/jobs')")
require("setInterval(refreshJobs,3000)")

# Reloading the page may query saved status, but must never start discovery.
if "/api/printers" in refresh_body:
    raise AssertionError("page refresh must not rescan or replace the saved printer")

print("Frontend management contract tests passed")
