#!/usr/bin/env python3
"""Names the NEXUS.exe offsets in a NEXUS crash report (functions and source lines).

Each release has a NEXUS-<version>-symbols-x64.zip next to the installer; it holds NEXUS.debug,
the debug information of that exact NEXUS.exe. A report from one version needs that version's
NEXUS.debug: another build gives wrong names. Needs GNU addr2line, from MinGW-w64 binutils
(WinLibs on Windows: addr2line.exe; Debian/Ubuntu: x86_64-w64-mingw32-addr2line).

usage: symbolize.py NEXUS.debug report.txt [--addr2line PATH]
       symbolize.py NEXUS.debug 0x1A2B3C ...       (offsets as in NEXUS.exe+0x1A2B3C)
"""
import bisect
import os
import re
import shutil
import struct
import subprocess
import sys

FRAME = re.compile(r"(#(\d+) )?NEXUS\.exe\+0x([0-9A-Fa-f]+)")


def image_base(path):
    data = open(path, "rb").read(4096)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0" or struct.unpack_from("<H", data, pe + 24)[0] != 0x20B:
        sys.exit(f"{path}: not a 64-bit PE debug file")
    return struct.unpack_from("<Q", data, pe + 24 + 24)[0]


def find_addr2line(explicit):
    for name in ([explicit] if explicit else ["x86_64-w64-mingw32-addr2line", "addr2line"]):
        found = shutil.which(name) if name else None
        if found:
            return found
    sys.exit("addr2line not found: install MinGW-w64 binutils or pass --addr2line PATH")


def nearest_symbols(nm, debug):
    """Code symbols sorted by address, for code without line information (the static C/C++
    runtime): there addr2line prints "??" and a misleading file, the symbol table still knows."""
    out = subprocess.run([nm, "-n", "-C", "--defined-only", debug], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in ("t", "T") and not parts[2].startswith("."):
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def main(argv):
    tool = None
    if "--addr2line" in argv:
        i = argv.index("--addr2line")
        tool = argv[i + 1]
        del argv[i:i + 2]
    if len(argv) < 2:
        sys.exit(__doc__)
    debug, inputs = argv[0], argv[1:]
    base = image_base(debug)
    if len(inputs) == 1 and not re.fullmatch(r"(0x)?[0-9A-Fa-f]+", inputs[0]):
        lines = open(inputs[0], encoding="utf-8", errors="replace").read().splitlines()
    else:
        lines = [f"NEXUS.exe+0x{int(x, 16):X}" for x in inputs]

    # Frames after #0 are return addresses: the call itself is the instruction before, so look up
    # address - 1 (otherwise a call at the end of a function or inlined block is misnamed).
    queries = []
    for line in lines:
        for m in FRAME.finditer(line):
            offset = int(m.group(3), 16)
            is_return = m.group(2) is not None and m.group(2) != "0"
            queries.append(base + offset - (1 if is_return else 0))
    if not queries:
        sys.exit("no NEXUS.exe+0x... offsets found")
    addr2line = find_addr2line(tool)
    out = subprocess.run([addr2line, "-e", debug, "-f", "-C", "-i", "-p"] + [f"0x{q:X}" for q in queries],
                         capture_output=True, text=True, check=True).stdout.splitlines()
    nm = os.path.join(os.path.dirname(addr2line), os.path.basename(addr2line).replace("addr2line", "nm"))
    symbols = nearest_symbols(nm, debug) if os.path.exists(nm) or shutil.which(nm) else ([], [])
    # -i prints inlined callers as extra " (inlined by) ..." lines after their address's line.
    names = []
    for o in out:
        if o.lstrip().startswith("(inlined by)") and names:
            names[-1] += "\n        " + o.strip()
        else:
            names.append(o)
    for i, (name, q) in enumerate(zip(names, queries)):
        k = bisect.bisect_right(symbols[0], q) - 1
        if name.startswith("??") and k >= 0:
            names[i] = f"{symbols[1][k]} + 0x{q - symbols[0][k]:X} (no line information)"
    it = iter(names)
    for line in lines:
        print(line)
        for _ in FRAME.finditer(line):
            print("      -> " + next(it, "?"))


if __name__ == "__main__":
    main(sys.argv[1:])
