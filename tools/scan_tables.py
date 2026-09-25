import struct
data = open(r'E:\Program Files (x86)\KNIGHT\The Godfather II\godfather2.exe', 'rb').read()
IB = 0x400000
SECS = [('.text', 0x401000, 0x94f000, 0x1000),
        ('.rdata', 0xd50000, 0x0fb000, 0x950000),
        ('.data', 0xe4b000, 0x2d0000, 0xa4b000)]

def off2rva(off):
    for n, va, vs, raw in SECS:
        if raw <= off < raw + vs:
            return off - raw + va
    return None

for target, name in [(0x005CC5B0, 'DrawString'), (0x005C52E0, 'SetVtxMtx'), (0x005cbdb0, 'type2rend')]:
    pat = struct.pack('<I', target)
    start = 0
    hits = []
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        rva = off2rva(i)
        if rva is not None:
            hits.append(hex(IB + rva))
        start = i + 1
    print(name, hits)
