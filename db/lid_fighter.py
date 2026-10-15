"""Fighter level/stat editor for LET IT DIE saves (game must be CLOSED).

Why a save edit and not live XP: the stat-up screen validates against the native XP pool
(FUN_14118e9c0 -> "body_lvl distribute failed! ... exp=, nec_exp=" + assert), so raising XP
from script desyncs and crashes.

What a fighter's ceiling actually is (verified against master data and two in-game maxima):
  level = 1 + sum(stat - 1 for the 6 stats) + skill + bag + rage      (9 distributable fields)
The game's validator (FUN_141362da0, fed by FUN_1411a1ab0) derives the caps from the fighter's
GRADE alone - it scans every limit_break band of that grade (it passes limit_break = -1), so:
  stat cap          = max(param_lv_max) over the grade's bands
  skill/bag/rage cap = max(slots/bag/rage over the bands) - the limit_break 0 value
Raising the grade and maxing the stats are separate actions, because grade is normally something
you buy: "grade" raises the ceiling, "max" fills the fighter up to the ceiling of its current grade.

  python lid_fighter.py list
  python lid_fighter.py grade <name|cid-prefix|current|all> <1-6|top>
  python lid_fighter.py max   <name|cid-prefix|current|all>
"""
import glob
import os
import sqlite3
import subprocess
import sys
from typing import NamedTuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lid_save import backup, load_json, save_json  # noqa: E402

STATS = ["hp", "str", "dex", "vit", "stm", "luk"]
POINTS = ["skill", "bag", "rage"]          # distributable too, and they count toward the level


def game_paths(game_root):
    """(save path, masters.db path). The live save is the newest *.sav (backups have extra suffixes)."""
    saves = sorted(glob.glob(os.path.join(game_root, "Savedata", "*.sav")), key=os.path.getmtime)
    if not saves:
        raise FileNotFoundError(f"no .sav in {game_root}\\Savedata")
    return saves[-1], os.path.join(game_root, "BrgGame", "Content", "masters.db")


def open_masters(db_path):
    return sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)


def game_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq BrgGame-Steam.exe"], capture_output=True, text=True,
                         creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout
    return "BrgGame-Steam.exe" in out


class Ceiling(NamedTuple):
    """The highest values the game will accept for a fighter of this (type, grade)."""
    stat_max: int
    skill_max: int
    bag_max: int
    rage_max: int
    limit_break: int        # the band that grants stat_max

    def hp_at(self, db, type_, grade, stat_lvl):
        """Max HP for an HP stat level. A grade's bands cover disjoint level ranges, so this is unique."""
        row = db.execute("select hp from master_bodylvl_status_value where type=? and grade=? and lvl=?",
                         (type_, grade, stat_lvl)).fetchone()
        return row[0] if row else None


def _slots(csv):
    return len([x for x in (csv or "").split(",") if x.strip()])


def ceiling(db, type_, grade):
    """Mirror of the game's own cap lookup: scan every limit_break band of this grade."""
    rows = db.execute("select limit_break, param_lv_max, skill_slots, bag_capacity, rage_capacity "
                      "from master_body_detail where type=? and grade=? order by limit_break",
                      (type_, grade)).fetchall()
    if not rows:
        return None
    base = rows[0]                                      # limit_break 0 = before any limit break
    return Ceiling(max(r[1] for r in rows),
                   max(_slots(r[2]) for r in rows) - _slots(base[2]),
                   max(r[3] for r in rows) - base[3],
                   max(r[4] for r in rows) - base[4],
                   max(rows, key=lambda r: r[1])[0])


def level_for(cap):
    return 1 + len(STATS) * (cap.stat_max - 1) + cap.skill_max + cap.bag_max + cap.rage_max


def top_grade(db, type_):
    row = db.execute("select max(grade) from master_body_detail where type=?", (type_,)).fetchone()
    return row[0] if row and row[0] else 1


def fighters(j):
    chrs = {c["cid"]: c for c in j["soul"]["chr"]["chrs"]["1"]}
    for b in j["bodyuser"]["1"]:
        yield b, chrs.get(b["cid"], {})


# Not player fighters: e.g. "labo_buddy" (state SOCKPUPPET) is the game's own puppet record.
NON_FIGHTER_STATES = {"SOCKPUPPET"}


def matches(sel, b, c):
    if c.get("state") in NON_FIGHTER_STATES:
        return False
    if sel == "all":
        return True
    if sel == "current":
        return c.get("state") == "USE"
    return b["cid"].startswith(sel) or c.get("name", "").lower() == sel.lower()


def band_for(db, type_, grade, stat_lvl):
    """The limit_break band of this grade whose level range contains stat_lvl (bands are disjoint)."""
    row = db.execute("select limit_break from master_bodylvl_status_value where type=? and grade=? and lvl=?",
                     (type_, grade, stat_lvl)).fetchone()
    return row[0] if row else None


