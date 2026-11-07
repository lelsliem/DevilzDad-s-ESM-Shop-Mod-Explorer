import struct, os
HERE = r'D:\Projects\Address Library\SF-Shop\SF Shop Explorer\tools\renderer_recon'
for f in os.listdir(HERE):
    if f.startswith('_tex_') and f.endswith('.bin'):
        b = open(os.path.join(HERE, f), 'rb').read()
        W, H, dxgi, method = struct.unpack('<HHBB', b[:6])
        raw = b[6:]
        # DDS header, DXT1 (BC1)
        h = (struct.pack('<4s7I11I', b'DDS ', 124, 0x1007, H, W,
                         len(raw) // max(1, H // 4), 0, 0,
                         *([0] * 11)) +
             struct.pack('<I I 4s I 4I', 32, 0x4, b'DXT1', 0, 0, 0, 0, 0) +
             struct.pack('<5I', 0x1000, 0, 0, 0, 0))
        out = os.path.join(HERE, f.replace('.bin', '.dds'))
        open(out, 'wb').write(h + raw)
        print(out, len(raw))
