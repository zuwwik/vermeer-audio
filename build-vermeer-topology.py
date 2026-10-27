#!/usr/bin/env python3
"""Build Vermeer firmware without replacing a good binary on m4/compiler failure."""

import argparse
import ctypes
import ctypes.util
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("topology_dir", type=Path,
                        help="linux-msm/audioreach-topology checkout root")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    upstream = args.topology_dir.resolve()
    if not (upstream / "audioreach/audioreach.m4").is_file():
        parser.error("topology_dir must contain audioreach/audioreach.m4")

    # Same filesystem as the final files, so each replacement is atomic.
    with tempfile.TemporaryDirectory(prefix=".vermeer-build-", dir=here) as tmp:
        conf = Path(tmp) / "SM8550-VERMEER.conf"
        binary = Path(tmp) / "SM8550-VERMEER-tplg.bin"
        result = subprocess.run(
            ["m4", "-I", str(here), "-I", str(upstream),
             str(here / "SM8550-VERMEER.m4")],
            cwd=here, stdout=subprocess.PIPE, check=True)
        text = result.stdout.decode()
        if ('SectionVendorTuples."device40.tdm_rx1_tuples"' not in text or
                'AR_TKN_U32_MODULE_ID "0x0700100E"' not in text or
                'SectionWidget."device40.mfc1"' not in text):
            raise RuntimeError("m4 did not generate the expected TDM_SINK/MFC graph")
        conf.write_bytes(result.stdout)

        compiler = shutil.which("alsatplg")
        if compiler:
            subprocess.run([compiler, "-c", str(conf), "-o", str(binary)], check=True)
        else:
            # Gentoo may have libatopology but no alsatplg executable.
            library = ctypes.util.find_library("atopology")
            if not library:
                raise RuntimeError("install alsatplg or libatopology")
            lib = ctypes.CDLL(library)
            lib.snd_tplg_new.restype = ctypes.c_void_p
            lib.snd_tplg_build_file.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                              ctypes.c_char_p]
            lib.snd_tplg_build_file.restype = ctypes.c_int
            lib.snd_tplg_free.argtypes = [ctypes.c_void_p]
            ctx = lib.snd_tplg_new()
            if not ctx:
                raise MemoryError("snd_tplg_new failed")
            try:
                ret = lib.snd_tplg_build_file(ctx, os.fsencode(conf), os.fsencode(binary))
                if ret < 0:
                    raise RuntimeError(f"topology compiler failed: {ret}")
            finally:
                lib.snd_tplg_free(ctx)

        data = binary.read_bytes()
        if len(data) < 36 or struct.unpack_from("<I", data)[0] != 0x41536F43:
            raise RuntimeError("compiler produced an invalid/empty topology")
        binary.chmod(0o644)
        conf.chmod(0o644)
        conf.replace(here / conf.name)
        binary.replace(here / binary.name)
        print(f"{len(data)} bytes  sha256={hashlib.sha256(data).hexdigest()}")


if __name__ == "__main__":
    main()
