#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate exported ACDB parameter boundaries; print a compact manifest.

This validates the reference data, not DSP support or call-audio operation.
"""
import argparse
import hashlib
import importlib.util
import json
import struct
from pathlib import Path

spec = importlib.util.spec_from_file_location("gpr", Path(__file__).with_name("decode-gpr.py"))
gpr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gpr)


def words(data):
    if len(data) % 4:
        raise ValueError("unaligned word list")
    return [v[0] for v in struct.iter_unpack("<I", data)]


def check_profile(directory, profile):
    blobs = {suffix: (directory / f"{profile}.{suffix}.bin").read_bytes()
             for suffix in ("open", "cal", "persist", "sgids", "driver", "gkv", "ckv")}
    ids = words(blobs["sgids"])
    if not ids or len(set(ids)) != len(ids):
        raise ValueError("missing or duplicate subgraph ID")
    found = []
    modules = {}
    framework_params = []
    for module, param, error, payload in gpr.parameters(blobs["open"]):
        if error:
            raise ValueError("nonzero request error field")
        values = words(payload)
        if module == 1 and param == 0x08001001:
            # ACDB exports one subgraph's configuration per parameter.
            if len(values) < 3 or values[0] != 1:
                raise ValueError("unexpected subgraph configuration format")
            found.append(values[1])
        elif module == 1 and param == 0x08001002:
            if not values:
                raise ValueError("missing module-list count")
            offset = 1
            for _ in range(values[0]):
                if len(values) - offset < 3:
                    raise ValueError("short container module list")
                sg, container, count = values[offset:offset + 3]
                offset += 3
                if sg not in ids or count > (len(values) - offset) // 2:
                    raise ValueError("invalid container module count/subgraph")
                for i in range(count):
                    mid, iid = values[offset + 2 * i:offset + 2 * i + 2]
                    if iid in modules:
                        raise ValueError("duplicate module instance ID")
                    modules[iid] = {"module_id": hex(mid), "subgraph": hex(sg),
                                    "container": hex(container)}
                offset += 2 * count
            if offset != len(values):
                raise ValueError("module-list trailer")
        elif module == 4:
            framework_params.append(hex(param))
    if set(found) != set(ids) or len(found) != len(ids):
        raise ValueError("GRAPH_OPEN subgraphs differ from exported subgraph list")
    gpr.parameters(blobs["cal"])
    persistent = []
    offset = 0
    while offset < len(blobs["persist"]):
        if len(blobs["persist"]) - offset < 8:
            raise ValueError("short persistent ACDB header")
        sg, size = struct.unpack_from("<2I", blobs["persist"], offset)
        offset += 8
        if sg not in ids or size > len(blobs["persist"]) - offset:
            raise ValueError("invalid persistent subgraph/size")
        params = gpr.parameters(blobs["persist"][offset:offset + size])
        if not params:
            raise ValueError("empty persistent table")
        persistent.append({"subgraph": hex(sg), "size": size,
                           "parameters": [hex(p[1]) for p in params]})
        offset += size
    if len({p["subgraph"] for p in persistent}) != len(persistent):
        raise ValueError("duplicate persistent subgraph table")
    return {"subgraphs": [hex(v) for v in ids], "modules": {hex(k): v for k, v in modules.items()},
            "vcpm_open_parameters": framework_params, "persistent": persistent,
            "files": {suffix: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                      for suffix, data in blobs.items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    profiles = ("handset-rx", "handset-tx", "speaker-rx", "speaker-tx")
    try:
        result = {name: check_profile(args.directory, name) for name in profiles}
    except (ValueError, OSError) as exc:
        parser.exit(1, f"Invalid ACDB export: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
