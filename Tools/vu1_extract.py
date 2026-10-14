"""Extract the game's VU1 microprograms statically from the linked ELF.

MANAGER keeps every VU1 microprogram in its `vutext` section and uploads them only through its
microcode manager (`transMicroCode` / `transMicroCodeWithFlush`), which indexes a table of pointers
(`getMicroCodeAddress`: 13 entries + a 0 terminator for SCES-53326 v1.00). Each entry points at a
DMA packet whose VIF stream holds MPG commands loading the program into VU1 micro memory at 0.

This tool finds that table (the longest run of pointers into vutext, terminated by 0), decodes each
packet's VIF stream, assembles the MPG payloads and writes one file per program containing exactly
its bytes from VU1 address 0:

    <out>/vu1prog_<index>_<vaddr>.bin   (length = program size, a multiple of 8)
    <out>/manifest.txt                  (index, packet vaddr, length, fnv1a64 of the bytes)

`vu1_recomp` compiles these programs; the runtime selects one when VU1 micro memory starts with its
bytes, so the stale tail an earlier, longer program leaves behind no longer matters.

Usage: vu1_extract.py <sotc_linked.elf> <out-dir>
"""
import os
import struct
import sys

VU1_CODE_SIZE = 16 * 1024


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class Elf:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
            raise SystemExit(f"{path}: not a little-endian 32-bit ELF")
        shoff, = struct.unpack_from("<I", d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
        hdrs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
        stroff = hdrs[shstrndx][4]
        self.sections = []
        for h in hdrs:
            name = d[stroff + h[0]: d.index(b"\0", stroff + h[0])].decode()
            # name, type, vaddr, file offset, size
            self.sections.append((name, h[1], h[3], h[4], h[5]))

    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s
        raise SystemExit(f"section {name} not found")

    def read(self, vaddr, size):
        for name, typ, addr, off, sz in self.sections:
            if typ != 8 and addr <= vaddr and vaddr + size <= addr + sz:  # 8 = SHT_NOBITS
                return self.data[off + vaddr - addr: off + vaddr - addr + size]
        raise SystemExit(f"address {vaddr:08x}+{size:x} is not in a loaded section")


def find_table(elf, vutext):
    lo, hi = vutext[2], vutext[2] + vutext[4]
    best = None
    for name, typ, addr, off, size in elf.sections:
        if typ == 8 or size < 8 or name == vutext[0] or not (name.endswith(".data") or name.endswith(".rodata")):
            continue
        words = struct.unpack_from(f"<{size // 4}I", elf.data, off)
        run_start = None
        for i, w in enumerate(words + (1,)):
            inside = lo <= w < hi
            if inside and run_start is None:
                run_start = i
            elif not inside and run_start is not None:
                count = i - run_start
                terminated = i < len(words) and words[i] == 0
                if terminated and (best is None or count > best[1]):
                    best = (addr + run_start * 4, count, list(words[run_start:i]))
                run_start = None
    if best is None:
        raise SystemExit("microcode pointer table not found")
    return best


def decode_program(elf, packet_vaddr):
    """Decode the DMA packet at packet_vaddr and return the VU1 micro memory image its MPGs produce."""
    tag_lo, tag_addr = struct.unpack("<II", elf.read(packet_vaddr, 8))
    qwc = tag_lo & 0xFFFF
    tag_id = (tag_lo >> 28) & 7
    # VIF stream: the two VIFcodes in the upper half of the DMA tag, then qwc quadwords.
    stream = elf.read(packet_vaddr + 8, 8 + qwc * 16)
    mem = bytearray(VU1_CODE_SIZE)
    written = bytearray(VU1_CODE_SIZE // 8)
    p = 0
    mpg = 0
    while p + 4 <= len(stream):
        code, = struct.unpack_from("<I", stream, p)
        cmd = (code >> 24) & 0x7F
        p += 4
        if cmd == 0x4A:  # MPG: NUM 64-bit words to micro memory at IMM*8
            num = (code >> 16) & 0xFF or 256
            dst = (code & 0xFFFF) * 8
            size = num * 8
            if dst + size > VU1_CODE_SIZE or p + size > len(stream):
                raise SystemExit(f"packet {packet_vaddr:08x}: MPG out of range (dst {dst:#x}, size {size:#x})")
            mem[dst:dst + size] = stream[p:p + size]
            for k in range(dst // 8, (dst + size) // 8):
                written[k] = 1
            p += size
            mpg += 1
        elif cmd in (0x00, 0x10, 0x11, 0x13, 0x14, 0x15, 0x17, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07):
            # NOP, FLUSHE, FLUSH, FLUSHA, MSCAL, MSCALF, MSCNT, STCYCL, OFFSET, BASE, ITOP, STMOD, MSKPATH3, MARK
            continue
        elif cmd in (0x20, 0x30, 0x31):  # STMASK (+1 word), STROW/STCOL (+4 words)
            p += 4 if cmd == 0x20 else 16
        else:
            raise SystemExit(f"packet {packet_vaddr:08x}: unexpected VIFcode {code:08x} at +{p - 4:#x}; "
                             "the microcode format differs from what this tool understands")
    if mpg == 0:
        raise SystemExit(f"packet {packet_vaddr:08x}: no MPG command")
    length = 0
    for k in range(len(written)):
        if written[k]:
            length = (k + 1) * 8
    if not all(written[:length // 8]):
        gap = next(k for k in range(length // 8) if not written[k]) * 8
        raise SystemExit(f"packet {packet_vaddr:08x}: program has a hole at VU1 {gap:#06x}")
    return bytes(mem[:length]), tag_id, qwc, mpg


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    elf_path, out_dir = sys.argv[1], sys.argv[2]
    elf = Elf(elf_path)
    vutext = elf.section("manager.vutext")
    table_vaddr, count, pointers = find_table(elf, vutext)
    print(f"microcode table at {table_vaddr:08x}: {count} programs (vutext {vutext[2]:08x}+{vutext[4]:x})")

    os.makedirs(out_dir, exist_ok=True)
    for name in os.listdir(out_dir):
        if name.startswith("vu1prog_") and name.endswith(".bin"):
            os.remove(os.path.join(out_dir, name))

    manifest = []
    seen = {}
    for index, packet in enumerate(pointers):
        program, tag_id, qwc, mpg = decode_program(elf, packet)
        h = fnv1a64(program)
        if h in seen:
            print(f"  [{index:2d}] {packet:08x}: identical to [{seen[h]}], skipped")
            continue
        seen[h] = index
        name = f"vu1prog_{index:02d}_{packet:08x}.bin"
        with open(os.path.join(out_dir, name), "wb") as f:
            f.write(program)
        manifest.append(f"{index} {packet:08x} {len(program):#06x} {h:016x}")
        print(f"  [{index:2d}] {packet:08x}: DMA id {tag_id} qwc {qwc:#x}, {mpg} MPG -> {len(program):#06x} bytes, "
              f"fnv1a64 {h:016x}")
    with open(os.path.join(out_dir, "manifest.txt"), "w", newline="\n") as f:
        f.write("# index packet_vaddr length fnv1a64\n" + "\n".join(manifest) + "\n")
    print(f"{len(manifest)} VU1 programs written to {out_dir}")


if __name__ == "__main__":
    main()
