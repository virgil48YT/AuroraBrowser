#!/usr/bin/env python
"""Append an Aurora payload ZIP to the setup UI and write the SFX footer.

Layout:  [aurora_setup_ui.exe] [payload.zip] [32-byte footer]
Footer:  magic "AURSFX01", version=1, reserved, zip offset, zip size (LE).

The installer (installer/src/payload.cpp) reads the footer from the end of its
own executable, so the result is one self-contained setup.exe while the UI is
still a normal PE image with an icon and version resource.
"""

import argparse
import os
import struct
import sys
import zipfile

MAGIC = b"AURSFX01"
FOOTER_VERSION = 1
FOOTER = struct.Struct("<8sIIQQ")
REQUIRED = ("aurora_browser.exe", "aurora_bootstrap.exe", "aurora_build.manifest")


def fail(message: str) -> "NoReturn":
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ui", required=True)
    parser.add_argument("--payload", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    ui = os.path.abspath(args.ui)
    payload = os.path.abspath(args.payload)
    output = os.path.abspath(args.out)
    if not os.path.isfile(ui):
        fail(f"setup UI not found: {ui}")
    if not os.path.isfile(payload):
        fail(f"payload ZIP not found: {payload}")

    with zipfile.ZipFile(payload) as archive:
        names = {name.lower() for name in archive.namelist()}
        for required in REQUIRED:
            if required not in names:
                fail(f"payload is missing {required}")

    ui_size = os.path.getsize(ui)
    payload_size = os.path.getsize(payload)
    if ui_size == 0 or payload_size == 0:
        fail("setup UI and payload must both be non-empty")
    if ui_size + payload_size + FOOTER.size > 0xFFFFFFFF:
        fail("setup executable would exceed the ZIP32 size limit")

    os.makedirs(os.path.dirname(output) or ".", exist_ok=True)
    with open(output, "wb") as destination:
        with open(ui, "rb") as source:
            while chunk := source.read(1 << 20):
                destination.write(chunk)
        with open(payload, "rb") as source:
            while chunk := source.read(1 << 20):
                destination.write(chunk)
        destination.write(FOOTER.pack(MAGIC, FOOTER_VERSION, 0, ui_size,
                                      payload_size))

    expected = ui_size + payload_size + FOOTER.size
    actual = os.path.getsize(output)
    if actual != expected:
        fail(f"output size {actual} does not match expected {expected}")
    print(f"Self-extracting setup: {actual} bytes -> {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
