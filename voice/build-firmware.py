#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Package the device's ACDB export for the native APM voice driver.

The firmware contains DSP topology/calibration data, never captured audio.
It is private device firmware and is not embedded in the kernel source.
"""
import argparse
import importlib.util
import struct
from pathlib import Path

spec = importlib.util.spec_from_file_location("check_export", Path(__file__).with_name("check-export.py"))
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)

PROFILES = {
    "handset": ((40, 0x0700100e, 0x4120, (0x465b, 0x4411)),
                (120, 0x07001024, 0x43af, ())),
    "speaker": ((40, 0x0700100e, 0x4433, (0x448d, 0x41c5)),
                (120, 0x07001024, 0x43ad, ())),
}


def pack(directory, route):
    result = bytearray(struct.pack("<8sII", b"ARVOICE\0", 3, 2))
    for direction, (backend, module, instance, mfcs) in zip(("rx", "tx"), PROFILES[route]):
        profile = f"{route}-{direction}"
        manifest = check.check_profile(directory, profile)
        if manifest["modules"][hex(instance)]["module_id"] != hex(module):
            raise ValueError(f"{profile}: wrong hardware endpoint")
        for iid in mfcs:
            if manifest["modules"][hex(iid)]["module_id"] != "0x7001015":
                raise ValueError(f"{profile}: wrong media format converter")
        blobs = {name: (directory / f"{profile}.{name}.bin").read_bytes()
                 for name in ("sgids", "open", "cal", "persist")}
        # Common TX-stream volume controller, after the raw microphone tap.
        # Basic GAIN (07001002) has a different parameter interface.
        gain = 0x455e if direction == "tx" else 0
        if gain and manifest["modules"][hex(gain)]["module_id"] != "0x700101b":
            raise ValueError(f"{profile}: wrong TX volume-control module")
        result += struct.pack("<9I", backend, module, instance,
                              len(blobs["sgids"]) // 4, len(mfcs),
                              len(blobs["open"]), len(blobs["cal"]), len(blobs["persist"]), gain)
        result += blobs["sgids"] + struct.pack(f"<{len(mfcs)}I", *mfcs)
        result += blobs["open"] + blobs["cal"] + blobs["persist"]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("export_directory", type=Path)
    parser.add_argument("output_directory", type=Path)
    args = parser.parse_args()
    # Validate both complete images before creating any output.
    images = {route: pack(args.export_directory, route) for route in PROFILES}
    args.output_directory.mkdir(parents=True, exist_ok=True)
    for route, data in images.items():
        path = args.output_directory / f"vermeer-voice-{route}.bin"
        with path.open("xb") as target:
            target.write(data)
        print(f"{path.name}: {len(data)} bytes")


if __name__ == "__main__":
    main()
