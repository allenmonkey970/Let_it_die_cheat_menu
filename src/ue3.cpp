// Minimal UE3 bridge for LET IT DIE 5.0.1. All offsets come from Ghidra (see MODLOG.md).
//   ProcessEvent    = base + 0x10E770  void(UObject* this, UFunction* fn, void* parms, void* unused)
//   ProcessInternal = base + 0x10EC70  void(UObject* this, FFrame& stack, void* result) - runs EVERY script
//                     function body (CallFunction calls it directly for script->script calls).
//   FString::op=    = base + 0x019850  FString* (FString* dst, const FString* src)   (engine allocator)
//   GNames          = base + 0x2949C98 TArray<FNameEntry*>; entry name @+0x14, flags @+0x08 (bit0 = wide)
//   UObject         : Index @+0x38, Outer @+0x40, Name(FName idx,num) @+0x48, Class @+0x50
//   UField::Next @+0x60; UStruct::Children @+0x80; UProperty::Flags @+0x70, Offset @+0x94
//   FFrame          : Node(UFunction*) @+0x14, Object @+0x1C, Code @+0x24, Locals @+0x2C
//
// Script calls we make must run on the game thread: requests are queued and executed from inside the
// player's PlayerTick event (ProcessEvent hook), so they never run inside teardown/GC-adjacent events. The
// live GameInfo/UserData are reached from that PlayerController (WorldInfo.Game.mUserData), not by scanning.
//
// Cheats (skip a decrement-only script function when it acts for the local player):
//   durability: BrgCommonPawn_CustomChara.{Weapon,WeaponShot,Armor}DurabilityDown  (owner = this.mPawn)
//   ammo:       BrgWeapon_*.BulletConsumption (9 overrides, void, no parms)          (owner = this.mOwnerPawn)
//   stamina:    BrgPawn_CustomCharaPlayer.StaminaConsumption                         (owner = this)
// Repairs (PartDurabilityRecovery*), reloads, pickups and swaps are untouched.
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "MinHook.h"
#include "ue3.h"

namespace {
    constexpr uintptr_t RVA_PROCESSEVENT = 0x10E770;
    constexpr uintptr_t RVA_PROCESSINTERNAL = 0x10EC70;
    constexpr uintptr_t RVA_FSTRING_ASSIGN = 0x019850;
    constexpr uintptr_t RVA_GNAMES = 0x2949C98;
    constexpr int OFF_INDEX = 0x38, OFF_OUTER = 0x40, OFF_NAME = 0x48, OFF_CLASS = 0x50;
    constexpr int OFF_PROP_OFFSET = 0x94;
    constexpr int FR_NODE = 0x14;
    constexpr size_t kParmBytes = 512;    // ProcessEvent copies ParmsSize bytes in AND back out: never undersize
    constexpr int kMaxWorldResolves = 10;
    // Native save-store soul record (static object). The stat-distribute check (FUN_141366480 -> FUN_14133f7c0)
    // takes its Bloodnium pool from soul+0xB4 plus the current chara record +0x5C, and spends from those - not
    // from the BrgUserData object the UI reads. So a UI-side SetUserBloodnium alone crashes the level-up.
    constexpr uintptr_t RVA_NATIVE_SOUL = 0xF89EF80;
    constexpr int OFF_SOUL_BLOODNIUM = 0xB4;
    constexpr int OFF_SOUL_REPLICA_BLOODNIUM = 0xB8;   // -1, or a cap the spend path clamps to (>= the pool)
    constexpr DWORD kExpectedPeTimestamp = 0x6A94FD6E; // the 5.0.1 build these offsets were taken from

    bool IsExpectedBuild() {
        uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
        auto dos = (IMAGE_DOS_HEADER*)base;
        auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        return nt->FileHeader.TimeDateStamp == kExpectedPeTimestamp;
    }
    constexpr DWORD kNoTickFallbackMs = 10000;

    using PE_t = void(__fastcall*)(void*, void*, void*, void*);
    using PI_t = void(__fastcall*)(void*, void*, void*);
    struct FString { const wchar_t* Data; int Num; int Max; };
    using FStrAssign_t = FString*(__fastcall*)(FString*, const FString*);
    struct TArrayRaw { void** Data; int Num; int Max; };

    PE_t oProcessEvent = nullptr;
    PI_t oProcessInternal = nullptr;
    FStrAssign_t FStringAssign = nullptr;
    uintptr_t gBase = 0;
    TArrayRaw* gObjects = nullptr;
    DWORD gInstallTick = 0;

