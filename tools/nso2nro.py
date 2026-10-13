#!/usr/bin/env python3
"""Wrap an uncompressed NSO into an NRO, for tests of the .nro launch path.

    tools/nso2nro.py <in.nso> <out.nro> [--name NAME]

As in a real NRO the header is the first 0x80 bytes of the text segment and the
first word is a branch over it, so the NSO's text goes at 0x80 and the other
segments keep their memory offsets shifted by the same amount, which must stay
page-aligned (true for a text-only module such as examples/hello). An ASET
block carries a NACP naming the module, no icon and no RomFS. Compressed NSOs
are refused: this is a fixture builder, not a packer. Standard library only.
"""
import struct
import sys


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    name = argv[argv.index("--name") + 1] if "--name" in argv else "module"
    nso = open(argv[1], "rb").read()
    if nso[:4] != b"NSO0":
        sys.exit(f"{argv[1]}: not an NSO")
    flags = struct.unpack_from("<I", nso, 0xC)[0]
    if flags & 7:
        sys.exit(f"{argv[1]}: compressed segments are not supported")
    segs = [struct.unpack_from("<III", nso, 0x10 + 0x10 * i) for i in range(3)]  # file, mem, size
    bss = struct.unpack_from("<I", nso, 0x3C)[0]
    text_file, text_mem, text_size = segs[0]
    if text_mem != 0:
        sys.exit(f"{argv[1]}: text must start at memory offset 0")
    placed = [(0, (0x80 + text_size + 0xFFF) & ~0xFFF)]  # text: header + code, page-rounded
    for file_off, mem, size in segs[1:]:
        off = 0x80 + mem if size else placed[-1][0] + placed[-1][1]  # empty: right after
        if off & 0xFFF:
            sys.exit(f"{argv[1]}: a segment at {mem:#x} would not be page-aligned in the NRO")
        placed.append((off, size))
    image_size = max(off + size for off, size in placed)
    image_size = (image_size + 0xFFF) & ~0xFFF
    image = bytearray(image_size)
    for (file_off, mem, size), (off, _) in zip(segs, placed):
        dst = off if mem else 0x80
        image[dst:dst + size] = nso[file_off:file_off + size]
    struct.pack_into("<II", image, 0, 0x14000020, 0)  # b +0x80 over the header; no MOD0
    image[0x10:0x14] = b"NRO0"
    struct.pack_into("<III", image, 0x14, 0, image_size, 0)  # version, size, flags
    for i, (off, size) in enumerate(placed):
        struct.pack_into("<II", image, 0x20 + 8 * i, off, size)
    struct.pack_into("<I", image, 0x38, bss)
    nacp = bytearray(0x4000)
    for lang in range(16):  # title and author in every language slot
        nacp[lang * 0x300:lang * 0x300 + len(name)] = name.encode()
        nacp[lang * 0x300 + 0x200:lang * 0x300 + 0x200 + 9] = b"switch2pc"
    aset = bytearray(0x38)
    aset[0:4] = b"ASET"
    struct.pack_into("<QQ", aset, 0x18, 0x38, len(nacp))  # nacp offset, size
    with open(argv[2], "wb") as f:
        f.write(bytes(image) + bytes(aset) + bytes(nacp))


if __name__ == "__main__":
    main(sys.argv)
