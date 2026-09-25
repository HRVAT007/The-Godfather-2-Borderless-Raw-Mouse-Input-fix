import re, sys

path = r"E:\Program Files (x86)\KNIGHT\The Godfather II\godfather2.exe"
data = open(path, "rb").read()

pats = [re.compile(p.encode(), re.I) for p in sys.argv[1:]] or [re.compile(rb"subtitle", re.I)]

ascii_re = re.compile(rb"[\x20-\x7e]{6,}")
utf16_re = re.compile(rb"(?:[\x20-\x7e]\x00){6,}")

seen = set()
for m in ascii_re.finditer(data):
    s = m.group()
    if any(p.search(s) for p in pats):
        key = (m.start(), s)
        if key not in seen:
            seen.add(key)
            print(f"A {m.start():#x} {s[:160]!r}")
for m in utf16_re.finditer(data):
    s = m.group().decode("utf-16-le").encode()
    if any(p.search(s) for p in pats):
        key = (m.start(), s)
        if key not in seen:
            seen.add(key)
            print(f"W {m.start():#x} {s.decode()[:160]!r}")