    // ---- requests (game thread) ----
    enum Kind { PART_EQUIP, ADD_CURRENCY };
    struct Request { Kind kind; std::wstring a, b; int n; int amount; };
    std::mutex gMu;
    std::deque<Request> gQueue;
    std::atomic<bool> gHaveWork{ false };       // queue non-empty or resolve pending: checked before any lock
    thread_local bool tInPump = false;

    std::mutex gStatusMu;
    std::string gStatus = "UE3 bridge: not installed";
    void SetStatus(const char* fmt, ...) {
        char b[512];
        va_list ap; va_start(ap, fmt); _vsnprintf_s(b, sizeof b, _TRUNCATE, fmt, ap); va_end(ap);
        std::lock_guard<std::mutex> lk(gStatusMu);
        gStatus = b;
    }

    // ---- resolved targets ----
    struct Fn { uintptr_t ptr; int cheat; };
    Fn gCheatFns[32];
    int gCheatFnCount = 0;
    uintptr_t gFnPartEquip = 0;

    // ---- currencies: BrgUserData natives (they write the game's own user-data store, so changes are saved) ----
    // Each add = Get current -> clamp(current + amount, 0, limit) -> Set. Carried Kill Coins/SPLithium use the
    // per-fighter Add*/Get* natives with cindex = -1 (current fighter).
    struct CurrencySpec {
        const char* label;
        const char* getFn;       // int Get...([optional int cindex])
        const char* setFn;       // bool Set...(int value [, optional int cindex])   (or Add... when isAdd)
        const char* limitFn;     // int Get...Limit()   (nullptr: fixed cap below)
        int fixedCap;
        bool perFighter;         // takes optional cindex (= -1)
        bool isAdd;              // setFn adds a delta instead of setting a value
    };
    const CurrencySpec kCurrencies[ue3::CUR_COUNT] = {
        { "Kill Coins",     "GetMoney",         "AddMoney",         nullptr,              0,        true,  true  },
        { "SPLithium",      "GetSpirit",        "AddSpirit",        nullptr,              0,        true,  true  },
        { "Bank funds",     "GetSafeMoney",     "SetSafeMoney",     "GetSafeLimit",       0,        false, false },
        { "SPLithium Tank", "GetSafeSpirit",    "SetSafeSpirit",    "GetSpiritTankLimit", 0,        false, false },
        { "Death Metals",   "GetMedal",         "SetMedal",         nullptr,              99999999, false, false },
        { "Bloodnium",      "GetUserBloodnium", "SetUserBloodnium", "GetBloodniumLimit",  999999,   false, false },
        { "TDM Points",     "GetTdmPoint",      "SetTdmPoint",      nullptr,              99999999, false, false },
    };
    struct CurrencyFns { uintptr_t get, set, limit; };
    CurrencyFns gCurFns[ue3::CUR_COUNT] = {};
    int gCurNameIds[ue3::CUR_COUNT][3];
    const char* CurFnName(int c, int f) {
        const CurrencySpec& cs = kCurrencies[c];
        return f == 0 ? cs.getFn : f == 1 ? cs.setFn : cs.limitFn;
    }
    int gOffCommonPawnPawn = -1;       // BrgCommonPawn_CustomChara.mPawn
    int gOffWeaponOwnerPawn = -1;      // BrgWeapon_Base.mOwnerPawn
    int gOffWorldInfo = -1;            // Actor.WorldInfo
    int gOffGame = -1;                 // WorldInfo.Game
    int gOffUserData = -1;             // BrgGameInfo.mUserData
    int gOffDeadCancel = -1;           // BrgGameInfo.mbDeadInfoOutputCancel (bitfield: whole DWORD restored)
    int gNamePlayerTick = -1;
    std::atomic<bool> gResolved{ false };      // every cheat target found
    std::atomic<bool> gScanned{ false };       // at least one full pass done (request functions usable)
    std::atomic<bool> gGaveUp{ false };        // targets missing after kMaxWorldResolves in-world passes
    std::atomic<bool> gTickSeen{ false };      // PlayerTick observed through ProcessEvent
    DWORD gNextResolveTick = 0;
    std::atomic<uintptr_t> gPlayerClass{ 0 };

    // ---- cheats ----
    std::atomic<int> gCheatMask{ 0 };
    std::atomic<int> gBlocked[3]{};    // dur, ammo, stamina
    std::atomic<bool> gFaulted{ false };

    // ---- SEH-safe reads ----
    bool ReadPtr(uintptr_t a, uintptr_t* out) { __try { *out = *(uintptr_t*)a; return true; } __except (1) { return false; } }
    bool ReadI32(uintptr_t a, int* out)       { __try { *out = *(int*)a; return true; } __except (1) { return false; } }
    bool WriteI32(uintptr_t a, int v)         { __try { *(int*)a = v; return true; } __except (1) { return false; } }

