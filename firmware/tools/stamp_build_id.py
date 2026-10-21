#!/usr/bin/env python3
"""Write a stamped, debug-only copy of the firmware ELF.

The copy keeps the DWARF and symbols and drops every loadable byte, so it holds
no Wi-Fi credentials and no .rodata. A non-ALLOC .note.gnu.build-id section
carries the first 20 bytes of SHA-256 of the whole input ELF, which is the
build id the daemon derives from the `elf` hash in a crash report.

The note layout (namesz 4, descsz 20, type 3 NT_GNU_BUILD_ID, "GNU\\0", id) and
the idea of stamping after the link come from scripts/stamp_build_id.py in
getsentry/sentry-micro (MIT). The input is never modified: `elf2image` hashes
the whole ELF, so stamping it in place would change the stored hash.

Usage: stamp_build_id.py --objcopy OBJCOPY INPUT.elf OUTPUT.elf
"""

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

# The daemon reports these as the image range (app/src-tauri/src/daemon/reporting.rs).
# Sentry places the image at the first PT_LOAD, so they must match the linked ELF.
IMAGE_ADDR = 0x30100000
IMAGE_END = 0x50110000  # IMAGE_ADDR + the daemon's image_size 0x20010000

PT_LOAD = 1
EM_RISCV = 243
NT_GNU_BUILD_ID = 3
BUILD_ID_LEN = 20


def fail(msg):
    sys.exit(f"stamp_build_id: {msg}")


def load_segments(path):
    """Return (vaddr, filesz, memsz) for each PT_LOAD of a 32-bit little-endian ELF."""
    with open(path, "rb") as f:
        ehdr = f.read(0x34)
        if len(ehdr) < 0x34 or ehdr[:4] != b"\x7fELF" or ehdr[4] != 1 or ehdr[5] != 1:
            fail(f"{path} is not a 32-bit little-endian ELF")
        machine, = struct.unpack_from("<H", ehdr, 0x12)
        if machine != EM_RISCV:
            fail(f"{path} has e_machine {machine}, not {EM_RISCV} (RISC-V)")
        phoff, = struct.unpack_from("<I", ehdr, 0x1C)
        phentsize, phnum = struct.unpack_from("<HH", ehdr, 0x2A)
        if phentsize < 32:
            fail(f"{path} has an unexpected program header size {phentsize}")
        f.seek(phoff)
        table = f.read(phentsize * phnum)
    if len(table) < phentsize * phnum:
        fail(f"{path} has a truncated program header table")
    segments = []
    for i in range(phnum):
        p_type, _off, vaddr, _paddr, filesz, memsz, _flags, _align = struct.unpack_from(
            "<8I", table, i * phentsize
        )
        if p_type == PT_LOAD:
            segments.append((vaddr, filesz, memsz))
    return segments


def check_segments(path):
    segments = load_segments(path)
    if not segments:
        fail(f"{path} has no PT_LOAD segment")
    loaded = [s for s in segments if s[1] != 0]
    if loaded:
        fail(f"{len(loaded)} PT_LOAD segment(s) still have p_filesz != 0, so code or data would be uploaded")
    lowest = min(vaddr for vaddr, _filesz, _memsz in segments)
    if segments[0][0] != IMAGE_ADDR or lowest < IMAGE_ADDR:
        fail(
            f"the first PT_LOAD is at {segments[0][0]:#x} and the lowest at {lowest:#x}, "
            f"but the first must be at {IMAGE_ADDR:#x} with none lower. "
            "Change IMAGE_ADDR here and the image address in the daemon (reporting.rs) together"
        )
    end = max(vaddr + memsz for vaddr, _filesz, memsz in segments)
    if end > IMAGE_END:
        fail(
            f"a PT_LOAD ends at {end:#x}, past {IMAGE_END:#x}. "
            "Change IMAGE_END here and the image size in the daemon (reporting.rs) together"
        )


def stamp(args):
    load_segments(args.elf)  # rejects a wrong class, endianness or machine before objcopy runs
    with open(args.elf, "rb") as f:
        build_id = hashlib.sha256(f.read()).digest()[:BUILD_ID_LEN]
    note = struct.pack("<III", 4, BUILD_ID_LEN, NT_GNU_BUILD_ID) + b"GNU\0" + build_id

    out_dir = os.path.dirname(os.path.abspath(args.output))
    with tempfile.TemporaryDirectory(dir=out_dir, prefix=".stamp-") as tmp:
        note_path = os.path.join(tmp, "note.bin")
        stamped = os.path.join(tmp, "stamped.elf")
        with open(note_path, "wb") as f:
            f.write(note)
        subprocess.run(
            [
                args.objcopy,
                "--only-keep-debug",
                "--add-section", f".note.gnu.build-id={note_path}",
                "--set-section-flags", ".note.gnu.build-id=noload,readonly",
                args.elf,
                stamped,
            ],
            check=True,
        )
        # objcopy can only align a section that is already in its input, so this takes a second
        # pass. It warns about every section's LMA, which is noise for a file with no contents.
        aligned = os.path.join(tmp, "aligned.elf")
        result = subprocess.run(
            [args.objcopy, "--set-section-alignment", ".note.gnu.build-id=4", stamped, aligned],
            stderr=subprocess.PIPE,
            text=True,
        )
        for line in result.stderr.splitlines():
            if "adjusted to" not in line:
                print(line, file=sys.stderr)
        result.check_returncode()
        check_segments(aligned)
        os.replace(aligned, args.output)  # same filesystem as the temp dir, so atomic

    print(f"stamp_build_id: {os.path.basename(args.output)} build id {build_id.hex()}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--objcopy", required=True, help="objcopy for the ELF's architecture")
    ap.add_argument("elf", help="the linked watchdeck.elf (read only)")
    ap.add_argument("output", help="the debug-only copy to write")
    args = ap.parse_args()
    try:
        stamp(args)
    except BaseException:
        # A stale debug copy must not survive a failed stamp, or fw:symbols could upload it.
        if os.path.lexists(args.output):
            os.remove(args.output)
        raise


if __name__ == "__main__":
    main()
