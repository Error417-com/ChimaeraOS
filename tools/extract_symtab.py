#!/usr/bin/env python3
"""
tools/extract_symtab.py
=======================
Extract the .symtab and .strtab sections from a kernel ELF file as raw
binary blobs.  Used in the two-pass kernel build to embed the symbol table
into the final kernel image so the panic handler can perform symbol lookup.

Usage
-----
    python3 tools/extract_symtab.py <kernel.elf> <symtab.bin> <strtab.bin>

Exits 0 on success, 1 on error.
"""

import struct
import sys
import os


def extract_sections(elf_path: str, symtab_path: str, strtab_path: str) -> int:
    with open(elf_path, "rb") as f:
        data = f.read()

    # Validate ELF magic
    if data[:4] != b"\x7fELF":
        print(f"ERROR: {elf_path} is not an ELF file", file=sys.stderr)
        return 1

    ei_class = data[4]
    if ei_class != 1:
        print(f"ERROR: only ELF32 supported (got class {ei_class})", file=sys.stderr)
        return 1

    # ELF32 header fields
    e_shoff     = struct.unpack_from("<I", data, 32)[0]
    e_shentsize = struct.unpack_from("<H", data, 46)[0]
    e_shnum     = struct.unpack_from("<H", data, 48)[0]
    e_shstrndx  = struct.unpack_from("<H", data, 50)[0]

    if e_shoff == 0 or e_shnum == 0:
        print("ERROR: no section header table", file=sys.stderr)
        return 1

    # Read section name string table (.shstrtab)
    shstrtab_hdr = e_shoff + e_shstrndx * e_shentsize
    shstr_off  = struct.unpack_from("<I", data, shstrtab_hdr + 16)[0]
    shstr_size = struct.unpack_from("<I", data, shstrtab_hdr + 20)[0]
    shstrtab   = data[shstr_off : shstr_off + shstr_size]

    def get_name(name_idx: int) -> str:
        end = shstrtab.index(b"\x00", name_idx)
        return shstrtab[name_idx:end].decode("ascii", errors="replace")

    # Find .symtab and .strtab sections
    symtab_off = symtab_size = 0
    strtab_off = strtab_size = 0

    for i in range(e_shnum):
        hdr = e_shoff + i * e_shentsize
        name_idx = struct.unpack_from("<I", data, hdr)[0]
        sh_type  = struct.unpack_from("<I", data, hdr + 4)[0]
        sh_off   = struct.unpack_from("<I", data, hdr + 16)[0]
        sh_size  = struct.unpack_from("<I", data, hdr + 20)[0]
        name     = get_name(name_idx)

        if name == ".symtab" and sh_type == 2:  # SHT_SYMTAB
            symtab_off  = sh_off
            symtab_size = sh_size
        elif name == ".strtab" and sh_type == 3:  # SHT_STRTAB
            strtab_off  = sh_off
            strtab_size = sh_size

    if symtab_size == 0:
        print("WARNING: no .symtab section found — symbol lookup will be unavailable",
              file=sys.stderr)
        # Write empty files so the build doesn't fail
        open(symtab_path, "wb").close()
        open(strtab_path, "wb").close()
        return 0

    with open(symtab_path, "wb") as f:
        f.write(data[symtab_off : symtab_off + symtab_size])

    with open(strtab_path, "wb") as f:
        f.write(data[strtab_off : strtab_off + strtab_size])

    print(f"Extracted .symtab: {symtab_size} bytes → {symtab_path}")
    print(f"Extracted .strtab: {strtab_size} bytes → {strtab_path}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <kernel.elf> <symtab.bin> <strtab.bin>",
              file=sys.stderr)
        sys.exit(1)
    sys.exit(extract_sections(sys.argv[1], sys.argv[2], sys.argv[3]))
