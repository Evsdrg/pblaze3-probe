#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Offline regression tests for pbregs. They exercise argument parsing,
# the PCI preflight guards and the all-ones MMIO check against a fake
# config/resource0 file pair, so they need no hardware and no root.
#
# The fake BAR is all zeros, so mailbox commands time out; the tests
# therefore assert on exit codes and on text that is produced before or
# independently of any device response.

import pathlib
import struct
import subprocess
import sys
import tempfile

EXE = pathlib.Path(__file__).resolve().parent.parent / "pbregs"


def main():
    if not EXE.exists():
        print(f"SKIP: {EXE} not built", file=sys.stderr)
        return 77

    with tempfile.TemporaryDirectory(prefix="pblaze-reg-") as d:
        p = pathlib.Path(d)
        cfg = bytearray(256)
        # vendor/device, memory decode enabled
        struct.pack_into("<HHH", cfg, 0, 0x1c5f, 0x0530, 2)
        # PCIe capability at 0x40, link status x8 + 5.0GT/s
        cfg[0x34] = 0x40
        cfg[0x40] = 0x10
        struct.pack_into("<H", cfg, 0x40 + 0x12, 0x0080 | 0x0002)
        bar = bytearray(65536)
        struct.pack_into("<I", bar, 0x30, 0x20151017)
        (p / "config").write_bytes(cfg)
        (p / "resource0").write_bytes(bar)

        def run(args, ok, match):
            r = subprocess.run([str(EXE), "-d", str(p / "resource0"), *args],
                               capture_output=True, text=True)
            assert (r.returncode == 0) == ok, (args, r.returncode, r.stdout, r.stderr)
            assert match in r.stdout + r.stderr, (args, r.stdout, r.stderr)
            print("PASS", args)

        run(["peek", "0"], True, "peek @0x000")
        run(["peek", "30"], True, "0x20151017")
        run(["peek", "1"], False, "offset must")
        run(["-t", "0", "peek"], False, "timeout must")
        run(["-t", "99999", "peek"], False, "timeout must")
        run(["bogus"], False, "Usage:")
        run(["peek", "0", "extra"], False, "Usage:")
        run(["-p", "status"], True, "Model=")
        run(["status"], False, "状态监控")

        # Unreachable endpoint: config all ones
        cfg[:] = b"\xff" * 256
        (p / "config").write_bytes(cfg)
        run(["status"], False, "refusing BAR access")
        run(["peek", "0"], False, "refusing BAR access")

        # Memory decode disabled
        cfg[:] = bytearray(256)
        struct.pack_into("<HH", cfg, 0, 0x1c5f, 0x0530)
        (p / "config").write_bytes(cfg)
        run(["status"], False, "memory decoding is disabled")

        # Wrong device
        struct.pack_into("<HH", cfg, 0, 0x1234, 0x5678)
        (p / "config").write_bytes(cfg)
        run(["status"], False, "No such device")

        # Truncated config
        (p / "config").write_bytes(b"\x00" * 4)
        run(["status"], False, "Input/output error")

    print("ALL REGRESSION PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
