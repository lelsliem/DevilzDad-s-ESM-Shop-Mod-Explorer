import struct
CDB = r'D:\Projects\Address Library\SF-Shop\SF Shop Explorer\tools\renderer_recon\extracted\materialsbeta.cdb'
b = open(CDB, 'rb').read()
print('size:', len(b))
print('head:', b[:96])
# 1. are texture _color paths stored as strings?
for needle in [b'heater_slideandlower_color', b'ramen_color', b'genbooka01_color']:
    print(needle, 'at', b.find(needle))
# 2. booklarge anywhere?
i = b.find(b'booklarge')
print('booklarge at', i)
if i >= 0:
    print(repr(b[i - 60:i + 160]))
# 3. PIL DDS?
try:
    from PIL import Image
    print('PIL available', Image.__version__)
except ImportError as e:
    print('no PIL', e)
