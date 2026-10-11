// In-process memory scanner for the LET IT DIE overlay.
// Control channel = plain-text files the agent reads/writes:
//   command : C:\Users\antho\lid-overlay-mod\rpc\cmd.txt
//   result  : C:\Users\antho\lid-overlay-mod\rpc\result.txt
// Each command has an incrementing id= line; the DLL executes once per new id and
// echoes that id into result.txt when done. Heavy scans run on this worker thread,
// never the render thread. Frozen addresses are rewritten every loop.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>

#include "scanner.h"
#include "settings.h"
#include "ue3.h"

std::string Overlay_InputDiag();   // dllmain.cpp

static std::wstring ToWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static const char* kDir    = "C:\\Users\\antho\\lid-overlay-mod\\rpc";
static const char* kCmd    = "C:\\Users\\antho\\lid-overlay-mod\\rpc\\cmd.txt";
static const char* kResult = "C:\\Users\\antho\\lid-overlay-mod\\rpc\\result.txt";

enum ValType { T_I32, T_F32 };

static std::vector<uintptr_t> g_addrs;     // current candidate addresses
static std::vector<uint32_t>  g_last;      // raw 4-byte value at last scan (parallel)
static ValType                g_type = T_I32;
static long long              g_lastId = -1;
static char                   g_status[256] = "scanner idle";

struct Freeze { uintptr_t addr; uint32_t raw; };
static std::vector<Freeze>    g_freezes;
static CRITICAL_SECTION       g_cs;

int  Scanner_FreezeCount()  { return (int)g_freezes.size(); }
const char* Scanner_LastStatus() { return g_status; }

