"""Applies save edits queued from the in-game overlay, after LET IT DIE has exited.

Started by dinput8.dll at launch (pythonw, no window):
  pythonw -I lid_apply_queue.py --pid <game pid> --mod <...\\Binaries\\Win64\\lid_mod>
It waits for that process to end (the game writes its save on exit), then applies lid_mod\\save_queue.txt:
  locker\\t<part id>\\t<label>        -> new part record (owner COIN_LOCKER) + the first empty soul.cl slot
  locker_item\\t<item id>\\t<label>   -> new item record (R&D material) + the first empty slot (type 3)
  fighter_max\\t<current|all>\\t<label> -> lid_fighter.max_fighters
The save is backed up first and written atomically. Results go to lid_mod\\save_queue_result.txt, which the
overlay shows on the next launch. Can also be run by hand with the game closed: --pid 0.
"""
import argparse, ctypes, os, sys, time, traceback, uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lid_fighter import game_paths, game_running, max_fighters, open_masters  # noqa: E402
from lid_save import backup, load_json, save_json  # noqa: E402

SYNCHRONIZE, INFINITE = 0x00100000, 0xFFFFFFFF


def wait_for_pid(pid):
    if pid <= 0:
        return
    k32 = ctypes.windll.kernel32
    k32.OpenProcess.restype = ctypes.c_void_p
    h = k32.OpenProcess(SYNCHRONIZE, False, pid)
    if not h:
        return  # already gone
    k32.WaitForSingleObject(ctypes.c_void_p(h), INFINITE)
    k32.CloseHandle(ctypes.c_void_p(h))


def read_queue(path):
    out = []
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                parts = line.rstrip("\r\n").split("\t")
                if len(parts) >= 2 and parts[0] and parts[1]:
                    out.append((parts[0], parts[1], parts[2] if len(parts) > 2 else parts[1]))
    return out


def take_lock(path):
    """Exclusive lock file for the apply phase (the next game launch waits while it exists).
    A lock older than 5 minutes is stale (helper killed) and is replaced."""
    for _ in range(2):
        try:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, str(os.getpid()).encode()); os.close(fd)
            return True
        except FileExistsError:
            if time.time() - os.path.getmtime(path) > 300:
                os.remove(path)
            else:
                return False
    return False


def take_locker_slot(j, label):
    """First free Coin Locker entry. The game sizes soul.cl itself (30 entries on a new save, 70 later), so the
    array length is the capacity. Returns (slot, None) or (None, (ok, retry_later, message))."""
    cl = j["soul"]["cl"]
    slot = next((c for c in cl if c.get("type", -1) == -1), None)
    if slot is None:
        return None, (False, True, f"{label}: Coin Locker is full ({len(cl)}/{len(cl)}) - free a slot and it will be retried")
    return slot, None


def deliver_to_locker(j, db, ptid, label, now):
    """Returns (ok, retry_later, message)."""
    row = db.execute("select dur, capacity, spare from master_part where id=?", (ptid,)).fetchone()
    if not row:
        return False, False, f"{label}: unknown part id {ptid}"
    slot, err = take_locker_slot(j, label)
    if err:
        return err
    dur, cap, spare = row
    eid = str(uuid.uuid4())
    # Same 11 fields, in the same order, as records the game creates itself.
    j["part"]["pts"].setdefault("1", []).append({
        "uid": 1, "eid": eid, "gettime": now, "owner": "COIN_LOCKER", "created": now, "modified": now,
        "ptid": ptid, "rest": cap or 0, "spare": spare or 0, "grade": 0, "dur": dur, "lvl": 1})
    slot["type"], slot["eid"] = 0, eid
    return True, False, f"{label}: delivered to Coin Locker slot {slot['slot'] + 1}"


