import struct, os, zlib
DATA = r'B:/SteamLibrary/steamapps/common/Starfield/Data'
WANT = [
    'textures/items/fooddrink_singleitems/ramen/ramen_color.dds',
    'textures/setdressing/books/genbooka01_color.dds',
    'textures/weapons/rattler/heater_receiver_color.dds',
    'textures/setdressing/ammobox01/ammobox01_color.dds',
]
# locate chunks
for a in sorted(os.listdir(DATA)):
    if not a.lower().endswith('.ba2'):
        continue
    f = open(os.path.join(DATA, a), 'rb')
    hdr = f.read(36)
    if hdr[:4] != b'BTDX':
        f.close(); continue
    hver = struct.unpack('<I', hdr[4:8])[0]
    htyp = hdr[8:12]
    num = struct.unpack('<I', hdr[12:16])[0]
    nt_off = struct.unpack('<Q', hdr[16:24])[0]
    method = struct.unpack('<I', hdr[32:36])[0] if hver >= 3 else 0
    if htyp == b'DX10':
        f.seek(36)
        texs = []
        ok = True
        for i in range(num):
            h = f.read(24)
            if len(h) < 24:
                ok = False; break
            nch = h[13]
            H = struct.unpack('<H', h[16:18])[0]; W = struct.unpack('<H', h[18:20])[0]
            if nch == 0 or nch > 16:
                continue
            chunks = []
            for c in range(nch):
                ch = f.read(24)
                off, packed, size = struct.unpack('<QII', ch[:16])
                sm, em = struct.unpack('<HH', ch[16:20])
                chunks.append((off, packed, size, sm, em))
            texs.append(((H, W, h[20], h[21]), chunks))
        if not ok:
            f.close(); continue
        f.seek(nt_off)
        for i in range(num):
            ln = struct.unpack('<H', f.read(2))[0]
            nm = f.read(ln).decode('latin-1').lower()
            if nm in WANT:
                (H, W, mips, dxgi), chunks = texs[i]
                best = None
                for ch in chunks:
                    if ch[3] == 0:
                        best = ch; break
                    if best is None or ch[2] > best[2]:
                        best = ch
                off, packed, size, sm, em = best
                f2 = open(os.path.join(DATA, a), 'rb')
                f2.seek(off)
                raw = f2.read(packed if packed else size)
                f2.close()
                if packed and packed != size:
                    if method == 3:
                        try:
                            import lz4.block
                        except ImportError:
                            print(nm, 'LZ4 skipped')
                            continue
                        raw = lz4.block.decompress(raw, uncompressed_size=size)
                    else:
                        raw = zlib.decompress(raw)
                print(nm, 'dxgi=', dxgi, 'WxH=', W, 'x', H, 'bytes=', len(raw[:100]))
                open(r'D:\Projects\Address Library\SF-Shop\SF Shop Explorer\tools\renderer_recon\_tex_' +
                     nm.split('/')[-1] + '.bin', 'wb').write(
                    struct.pack('<HHBB', W, H, dxgi, method) + raw[:size])
    f.close()
print('done')