// ---- safe in-process read/write (SEH, no C++ unwinding inside) ----
static bool SafeReadDword(uintptr_t a, uint32_t* out) {
    __try { *out = *reinterpret_cast<volatile uint32_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SafeWriteDword(uintptr_t a, uint32_t v) {
    __try { *reinterpret_cast<volatile uint32_t*>(a) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool RegionScannable(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    DWORD p = mbi.Protect & 0xFF;
    // writable protections only (gameplay values live in writable memory)
    return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

// Scan one committed region's bytes for a 4-byte pattern. SEH-guarded; appends hits.
static void ScanRegion(uint8_t* base, size_t size, uint32_t needle, std::vector<uintptr_t>& out, size_t cap) {
    __try {
        for (size_t off = 0; off + 4 <= size; off += 4) {
            if (*reinterpret_cast<uint32_t*>(base + off) == needle) {
                out.push_back(reinterpret_cast<uintptr_t>(base + off));
                if (out.size() >= cap) return;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* skip faulting region */ }
}

static uint32_t EncodeValue(ValType t, double v) {
    if (t == T_F32) { float f = (float)v; uint32_t r; memcpy(&r, &f, 4); return r; }
    int32_t i = (int32_t)v; uint32_t r; memcpy(&r, &i, 4); return r;
}
static std::string DecodeValue(ValType t, uint32_t raw) {
    char b[64];
    if (t == T_F32) { float f; memcpy(&f, &raw, 4); sprintf_s(b, "%.3f", f); }
    else { int32_t i; memcpy(&i, &raw, 4); sprintf_s(b, "%d", i); }
    return b;
}

static void NewScan(uint32_t needle) {
    g_addrs.clear(); g_last.clear();
    const size_t CAP = 4000000;
    SYSTEM_INFO si; GetSystemInfo(&si);
    uint8_t* addr = (uint8_t*)si.lpMinimumApplicationAddress;
    uint8_t* maxA = (uint8_t*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < maxA && g_addrs.size() < CAP) {
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) break;
        if (RegionScannable(mbi)) {
            ScanRegion((uint8_t*)mbi.BaseAddress, mbi.RegionSize, needle, g_addrs, CAP);
        }
        addr = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
    }
    g_last.assign(g_addrs.size(), needle);
}

// Filter existing candidates. mode: 0=eq(value) 1=dec 2=inc 3=changed 4=unchanged
static void FilterScan(int mode, uint32_t needle) {
    std::vector<uintptr_t> na; std::vector<uint32_t> nl;
    na.reserve(g_addrs.size());
    for (size_t i = 0; i < g_addrs.size(); ++i) {
        uint32_t cur;
        if (!SafeReadDword(g_addrs[i], &cur)) continue;
        bool keep = false;
        if (mode == 0) keep = (cur == needle);
        else {
            uint32_t prev = g_last[i];
            if (g_type == T_F32) {
                float c, p; memcpy(&c, &cur, 4); memcpy(&p, &prev, 4);
                if (mode == 1) keep = c < p; else if (mode == 2) keep = c > p;
                else if (mode == 3) keep = c != p; else keep = c == p;
            } else {
                int32_t c, p; memcpy(&c, &cur, 4); memcpy(&p, &prev, 4);
                if (mode == 1) keep = c < p; else if (mode == 2) keep = c > p;
                else if (mode == 3) keep = c != p; else keep = c == p;
            }
        }
        if (keep) { na.push_back(g_addrs[i]); nl.push_back(cur); }
    }
    g_addrs.swap(na); g_last.swap(nl);
}

// ---- tiny key=value parser ----
static std::string GetVal(const std::string& s, const char* key) {
    std::string k = std::string("\n") + key + "=";
    std::string hay = "\n" + s;
    size_t p = hay.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    size_t e = hay.find_first_of("\r\n", p);
    return hay.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

static std::string ReadFileStr(const char* path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string out; char buf[4096]; DWORD n;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
    CloseHandle(h);
    return out;
}
static void WriteResult(long long id, const std::string& body) {
    std::string tmp = std::string(kResult) + ".tmp";
    HANDLE h = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char hdr[64]; int n = sprintf_s(hdr, "id=%lld\n", id);
    DWORD w; WriteFile(h, hdr, n, &w, nullptr);
    WriteFile(h, body.c_str(), (DWORD)body.size(), &w, nullptr);
    CloseHandle(h);
    MoveFileExA(tmp.c_str(), kResult, MOVEFILE_REPLACE_EXISTING);
}

static void HandleCommand(const std::string& cmd) {
    long long id = _atoi64(GetVal(cmd, "id").c_str());
    if (id == g_lastId) return;            // already handled
    std::string op = GetVal(cmd, "op");
    std::string tp = GetVal(cmd, "type");
    if (tp == "f32") g_type = T_F32; else if (tp == "i32") g_type = T_I32;
    std::string vs = GetVal(cmd, "value");
    double val = atof(vs.c_str());
    std::string body;

    if (op == "scan_new") {
        NewScan(EncodeValue(g_type, val));
        char b[128]; sprintf_s(b, "status=ok\nop=scan_new\ntype=%s\nvalue=%s\ncount=%zu\n",
                               g_type==T_F32?"f32":"i32", vs.c_str(), g_addrs.size());
        body = b;
    } else if (op == "scan_eq") {
        FilterScan(0, EncodeValue(g_type, val));
        char b[128]; sprintf_s(b, "status=ok\nop=scan_eq\nvalue=%s\ncount=%zu\n", vs.c_str(), g_addrs.size());
        body = b;
    } else if (op == "scan_dec" || op == "scan_inc" || op == "scan_changed" || op == "scan_unchanged") {
        int mode = op=="scan_dec"?1 : op=="scan_inc"?2 : op=="scan_changed"?3 : 4;
        FilterScan(mode, 0);
        char b[128]; sprintf_s(b, "status=ok\nop=%s\ncount=%zu\n", op.c_str(), g_addrs.size());
        body = b;
    } else if (op == "list") {
        char b[128]; sprintf_s(b, "status=ok\nop=list\ncount=%zu\n", g_addrs.size());
        body = b;
        size_t lim = g_addrs.size() < 60 ? g_addrs.size() : 60;
        for (size_t i = 0; i < lim; ++i) {
            uint32_t cur; std::string v = SafeReadDword(g_addrs[i], &cur) ? DecodeValue(g_type, cur) : "?";
            char line[96]; sprintf_s(line, "addr=%llX val=%s\n", (unsigned long long)g_addrs[i], v.c_str());
            body += line;
        }
    } else if (op == "read") {
        uintptr_t a = (uintptr_t)_strtoui64(GetVal(cmd, "addr").c_str(), nullptr, 16);
        uint32_t cur; bool ok = SafeReadDword(a, &cur);
        char b[160];
        if (ok) { int32_t i; float f; memcpy(&i,&cur,4); memcpy(&f,&cur,4);
            sprintf_s(b, "status=ok\nop=read\naddr=%llX\ni32=%d\nf32=%.3f\n", (unsigned long long)a, i, f); }
        else sprintf_s(b, "status=err\nop=read\naddr=%llX\nmsg=unreadable\n", (unsigned long long)a);
        body = b;
    } else if (op == "freeze") {
        uintptr_t a = (uintptr_t)_strtoui64(GetVal(cmd, "addr").c_str(), nullptr, 16);
        uint32_t raw = EncodeValue(g_type, val);
        EnterCriticalSection(&g_cs); g_freezes.push_back({a, raw}); LeaveCriticalSection(&g_cs);
        char b[128]; sprintf_s(b, "status=ok\nop=freeze\naddr=%llX\nvalue=%s\nfrozen=%d\n",
                               (unsigned long long)a, vs.c_str(), (int)g_freezes.size());
        body = b;
    } else if (op == "set") {
        uintptr_t a = (uintptr_t)_strtoui64(GetVal(cmd, "addr").c_str(), nullptr, 16);
        bool ok = SafeWriteDword(a, EncodeValue(g_type, val));
        char b[128]; sprintf_s(b, "status=%s\nop=set\naddr=%llX\nvalue=%s\n", ok?"ok":"err",
                               (unsigned long long)a, vs.c_str());
        body = b;
    } else if (op == "ue3_diag") {
        body = "status=ok\nop=ue3_diag\n" + ue3::Diag() + "\nbridge=" + ue3::Status() + "\n";
    } else if (op == "vslot") {
        std::string cls = GetVal(cmd, "cls"); int off = (int)strtol(GetVal(cmd, "off").c_str(), nullptr, 16);
        body = "status=ok\nop=vslot\n" + ue3::VSlot(cls.c_str(), off) + "\n";
    } else if (op == "input_diag") {
        body = "status=ok\nop=input_diag\n" + Overlay_InputDiag() + "\n";
    } else if (op == "ue3_status") {
        body = "status=ok\nop=ue3_status\nbridge=" + ue3::Status() + "\ncheats=" + ue3::CheatStats() + "\n";
    } else if (op == "inf_dur" || op == "inf_ammo" || op == "inf_stamina") {
        ue3::Cheat c = op == "inf_dur" ? ue3::DURABILITY : op == "inf_ammo" ? ue3::AMMO : ue3::STAMINA;
        std::string v = GetVal(cmd, "value");
        if (v == "1" || v == "on" || v == "true")       settings::SetCheat(c, true);
        else if (v == "0" || v == "off" || v == "false") settings::SetCheat(c, false);
        else if (!v.empty()) { WriteResult(id, "status=err\nmsg=value must be on|off|1|0 (empty = query)\n"); g_lastId = id; return; }
        body = "status=ok\nop=" + op + "\non=" + (ue3::GetCheat(c) ? "1" : "0") + "\n";
    } else if (op == "spawn_part") {
        std::string site = GetVal(cmd, "site"), ptid = GetVal(cmd, "ptid");
        if (site.empty()) site = "Arm_R";
        bool ok = ue3::QueuePartEquip(ToWide(site), ToWide(ptid));
        body = std::string(ok ? "status=ok" : "status=err") + "\nop=spawn_part\nqueued=" + ptid + " site=" + site +
               "\nbridge=" + ue3::Status() + "\n";
    } else if (op == "add_money" || op == "add_spirit") {
        int amt = atoi(GetVal(cmd, "value").c_str());
        if (op == "add_money") ue3::QueueAddMoney(amt); else ue3::QueueAddSpirit(amt);
        body = "status=ok\nop=" + op + "\nqueued=" + std::to_string(amt) + "\n";
    } else if (op == "unfreeze_all") {
        EnterCriticalSection(&g_cs); g_freezes.clear(); LeaveCriticalSection(&g_cs);
        body = "status=ok\nop=unfreeze_all\nfrozen=0\n";
    } else {
        body = "status=err\nmsg=unknown op\n";
    }

    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE, "%s -> %zu hits, %d frozen",
                op.c_str(), g_addrs.size(), (int)g_freezes.size());
    g_lastId = id;
    WriteResult(id, body);
}

static DWORD WINAPI ScannerThread(LPVOID) {
    InitializeCriticalSection(&g_cs);
    CreateDirectoryA(kDir, nullptr);
    {   // ignore any command left over from a previous session (never replay stale RPC at startup)
        std::string stale = ReadFileStr(kCmd);
        if (!stale.empty()) g_lastId = _atoi64(GetVal(stale, "id").c_str());
    }
    int pollCtr = 0;
    for (;;) {
        // apply freezes every loop (~16ms) so values stay pinned
        EnterCriticalSection(&g_cs);
        for (auto& f : g_freezes) SafeWriteDword(f.addr, f.raw);
        LeaveCriticalSection(&g_cs);

        // check for a new command roughly every ~150ms
        if (++pollCtr >= 10) {
            pollCtr = 0;
            std::string cmd = ReadFileStr(kCmd);
            if (!cmd.empty()) HandleCommand(cmd);
        }
        Sleep(15);
    }
}

void Scanner_Start() {
    CreateThread(nullptr, 0, ScannerThread, nullptr, 0, nullptr);
}
