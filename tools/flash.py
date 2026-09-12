#!/usr/bin/env python3
"""Flash the SenseCAP Watcher over its CH342 bridge.

Why this exists instead of `idf.py flash`
-----------------------------------------
The Watcher's WCH CH342 USB-serial bridge drops bytes when the host sends more
than 256 at a time. esptool's defaults are far larger (0x1800 for RAM writes,
0x4000 for stub flash writes), so *every* write fails on its first block while
reads — small device->host replies — work perfectly. The ROM reports the missing
bytes as a CRC error, which looks like data corruption rather than an overflow.

Measured on this board: 512 and 384 byte blocks fail, 256 succeeds. Capping the
block size is the entire fix. Reads need no special handling.

This is a host-side limitation, not a board fault, and it is why the documented
`esptool.py -b 2000000 write_flash ...` from Seeed's wiki cannot work here.

Offsets come from the build's own `flash_args`
----------------------------------------------
They are never hardcoded. An earlier version of this script had them inline and
they silently went stale when partitions.csv changed — which would have written
a fresh `nvs` straight over the device's provisioned `nvsfactory` (its EUI and
SenseCraft credentials). Reading flash_args means the offsets cannot disagree
with the firmware that was actually built.

Usage:  python3 tools/flash.py [PORT] [--baud N] [--dry-run]
"""

import argparse
import glob
import os
import sys

import esptool
from esptool.loader import ESPLoader

# The whole point of this script. Measured, not guessed — see the module docstring.
CH342_SAFE_BLOCK = 256


def cap_block_sizes():
    """Force every esptool loader class down to CH342_SAFE_BLOCK.

    Patching ESPLoader alone is not enough: each target defines its own ROM
    class, and — the one that actually bit — ESP32S3StubLoader overrides
    FLASH_WRITE_SIZE to 16384 once the stub is running. A cap applied only to
    the base class silently does nothing after the stub loads, which looks like
    the fix failing rather than never being applied.
    """
    import inspect
    import esptool.targets as targets

    patched = 0
    mods = [esptool.loader, targets] + [
        m for _, m in inspect.getmembers(targets, inspect.ismodule)
    ]
    for mod in mods:
        for _, obj in vars(mod).items():
            if not inspect.isclass(obj):
                continue
            for attr in ("ESP_RAM_BLOCK", "FLASH_WRITE_SIZE"):
                if getattr(obj, attr, 0) and obj.__dict__.get(attr, None) is not None:
                    setattr(obj, attr, CH342_SAFE_BLOCK)
                    patched += 1
    # Belt and braces for classes reached by inheritance only.
    ESPLoader.ESP_RAM_BLOCK = CH342_SAFE_BLOCK
    ESPLoader.FLASH_WRITE_SIZE = CH342_SAFE_BLOCK
    return patched


def find_port():
    """The CH342 exposes two ports: the lower is the Himax, the higher the
    ESP32-S3. Guess the higher one; the caller can override."""
    cands = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    cands = [c for c in cands if "debug" not in c]
    if not cands:
        sys.exit("no serial port found — is the Watcher plugged in?")
    return cands[-1]


def read_flash_args(build):
    """Parse build/flash_args into (global_opts, [(offset, path), ...])."""
    path = os.path.join(build, "flash_args")
    if not os.path.isfile(path):
        sys.exit(f"{path} not found — run `idf.py build` first")
    opts, files = [], []
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if parts[0].startswith("--"):
                opts += parts
            else:
                # offset, relative path — pairs
                for i in range(0, len(parts) - 1, 2):
                    files.append((parts[i], os.path.join(build, parts[i + 1])))
    # Ascending offset order keeps the log readable and matches idf.py.
    files.sort(key=lambda f: int(f[0], 16))
    return opts, files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default=None)
    ap.add_argument("--baud", default="460800",
                    help="stub-mode baud; the 256-byte cap is what matters, not this")
    ap.add_argument("--build", default=None)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    build = args.build or os.path.join(here, "build")
    port = args.port or find_port()

    opts, files = read_flash_args(build)

    print(f"port  {port} @ {args.baud} baud, write block capped at {CH342_SAFE_BLOCK} B")
    for off, path in files:
        size = os.path.getsize(path) if os.path.isfile(path) else -1
        print(f"  {off:>10}  {os.path.relpath(path, build):<40} {size:>9,} B")
    if args.dry_run:
        return

    cap_block_sizes()

    # --no-compress is required, not an optimisation. The stub's *compressed*
    # write path batches data independently of FLASH_WRITE_SIZE, so the 256-byte
    # cap does not reach it and the CH342 drops bytes again — surfacing as
    # "Failed to write compressed data to flash after seq 0 (C100: Bad data
    # checksum)". Uncompressed writes honour the cap and go through.
    argv = ["--port", port, "--baud", args.baud, "--chip", "esp32s3",
            "--connect-attempts", "3", "write_flash", "--no-compress"] + opts
    for off, path in files:
        argv += [off, path]
    esptool.main(argv)


if __name__ == "__main__":
    main()
