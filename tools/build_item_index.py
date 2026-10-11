"""Build the item-icon atlas + item index for the LET IT DIE overlay item picker.

Run:  C:\\Python314\\python.exe -I tools\\build_item_index.py [--game DIR] [--icons DIR] [--out DIR]

Inputs (read-only):
  <game>\\BrgGame\\Content\\masters.db  (opened with ?mode=ro)
  <icons>\\<ICON_ID>.png               raw icons exported from BrgGame\\CookedPCConsole\\UI_Icon_<ICON_ID>_SF.upk
                                       by `uedump icons` (C:\\lid-ghidra\\uedump, see MODLOG "Item icons")
Outputs:
  <out>\\icons_atlas.png  RGBA, 64x64 cells, 40 columns, cell i at (i%40*64, i//40*64)
  <out>\\items.tsv        ptid name slot req_stat req_value icon rarity sort
  <out>\\README.txt

Icon resolution (first hit wins), see README/MODLOG for rationale:
  exact     <ptid>.png
  alias     hand-picked alias (Fists -> right-hand male fist texture of the multi-texture package)
  strip_G   "<id>_G" -> "<id>" (the _G parts use the identical 3D asset as their base part)
  asset     another part with the identical master_part_asset set that has an icon
  series    majority master_part.series among parts sharing the identical asset set
  grade     weapons only (PT_ARM_WPnnn_xyz): last digit g -> PT_ARM_WPnnn_00g, else lower _00k, else _001
            (approximation: used for collab variants whose own icon is not shipped in this PC build)
  none      icon = -1
"""
import argparse
import collections
import os
import re
import sqlite3
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # -I hides the user site-packages, where Pillow is installed on this machine
    import site
    sys.path.append(site.getusersitepackages())
    from PIL import Image

DEFAULT_GAME = r"C:\Users\antho\Downloads\LET.IT.DIE-SteamRIP.com\LET.IT.DIE"
DEFAULT_ICONS = r"C:\lid-tools\icons_raw"
CELL, COLS = 64, 40
SLOT_ORDER = ["Arm", "Head", "Body", "Legs", "Material"]
ALIASES = {
    # Fists: the package UI_Icon_PT_ARM_WP000_001_R_SF holds 3 textures (288x144 arm, _F, _M fists).
    "PT_ARM_WP000_001": "PT_ARM_WP000_001_R__TX_UI_Image_PT_ARM_WP000_001_R_M",
}


def slot_of(ptid):
    if ptid.startswith("PT_ARM_"):
        return "Arm"
    for tok, slot in (("_HEAD_", "Head"), ("_TOPS_", "Body"), ("_BTM_", "Legs")):
        if tok in ptid:
            return slot
    return None


def clean(s):
    return re.sub(r"[\t\r\n]+", " ", s).strip()


