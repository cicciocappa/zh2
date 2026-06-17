#!/usr/bin/env python3
# Ispettore FBX binario per debug del pipeline rig→VAT (vedi RIG_MIXAMO_NOTES.md).
# Legge i campi che contano per capire perche' un modello si baka storto:
#   - GlobalSettings: UpAxis / FrontAxis / CoordAxis (+ segni), UnitScaleFactor
#   - per ogni nodo Model: Lcl Scaling / Lcl Rotation / Lcl Translation, PreRotation
# Nessuna dipendenza (solo struct/zlib). Funziona su FBX binari (Kaydara FBX Binary).
#
#   python3 vat/fbx_inspect.py modello.fbx [altro.fbx ...]
#
# Confronto tipico: un modello che baka bene vs uno storto -> si vede subito la
# differenza (es. UpAxis Y vs Z, oppure mesh con Lcl Scaling 100).
import struct, sys

def read_fbx(path):
    d = open(path, 'rb').read()
    ver = struct.unpack('<I', d[23:27])[0]
    u64 = ver >= 7500
    OFF = '<Q' if u64 else '<I'; OS = 8 if u64 else 4
    def node(pos):
        end = struct.unpack(OFF, d[pos:pos+OS])[0]; pos += OS
        nprop = struct.unpack(OFF, d[pos:pos+OS])[0]; pos += OS
        pos += OS  # property list length (skip)
        nl = d[pos]; pos += 1
        name = d[pos:pos+nl].decode('latin1'); pos += nl
        props = []
        for _ in range(nprop):
            t = chr(d[pos]); pos += 1
            if t == 'Y': props.append(struct.unpack('<h', d[pos:pos+2])[0]); pos += 2
            elif t == 'C': props.append(bool(d[pos])); pos += 1
            elif t == 'I': props.append(struct.unpack('<i', d[pos:pos+4])[0]); pos += 4
            elif t == 'F': props.append(struct.unpack('<f', d[pos:pos+4])[0]); pos += 4
            elif t == 'D': props.append(struct.unpack('<d', d[pos:pos+8])[0]); pos += 8
            elif t == 'L': props.append(struct.unpack('<q', d[pos:pos+8])[0]); pos += 8
            elif t in 'fdlib':
                al, enc, cl = struct.unpack('<III', d[pos:pos+12]); pos += 12 + cl
                props.append('[array %s n=%d]' % (t, al))
            elif t in 'SR':
                L = struct.unpack('<I', d[pos:pos+4])[0]; pos += 4
                props.append(d[pos:pos+L].decode('latin1') if t == 'S' else '[raw %d]' % L); pos += L
            else:
                props.append('?%s' % t); break
        children = []
        nullsz = OS*3 + 1
        while pos < end:
            if d[pos:pos+nullsz] == b'\x00'*nullsz:
                pos += nullsz; break
            c, pos = node(pos); children.append(c)
        return (name, props, children), end
    nodes = []; p = 27; nullsz = OS*3 + 1
    while p < len(d) - 20:
        if d[p:p+nullsz] == b'\x00'*nullsz: break
        n, p = node(p); nodes.append(n)
    return ver, nodes

def walk(n):
    yield n
    for c in n[2]:
        yield from walk(c)

GS_KEYS = ('UpAxis', 'FrontAxis', 'CoordAxis', 'UnitScale', 'OriginalUp')
MODEL_KEYS = ('Lcl Scaling', 'Lcl Rotation', 'Lcl Translation', 'PreRotation',
              'GeometricScaling', 'GeometricRotation', 'GeometricTranslation')

def dump(path):
    ver, nodes = read_fbx(path)
    print("===== %s  (FBX %d)" % (path, ver))
    for n in nodes:
        for name, props, ch in walk(n):
            if name == 'GlobalSettings':
                for nm, pr, _ in walk((name, props, ch)):
                    if nm == 'P' and pr and any(k in str(pr[0]) for k in GS_KEYS):
                        print("  GS  %-26s = %s" % (pr[0], pr[4:]))
    for n in nodes:
        for name, props, ch in walk(n):
            if name == 'Model':
                print("  MODEL", props[1] if len(props) > 1 else '?')
                for nm, pr, _ in walk((name, props, ch)):
                    if nm == 'P' and pr and any(k in str(pr[0]) for k in MODEL_KEYS):
                        print("     %-22s = %s" % (pr[0], pr[4:]))

if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("uso: python3 vat/fbx_inspect.py modello.fbx [...]")
    for p in sys.argv[1:]:
        dump(p)
