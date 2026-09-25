"""RE helper for godfather2.exe: PE info, xref finder, disassembler."""
import sys, struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = r"E:\Program Files (x86)\KNIGHT\The Godfather II\godfather2.exe"
pe = pefile.PE(EXE, fast_load=True)
ib = pe.OPTIONAL_HEADER.ImageBase
data = open(EXE, "rb").read()

def off2rva(off):
    for s in pe.sections:
        if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
            return off - s.PointerToRawData + s.VirtualAddress
    return None

def rva2off(rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return rva - s.VirtualAddress + s.PointerToRawData
    return None

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

def find_xrefs_to_va(va):
    """Find absolute references (push/mov/cmp with imm32 == va) in .text."""
    pat = struct.pack("<I", va)
    out = []
    start = 0
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        rva = off2rva(i)
        if rva is not None:
            out.append((i, rva + ib))
        start = i + 1
    return out

def disasm_at(off, count=60, back=0):
    """Disassemble `count` instructions starting at file offset (optionally back up `back` bytes)."""
    o = off - back
    rva = off2rva(o)
    if rva is None:
        print("not in a section")
        return
    code = data[o:o + count * 8]
    for insn in md.disasm(code, ib + rva):
        print("%#010x  %-8s %s" % (insn.address, insn.mnemonic, insn.op_str))
        count -= 1
        if count <= 0:
            break

if __name__ == "__main__":
    print("ImageBase %#x" % ib)
    for s in pe.sections:
        print("%-8s VA %#010x VS %#08x RAW %#08x SZ %#08x" % (
            s.Name.rstrip(b"\0").decode(), ib + s.VirtualAddress, s.Misc_VirtualSize,
            s.PointerToRawData, s.SizeOfRawData))
    # string xrefs mode: python re_helper.py str <substring>
    if len(sys.argv) > 2 and sys.argv[1] == "str":
        import re
        pat = re.compile(re.escape(sys.argv[2].encode()), re.I)
        for m in re.finditer(rb"[\x20-\x7e]{4,}", data):
            if pat.search(m.group()):
                off = m.start()
                rva = off2rva(off)
                va = ib + rva if rva else None
                print("string %r at off %#x va %s" % (m.group()[:80], off, hex(va) if va else "?"))
                if va:
                    for soff, sva in find_xrefs_to_va(va):
                        print("   xref at off %#x va %#x" % (soff, sva))
    elif len(sys.argv) > 2 and sys.argv[1] == "dis":
        disasm_at(int(sys.argv[2], 0), int(sys.argv[3]) if len(sys.argv) > 3 else 40)
