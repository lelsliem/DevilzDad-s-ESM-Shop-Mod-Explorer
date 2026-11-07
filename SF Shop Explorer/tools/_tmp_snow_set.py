import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "renderer_recon"))
sys.path.insert(0, str(ROOT / "tools" / "ba2_audit"))

import aid_colour_audit as audit  # noqa: E402

cdb = audit.Cdb(audit.CDB)
for n in range(1, 7):
    mat = f"Materials\\SetDressing\\Landmarks\\Collectibles\\Snowglobe_Landmark_Backgrounds{n:02d}.mat"
    own, how = cdb.own_set(mat)
    print(f"{mat}\n   how={how} set={own}\n")
for name in ("Snowglobe_Landmark_Buildings", "Snowglobe_Landmark_Buildings02", "Snowglobe_Water",
             "Snowglobe_Landmarks_Glass"):
    mat = f"Materials\\SetDressing\\Landmarks\\Collectibles\\{name}.mat"
    own, how = cdb.own_set(mat)
    print(f"{mat}\n   how={how} set={own}\n")
