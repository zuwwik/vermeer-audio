#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Decode control packets from strace -f -ttt -xx -s 262144 -e trace=write.

Only aud_pasthru_adsp writes are selected. PCM data and other process writes
are excluded. A successful write confirms transport, not a DSP acknowledgement.
"""
import argparse
import json
import re
import struct
from pathlib import Path


COMMANDS = {
    0x01001000: "GRAPH_OPEN",
    0x01001001: "GRAPH_PREPARE",
    0x01001002: "GRAPH_START",
    0x01001003: "GRAPH_STOP",
    0x01001004: "GRAPH_CLOSE",
    0x01001006: "SET_CFG",
    0x01001008: "REGISTER_CFG",
    0x01001009: "DEREGISTER_CFG",
    0x0100100C: "MAP_REGIONS",
    0x0100100D: "UNMAP_REGIONS",
    0x0100100E: "REGISTER_EVENTS",
}


def parameters(blob):
    """APM parameter headers and their eight-byte-aligned payloads."""
    offset = 0
    result = []
    while offset < len(blob):
        if len(blob) - offset < 16:
            raise ValueError(f"truncated parameter header at {offset}")
        module, param, size, error = struct.unpack_from("<4I", blob, offset)
        end = offset + 16 + size
        if end > len(blob):
            raise ValueError(f"truncated parameter {param:#x} at {offset}")
        result.append((module, param, error, blob[offset + 16:end]))
        # SPF allows the final payload without trailing alignment padding.
        offset = end if end == len(blob) else (end + 7) & ~7
        if offset > len(blob):
            raise ValueError("truncated parameter padding")
    return result


def describe_parameter(module, param, error, payload):
    item = {"module": hex(module), "parameter": hex(param),
            "size": len(payload), "error": error}
    if module == 4 and param == 0x080011BC and len(payload) == 4:
        item["vsid"] = hex(struct.unpack("<I", payload)[0])
    elif module == 4 and param == 0x08001310 and len(payload) == 8:
        vsid, channels = struct.unpack("<2I", payload)
        item.update(vsid=hex(vsid), tx_channels=channels)
    elif module == 4 and param == 0x0800116B:
        if len(payload) < 8:
            raise ValueError("truncated VCPM calibration keys")
        vsid, count = struct.unpack_from("<2I", payload)
        if count != (len(payload) - 8) // 8 or len(payload) % 8:
            raise ValueError("invalid VCPM calibration key count")
        pairs = struct.iter_unpack("<2I", payload[8:])
        item.update(vsid=hex(vsid), calibration_keys={hex(k): v for k, v in pairs})
    elif module == 1 and param == 0x08001005:
        if len(payload) < 4:
            raise ValueError("truncated subgraph list")
        count = struct.unpack_from("<I", payload)[0]
        if len(payload) != 4 + 4 * count:
            raise ValueError("invalid subgraph count")
        item["subgraphs"] = [hex(v[0]) for v in struct.iter_unpack("<I", payload[4:])]
    else:
        item["payload_hex"] = payload.hex()
    return item


def decode(packet):
    if len(packet) < 24:
        raise ValueError("short GPR header")
    header, domains, source, destination, token, opcode = struct.unpack_from("<6I", packet)
    if header & 15 or ((header >> 4) & 15) != 6 or header >> 8 != len(packet):
        raise ValueError("invalid GPR version/header size/packet size")
    if opcode not in COMMANDS:
        return None
    record = {"opcode": hex(opcode), "command": COMMANDS[opcode],
              "source": hex(source), "destination": hex(destination),
              "token": hex(token), "size": len(packet), "domains": hex(domains)}
    if opcode == 0x0100100C:
        if len(packet) < 32:
            raise ValueError("short MAP_REGIONS")
        pool, regions, flags = struct.unpack_from("<HHI", packet, 24)
        if len(packet) != 32 + 12 * regions:
            raise ValueError("invalid MAP_REGIONS size")
        record.update(pool=pool, flags=flags, regions=[list(region) for region in
                      struct.iter_unpack("<3I", packet[32:])])
        return record
    if opcode == 0x0100100D:
        if len(packet) != 28:
            raise ValueError("invalid UNMAP_REGIONS size")
        record["memory_handle"] = hex(struct.unpack_from("<I", packet, 24)[0])
        return record
    if len(packet) < 40:
        raise ValueError("short APM command header")
    address, handle, size = struct.unpack_from("<QII", packet, 24)
    record.update(memory_handle=hex(handle), address=hex(address), payload_size=size)
    if handle:
        record["payload_in_capture"] = False
        if len(packet) != 40:
            raise ValueError("unexpected data after an out-of-band command header")
    else:
        if address or size > len(packet) - 40 or any(packet[40 + size:]):
            raise ValueError("invalid in-band payload address/size")
        record["payload_in_capture"] = True
        payload = packet[40:40 + size]
        if opcode == 0x0100100E:
            # apm_module_register_events_t has a different layout from the
            # usual four-word module-parameter header.
            record["event_payload_hex"] = payload.hex()
        else:
            record["parameters"] = [describe_parameter(*p) for p in parameters(payload)]
    return record


def read_trace(path, descriptor):
    # A line may end with '<unfinished ...>'. Its complete entry buffer is
    # still useful, but it does not yet carry a transport return value.
    entry = re.compile(r'^(\d+)\s+([\d.]+)\s+write\((\d+), '
                       r'"((?:\\x[0-9a-fA-F]{2})*)", (\d+)')
    records = []
    errors = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        match = entry.match(line)
        if not match or int(match[3]) != descriptor:
            continue
        packet = bytes.fromhex(match[4].replace("\\x", ""))
        if len(packet) != int(match[5]):
            errors.append(f"line {number}: truncated write buffer")
            continue
        try:
            record = decode(packet)
        except ValueError as exc:
            errors.append(f"line {number}: {exc}")
            continue
        if record is None:
            continue
        returned = re.search(r'\)\s+=\s+(-?\d+)', line)
        record.update(thread=int(match[1]), time=match[2], line=number,
                      write_result=int(returned[1]) if returned else None)
        records.append(record)
    return {"records": records, "errors": errors,
            "note": "write results are transport results; DSP replies were not traced"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--fd", type=int, required=True,
                        help="aud_pasthru_adsp fd in the traced process (check /proc/PID/fd)")
    args = parser.parse_args()
    result = read_trace(args.trace, args.fd)
    print(json.dumps(result, indent=2))
    return bool(result["errors"])


if __name__ == "__main__":
    raise SystemExit(main())
