import struct, sys, zlib, re
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
path = r"B:\SteamLibrary\steamapps\common\Starfield\Data\Starfield.esm"
out = r"D:\Projects\Address Library\SF-Shop\SF Shop Explorer\src\OmodClassTable.h"

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

weap = {}    # edid_lower -> [fid, edid]
quality = [] # (omod, weapon, rank, token)
effects = [] # (omod, rank)
armor_effects = []  # (omod, slot, rank); slot 0=body 1=helmet 2=pack
unresolved = []

def rec_body(pos, size, flags):
    body = data[pos:pos+size]
    if flags & 0x00040000 and len(body) >= 4:
        try: body = zlib.decompress(body[4:])
        except Exception: pass
    return body

QUALITY_RE = re.compile(r'^mod_(.+)_Quality_0([1-4])$', re.IGNORECASE)
LEGEND_RE = re.compile(r'^mod_Legendary_Wep_([123])_', re.IGNORECASE)
# Armor-side legendary effects are per equipment class, unlike the single
# universal weapon pool: mod_Legendary_Armor_0N_* goes on suits and apparel,
# mod_Legendary_Helmet_0N_* on helmets/hats, mod_Legendary_Pack_0N_* on
# backpacks. Rank matches the weapon convention (1 = rolls on any legendary
# item, 3 = only on 3-effect items). mod_Legendary_Armor_4_None is a
# placeholder, not an effect: rank 4 is not a rarity and is skipped.
ARMOR_LEGEND_RE = re.compile(r'^mod_Legendary_(Armor|Helmet|Pack)_0?([1-3])_', re.IGNORECASE)
ARMOR_LEGEND_SLOT = {'armor': 0, 'helmet': 1, 'pack': 2}

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
            if size > 64*1024*1024 or pos + 24 + size > end:
                pos += 1; continue
            if sig in (b'WEAP', b'OMOD'):
                body = rec_body(pos+24, size, flags)
                edid = ''
                for ss, sd in subrecs(body):
                    if ss == b'EDID':
                        edid = sd.split(b'\x00')[0].decode('ascii', 'replace')
                        break
                if sig == b'WEAP' and edid:
                    weap.setdefault(edid.lower(), [fid, edid])
                elif sig == b'OMOD' and edid:
                    mq = QUALITY_RE.match(edid)
                    ml = LEGEND_RE.match(edid)
                    ma = ARMOR_LEGEND_RE.match(edid)
                    if mq:
                        quality.append((fid, mq.group(1), int(mq.group(2))))
                    elif ml:
                        effects.append((fid, int(ml.group(1))))
                    elif ma:
                        armor_effects.append((fid, ARMOR_LEGEND_SLOT[ma.group(1).lower()], int(ma.group(2))))
            pos += 24 + size
        else:
            pos += 1

walk(0, fsz)
print('weap=%d quality-omods=%d legendary-wep-omods=%d legendary-armor-omods=%d' % (
    len(weap), len(quality), len(effects), len(armor_effects)))

# Resolve mod_<Token>_Quality -> WEAP fid (exact, then contains)
qrows = []
for fid, token, rank in quality:
    tl = token.lower()
    if tl in weap:
        qrows.append((fid, weap[tl][0], rank))
        continue
    hits = [v for k, v in weap.items() if k in tl or tl in k]
    if len(hits) == 1:
        qrows.append((fid, hits[0][0], rank))
    else:
        unresolved.append('%s (hits=%d %s)' % (token, len(hits), ','.join(h[1] for h in hits[:4])))

qrows.sort()
print('quality resolved=%d unresolved=%d' % (len(qrows), len(unresolved)))
for u in unresolved[:20]:
    print('  UNRESOLVED', u)

erows = sorted(effects)
print('effects resolved=%d' % len(erows))
aerows = sorted(armor_effects)
print('armor effects resolved=%d (body=%d helmet=%d pack=%d)' % (
    len(aerows),
    sum(1 for r in aerows if r[1] == 0), sum(1 for r in aerows if r[1] == 1),
    sum(1 for r in aerows if r[1] == 2)))

lines = []
lines.append('#pragma once')
lines.append('')
lines.append('#include <cstdint>')
lines.append('')
lines.append('namespace SFE')
lines.append('{')
lines.append('\t// Build-94 rarity tables generated from Starfield.esm by')
lines.append('\t// tools/gen_omod_class_table.py. Quality OMODs carry no MNAM and')
lines.append('\t// no model (so the MNAM census can never see them) and their')
lines.append('\t// runtime editor IDs are unreadable; the file-side EDID')
lines.append('\t// mod_<Weapon>_Quality_0N supplies both the weapon binding and')
lines.append('\t// the rank. mod_Legendary_Wep_N_* OMODs are the universal weapon')
lines.append('\t// effect pool (rank 1-3, no weapon binding, no AP).')
lines.append('\t//')
lines.append('\t// Armor effects are per class instead of one pool: '
             'mod_Legendary_Armor_0N_*')
lines.append('\t// for suits/apparel, mod_Legendary_Helmet_0N_* for helmets and'
             " hats,")
