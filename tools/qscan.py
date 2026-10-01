"""Scratch RE scanner. For a given 4-byte little-endian operand value (a struct
field offset like 0x284, or an absolute VA like 0x112A700), find every .text
instruction that *consumes* those exact bytes as its final 4 bytes (i.e. the
decode ends exactly at disp32/imm32 end). That end-alignment check is what
keeps the sweep from desyncing: no linear disassembly is trusted."""
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
import re_helper as R

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True
TEXT_OFF = R.rva2off(0x401000 - 0x400000)
TEXT_LEN = 0x954000
d = R.data


def find(val, want_off=None):
    pat = struct.pack('<I', val)
    res = []
    i = 0
    while True:
        i = d.find(pat, i)
        if i < 0:
            break
        j = i
        i += 1
        if not (TEXT_OFF <= j < TEXT_OFF + TEXT_LEN):
            continue
        want_end = 0x400000 + (j + 4 - TEXT_OFF)
        for st in range(max(TEXT_OFF, j - 9), j):
            ins = list(md.disasm(d[st:st + 20], 0x400000 + (st - TEXT_OFF)))
            if not ins:
                continue
            x = ins[0]
            if not (x.address <= 0x400000 + (j - TEXT_OFF) and x.address + x.size >= want_end):
                continue
            if '0x%x' % val not in x.op_str and ('%d' % val) not in x.op_str:
                continue
            res.append((x.address, x.mnemonic + ' ' + x.op_str, x.bytes.hex()))
            break
    return res


if __name__ == '__main__':
    for arg in sys.argv[1:]:
        v = int(arg, 0)
        hits = find(v)
        print('#### operand %#x : %d hits' % (v, len(hits)))
        for a, t, b in hits:
            print('   %#010x  %-46s ; %s' % (a, t, b))
