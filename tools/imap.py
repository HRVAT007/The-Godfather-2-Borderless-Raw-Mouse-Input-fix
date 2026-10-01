"""Verified instruction-boundary map for .text of godfather2.exe (non-ASLR).

MSVC pads between functions with runs of int3 (0xCC). Splitting .text on those
runs and disassembling each segment linearly from its first byte (a real
function entry) yields a bitmap of genuine instruction starts keyed by FILE
OFFSET (for .text, VA == 0x400000 + file offset). Xref lookups only report
instructions that really begin at a boundary, so a naive desyncing linear sweep
is never trusted.
"""
import re, struct, sys, os
import re_helper as R
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

TEXT_OFF = R.rva2off(0x1000)
TEXT_LEN = 0x954000
TEXT_END = TEXT_OFF + TEXT_LEN
IB = 0x400000
d = R.data
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'startmap2.bin')


def va_of(off):
    r = R.off2rva(off)
    return None if r is None else IB + r


def off_of(va):
    return R.rva2off(va - IB)


def _build():
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    bmp = bytearray(TEXT_END)
    seg = TEXT_OFF
    segs = []
    for x in re.finditer(rb'\xcc{2,}', d[TEXT_OFF:TEXT_END]):
        segs.append((seg, TEXT_OFF + x.start()))
        seg = TEXT_OFF + x.end()
    segs.append((seg, TEXT_END))
    n = 0
    for s, e in segs:
        if e - s < 4:
            continue
        for i in md.disasm(d[s:e], va_of(s)):
            bmp[off_of(i.address)] = 1
            n += 1
    with open(CACHE, 'wb') as fh:
        fh.write(bmp)
    return n


if os.path.exists(CACHE) and os.path.getsize(CACHE) == TEXT_END:
    BMP = bytearray(open(CACHE, 'rb').read())
else:
    _build()
    BMP = bytearray(open(CACHE, 'rb').read())

_MD = Cs(CS_ARCH_X86, CS_MODE_32)


def insn_at_off(o, maxlen=32):
    if not (TEXT_OFF <= o < TEXT_END) or not BMP[o]:
        return None
    for i in _MD.disasm(d[o:o + maxlen], va_of(o)):
        return i
    return None


def covering(off):
    """verified instruction whose bytes include file offsets off..off+3"""
    for k in range(0, 13):
        o = off - k
        if o < TEXT_OFF:
            break
        if not BMP[o]:
            continue
        i = insn_at_off(o)
        if i is not None and off_of(i.address) + i.size >= off + 4:
            return i
    return None


def xref_val(val, lo=None, hi=None):
    pat = struct.pack('<I', val)
    hits = []
    i = TEXT_OFF
    while True:
        i = d.find(pat, i, TEXT_END)
        if i < 0:
            break
        j = i
        i += 1
        ins = covering(j)
        if ins is None:
            continue
        if not (('0x%x' % val) in ins.op_str or
                re.search(r'(?<![0-9a-f])%d(?![0-9a-f])' % val, ins.op_str)):
            continue
        if lo and not (lo <= ins.address <= (hi or 0xFFFFFFFF)):
            continue
        hits.append((ins.address, ins.mnemonic + ' ' + ins.op_str, ins.bytes.hex()))
    return hits


def callsites(target):
    res = []
    o = TEXT_OFF
    while True:
        o = d.find(b'\xe8', o, TEXT_END)
        if o < 0:
            break
        o += 1
        if not BMP[o - 1]:
            continue
        ins = insn_at_off(o - 1)
        if ins and ins.mnemonic == 'call' and ('0x%x' % target) in ins.op_str:
            res.append(ins.address)
    return res


def dis(va, n=40):
    out = []
    o = off_of(va)
    for _ in range(n):
        i = insn_at_off(o)
        if i is None:
            out.append(' %#010x  <not a verified boundary>' % (va_of(o) or 0))
            break
        out.append('%#010x  %-46s ; %s' % (i.address, i.mnemonic + ' ' + i.op_str, i.bytes.hex()))
        o += i.size
    return out


if __name__ == '__main__':
    print('verified instruction starts: %d' % sum(BMP))
    for arg in sys.argv[1:]:
        if arg.startswith('dis:'):
            p = arg.split(':')[1:]
            for line in dis(int(p[0], 0), int(p[1]) if len(p) > 1 else 40):
                print(line)
        elif arg.startswith('call:'):
            t = int(arg.split(':')[1], 0)
            cs = callsites(t)
            print('#### call sites of %#x : %d -> %s' % (t, len(cs), ', '.join(hex(x) for x in cs)))
        else:
            v = int(arg, 0)
            hs = xref_val(v)
            print('#### operand %#x : %d hits' % (v, len(hs)))
            for a, t, b in hs:
                print('   %#010x  %-46s ; %s' % (a, t, b))
