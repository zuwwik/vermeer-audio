#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measure native PipeWire microphone levels without saving audio samples.

Run in the desktop user's session; uses the built-in mic unless --target is
given. Output contains level statistics only. Speak at a normal distance.
"""
import argparse
import array
import math
import os
import select
import signal
import subprocess
import time


def dbfs(value):
    return round(20 * math.log10(value / 32768), 1) if value else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--target", default="alsa_input.platform-sound.HiFi__Mic__source")
    args = parser.parse_args()
    if not 1 <= args.seconds <= 60:
        parser.error("--seconds must be 1..60")
    command = ["pw-cat", "--record", "--raw", "--rate=48000", "--channels=2",
               "--format=s16", "--target=" + args.target, "-"]
    proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    start, count, squares, peak, clipped, nonzero = time.monotonic(), 0, 0, 0, 0, 0
    remainder = b""
    try:
        while time.monotonic() - start < args.seconds:
            ready, _, _ = select.select([proc.stdout], [], [], .2)
            if not ready:
                if proc.poll() is not None:
                    break
                continue
            data = remainder + os.read(proc.stdout.fileno(), 16384)
            if not data:
                break
            remainder, data = data[len(data) & ~1:], data[:len(data) & ~1]
            samples = array.array("h")
            samples.frombytes(data)
            for sample in samples:
                value = abs(sample)
                count += 1
                squares += sample * sample
                peak = max(peak, value)
                nonzero += value != 0
                clipped += value >= 32760
    finally:
        proc.send_signal(signal.SIGINT)
        try:
            proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()
    if not count:
        raise RuntimeError("No microphone samples received")
    print({"frames": count // 2, "audio_seconds": round(count / 96000, 2),
           "rms_dbfs": dbfs(math.sqrt(squares / count)), "peak_dbfs": dbfs(peak),
           "nonzero_percent": round(100 * nonzero / count, 2),
           "clipped_percent": round(100 * clipped / count, 4)})


if __name__ == "__main__":
    main()
