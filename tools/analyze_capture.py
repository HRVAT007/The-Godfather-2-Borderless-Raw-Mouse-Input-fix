"""Parse gf2fix.log capture region: cluster CAP DS / CAP SVM matrices to find subtitle transform."""
import re, sys
from collections import Counter, defaultdict

LOG = r"E:\Program Files (x86)\KNIGHT\The Godfather II\gf2fix.log"
lines = open(LOG, "r", errors="replace").read().splitlines()

# find capture 2 region (the one with DrawString>0)
starts = [i for i,l in enumerate(lines) if "CAPTURE START" in l]
ends   = [i for i,l in enumerate(lines) if "CAPTURE END" in l]
print("captures:", len(starts))
for s,e in zip(starts, ends):
    print("  ", lines[s].strip())
    print("  ", lines[e].strip())

# use the LAST capture (richest)
lo, hi = starts[-1], ends[-1]
region = lines[lo:hi]
print("\nanalyzing region lines %d..%d (%d lines)" % (lo, hi, len(region)))

ds_re  = re.compile(r"CAP DS #\d+ ctx=([0-9A-Fa-f]+) m=\{([-\d.]+), ([-\d.]+), ([-\d.]+), ([-\d.]+), ([-\d.]+), ([-\d.]+)\}")
svm_re = re.compile(r"CAP SVM #\d+ \{([-\d.]+), ([-\d.]+), ([-\d.]+), ([-\d.]+), ([-\d.]+), ([-\d.]+)\}")

ds = []
svm = []
for l in region:
    m = ds_re.search(l)
    if m:
        ds.append((m.group(1), tuple(float(x) for x in m.groups()[1:])))
        continue
    m = svm_re.search(l)
    if m:
        svm.append(tuple(float(x) for x in m.groups()))

print("parsed CAP DS=%d  CAP SVM=%d" % (len(ds), len(svm)))

# ---- DrawString matrix analysis ----
print("\n=== CAP DS: ctx pointers ===")
for ctx,c in Counter(c for c,_ in ds).most_common():
    print("  ctx=%s  count=%d" % (ctx, c))

print("\n=== CAP DS: ty distribution (matrix[5]) ===")
tys = [m[5] for _,m in ds]
if tys:
    print("  ty min=%.1f max=%.1f" % (min(tys), max(tys)))
    buckets = Counter(int(t//50)*50 for t in tys)
    for b in sorted(buckets):
        print("   ty[%4d..%4d): %d" % (b, b+50, buckets[b]))

print("\n=== CAP DS: tx distribution (matrix[4]) ===")
txs = [m[4] for _,m in ds]
if txs:
    print("  tx min=%.1f max=%.1f" % (min(txs), max(txs)))
    buckets = Counter(int(t//50)*50 for t in txs)
    for b in sorted(buckets):
        print("   tx[%4d..%4d): %d" % (b, b+50, buckets[b]))

print("\n=== CAP DS: linear coeff a (matrix[0]) distribution ===")
aa = Counter(round(m[0],2) for _,m in ds)
for a,c in aa.most_common(15):
    print("  a=%.2f count=%d" % (a, c))

print("\n=== CAP DS: linear coeff d (matrix[3]) distribution ===")
dd = Counter(round(m[3],2) for _,m in ds)
for d,c in dd.most_common(15):
    print("  d=%.2f count=%d" % (d, c))

print("\n=== CAP DS: most common full matrices (rounded) ===")
full = Counter(tuple(round(x,2) for x in m) for _,m in ds)
for mat,c in full.most_common(25):
    print("  count=%4d  {a=%.3f b=%.3f c=%.3f d=%.3f tx=%.1f ty=%.1f}" % (c, *mat))

# ---- per-ctx ty ranges (subtitle ctx vs hud ctx) ----
print("\n=== CAP DS: per-ctx ty/tx ranges ===")
byctx = defaultdict(list)
for ctx,m in ds:
    byctx[ctx].append(m)
for ctx, ms in byctx.items():
    tys=[m[5] for m in ms]; txs=[m[4] for m in ms]
    aas=[m[0] for m in ms]; dds=[m[3] for m in ms]
    print("  ctx=%s n=%d ty[%.1f..%.1f] tx[%.1f..%.1f] a[%.2f..%.2f] d[%.2f..%.2f]" % (
        ctx, len(ms), min(tys),max(tys), min(txs),max(txs), min(aas),max(aas), min(dds),max(dds)))