    void EnsureBase() { if (!gBase) gBase = (uintptr_t)GetModuleHandleW(nullptr); }
    TArrayRaw* GNames() { EnsureBase(); return (TArrayRaw*)(gBase + RVA_GNAMES); }

    bool NameOf(int idx, char* buf, size_t cap) {
        TArrayRaw* gn = GNames();
        if (idx < 0 || idx >= gn->Num) return false;
        uintptr_t e = 0;
        if (!ReadPtr((uintptr_t)gn->Data + (uintptr_t)idx * 8, &e) || !e) return false;
        int flags = 0; if (!ReadI32(e + 8, &flags)) return false;
        __try {
            if (flags & 1) { const wchar_t* w = (const wchar_t*)(e + 0x14); size_t i = 0; for (; i + 1 < cap && w[i]; ++i) buf[i] = (char)w[i]; buf[i] = 0; }
            else           { const char* s = (const char*)(e + 0x14);    size_t i = 0; for (; i + 1 < cap && s[i]; ++i) buf[i] = s[i];       buf[i] = 0; }
            return true;
        } __except (1) { return false; }
    }
    bool ObjName(uintptr_t obj, char* buf, size_t cap) {
        int idx = 0; if (!obj || !ReadI32(obj + OFF_NAME, &idx)) return false;
        return NameOf(idx, buf, cap);
    }
    bool ClassName(uintptr_t obj, char* buf, size_t cap) {
        uintptr_t c = 0; if (!ReadPtr(obj + OFF_CLASS, &c) || !c) return false;
        return ObjName(c, buf, cap);
    }
    bool OuterName(uintptr_t obj, char* buf, size_t cap) {
        uintptr_t o = 0; if (!ReadPtr(obj + OFF_OUTER, &o) || !o) return false;
        return ObjName(o, buf, cap);
    }