def deliver_item_to_locker(j, db, itemid, label, now):
    """R&D materials: same shape as the game's own item records (eid, gettime, itemid, owner) + a type-3 slot.
    Returns (ok, retry_later, message)."""
    if not db.execute("select 1 from master_item where itemid=?", (itemid,)).fetchone():
        return False, False, f"{label}: unknown item id {itemid}"
    slot, err = take_locker_slot(j, label)
    if err:
        return err
    eid = str(uuid.uuid4())
    j.setdefault("item", {}).setdefault("items", []).append(
        {"eid": eid, "gettime": now, "itemid": itemid, "owner": "COIN_LOCKER"})
    slot["type"], slot["eid"] = 3, eid
    return True, False, f"{label}: delivered to Coin Locker slot {slot['slot'] + 1}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, default=0)
    ap.add_argument("--mod", required=True)
    a = ap.parse_args()
    mod = os.path.abspath(a.mod)
    game_root = os.path.dirname(os.path.dirname(os.path.dirname(mod)))   # <game>\Binaries\Win64\lid_mod
    qpath = os.path.join(mod, "save_queue.txt")
    rpath = os.path.join(mod, "save_queue_result.txt")
    log = open(os.path.join(mod, "save_helper.log"), "a", encoding="utf-8")

    def say(msg):
        log.write(time.strftime("[%Y-%m-%d %H:%M:%S] ") + msg + "\n"); log.flush()

    say(f"waiting for game pid {a.pid}")
    wait_for_pid(a.pid)
    lock = os.path.join(mod, "save_helper.lock")
    if not take_lock(lock):
        say("another helper is applying - nothing to do"); return 0
    try:
        return apply(qpath, rpath, game_root, say)
    finally:
        os.remove(lock)


def apply(qpath, rpath, game_root, say):
    time.sleep(3)                       # let the OS release the save file
    queue = read_queue(qpath)
    if not queue:
        say("nothing queued"); return 0
    if game_running():
        say("game is running again - leaving the queue for the next exit"); return 0

    retry, results = [], []
    try:
        save, dbp = game_paths(game_root)
        ver, _, j = load_json(save)
        db = open_masters(dbp)
        now, changed = int(time.time()), False
        for op, arg, label in queue:
            # One bad entry must not block the rest: each op is isolated, and a failure is reported and dropped.
            # (The deliver functions only mutate the save after all their checks pass.)
            try:
                if op in ("locker", "locker_item"):
                    deliver = deliver_to_locker if op == "locker" else deliver_item_to_locker
                    ok, again, msg = deliver(j, db, arg, label, now)
                    if again:
                        retry.append((op, arg, label))
                    changed |= ok
                elif op == "fighter_max":
                    n, matched, msgs = max_fighters(j, db, arg)
                    ok, msg = matched > 0, "; ".join(msgs)
                    changed |= n > 0
                else:
                    ok, msg = False, f"unknown request '{op}'"
            except Exception as e:
                say(f"error applying {op} {arg}:\n" + traceback.format_exc())
                ok, msg = False, f"{label}: could not be applied ({type(e).__name__}) - see save_helper.log"
            results.append(("OK   " if ok else "FAIL ") + msg)
        if changed:
            if game_running():          # re-check right before writing: a relaunch must never race our write
                say("game was relaunched while applying - nothing written, queue kept"); return 0
            bak = backup(save, "pre-overlay")
            save_json(save, j, ver)
            say(f"saved {save} (backup {bak})")
    except Exception:
        say("ERROR - save left untouched, queue kept:\n" + traceback.format_exc())
        with open(rpath, "w", encoding="utf-8") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M") + "\nFAIL save helper error - nothing was changed (see lid_mod\\save_helper.log)\n")
        return 1

    with open(rpath, "w", encoding="utf-8") as f:
        f.write(time.strftime("%Y-%m-%d %H:%M") + "\n" + "\n".join(results) + "\n")
    tmp = qpath + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        for op, arg, label in retry:
            f.write(f"{op}\t{arg}\t{label}\n")
    os.replace(tmp, qpath)
    say("applied: " + " | ".join(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
