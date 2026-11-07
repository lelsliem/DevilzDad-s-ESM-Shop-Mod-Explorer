import sys,re,collections
from pathlib import Path
sys.path.insert(0,"tools/ba2_audit")
from ba2lib import Ba2Archive
from material_sweep import build_cdb_index
D=Path(r"B:\SteamLibrary\steamapps\common\Starfield\Data")
idx=build_cdb_index(Path("tools/renderer_recon/extracted/materialsbeta.cdb"),keep_empty=True)
modroot=Path(r"B:\MO2\Starfield\mods")
modlist=Path(r"B:\MO2\Starfield\profiles\Test profile\modlist.txt")
enabled={x[1:].strip().lower() for x in modlist.read_text(errors="replace").splitlines() if x.startswith("+") and not x[1:].strip().lower().endswith("_separator")}
mods=[]
for mod in sorted(modroot.iterdir()):
 if mod.is_dir() and mod.name.lower() in enabled: mods.extend(sorted(mod.glob("*.ba2")))
archives=mods+sorted(D.glob("*.ba2"))
print("archives:",len(mods),"enabled-mod +",len(archives)-len(mods),"base")
matrefs=collections.defaultdict(collections.Counter); nifs=0
for ap in archives:
 ar=Ba2Archive(ap)
 if not ar.valid: continue
 for name in ar.entries:
  if not (name.startswith("meshes/weapons/") and name.endswith(".nif")): continue
  data,_=ar.read_entry(name)
  if data is None: continue
  nifs+=1
  for m in re.finditer(rb"[ -~]{4,}",data):
   low=m.group().decode("latin-1").lower().replace(chr(92),"/")
   if low.startswith("materials/weapons/") and low.endswith(".mat"): matrefs[low][ap.name]+=1
print("weapon NIFs:",nifs,"unique weapon material paths:",len(matrefs),"references:",sum(sum(c.values()) for c in matrefs.values()))
def lookup(mat):
 stem=mat.rsplit("/",1)[-1][:-4]
 for key in (mat,"data/"+mat,stem):
  if key in idx:return key,idx[key]
 base=stem.rstrip("0123456789").rstrip("_")
 if base!=stem and base in idx:return base,idx[base]
 return None,None
watch={"barrel","receiver","stock","grip","decals","muzzlebreak","shortscope","reflexsight","longbarrel","lazersight","suppressor","compensator","ironsight","iron_sight","sight","frame","magazine","handle","front_receiver","cover","rail","front","decal","mods_grip","mods_supressor"}
counts=collections.Counter()
for mat,srcs in sorted(matrefs.items()):
 key,slot=lookup(mat)
 if key is None: counts["unmatched"]+=1;continue
 color=slot.get("color",""); opacity=slot.get("opacity","")
 if not color: counts["flat"]+=1;continue
 family=mat[len("materials/weapons/"):].split("/",1)[0]
 parts=color.split("/"); texfam=parts[2] if len(parts)>2 and parts[1]=="weapons" else ""
 if key in watch:
  counts["generic_key_real_weapon_mat"]+=1
  if texfam and texfam not in (family,"awp_shareable","rocketlauncher"):
   counts["cross_family"]+=1
   print("CROSS",mat,"=>",key,"=>",color,"mask",opacity,"from",",".join(srcs))
print("counts",dict(counts))
for mat,srcs in sorted(matrefs.items()):
 key,slot=lookup(mat)
 if key in {"mods_grip","mods_supressor","decals","barrel","receiver","stock","muzzlebreak","lazersight","shortscope","reflexsight","longbarrel","suppressor","compensator"}:
  print("GENERIC",mat,"=>",key,slot,"from",",".join(srcs))
