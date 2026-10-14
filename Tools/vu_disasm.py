"""Disassemble PS2 VU micro programs (upper/lower instruction pairs).

usage: python Tools/vu_disasm.py <program.bin> [start_hex] [end_hex]

Addresses are byte offsets in VU micro memory (8 bytes per pair). Branch targets are shown as
absolute addresses. Mnemonics follow the Sony VU manual; operand formatting is informal.
"""
import struct
import sys

BC = "xyzw"


def dest(bits):
    return "".join(c for i, c in enumerate("xyzw") if bits & (8 >> i))


def upper(word):
    op = word & 0x3F
    fd = (word >> 6) & 31
    fs = (word >> 11) & 31
    ft = (word >> 16) & 31
    d = dest((word >> 21) & 15)
    bc = BC[op & 3]
    flags = "".join(name for bit, name in ((31, "[I]"), (30, "[E]"), (29, "[M]"), (28, "[D]"), (27, "[T]")) if word >> bit & 1)
    base = {0x00: "add", 0x04: "sub", 0x08: "madd", 0x0C: "msub", 0x10: "max", 0x14: "mini", 0x18: "mul"}
    if op < 0x1C:
        name = base[op & 0x3C] + bc
        text = f"{name}.{d} vf{fd:02}, vf{fs:02}, vf{ft:02}{bc}"
    elif op < 0x30:
        names = {0x1C: ("mulq", "q"), 0x1D: ("maxi", "i"), 0x1E: ("muli", "i"), 0x1F: ("minii", "i"), 0x20: ("addq", "q"),
                 0x21: ("maddq", "q"), 0x22: ("addi", "i"), 0x23: ("maddi", "i"), 0x24: ("subq", "q"), 0x25: ("msubq", "q"),
                 0x26: ("subi", "i"), 0x27: ("msubi", "i")}
        if op in names:
            name, src = names[op]
            text = f"{name}.{d} vf{fd:02}, vf{fs:02}, {src}"
        else:
            name = {0x28: "add", 0x29: "madd", 0x2A: "mul", 0x2B: "max", 0x2C: "sub", 0x2D: "msub", 0x2E: "opmsub", 0x2F: "mini"}.get(op, f"?u{op:02x}")
            text = f"{name}.{d} vf{fd:02}, vf{fs:02}, vf{ft:02}"
    elif op >= 0x3C:
        sp = (op & 3) | (fd << 2)
        if sp < 0x10 or 0x18 <= sp < 0x1C:
            name = {0x00: "adda", 0x04: "suba", 0x08: "madda", 0x0C: "msuba", 0x18: "mula"}[sp & 0x3C] + BC[sp & 3]
            text = f"{name}.{d} acc, vf{fs:02}, vf{ft:02}{BC[sp & 3]}"
        elif sp < 0x18:
            name = ("itof", "ftoi")[(sp - 0x10) >> 2] + ("0", "4", "12", "15")[sp & 3]
            text = f"{name}.{d} vf{ft:02}, vf{fs:02}"
        else:
            simple = {0x1C: "mulaq", 0x1E: "mulai", 0x20: "addaq", 0x21: "maddaq", 0x22: "addai", 0x23: "maddai", 0x24: "subaq",
                      0x25: "msubaq", 0x26: "subai", 0x27: "msubai"}
            if sp in simple:
                text = f"{simple[sp]}.{d} acc, vf{fs:02}"
            elif sp == 0x1D:
                text = f"abs.{d} vf{ft:02}, vf{fs:02}"
            elif sp == 0x1F:
                text = f"clipw.xyz vf{fs:02}, vf{ft:02}w"
            elif sp == 0x2F:
                text = "nop"
            else:
                name = {0x28: "adda", 0x29: "madda", 0x2A: "mula", 0x2C: "suba", 0x2D: "msuba", 0x2E: "opmula"}.get(sp, f"?us{sp:02x}")
                text = f"{name}.{d} acc, vf{fs:02}, vf{ft:02}"
    else:
        text = f"?u{op:02x}"
    return text + (" " + flags if flags else "")