def main():
    sys.stdout.reconfigure(errors="replace")  # -I ignores PYTHONIOENCODING; console may be cp1252
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    here = Path(__file__).resolve().parent
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--icons", default=DEFAULT_ICONS)
    ap.add_argument("--out", default=str(here.parent / "assets"))
    args = ap.parse_args()

    db_path = Path(args.game) / "BrgGame" / "Content" / "masters.db"
    icons_dir = Path(args.icons)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    con = sqlite3.connect("file:" + db_path.as_posix() + "?mode=ro", uri=True)
    text = {(s, i): t for s, i, t in con.execute("SELECT sct, id, txt FROM master_text WHERE lang='int'")}
    parts = {}
    for pid, nm, series, lt, lv, rar, sort_no in con.execute(
            "SELECT id, name, series, lvllmttp, lvllmt, rarity, sort_no FROM master_part"):
        sct, _, tid = (nm or "").partition(".")
        parts[pid] = dict(name=text.get((sct, tid)), series=series or "", req_stat=lt or "",
                          req_value=lv if lv is not None else 0, rarity=rar, sort=sort_no)
    # R&D materials (Iron Scraps, Veneer Plank...): master_item ITTP_MATERIAL, icon UI_Icon_<itemid>_SF.upk
    materials = []
    for iid, nm, rar, sort_no in con.execute(
            "SELECT itemid, name, rarity, sort_no FROM master_item WHERE itemtype='ITTP_MATERIAL'"):
        sct, _, tid = (nm or "").partition(".")
        materials.append(dict(ptid=iid, name=text.get((sct, tid)), rarity=rar, sort=sort_no))
    assets = collections.defaultdict(set)
    for pid, tp, a in con.execute("SELECT id, tp, asset FROM master_part_asset"):
        assets[pid].add((tp, a))
    con.close()
    by_assetset = collections.defaultdict(list)
    for pid, s in assets.items():
        by_assetset[frozenset(s)].append(pid)

    icon_files = {p.stem for p in icons_dir.glob("*.png")}
    if not icon_files:
        sys.exit(f"no PNGs in {icons_dir} - run `uedump icons` first")

    def direct(pid):
        """exact / alias / strip_G (no cross-part lookups)."""
        if pid in icon_files:
            return pid, "exact"
        if pid in ALIASES and ALIASES[pid] in icon_files:
            return ALIASES[pid], "alias"
        if pid.endswith("_G") and pid[:-2] in icon_files:
            return pid[:-2], "strip_G"
        return None, None

    def resolve(pid):
        icon, how = direct(pid)
        if icon:
            return icon, how
        group = by_assetset.get(frozenset(assets.get(pid, ())), []) if assets.get(pid) else []
        pfx = pid.removesuffix("_G").rsplit("_", 1)[0]
        cands = sorted({direct(q)[0] for q in group if q != pid and direct(q)[0]},
                       key=lambda c: (c.rsplit("_", 1)[0] == pfx, c), reverse=True)
        if cands:
            return cands[0], "asset"
        series = collections.Counter(parts[q]["series"] for q in (group or [pid])
                                     if q in parts and parts[q]["series"] and parts[q]["series"] != q)
        for s, _ in series.most_common():
            icon, _h = direct(s)
            if icon:
                return icon, "series"
        m = re.match(r"^(PT_ARM_WP\d{3})_\w*?(\d)$", pid.removesuffix("_G"))
        if m:
            base, g = m.group(1), int(m.group(2))
            for k in list(range(g, 0, -1)) + [1]:
                cand = f"{base}_00{k}"
                if cand in icon_files:
                    return cand, "grade"
        return None, "none"

    rows = []
    for pid, p in parts.items():
        slot = slot_of(pid)
        name = clean(p["name"] or "")
        if slot is None or not name or name.lower() == "none" or pid.startswith("PT_NONE_"):
            continue
        icon, how = resolve(pid)
        rows.append(dict(ptid=pid, name=name, slot=slot, req_stat=p["req_stat"], req_value=p["req_value"],
                         icon_id=icon, how=how, rarity=p["rarity"], sort=p["sort"]))
    for m in materials:
        name = clean(m["name"] or "")
        if name:
            rows.append(dict(ptid=m["ptid"], name=name, slot="Material", req_stat="", req_value=0,
                             icon_id=m["ptid"] if m["ptid"] in icon_files else None,
                             how="exact" if m["ptid"] in icon_files else "none", rarity=m["rarity"], sort=m["sort"]))
    rows.sort(key=lambda r: (SLOT_ORDER.index(r["slot"]), r["sort"], r["ptid"]))

    # atlas: one cell per distinct icon, in order of first use
    cell_of = {}
    for r in rows:
        if r["icon_id"] and r["icon_id"] not in cell_of:
            cell_of[r["icon_id"]] = len(cell_of)
    n = len(cell_of)
    nrows = max(1, (n + COLS - 1) // COLS)
    W, H = COLS * CELL, nrows * CELL
    if H > 4096:
        sys.exit(f"atlas would be {W}x{H} (> 4096)")
    atlas = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    for icon_id, i in cell_of.items():
        im = Image.open(icons_dir / f"{icon_id}.png").convert("RGBA")
        s = min(CELL / im.width, CELL / im.height)
        w, h = max(1, round(im.width * s)), max(1, round(im.height * s))
        # resample in premultiplied alpha so transparent edges don't bleed dark fringes
        small = im.convert("RGBa").resize((w, h), Image.Resampling.LANCZOS).convert("RGBA")
        atlas.paste(small, ((i % COLS) * CELL + (CELL - w) // 2, (i // COLS) * CELL + (CELL - h) // 2))
    atlas.save(out_dir / "icons_atlas.png", optimize=True)

    with open(out_dir / "items.tsv", "w", encoding="utf-8", newline="\n") as f:
        f.write("ptid\tname\tslot\treq_stat\treq_value\ticon\trarity\tsort\n")
        for r in rows:
            icon = cell_of[r["icon_id"]] if r["icon_id"] else -1
            f.write(f"{r['ptid']}\t{r['name']}\t{r['slot']}\t{r['req_stat']}\t{r['req_value']}\t"
                    f"{icon}\t{r['rarity']}\t{r['sort']}\n")

    how = collections.Counter(r["how"] for r in rows)
    with_icon = sum(1 for r in rows if r["icon_id"])
    per_slot = collections.Counter(r["slot"] for r in rows)
    readme = f"""Generated by tools\\build_item_index.py (do not hand-edit) from masters.db (master_part, master_text lang=int, master_part_asset)
+ raw icons exported from BrgGame\\CookedPCConsole\\UI_Icon_PT_*_SF.upk and UI_Icon_ITMT_*_SF.upk (R&D materials, master_item ITTP_MATERIAL) by `uedump icons` (run once per pattern) (C:\\lid-ghidra\\uedump) into {icons_dir}.
items.tsv: {len(rows)} rows ({', '.join(f'{s} {per_slot[s]}' for s in SLOT_ORDER)}), {with_icon} with an icon, {len(rows) - with_icon} with icon=-1.
icons_atlas.png: {W}x{H}, {n} cells of {CELL}x{CELL} (icons are 108x132, fitted and centered), {COLS} columns, cell i at (i%{COLS}*{CELL}, i/{COLS}*{CELL}).
Icon fallback (first hit wins): exact {how['exact']}, alias {how['alias']} (Fists), strip "_G" {how['strip_G']}, same 3D asset (master_part_asset) {how['asset']},
  asset-group majority master_part.series {how['series']} (approx: WoT collab reskins), weapon last digit -> PT_ARM_WPnnn_00g else lower/_001
  {how['grade']} (approx: Demon Gun/GLIDER/KAMAS-WoT collab models), none {how['none']} (Kat/Tank Commander collab armor, Landmine, Sand).
  The game itself has no fallback: it loads Texture2D'UI_Icon_<id>.TX_UI_Image_<id>' for the exact ptid; those collab icons are not in this PC build.
Names are UTF-8 and include U+2019 (right single quote); add it to the ImGui font glyph ranges or it renders as '?'.
These are extracted game assets (GungHo/Grasshopper). Personal use only: never redistribute or commit them.
"""
    (out_dir / "README.txt").write_text(readme, encoding="utf-8")
    print(readme)
    for r in rows:
        if r["how"] in ("grade", "series", "none"):
            print(f"  {r['how']:6s} {r['ptid']:22s} -> {r['icon_id'] or '-'}  ({r['name']})")


if __name__ == "__main__":
    main()