lines.append('\t// mod_Legendary_Pack_0N_* for backpacks. Same rank convention.')
lines.append('\t// All tables are sorted by OMOD formID for binary search. Base game only.')
lines.append('\tstruct OmodQualityEntry')
lines.append('\t{')
lines.append('\t\tstd::uint32_t omod;')
lines.append('\t\tstd::uint32_t weapon;')
lines.append('\t\tstd::uint32_t rank;   // 1=Common(01) 2=Calibrated 3=Refined 4=Advanced')
lines.append('\t};')
lines.append('')
lines.append('\tstruct OmodLegendaryEntry')
lines.append('\t{')
lines.append('\t\tstd::uint32_t omod;')
lines.append('\t\tstd::uint32_t rank;   // 1..3')
lines.append('\t};')
lines.append('')
lines.append('\tinline constexpr OmodQualityEntry kOmodQualityTable[] = {')
for omod, weapon, rank in qrows:
    lines.append('\t\t{ 0x%08Xu, 0x%08Xu, %du },' % (omod, weapon, rank))
lines.append('\t};')
lines.append('')
lines.append('\tinline constexpr OmodLegendaryEntry kOmodLegendaryTable[] = {')
for omod, rank in erows:
    lines.append('\t\t{ 0x%08Xu, %du },' % (omod, rank))
lines.append('\t};')
lines.append('')
lines.append('\tinline const OmodQualityEntry* LookupOmodQuality(std::uint32_t a_omod)')
lines.append('\t{')
lines.append('\t\tstd::uint32_t lo = 0;')
lines.append('\t\tstd::uint32_t hi = static_cast<std::uint32_t>(sizeof(kOmodQualityTable) / sizeof(kOmodQualityTable[0]));')
lines.append('\t\twhile (lo < hi) {')
lines.append('\t\t\tconst auto mid = lo + (hi - lo) / 2;')
lines.append('\t\t\tif (kOmodQualityTable[mid].omod < a_omod) {')
lines.append('\t\t\t\tlo = mid + 1;')
lines.append('\t\t\t} else {')
lines.append('\t\t\t\thi = mid;')
lines.append('\t\t\t}')
lines.append('\t\t}')
lines.append('\t\tif (lo < sizeof(kOmodQualityTable) / sizeof(kOmodQualityTable[0]) && kOmodQualityTable[lo].omod == a_omod) {')
lines.append('\t\t\treturn &kOmodQualityTable[lo];')
lines.append('\t\t}')
lines.append('\t\treturn nullptr;')
lines.append('\t}')
lines.append('')
lines.append('\tstruct OmodArmorLegendaryEntry')
lines.append('\t{')
lines.append('\t\tstd::uint32_t omod;')
lines.append('\t\tstd::uint32_t slot;   // 0 = suit/apparel body, 1 = helmet/hat, 2 = backpack')
lines.append('\t\tstd::uint32_t rank;   // 1..3')
lines.append('\t};')
lines.append('')
lines.append('\tinline constexpr OmodArmorLegendaryEntry kOmodArmorLegendaryTable[] = {')
for omod, slot, rank in aerows:
    lines.append('\t\t{ 0x%08Xu, %du, %du },' % (omod, slot, rank))
lines.append('\t};')
lines.append('')
lines.append('\tinline const OmodLegendaryEntry* LookupOmodLegendary(std::uint32_t a_omod)')
lines.append('\t{')
lines.append('\t\tstd::uint32_t lo = 0;')
lines.append('\t\tstd::uint32_t hi = static_cast<std::uint32_t>(sizeof(kOmodLegendaryTable) / sizeof(kOmodLegendaryTable[0]));')
lines.append('\t\twhile (lo < hi) {')
lines.append('\t\t\tconst auto mid = lo + (hi - lo) / 2;')
lines.append('\t\t\tif (kOmodLegendaryTable[mid].omod < a_omod) {')
lines.append('\t\t\t\tlo = mid + 1;')
lines.append('\t\t\t} else {')
lines.append('\t\t\t\thi = mid;')
lines.append('\t\t\t}')
lines.append('\t\t}')
lines.append('\t\tif (lo < sizeof(kOmodLegendaryTable) / sizeof(kOmodLegendaryTable[0]) && kOmodLegendaryTable[lo].omod == a_omod) {')
lines.append('\t\t\treturn &kOmodLegendaryTable[lo];')
lines.append('\t\t}')
lines.append('\t\treturn nullptr;')
lines.append('\t}')
lines.append('')
lines.append('\tinline const OmodArmorLegendaryEntry* LookupOmodArmorLegendary(std::uint32_t a_omod)')
lines.append('\t{')
lines.append('\t\tstd::uint32_t lo = 0;')
lines.append('\t\tstd::uint32_t hi = static_cast<std::uint32_t>(sizeof(kOmodArmorLegendaryTable) / sizeof(kOmodArmorLegendaryTable[0]));')
lines.append('\t\twhile (lo < hi) {')
lines.append('\t\t\tconst auto mid = lo + (hi - lo) / 2;')
lines.append('\t\t\tif (kOmodArmorLegendaryTable[mid].omod < a_omod) {')
lines.append('\t\t\t\tlo = mid + 1;')
lines.append('\t\t\t} else {')
lines.append('\t\t\t\thi = mid;')
lines.append('\t\t\t}')
lines.append('\t\t}')
lines.append('\t\tif (lo < sizeof(kOmodArmorLegendaryTable) / sizeof(kOmodArmorLegendaryTable[0]) && kOmodArmorLegendaryTable[lo].omod == a_omod) {')
lines.append('\t\t\treturn &kOmodArmorLegendaryTable[lo];')
lines.append('\t\t}')
lines.append('\t\treturn nullptr;')
lines.append('\t}')
lines.append('}')
with open(out, 'w', encoding='ascii', newline='\n') as f:
    f.write('\n'.join(lines) + '\n')
print('wrote', out)
