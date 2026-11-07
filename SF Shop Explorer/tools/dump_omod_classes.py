import struct, sys, zlib
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
path = r"B:\SteamLibrary\steamapps\common\Starfield\Data\Starfield.esm"

def subrecs(body):
    outl = []
    p = 0
    while p + 6 <= len(body):
        ss = body[p:p+4]
        if not all(0x20 <= c < 0x7F for c in ss):
            break
        sz = struct.unpack_from('<H', body, p+4)[0]
        if p + 6 + sz > len(body):
            outl.append((ss, body[p+6:])); break
        outl.append((ss, body[p+6:p+6+sz]))
        p += 6 + sz
    return outl

data = open(path, 'rb').read()
fsz = len(data)

kywd = {}     # fid -> edid
omods = []    # (fid, body)

def rec_body(pos, size, flags):
    body = data[pos:pos+size]
    if flags & 0x00040000 and len(body) >= 4:
        try: body = zlib.decompress(body[4:])
        except Exception: pass
    return body

def walk(pos, end):
    while pos + 24 <= end:
        sig = data[pos:pos+4]
        if sig == b'GRUP':
            gsz = struct.unpack_from('<I', data, pos+4)[0]
            if gsz < 24 or pos + gsz > end: break
            walk(pos+24, pos+gsz)
            pos += gsz
        elif sig == b'TES4' or all((0x41 <= c <= 0x5A or 0x61 <= c <= 0x7A or 0x30 <= c <= 0x39) for c in sig):
            size, flags, fid = struct.unpack_from('<III', data, pos+4)
            if size > 64*1024*1024 or pos+24+size > end:
                pos += 1; continue
            if sig == b'KYWD':
                body = rec_body(pos+24, size, flags)
                for ss, sd in subrecs(body):
                    if ss == b'EDID':
                        kywd[fid] = sd.split(b'\x00')[0].decode('ascii', 'replace')
                        break
            elif sig == b'OMOD':
                omods.append((fid, rec_body(pos+24, size, flags)))
            pos += 24 + size
        else:
            pos += 1

walk(0, fsz)
print('kywd=%d omod=%d' % (len(kywd), len(omods)))

def kw_edids(sd):
    # KWDA: array of formIDs; KSIZ immediately before gives the count
    n = len(sd) // 4
    out = []
    for i in range(min(n, 12)):
        f = struct.unpack_from('<I', sd, i*4)[0]
        out.append(kywd.get(f, '%08X' % f))
    return out

rows = []
for fid, body in omods:
    tags = []
    full = ''
    mnam = 0
    kws = []
    ksiz = 0
    modl = False
    for ss, sd in subrecs(body):
        tags.append(ss.decode('ascii', 'replace'))
        if ss == b'FULL':
            full = sd.split(b'\x00')[0].decode('ascii', 'replace')
        elif ss == b'MNAM' and len(sd) >= 4:
            mnam = struct.unpack_from('<I', sd, 0)[0]
        elif ss == b'KSIZ' and len(sd) >= 4:
            ksiz = struct.unpack_from('<I', sd, 0)[0]
        elif ss == b'KWDA':
            kws = kw_edids(sd)
        elif ss == b'MODL':
            modl = True
    rows.append((fid, full, mnam, ksiz, kws, modl, tags))

# Quality / effect / parts-sample classification
QUALITY = ('calibrated', 'refined', 'advanced', 'superior', 'exceptional')
EFFECT_SAMPLE = ('armor-piercing', 'one inch punch', 'combat veteran', 'hitman', 'hit man')
PART_SAMPLE = ('high velocity', 'hair trigger', 'high powered')

def show(row, why):
    fid, full, mnam, ksiz, kws, modl, tags = row
    print('%s %08X full=%r mnam=%s ksiz=%d modl=%d kws=%s tags=%s' % (
        why, fid, full, kywd.get(mnam, '%08X' % mnam) if mnam else '-', ksiz, int(modl),
        ','.join(kws[:8]), ','.join(tags)))

nq = ne = 0
for row in rows:
    full = row[1].lower()
    if any(q in full for q in QUALITY):
        show(row, 'QUALITY')
        nq += 1
    elif any(e in full for e in EFFECT_SAMPLE):
        show(row, 'EFFECT ')
        ne += 1
print('quality-omods=%d effect-samples=%d' % (nq, ne))
for row in rows:
    if row[1].lower() in PART_SAMPLE:
        show(row, 'PART   ')
