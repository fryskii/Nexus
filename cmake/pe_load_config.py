#!/usr/bin/env python3
"""Points the PE load-configuration data directory of a MinGW-built NEXUS.exe at the
_load_config_used structure defined in src/app/LoadConfig.cpp.

GNU ld before binutils 2.42 does not fill in that directory, so without this step the
DependentLoadFlags setting (System32-only resolution of static imports) would be ignored.
The structure is located by its exact contents; the step fails unless there is exactly one
match, so it can never patch the wrong bytes.

usage: pe_load_config.py <exe>   (exit status 0 = directory set and verified)
"""
import struct
import sys

LOAD_CONFIG_SIZE = 0x70
DEPENDENT_LOAD_FLAGS_OFFSET = 0x4E
LOAD_LIBRARY_SEARCH_SYSTEM32 = 0x0800
DIRECTORY_INDEX = 10  # IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG


def expected_struct():
    blob = bytearray(LOAD_CONFIG_SIZE)
    struct.pack_into("<I", blob, 0, LOAD_CONFIG_SIZE)
    struct.pack_into("<H", blob, DEPENDENT_LOAD_FLAGS_OFFSET, LOAD_LIBRARY_SEARCH_SYSTEM32)
    return bytes(blob)


def main(path):
    data = bytearray(open(path, "rb").read())
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        sys.exit(f"{path}: not a PE file")
    nsections, opt_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x20B:
        sys.exit(f"{path}: expected a PE32+ (64-bit) image")
    ndirs = struct.unpack_from("<I", data, opt + 108)[0]
    if ndirs <= DIRECTORY_INDEX:
        sys.exit(f"{path}: image has no load-config directory slot")
    dir_off = opt + 112 + DIRECTORY_INDEX * 8

    sections = []
    for i in range(nsections):
        s = opt + opt_size + i * 40
        vsize, va, raw_size, raw_ptr = struct.unpack_from("<IIII", data, s + 8)
        sections.append((va, vsize, raw_ptr, raw_size))

    needle = expected_struct()
    matches = []
    start = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        for va, vsize, raw_ptr, raw_size in sections:
            if raw_ptr <= i and i + LOAD_CONFIG_SIZE <= raw_ptr + min(raw_size, max(vsize, raw_size)):
                matches.append(va + (i - raw_ptr))
        start = i + 1
    if len(matches) != 1:
        sys.exit(f"{path}: found {len(matches)} candidate load-config structures (expected 1); refusing to patch")
    rva = matches[0]
    if rva % 8:
        sys.exit(f"{path}: load-config structure is not 8-byte aligned (RVA {rva:#x})")

    struct.pack_into("<II", data, dir_off, rva, LOAD_CONFIG_SIZE)
    open(path, "wb").write(data)

    check = open(path, "rb").read()
    if struct.unpack_from("<II", check, dir_off) != (rva, LOAD_CONFIG_SIZE):
        sys.exit(f"{path}: verification failed")
    print(f"{path}: load-config directory -> RVA {rva:#x} (DependentLoadFlags = LOAD_LIBRARY_SEARCH_SYSTEM32)")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