def lower(word, pc):
    op = (word >> 25) & 0x7F
    ft = (word >> 16) & 31
    fs = (word >> 11) & 31
    fd = (word >> 6) & 31
    d = dest((word >> 21) & 15)
    imm11 = word & 0x7FF
    simm11 = imm11 - 0x800 if imm11 & 0x400 else imm11
    target = (pc + 8 + simm11 * 8) & 0x3FFF
    fsf = "xyzw"[(word >> 21) & 3]
    ftf = "xyzw"[(word >> 23) & 3]
    if op == 0x40:
        low = word & 0x3F
        if low < 0x3C:
            name = {0x30: "iadd", 0x31: "isub", 0x32: "iaddi", 0x34: "iand", 0x35: "ior"}.get(low, f"?l40_{low:02x}")
            if low == 0x32:
                imm5 = (word >> 6) & 31
                imm5 = imm5 - 32 if imm5 & 16 else imm5
                return f"iaddi vi{ft:02}, vi{fs:02}, {imm5}"
            return f"{name} vi{fd:02}, vi{fs:02}, vi{ft:02}"
        sp = (low & 3) | (fd << 2)
        names = {0x30: f"move.{d} vf{ft:02}, vf{fs:02}", 0x31: f"mr32.{d} vf{ft:02}, vf{fs:02}",
                 0x34: f"lqi.{d} vf{ft:02}, (vi{fs:02}++)", 0x35: f"sqi.{d} vf{fs:02}, (vi{ft:02}++)",
                 0x36: f"lqd.{d} vf{ft:02}, (--vi{fs:02})", 0x37: f"sqd.{d} vf{fs:02}, (--vi{ft:02})",
                 0x38: f"div q, vf{fs:02}{fsf}, vf{ft:02}{ftf}", 0x39: f"sqrt q, vf{ft:02}{ftf}",
                 0x3A: f"rsqrt q, vf{fs:02}{fsf}, vf{ft:02}{ftf}", 0x3B: "waitq",
                 0x3C: f"mtir vi{ft:02}, vf{fs:02}{fsf}", 0x3D: f"mfir.{d} vf{ft:02}, vi{fs:02}",
                 0x3E: f"ilwr.{d} vi{ft:02}, (vi{fs:02})", 0x3F: f"iswr.{d} vi{ft:02}, (vi{fs:02})",
                 0x40: f"rnext.{d} vf{ft:02}, r", 0x41: f"rget.{d} vf{ft:02}, r", 0x42: f"rinit r, vf{fs:02}{fsf}",
                 0x43: f"rxor r, vf{fs:02}{fsf}", 0x64: f"mfp.{d} vf{ft:02}, p", 0x68: f"xtop vi{ft:02}",
                 0x69: f"xitop vi{ft:02}", 0x6C: f"xgkick vi{fs:02}", 0x70: f"esadd p, vf{fs:02}",
                 0x71: f"ersadd p, vf{fs:02}", 0x72: f"eleng p, vf{fs:02}", 0x73: f"erleng p, vf{fs:02}",
                 0x74: f"eatanxy p, vf{fs:02}", 0x75: f"eatanxz p, vf{fs:02}", 0x76: f"esum p, vf{fs:02}",
                 0x78: f"esqrt p, vf{fs:02}{fsf}", 0x79: f"ersqrt p, vf{fs:02}{fsf}", 0x7A: f"ercpr p, vf{fs:02}{fsf}",
                 0x7B: "waitp", 0x7C: f"esin p, vf{fs:02}{fsf}", 0x7D: f"eatan p, vf{fs:02}{fsf}", 0x7E: f"eexp p, vf{fs:02}{fsf}"}
        if sp == 0x30 and not d:
            return "nop"
        return names.get(sp, f"?l40s_{sp:02x}")
    if op == 0x00:
        return f"lq.{d} vf{ft:02}, {simm11}(vi{fs:02})"
    if op == 0x01:
        return f"sq.{d} vf{fs:02}, {simm11}(vi{ft:02})"
    if op == 0x04:
        return f"ilw.{d} vi{ft:02}, {simm11}(vi{fs:02})"
    if op == 0x05:
        return f"isw.{d} vi{ft:02}, {simm11}(vi{fs:02})"
    if op in (0x08, 0x09):
        imm15 = ((word >> 10) & 0x7800) | imm11
        return f"{'iaddiu' if op == 8 else 'isubiu'} vi{ft:02}, vi{fs:02}, 0x{imm15:x}"
    if 0x10 <= op <= 0x1C:
        imm24 = word & 0xFFFFFF
        imm12 = ((word >> 10) & 0x800) | imm11
        name = {0x10: "fceq", 0x11: "fcset", 0x12: "fcand", 0x13: "fcor", 0x14: "fseq", 0x15: "fsset", 0x16: "fsand",
                0x17: "fsor", 0x18: "fmeq", 0x1A: "fmand", 0x1B: "fmor", 0x1C: "fcget"}.get(op, f"?l{op:02x}")
        if op <= 0x13:
            return f"{name} vi01, 0x{imm24:06x}"
        if op <= 0x17:
            return f"{name} vi{ft:02}, 0x{imm12:03x}"
        if op == 0x1C:
            return f"fcget vi{ft:02}"
        return f"{name} vi{ft:02}, vi{fs:02}"
    branches = {0x20: "b", 0x21: "bal", 0x28: "ibeq", 0x29: "ibne", 0x2C: "ibltz", 0x2D: "ibgtz", 0x2E: "iblez", 0x2F: "ibgez"}
    if op == 0x20:
        return f"b 0x{target:04x}"
    if op == 0x21:
        return f"bal vi{ft:02}, 0x{target:04x}"
    if op == 0x24:
        return f"jr vi{fs:02}"
    if op == 0x25:
        return f"jalr vi{ft:02}, vi{fs:02}"
    if op in (0x28, 0x29):
        return f"{branches[op]} vi{ft:02}, vi{fs:02}, 0x{target:04x}"
    if op in branches:
        return f"{branches[op]} vi{fs:02}, 0x{target:04x}"
    return f"?l{op:02x}"


def main():
    data = open(sys.argv[1], "rb").read()
    start = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    end = int(sys.argv[3], 16) if len(sys.argv) > 3 else len(data)
    for pc in range(start, min(end, len(data) - 7), 8):
        lo, hi = struct.unpack_from("<II", data, pc)
        up = upper(hi)
        if hi >> 31 & 1:
            low = f"loi 0x{lo:08x} ({struct.unpack('<f', struct.pack('<I', lo))[0]:g})"
        else:
            low = lower(lo, pc)
        print(f"{pc:04x}:  {up:<44} {low}")


if __name__ == "__main__":
    main()
