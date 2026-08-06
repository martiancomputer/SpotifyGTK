#!/usr/bin/env python3
"""
Recover protobuf message schemas from DescriptorProto blobs in a binary.

Anchors on the message name rather than the file name: a DescriptorProto starts
with field 1 (name), so 0x0A <len> <Name> is the marker, and field 2 is a
repeated FieldDescriptorProto carrying name, number and type. Anchoring per
message rather than per file also catches descriptors whose FileDescriptorProto
never names a .proto path.

Validation is what keeps this honest -- random bytes routinely parse as *some*
protobuf, so a candidate is only accepted when every field it claims has both a
name and a number, and the names look like identifiers.
"""
import sys, re

TYPES = {1:"double",2:"float",3:"int64",4:"uint64",5:"int32",6:"fixed64",
         7:"fixed32",8:"bool",9:"string",10:"group",11:"message",12:"bytes",
         13:"uint32",14:"enum",15:"sfixed32",16:"sfixed64",17:"sint32",
         18:"sint64"}
LABELS = {1:"optional",2:"required",3:"repeated"}
IDENT = re.compile(rb"^[A-Za-z_][A-Za-z0-9_]{1,63}$")

def varint(b, i):
    r = s = 0
    while i < len(b):
        x = b[i]; i += 1
        r |= (x & 0x7f) << s
        if not x & 0x80: return r, i
        s += 7
        if s > 63: raise ValueError
    raise ValueError

def fields(b):
    i = 0
    while i < len(b):
        t, i = varint(b, i)
        n, w = t >> 3, t & 7
        if n == 0: raise ValueError
        if   w == 0: v, i = varint(b, i)
        elif w == 2:
            l, i = varint(b, i)
            if i + l > len(b): raise ValueError
            v = b[i:i+l]; i += l
        elif w == 5: v = b[i:i+4]; i += 4
        elif w == 1: v = b[i:i+8]; i += 8
        else: raise ValueError
        yield n, w, v

def parse_fielddesc(b):
    name = num = typ = lab = tname = None
    for n, w, v in fields(b):
        if   n == 1 and w == 2: name = v
        elif n == 3 and w == 0: num = v
        elif n == 4 and w == 0: lab = v
        elif n == 5 and w == 0: typ = v
        elif n == 6 and w == 2: tname = v
    return name, num, typ, lab, tname

def try_message(b):
    """Parse a DescriptorProto; return (name, [fields]) or None."""
    name, flds = None, []
    for n, w, v in fields(b):
        if   n == 1 and w == 2: name = v
        elif n == 2 and w == 2:
            f = parse_fielddesc(v)
            if f[0] is None or f[1] is None: return None
            if not IDENT.match(f[0]): return None
            if not (1 <= f[1] <= 536870911): return None
            flds.append(f)
        elif n in (3,4,5,6,7,8,9,10): pass
        else: return None
    if not name or not IDENT.match(name) or not flds: return None
    return name.decode(), flds

def main():
    data = open(sys.argv[1], "rb").read()
    pat  = sys.argv[2].encode() if len(sys.argv) > 2 else b""
    out, seen = [], set()
    for m in re.finditer(rb"\x0a([\x03-\x40])([A-Za-z_][A-Za-z0-9_]{2,63})", data):
        ln, name = m.group(1)[0], m.group(2)
        if ln != len(name): continue
        if pat and pat.lower() not in name.lower(): continue
        start = m.start()
        best = None
        for size in (16000, 4000, 1200, 400, 120):
            blob = data[start:start+size]
            try: r = try_message(blob)
            except Exception: r = None
            if r and (best is None or len(r[1]) > len(best[1])): best = r
        if not best: continue
        key = (best[0], tuple(f[1] for f in best[1]))
        if key in seen: continue
        seen.add(key)
        out.append(best)
    for name, flds in out:
        print(f"message {name} {{")
        for nm, num, typ, lab, tn in sorted(flds, key=lambda f: f[1]):
            t = tn.decode().lstrip(".") if tn else TYPES.get(typ, f"type{typ}")
            print(f"    {LABELS.get(lab,'')} {t} {nm.decode()} = {num};")
        print("}")
    print(f"\n[{len(out)} message(s)]", file=sys.stderr)

main()