    TArrayRaw* FindGObjects() {
        EnsureBase();
        auto dos = (IMAGE_DOS_HEADER*)gBase;
        auto nt = (IMAGE_NT_HEADERS64*)(gBase + dos->e_lfanew);
        auto sec = IMAGE_FIRST_SECTION(nt);
        for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            uintptr_t lo = gBase + sec->VirtualAddress, hi = lo + sec->Misc.VirtualSize;
            for (uintptr_t a = lo; a + 16 <= hi; a += 8) {
                if (a == gBase + RVA_GNAMES) continue;
                uintptr_t data = 0; int num = 0, max = 0;
                if (!ReadPtr(a, &data) || !ReadI32(a + 8, &num) || !ReadI32(a + 12, &max)) continue;
                if (!data || num < 20000 || num > 5000000 || max < num || max > 8000000) continue;
                int good = 0, checked = 0;
                for (int i = 0; i < 64 && checked < 24; ++i) {
                    uintptr_t o = 0; if (!ReadPtr(data + (uintptr_t)i * 8, &o)) { good = -1000; break; }
                    if (!o) continue;
                    ++checked;
                    int idx = -1; if (ReadI32(o + OFF_INDEX, &idx) && idx == i) ++good;
                }
                if (checked >= 10 && good == checked) return (TArrayRaw*)a;
            }
        }
        return nullptr;
    }
    bool EnsureObjects() { if (!gObjects) gObjects = FindGObjects(); return gObjects != nullptr; }

    // FName indices of the names we care about (one GNames pass instead of decoding every object's name).
    enum NameId { N_WeaponDur, N_WeaponShotDur, N_ArmorDur, N_BulletCons, N_StaminaCons, N_DebugPartEquip,
                  N_mPawn, N_mOwnerPawn, N_WorldInfo, N_Game, N_mUserData,
                  N_mbDeadInfoOutputCancel, N_PlayerTick, N_COUNT };
    const char* kNames[N_COUNT] = { "WeaponDurabilityDown", "WeaponShotDurabilityDown", "ArmorDurabilityDown",
                                    "BulletConsumption", "StaminaConsumption", "DebugPartEquip",
                                    "mPawn", "mOwnerPawn", "WorldInfo", "Game", "mUserData",
                                    "mbDeadInfoOutputCancel", "PlayerTick" };
    int gNameIds[N_COUNT] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

    // Property offsets: exact declaring class preferred; a same-family parent class is accepted as a fallback
    // (an inherited property keeps the parent's offset).
    struct PropSpec { NameId id; const char* outer; const char* family; int* out; };
    const PropSpec kProps[] = {
        { N_mPawn,      "BrgCommonPawn_CustomChara", "BrgCommonPawn", &gOffCommonPawnPawn },
        { N_mOwnerPawn, "BrgWeapon_Base",            "BrgWeapon",     &gOffWeaponOwnerPawn },
        { N_WorldInfo,  "Actor",                     nullptr,         &gOffWorldInfo },
        { N_Game,       "WorldInfo",                 nullptr,         &gOffGame },
        { N_mUserData,  "BrgGameInfo",               "BrgGameInfo",   &gOffUserData },
        { N_mbDeadInfoOutputCancel, "BrgGameInfo",   "BrgGameInfo",   &gOffDeadCancel },
    };

    // Single pass over GNames + GObjects on the game thread. In-world passes are capped (kMaxWorldResolves)
    // so a target that does not exist in this build cannot cause periodic hitches for the whole session.
    void ResolveAll(bool inWorld) {
        static int worldAttempts = 0;
        static bool any = false;
        DWORD now = GetTickCount();
        if (any && (int)(now - gNextResolveTick) < 0) return;
        any = true;
        gNextResolveTick = now + 3000;
        if (inWorld && ++worldAttempts > kMaxWorldResolves) {
            gGaveUp = true;
            SetStatus("cheats unavailable: some game functions were not found (see Settings > Diagnostics)");
            return;
        }
        if (!EnsureObjects()) { SetStatus("cheats: GObjects not found yet (retrying)"); return; }

        static bool curInit = false;
        if (!curInit) { for (auto& row : gCurNameIds) for (int& v : row) v = -1; curInit = true; }
        bool missing = false;
        for (int k = 0; k < N_COUNT; ++k) missing |= gNameIds[k] < 0;
        for (int c = 0; c < ue3::CUR_COUNT; ++c)
            for (int f = 0; f < 3; ++f) missing |= CurFnName(c, f) && gCurNameIds[c][f] < 0;
        if (missing) {
            TArrayRaw* gn = GNames();
            char nb[64];
            for (int i = 0; i < gn->Num; ++i) {
                if (!NameOf(i, nb, sizeof nb)) continue;
                for (int k = 0; k < N_COUNT; ++k) if (gNameIds[k] < 0 && !strcmp(nb, kNames[k])) gNameIds[k] = i;
                for (int c = 0; c < ue3::CUR_COUNT; ++c)
                    for (int f = 0; f < 3; ++f)
                        if (CurFnName(c, f) && gCurNameIds[c][f] < 0 && !strcmp(nb, CurFnName(c, f))) gCurNameIds[c][f] = i;
            }
        }
        CurrencyFns cur[ue3::CUR_COUNT] = {};
        gNamePlayerTick = gNameIds[N_PlayerTick];

        Fn fns[32]; int nf = 0;
        uintptr_t partEquip = 0;
        int offs[6] = { -1, -1, -1, -1, -1, -1 };
        bool exact[6] = {};
        char cls[96], outer[96];
        for (int i = 0; i < gObjects->Num; ++i) {
            uintptr_t obj = 0;
            if (!ReadPtr((uintptr_t)gObjects->Data + (uintptr_t)i * 8, &obj) || !obj) continue;
            int nidx = -1; if (!ReadI32(obj + OFF_NAME, &nidx) || nidx < 0) continue;
            bool curHit = false;
            for (int c = 0; c < ue3::CUR_COUNT && !curHit; ++c)
                for (int f = 0; f < 3; ++f) curHit |= gCurNameIds[c][f] == nidx;
            if (curHit && ClassName(obj, cls, sizeof cls) && !strcmp(cls, "Function") &&
                OuterName(obj, outer, sizeof outer) && !strcmp(outer, "BrgUserData")) {
                for (int c = 0; c < ue3::CUR_COUNT; ++c) {
                    if (gCurNameIds[c][0] == nidx) cur[c].get = obj;
                    if (gCurNameIds[c][1] == nidx) cur[c].set = obj;
                    if (gCurNameIds[c][2] == nidx) cur[c].limit = obj;
                }
                continue;
            }
            int k = 0; for (; k < N_COUNT; ++k) if (gNameIds[k] == nidx) break;
            if (k == N_COUNT || k == N_PlayerTick) continue;
            if (!ClassName(obj, cls, sizeof cls) || !OuterName(obj, outer, sizeof outer)) continue;
            bool isFn = !strcmp(cls, "Function");
            if (!isFn && strstr(cls, "Property")) {
                for (int p = 0; p < 6; ++p) {
                    const PropSpec& ps = kProps[p];
                    if (ps.id != k) continue;
                    int off = -1; ReadI32(obj + OFF_PROP_OFFSET, &off);
                    if (!strcmp(outer, ps.outer)) { offs[p] = off; exact[p] = true; }
                    else if (!exact[p] && ps.family && !strncmp(outer, ps.family, strlen(ps.family))) offs[p] = off;
                }
                continue;
            }
            if (!isFn) continue;
            int cheat = 0;
            if ((k == N_WeaponDur || k == N_WeaponShotDur || k == N_ArmorDur) && !strcmp(outer, "BrgCommonPawn_CustomChara")) cheat = ue3::DURABILITY;
            else if (k == N_BulletCons && !strncmp(outer, "BrgWeapon_", 10)) cheat = ue3::AMMO;
            else if (k == N_StaminaCons && !strcmp(outer, "BrgPawn_CustomCharaPlayer")) cheat = ue3::STAMINA;
            else if (k == N_DebugPartEquip && !strcmp(outer, "BrgGameInfo")) partEquip = obj;
            if (cheat && nf < 32) fns[nf++] = { obj, cheat };
        }
        int dur = 0, ammo = 0, stam = 0;
        for (int i = 0; i < nf; ++i) { dur += fns[i].cheat == ue3::DURABILITY; ammo += fns[i].cheat == ue3::AMMO; stam += fns[i].cheat == ue3::STAMINA; }

        // Publish: function table first, count last (the ProcessInternal hook reads count then entries).
        for (int i = 0; i < nf; ++i) gCheatFns[i] = fns[i];
        gCheatFnCount = nf;
        gFnPartEquip = partEquip;
        for (int c = 0; c < ue3::CUR_COUNT; ++c) gCurFns[c] = cur[c];
        for (int p = 0; p < 6; ++p) *kProps[p].out = offs[p];
        gScanned = true;
        bool ok = dur == 3 && ammo >= 1 && stam == 1 && gOffCommonPawnPawn > 0 && gOffWeaponOwnerPawn > 0;
        if (ok) gResolved = true;
        SetStatus("cheat hooks %s: durability %d/3, ammo %d guns, stamina %d/1",
                  ok ? "armed" : "incomplete (retrying)", dur, ammo, stam);
    }

    bool IsPlayerPawn(uintptr_t p) {
        uintptr_t cls = 0;
        if (!p || !ReadPtr(p + OFF_CLASS, &cls) || !cls) return false;
        if (cls == gPlayerClass) return true;
        char c[96];
        if (ObjName(cls, c, sizeof c) && !strcmp(c, "BrgPawn_CustomCharaPlayer")) { gPlayerClass = cls; return true; }
        return false;
    }

    bool ActsForPlayer(int cheat, uintptr_t obj) {
        if (cheat == ue3::STAMINA) return IsPlayerPawn(obj);
        int off = cheat == ue3::DURABILITY ? gOffCommonPawnPawn : gOffWeaponOwnerPawn;
        uintptr_t owner = 0;
        return off > 0 && ReadPtr(obj + off, &owner) && IsPlayerPawn(owner);
    }

    void __fastcall hkProcessInternal(void* obj, void* stack, void* result) {
        int mask = gCheatMask.load(std::memory_order_relaxed);
        if (mask && !gFaulted.load(std::memory_order_relaxed)) {
            uintptr_t node = *(uintptr_t*)((uintptr_t)stack + FR_NODE);
            int n = gCheatFnCount;
            for (int i = 0; i < n; ++i) {
                if (gCheatFns[i].ptr != node) continue;
                int cheat = gCheatFns[i].cheat;
                if ((mask & cheat) && ActsForPlayer(cheat, (uintptr_t)obj)) {
                    ++gBlocked[cheat == ue3::DURABILITY ? 0 : cheat == ue3::AMMO ? 1 : 2];
                    return;                       // decrement-only void function: skipping it is a no-op
                }
                break;
            }
        }
        oProcessInternal(obj, stack, result);
    }

    // Fallback only (used if this build never routes PlayerTick through ProcessEvent).
    uintptr_t FindInstance(const char* prefix) {
        if (!EnsureObjects()) return 0;
        char n[128], c[128];
        size_t plen = strlen(prefix);
        uintptr_t bestExact = 0, bestAny = 0;
        for (int i = 0; i < gObjects->Num; ++i) {
            uintptr_t obj = 0; if (!ReadPtr((uintptr_t)gObjects->Data + (uintptr_t)i * 8, &obj) || !obj) continue;
            if (!ClassName(obj, c, sizeof c) || strncmp(c, prefix, plen)) continue;
            if (!ObjName(obj, n, sizeof n) || !strncmp(n, "Default__", 9)) continue;
            if (!strcmp(c, prefix)) bestExact = obj; else bestAny = obj;
        }
        return bestExact ? bestExact : bestAny;
    }

    bool ClassStartsWith(uintptr_t obj, const char* prefix) {
        char c[96];
        return obj && ClassName(obj, c, sizeof c) && !strncmp(c, prefix, strlen(prefix));
    }

    // PlayerController -> WorldInfo -> Game: the GameInfo of the world that is ticking right now.
    uintptr_t LiveGameInfo(uintptr_t pc) {
        uintptr_t wi = 0, gi = 0;
        if (pc && gOffWorldInfo > 0 && gOffGame > 0 && ReadPtr(pc + gOffWorldInfo, &wi) && wi &&
            ReadPtr(wi + gOffGame, &gi) && ClassStartsWith(gi, "BrgGameInfo"))
            return gi;
        return pc ? 0 : FindInstance("BrgGameInfo");
    }
    uintptr_t LiveUserData(uintptr_t pc) {
        uintptr_t gi = LiveGameInfo(pc), ud = 0;
        if (gi && gOffUserData > 0 && ReadPtr(gi + gOffUserData, &ud) && ClassStartsWith(ud, "BrgUserData")) return ud;
        return pc ? 0 : FindInstance("BrgUserData");
    }

    // Engine faults inside our calls are caught so the game survives, but script-VM state may be left
    // unbalanced, so everything is latched off and the user is told to save and restart.
    bool CallPE(uintptr_t obj, uintptr_t fn, void* parms) {
        __try { oProcessEvent((void*)obj, (void*)fn, parms, nullptr); return true; }
        __except (1) {
            gFaulted = true;
            gCheatMask = 0;
            return false;
        }
    }

    // Parm strings must be engine-owned: script may reassign them (DebugPartEquip does `inDbPartId = ""`),
    // which frees/reallocs through GMalloc. A few bytes leak per call; that is the price of never freeing
    // memory the engine might already have replaced.
    void EngineFStr(FString* dst, const std::wstring& s) {
        FString tmp{ s.c_str(), (int)s.size() + 1, (int)s.size() + 1 };
        FStringAssign(dst, &tmp);
    }

    void RunPartEquip(Request& r, uintptr_t pc) {
        uintptr_t gi = LiveGameInfo(pc);
        if (!gFnPartEquip || !gi) { SetStatus("try-on failed: %s", gFnPartEquip ? "game world not ready" : "DebugPartEquip not found"); return; }
        alignas(16) unsigned char parms[kParmBytes] = {};
        FString* p = (FString*)parms;
        EngineFStr(&p[0], r.a);
        EngineFStr(&p[1], r.b);
        // DebugPartEquip sets the debug flag mbDeadInfoOutputCancel (alters death bookkeeping): put it back.
        int saved = 0;
        bool haveFlag = gOffDeadCancel > 0 && ReadI32(gi + gOffDeadCancel, &saved);
        bool ok = CallPE(gi, gFnPartEquip, parms);
        if (haveFlag) WriteI32(gi + gOffDeadCancel, saved);
        SetStatus(ok ? "trying on %ls (%ls) - temporary, not saved" : "try-on FAULTED for %ls - cheats paused, save & restart",
                  r.b.c_str(), r.a.c_str());
    }

    // int Get...([cindex]): the return value follows the optional cindex when present.
    bool CallGetInt(uintptr_t ud, uintptr_t fn, bool perFighter, int* out) {
        alignas(16) unsigned char parms[kParmBytes] = {};
        if (perFighter) *(int*)parms = -1;
        if (!CallPE(ud, fn, parms)) return false;
        *out = *(int*)(parms + (perFighter ? 4 : 0));
        return true;
    }

    void RunAddCurrency(const Request& r, uintptr_t pc) {
        if (r.n < 0 || r.n >= ue3::CUR_COUNT) return;
        const CurrencySpec& cs = kCurrencies[r.n];
        const CurrencyFns& fn = gCurFns[r.n];
        int amount = r.amount;
        uintptr_t ud = LiveUserData(pc);
        if (!fn.get || !fn.set || !ud) {
            SetStatus("%s failed: %s", cs.label, ud ? "game function not found" : "game world not ready");
            return;
        }
        int before = 0, limit = cs.fixedCap;
        if (!CallGetInt(ud, fn.get, cs.perFighter, &before)) { SetStatus("%s FAULTED - cheats paused, save & restart", cs.label); return; }
        if (fn.limit) {
            int l = 0;
            if (!CallGetInt(ud, fn.limit, false, &l)) { SetStatus("%s FAULTED - cheats paused, save & restart", cs.label); return; }
            if (l > 0) limit = l;
        }
        if (amount <= 0) { SetStatus("%s: nothing to add", cs.label); return; }
        if (limit > 0 && before >= limit) {     // never lower a balance that is already at/over the limit
            SetStatus("%s is already at the game limit (%d / %d)", cs.label, before, limit);
            return;
        }
        // Bloodnium: the native soul field must hold exactly what the game just reported, which proves the offset
        // before we write it.
        int nativeBefore = INT_MIN;
        bool nativeOk = false;
        if (r.n == ue3::CUR_BLOODNIUM) {
            // Proof that soul+0xB4 is the pool: right exe build, value equal to what the game reports, and the
            // neighbouring replica cap is -1 or >= the pool (a random zeroed global fails at least one of these).
            int replica = 0;
            nativeOk = IsExpectedBuild() &&
                       ReadI32(gBase + RVA_NATIVE_SOUL + OFF_SOUL_BLOODNIUM, &nativeBefore) && nativeBefore == before &&
                       ReadI32(gBase + RVA_NATIVE_SOUL + OFF_SOUL_REPLICA_BLOODNIUM, &replica) &&
                       (replica == -1 || replica >= nativeBefore);
            if (!nativeOk) {            // change nothing: a UI-only raise is exactly what crashes the level-up
                SetStatus("Bloodnium NOT added: native record check failed (game=%d, native=%d) - please report",
                          before, nativeBefore);
                return;
            }
        }
        long long want = (long long)before + amount;
        if (limit > 0 && want > limit) want = limit;
        if (want < 0) want = 0;
        alignas(16) unsigned char parms[kParmBytes] = {};
        *(int*)(parms + 0) = cs.isAdd ? (int)(want - before) : (int)want;
        if (cs.perFighter) *(int*)(parms + 4) = -1;
        if (!CallPE(ud, fn.set, parms)) { SetStatus("%s FAULTED - cheats paused, save & restart", cs.label); return; }
        int after = before;
        if (!CallGetInt(ud, fn.get, cs.perFighter, &after)) { SetStatus("%s FAULTED - cheats paused, save & restart", cs.label); return; }
        if (r.n == ue3::CUR_BLOODNIUM) {        // verified above: mirror into the native store, or undo the UI raise
            if (after != (int)want || !WriteI32(gBase + RVA_NATIVE_SOUL + OFF_SOUL_BLOODNIUM, after)) {
                alignas(16) unsigned char undo[kParmBytes] = {};
                *(int*)undo = before;
                CallPE(ud, fn.set, undo);
                SetStatus("Bloodnium NOT added: could not update the native record (reverted)");
                return;
            }
        }
        if (limit > 0) SetStatus("%s: %d -> %d (game limit %d)", cs.label, before, after, limit);
        else           SetStatus("%s: %d -> %d", cs.label, before, after);
    }

    bool IsPlayerTick(void* fn) {
        int idx = -1;
        return gNamePlayerTick >= 0 && ReadI32((uintptr_t)fn + OFF_NAME, &idx) && idx == gNamePlayerTick;
    }

    void __fastcall hkProcessEvent(void* obj, void* fn, void* parms, void* res) {
        if (gHaveWork.load(std::memory_order_relaxed) && !tInPump) {
            bool tick = IsPlayerTick(fn);
            if (tick) gTickSeen = true;
            // Requests only inside PlayerTick; if this build never fires it, fall back to any event after 10 s.
            bool fallback = !gTickSeen && GetTickCount() - gInstallTick > kNoTickFallbackMs;
            bool resolveOk = gNamePlayerTick < 0 || tick || fallback;      // read-only scans: early is fine
            if (resolveOk) {
                tInPump = true;
                if (!gResolved && !gGaveUp) ResolveAll(tick || fallback);
                if ((tick || fallback) && gScanned && !gFaulted) {
                    Request req; bool have = false;
                    {
                        std::lock_guard<std::mutex> lk(gMu);
                        if (!gQueue.empty()) { req = gQueue.front(); gQueue.pop_front(); have = true; }
                    }
                    uintptr_t pc = tick ? (uintptr_t)obj : 0;
                    if (have) { if (req.kind == PART_EQUIP) RunPartEquip(req, pc); else RunAddCurrency(req, pc); }
                }
                {
                    std::lock_guard<std::mutex> lk(gMu);
                    gHaveWork = !gQueue.empty() || (!gResolved && !gGaveUp);
                }
                tInPump = false;
            }
        }
        oProcessEvent(obj, fn, parms, res);
    }

    void Enqueue(Request r, const char* what) {
        if (gFaulted) { SetStatus("refused %s: an engine call faulted earlier - save & restart the game", what); return; }
        std::lock_guard<std::mutex> lk(gMu);
        gQueue.push_back(std::move(r));
        gHaveWork = true;
        SetStatus("queued %s (runs on the next game tick)", what);
    }
}

