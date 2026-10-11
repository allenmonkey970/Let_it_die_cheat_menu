// File-based control channel for scripting/diagnostics (see rpc.sh):
//   command : lid_mod\rpc\cmd.txt     (key=value lines; an incrementing id= line)
//   result  : lid_mod\rpc\result.txt  (echoes the id when done)
// The DLL executes each new id once. A command left over from a previous session is never replayed.

#include <windows.h>
#include <string>

#include "rpc.h"
#include "settings.h"
#include "ue3.h"

std::string Overlay_InputDiag();   // dllmain.cpp

namespace {
    long long g_lastId = -1;

    std::wstring CmdPath()    { return settings::ModDir() + L"\\rpc\\cmd.txt"; }
    std::wstring ResultPath() { return settings::ModDir() + L"\\rpc\\result.txt"; }

    std::wstring ToWide(const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
        return w;
    }

    std::string GetVal(const std::string& s, const char* key) {
        std::string k = std::string("\n") + key + "=";
        std::string hay = "\n" + s;
        size_t p = hay.find(k);
        if (p == std::string::npos) return "";
        p += k.size();
        size_t e = hay.find_first_of("\r\n", p);
        return hay.substr(p, e == std::string::npos ? std::string::npos : e - p);
    }

    std::string ReadFileStr(const std::wstring& path) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return "";
        std::string out; char buf[4096]; DWORD n;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
        CloseHandle(h);
        return out;
    }

    void WriteResult(long long id, const std::string& body) {
        std::wstring tmp = ResultPath() + L".tmp";
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        std::string all = "id=" + std::to_string(id) + "\n" + body;
        DWORD w; WriteFile(h, all.c_str(), (DWORD)all.size(), &w, nullptr);
        CloseHandle(h);
        MoveFileExW(tmp.c_str(), ResultPath().c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    bool IsNum(const std::string& v) {
        return !v.empty() && v.size() <= 9 && v.find_first_not_of("0123456789") == std::string::npos;
    }

    std::string Handle(const std::string& cmd) {
        std::string op = GetVal(cmd, "op");
        if (op == "ue3_diag")   return "status=ok\nop=ue3_diag\n" + ue3::Diag() + "\nbridge=" + ue3::Status() + "\n";
        if (op == "ue3_status") return "status=ok\nop=ue3_status\nbridge=" + ue3::Status() + "\ncheats=" + ue3::CheatStats() + "\n";
        if (op == "input_diag") return "status=ok\nop=input_diag\n" + Overlay_InputDiag() + "\n";

        if (op == "inf_dur" || op == "inf_ammo" || op == "inf_stamina") {
            ue3::Cheat c = op == "inf_dur" ? ue3::DURABILITY : op == "inf_ammo" ? ue3::AMMO : ue3::STAMINA;
            std::string v = GetVal(cmd, "value");
            if (v == "1" || v == "on" || v == "true")       settings::SetCheat(c, true);
            else if (v == "0" || v == "off" || v == "false") settings::SetCheat(c, false);
            else if (!v.empty()) return "status=err\nmsg=value must be on|off|1|0 (empty = query)\n";
            return "status=ok\nop=" + op + "\non=" + (ue3::GetCheat(c) ? "1" : "0") + "\n";
        }
        if (op == "spawn_part") {
            std::string site = GetVal(cmd, "site"), ptid = GetVal(cmd, "ptid");
            if (site.empty()) site = "Arm_R";
            bool ok = ue3::QueuePartEquip(ToWide(site), ToWide(ptid));
            return std::string(ok ? "status=ok" : "status=err") + "\nop=spawn_part\nqueued=" + ptid + " site=" + site +
                   "\nbridge=" + ue3::Status() + "\n";
        }
        if (op == "add_currency") {
            // which = 0 Kill Coins, 1 SPLithium, 2 Bank, 3 Tank, 4 Death Metals, 5 Bloodnium, 6 TDM
            std::string sv = GetVal(cmd, "value"), sw = GetVal(cmd, "which");
            int which = IsNum(sw) ? atoi(sw.c_str()) : -1;
            int amt = IsNum(sv) ? atoi(sv.c_str()) : 0;
            if (which < 0 || which >= ue3::CUR_COUNT || amt < 1 || amt > 99999999)
                return "status=err\nmsg=need which=0.." + std::to_string(ue3::CUR_COUNT - 1) + " and value=1..99999999\n";
            ue3::QueueAddCurrency((ue3::Currency)which, amt);
            return "status=ok\nop=add_currency\ncurrency=" + std::string(ue3::CurrencyName((ue3::Currency)which)) +
                   "\nqueued=" + std::to_string(amt) + "\n";
        }
        return "status=err\nmsg=unknown op\n";
    }

    DWORD WINAPI RpcThread(LPVOID) {
        CreateDirectoryW((settings::ModDir() + L"\\rpc").c_str(), nullptr);
        std::string stale = ReadFileStr(CmdPath());     // never replay a command from a previous session
        if (!stale.empty()) g_lastId = _atoi64(GetVal(stale, "id").c_str());
        for (;;) {
            Sleep(150);
            std::string cmd = ReadFileStr(CmdPath());
            if (cmd.empty()) continue;
            long long id = _atoi64(GetVal(cmd, "id").c_str());
            if (id == g_lastId) continue;
            g_lastId = id;
            WriteResult(id, Handle(cmd));
        }
    }
}

void Rpc_Start() {
    HANDLE h = CreateThread(nullptr, 0, RpcThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}
