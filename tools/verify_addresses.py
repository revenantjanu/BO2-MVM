#!/usr/bin/env python3
"""Check that a t6zm.exe / t6mp.exe still contains the byte signatures bo2mvm.dll hooks.
Usage: python verify_addresses.py path\\to\\t6zm.exe / t6mp.exe
Run this after a Plutonium update. If something prints MISSING the DLL will refuse to start
(it never patches memory it cannot verify)."""
import re, sys

SIGS = {
    "CG_CalcViewValues": (0x7D4BF0, "83 EC 70 53 8B 5C 24 78 56 8B 35 ?? ?? ?? ?? F6 86 2C 85 04 00 04"),
    "Cbuf_AddText":      (0x4C7120, "6A 33 E8 ?? ?? ?? ?? 8B 4C 24 0C 83 C4 04 8B D1 85 C9 74 1F"),
    "AnglesToAxis":      (0x429360, "83 EC 20 F3 0F 10 0D ?? ?? ?? ?? 56 8B 74 24 28"),
}

def load(path):
    d = open(path, "rb").read()
    pe = int.from_bytes(d[0x3c:0x40], "little")
    nsec = int.from_bytes(d[pe + 6:pe + 8], "little")
    opt = int.from_bytes(d[pe + 20:pe + 22], "little")
    base = int.from_bytes(d[pe + 52:pe + 56], "little")
    so = pe + 24 + opt
    secs = []
    for i in range(nsec):
        s = d[so + i * 40: so + i * 40 + 40]
        secs.append((s[:8].rstrip(b"\0"), int.from_bytes(s[12:16], "little"), int.from_bytes(s[8:12], "little"),
                     int.from_bytes(s[20:24], "little"), int.from_bytes(s[16:20], "little")))
    return d, base, secs

def rx(pattern):
    return re.compile(b"".join(b"." if t == "??" else re.escape(bytes([int(t, 16)])) for t in pattern.split()), re.S)

def main():
    if len(sys.argv) < 2:
        print(__doc__); return 1
    d, base, secs = load(sys.argv[1])
    text = next(s for s in secs if s[0] == b".text")
    code = d[text[3]: text[3] + text[4]]
    bad = 0
    for name, (va, pat) in SIGS.items():
        m = list(rx(pat).finditer(code))
        found = [base + text[1] + x.start() for x in m]
        status = "OK     " if found == [va] else ("MOVED  " if found else "MISSING")
        bad += status != "OK     "
        print(f"{status} {name:18} expected {va:#x}  found {[hex(f) for f in found[:3]]}")
    return 1 if bad else 0

sys.exit(main())
