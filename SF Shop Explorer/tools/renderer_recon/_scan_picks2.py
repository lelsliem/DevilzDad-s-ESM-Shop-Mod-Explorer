import re
txt = open(r'C:\Users\mrlma\Documents\My Games\Starfield\SFSE\Logs\SFShopExplorer.log',
           encoding='utf-8', errors='replace').read()
flat = txt.replace('\n', ' ')
pat = re.compile(r"prefetched '(.*?\.mat)' <- (\S+?\.dds)")
hits = pat.findall(flat)
seen = set()
for mat, tex in hits:
    ml = mat.lower()
    if 'rattler' in ml or 'ramen' in ml or 'succulent' in ml or 'pottedplant' in ml or 'deskplant' in ml or 'booklarge02' in ml or 'bookomega' in ml or 'redrover' in ml:
        if (mat, tex) not in seen:
            seen.add((mat, tex))
            print(mat, '<-', tex)
