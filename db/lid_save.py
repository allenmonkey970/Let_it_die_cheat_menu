"""LET IT DIE (Offline 5.0.1) save codec.
Format: "BRG\\0" + ver(u32 LE) + total_uncompressed(u32 LE) + "ZLIB"
        then chunks [usize u32][csize u32][zlib(csize bytes)] whose outputs concat into one UTF-8 JSON,
        then a 4-byte 0x00000000 terminator. No checksum.
"""
import json, os, shutil, struct, time, zlib

MAGIC = b"BRG\x00"

class SaveFormatError(ValueError):
    pass

def _check(cond, msg):
    # Real checks, not `assert`: they must also run under python -O.
    if not cond:
        raise SaveFormatError(msg)

def decode(data: bytes):
    _check(data[:4] == MAGIC, f"bad magic {data[:4]!r}")
    ver = struct.unpack_from("<I", data, 4)[0]
    total = struct.unpack_from("<I", data, 8)[0]
    _check(data[12:16] == b"ZLIB", f"missing ZLIB marker {data[12:16]!r}")
    off = 16
    out = bytearray()
    while off + 8 <= len(data):
        usize, csize = struct.unpack_from("<II", data, off)
        if csize == 0:
            break
        chunk = zlib.decompress(data[off + 8: off + 8 + csize])
        _check(len(chunk) == usize, f"chunk usize mismatch {len(chunk)}!={usize}")
        out += chunk
        off += 8 + csize
    _check(len(out) == total, f"total mismatch {len(out)}!={total}")
    return ver, bytes(out)

def encode(json_bytes: bytes, ver: int = 2, chunk_size: int = 37315) -> bytes:
    out = bytearray(MAGIC)
    out += struct.pack("<I", ver)
    out += struct.pack("<I", len(json_bytes))
    out += b"ZLIB"
    for i in range(0, len(json_bytes), chunk_size):
        chunk = json_bytes[i:i + chunk_size]
        comp = zlib.compress(chunk, 9)
        out += struct.pack("<II", len(chunk), len(comp))
        out += comp
    out += b"\x00\x00\x00\x00"  # terminator
    return bytes(out)

def load_json(path):
    with open(path, "rb") as f:
        ver, jb = decode(f.read())
    return ver, jb, json.loads(jb.decode("utf-8"))

def backup(path, tag):
    """Copy the save to <path>.<tag>-<timestamp> and return the backup path."""
    bak = f"{path}.{tag}-{time.strftime('%Y%m%d-%H%M%S')}"
    shutil.copy2(path, bak)
    return bak

def save_json(path, obj, ver):
    """Atomic write: encode to <path>.tmp, decode it back and compare, then replace the real save.
    The original is never truncated, so any failure leaves the previous save intact."""
    jb = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(encode(jb, ver))
        f.flush()
        os.fsync(f.fileno())            # data on disk before the rename makes it the live save
    _, _, check = load_json(tmp)
    if check != obj:
        os.remove(tmp)
        raise RuntimeError("verify failed: re-decoded save differs; original left untouched")
    os.replace(tmp, path)