namespace ue3 {
    bool Install() {
        EnsureBase();
        gInstallTick = GetTickCount();
        FStringAssign = (FStrAssign_t)(gBase + RVA_FSTRING_ASSIGN);
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { SetStatus("UE3 bridge: MH_Initialize failed"); return false; }
        void* pe = (void*)(gBase + RVA_PROCESSEVENT);
        void* pi = (void*)(gBase + RVA_PROCESSINTERNAL);
        if (MH_CreateHook(pe, &hkProcessEvent, (void**)&oProcessEvent) != MH_OK ||
            MH_CreateHook(pi, &hkProcessInternal, (void**)&oProcessInternal) != MH_OK ||
            MH_EnableHook(pe) != MH_OK || MH_EnableHook(pi) != MH_OK) {
            SetStatus("UE3 bridge: hook install failed"); return false;
        }
        gHaveWork = true;            // resolve targets on the first player tick
        SetStatus("UE3 bridge: ready (waiting for the game world)");
        return true;
    }

    bool QueuePartEquip(const std::wstring& site, const std::wstring& ptid) {
        // DebugPartEquip: "All" wipes the inventory and unknown names silently fall through to Head (site 0),
        // so only the five real sites are accepted; "NONE"/empty ids would hand the bag a part with no id.
        static const wchar_t* kSites[] = { L"Head", L"Arm_R", L"Arm_L", L"Body", L"Legs" };
        bool siteOk = false;
        for (auto s : kSites) siteOk |= site == s;
        if (!siteOk || ptid.size() < 4 || ptid.compare(0, 3, L"PT_") != 0 || ptid.find(L"NONE") != std::wstring::npos) {
            SetStatus("try-on refused: bad slot or part id");
            return false;
        }
        Enqueue({ PART_EQUIP, site, ptid, 0, 0 }, "try-on");
        return true;
    }
    void QueueAddCurrency(Currency c, int amount) {
        if (c < 0 || c >= CUR_COUNT) return;
        Enqueue({ ADD_CURRENCY, L"", L"", (int)c, amount }, kCurrencies[c].label);
    }
    const char* CurrencyName(Currency c) { return (c >= 0 && c < CUR_COUNT) ? kCurrencies[c].label : "?"; }
    static_assert(sizeof(kCurrencies) / sizeof(kCurrencies[0]) == CUR_COUNT, "currency table out of sync");
    bool CurrencyAvailable(Currency c) { return c >= 0 && c < CUR_COUNT && gCurFns[c].get && gCurFns[c].set; }

