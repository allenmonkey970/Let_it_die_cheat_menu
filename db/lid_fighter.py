"""Fighter level/stat editor for LET IT DIE saves (game must be CLOSED).

Why a save edit and not live XP: the stat-up screen validates against the native XP pool
(FUN_14118e9c0 -> "body_lvl distribute failed! ... exp=, nec_exp=" + assert), so raising XP
from script desyncs and crashes. Stat levels live in bodyuser["1"][i] and level = 1 + points spent,
capped per stat by master_body_detail.param_lv_max for the fighter's (type, grade, limit_break).

  python lid_fighter.py list
  python lid_fighter.py max <name|cid-prefix|current|all>
"""
import glob, os, sqlite3, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lid_save import backup, load_json, save_json  # noqa: E402

DEFAULT_GAME = r"C:\Users\antho\Downloads\LET.IT.DIE-SteamRIP.com\LET.IT.DIE"
STATS = ["hp", "str", "dex", "vit", "stm", "luk"]


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


def max_fighters(j, db, sel):
    """Raise every stat of the selected fighters to the legal cap. Mutates j.
    Returns (changed, matched, messages): matched counts fighters found, including already-maxed ones."""
    changed, matched, msgs = 0, 0, []
    for b, c in fighters(j):
        if not matches(sel, b, c):
            continue
        name = c.get("name", b["cid"][:8])
        key = (c.get("type"), c.get("grade"), c.get("limit_break", 0))
        row = db.execute("select param_lv_max from master_body_detail where type=? and grade=? and limit_break=?",
                         key).fetchone()
        if not row:
            msgs.append(f"{name}: skipped (no stat cap for {key})")
            continue
        cap = row[0]
        matched += 1
        added = sum(max(0, cap - b[s]) for s in STATS)
        if not added:
            msgs.append(f"{name}: already maxed (level {b['lvl']})")
            continue
        old_lvl = b["lvl"]
        for s in STATS:
            b[s] = max(b[s], cap)
        b["lvl"] += added
        hp = db.execute("select hp from master_bodylvl_status_value where lvl=? and type=? and grade=? and limit_break=?",
                        (b["hp"],) + key).fetchone()
        if hp and c:
            c["hp"] = hp[0]
        msgs.append(f"{name}: level {old_lvl} -> {b['lvl']}, all stats {cap}, HP {c.get('hp')}")
        changed += 1
    if not msgs:
        msgs.append(f"no fighter matched '{sel}'")
    return changed, matched, msgs


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("list", "max"):
        print(__doc__); return 1
    save, dbp = game_paths(os.environ.get("LID_GAME", DEFAULT_GAME))
    ver, _, j = load_json(save)
    db = open_masters(dbp)
    if sys.argv[1] == "list":
        for b, c in fighters(j):
            print(f'{c.get("name","?"):12} cid={b["cid"][:8]} type={c.get("type")} grade={c.get("grade")} '
                  f'lvl={b["lvl"]} ' + " ".join(f"{s.upper()}={b[s]}" for s in STATS) + f' state={c.get("state")}')
        return 0
    if len(sys.argv) < 3:
        print("usage: lid_fighter.py max <name|cid-prefix|current|all>"); return 1
    if game_running():
        print("REFUSING: close LET IT DIE first (the game overwrites the save on exit)."); return 2
    changed, _, msgs = max_fighters(j, db, sys.argv[2])
    print("\n".join(msgs))
    if changed:
        bak = backup(save, "pre-fighter")
        save_json(save, j, ver)
        print(f"saved. backup: {bak}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