def _retune(db, b, c, type_, grade):
    """After a grade or stat change: put the fighter in the right limit_break band and fix max HP.
    The band is never lowered: a band grants skill slots and bag capacity, and the band matching the
    current stats is usually lower right after a grade raise, which would take those away."""
    band = band_for(db, type_, grade, max(b[s] for s in STATS))
    if band is not None:
        c["limit_break"] = max(c.get("limit_break", 0), band)
    hp = Ceiling(0, 0, 0, 0, 0).hp_at(db, type_, grade, b["hp"])
    if hp:
        c["hp"] = hp


def set_grade(j, db, sel, target):
    """Raise the selected fighters' grade (never lowers it). Mutates j.
    Grade only raises the ceiling - it adds no stats; run max afterwards to fill up to it.
    Returns (changed, matched, messages)."""
    changed, matched, msgs = 0, 0, []
    for b, c in fighters(j):
        if not matches(sel, b, c):
            continue
        name = c.get("name", b["cid"][:8])
        type_, grade = c.get("type"), c.get("grade", 1)
        top = top_grade(db, type_)
        try:
            want = top if target == "top" else min(int(target), top)
        except (TypeError, ValueError):
            msgs.append(f"{name}: skipped (grade must be a number 1-{top} or 'top', got {target!r})")
            continue
        matched += 1
        if want <= grade:
            msgs.append(f"{name}: already grade {grade}" + (f" (top for {type_})" if grade >= top else ""))
            continue
        cap = ceiling(db, type_, want)
        if cap is None:
            msgs.append(f"{name}: skipped (no data for {type_} grade {want})")
            continue
        c["grade"] = want
        _retune(db, b, c, type_, want)
        msgs.append(f"{name}: grade {grade} -> {want}, stat cap {cap.stat_max} (level up to {level_for(cap)}), "
                    f"HP {c.get('hp')}")
        changed += 1
    if not msgs:
        msgs.append(f"no fighter matched '{sel}'")
    return changed, matched, msgs


def max_fighters(j, db, sel):
    """Fill the selected fighters up to the ceiling of their CURRENT grade. Mutates j.
    Use set_grade first to raise that ceiling.
    Returns (changed, matched, messages): matched counts fighters found, already-maxed included."""
    changed, matched, msgs = 0, 0, []
    for b, c in fighters(j):
        if not matches(sel, b, c):
            continue
        name = c.get("name", b["cid"][:8])
        type_, grade = c.get("type"), c.get("grade", 1)
        cap = ceiling(db, type_, grade)
        if cap is None:
            msgs.append(f"{name}: skipped (no cap data for {type_} grade {grade})")
            continue
        matched += 1

        want = {s: max(b[s], cap.stat_max) for s in STATS}
        want.update({k: max(b.get(k, 0), getattr(cap, k + "_max")) for k in POINTS if k in b})
        if all(b[k] == v for k, v in want.items()):
            top = top_grade(db, type_)
            more = f" - raise the grade past {grade} for more" if grade < top else ""
            msgs.append(f"{name}: already maxed for grade {grade} (level {b['lvl']}){more}")
            continue

        old_lvl = b["lvl"]
        b.update(want)
        b["lvl"] = 1 + sum(b[s] - 1 for s in STATS) + sum(b.get(k, 0) for k in POINTS)
        _retune(db, b, c, type_, grade)
        msgs.append(f"{name}: level {old_lvl} -> {b['lvl']} (grade {grade}), stats {cap.stat_max}, "
                    f"skill +{cap.skill_max} bag +{cap.bag_max} rage +{cap.rage_max}, HP {c.get('hp')}")
        changed += 1
    if not msgs:
        msgs.append(f"no fighter matched '{sel}'")
    return changed, matched, msgs


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("list", "max", "grade"):
        print(__doc__); return 1
    game = os.environ.get("LID_GAME")
    if not game:
        print("set LID_GAME to your LET IT DIE folder"); return 1
    save, dbp = game_paths(game)
    ver, _, j = load_json(save)
    db = open_masters(dbp)
    if sys.argv[1] == "list":
        for b, c in fighters(j):
            cap = ceiling(db, c.get("type"), c.get("grade", 1))
            top = str(level_for(cap)) if cap else "?"
            print(f'{c.get("name","?"):12} cid={b["cid"][:8]} type={c.get("type")} grade={c.get("grade")} '
                  f'lvl={b["lvl"]}/{top} ' + " ".join(f"{s.upper()}={b[s]}" for s in STATS)
                  + f' state={c.get("state")}')
        return 0
    if len(sys.argv) < 3 or (sys.argv[1] == "grade" and len(sys.argv) < 4):
        print("usage: lid_fighter.py max <sel>  |  lid_fighter.py grade <sel> <1-6|top>"); return 1
    if game_running():
        print("REFUSING: close LET IT DIE first (the game overwrites the save on exit)."); return 2
    if sys.argv[1] == "grade":
        changed, _, msgs = set_grade(j, db, sys.argv[2], sys.argv[3])
    else:
        changed, _, msgs = max_fighters(j, db, sys.argv[2])
    print("\n".join(msgs))
    if changed:
        bak = backup(save, "pre-fighter")
        save_json(save, j, ver)
        print(f"saved. backup: {bak}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