    void SetCheat(Cheat c, bool on) {
        if (on && gFaulted) return;
        if (on) gCheatMask |= c; else gCheatMask &= ~c;
    }
    bool GetCheat(Cheat c) { return (gCheatMask & c) != 0; }
    bool Faulted() { return gFaulted; }
    bool Ready() { return gResolved; }
    bool Scanned() { return gScanned; }
    bool Unavailable() { return gGaveUp; }
    std::string CheatStats() {
        char b[200];
        _snprintf_s(b, sizeof b, _TRUNCATE, "Blocked so far: %d durability, %d shots, %d stamina",
                    gBlocked[0].load(), gBlocked[1].load(), gBlocked[2].load());
        return b;
    }

    std::string Status() { std::lock_guard<std::mutex> lk(gStatusMu); return gStatus; }
    std::string Diag() {
        char b[512];
        _snprintf_s(b, sizeof b, _TRUNCATE,
            "resolved=%d gaveUp=%d faulted=%d tickSeen=%d cheatFns=%d | offsets pawn=%X owner=%X worldinfo=%X game=%X "
            "userdata=%X deadcancel=%X | GObjects=%s GNames=%d",
            (int)gResolved.load(), (int)gGaveUp.load(), (int)gFaulted.load(), (int)gTickSeen.load(), gCheatFnCount,
            gOffCommonPawnPawn, gOffWeaponOwnerPawn, gOffWorldInfo, gOffGame, gOffUserData, gOffDeadCancel,
            gObjects ? "ok" : "not yet", GNames()->Num);
        return b;
    }
}
