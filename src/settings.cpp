#include "settings.h"

#include <windows.h>
#include <mutex>

#include "ue3.h"

namespace {
    settings::Values gValues;
    std::wstring gModDir;
    std::recursive_mutex gMu;        // menu (render thread), RPC (scanner thread) and the worker all use this

    std::wstring IniPath() { return settings::ModDir() + L"\\settings.ini"; }

    bool ReadBool(const wchar_t* key, bool def) {
        return GetPrivateProfileIntW(L"overlay", key, def ? 1 : 0, IniPath().c_str()) != 0;
    }
    void WriteBool(const wchar_t* key, bool v) {
        WritePrivateProfileStringW(L"overlay", key, v ? L"1" : L"0", IniPath().c_str());
    }
}

namespace settings {
    Values& Get() { return gValues; }

    // First call happens in DllMain (before any of our threads exist); afterwards the string is read-only.
    const std::wstring& ModDir() {
        if (gModDir.empty()) {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)&ModDir, &self);
            wchar_t path[MAX_PATH] = L"";
            GetModuleFileNameW(self, path, MAX_PATH);
            std::wstring dir(path);
            dir = dir.substr(0, dir.find_last_of(L"\\/"));
            gModDir = dir + L"\\lid_mod";
            CreateDirectoryW(gModDir.c_str(), nullptr);
        }
        return gModDir;
    }

    void Load() {
        std::lock_guard<std::recursive_mutex> lk(gMu);
        gValues.borderless = ReadBool(L"borderless", false);
        gValues.infDur     = ReadBool(L"infinite_durability", false);
        gValues.infAmmo    = ReadBool(L"infinite_ammo", false);
        gValues.infStamina = ReadBool(L"infinite_stamina", false);
        wchar_t py[MAX_PATH] = L"";
        GetPrivateProfileStringW(L"save_helper", L"python", L"", py, MAX_PATH, IniPath().c_str());
        gValues.python = py;
    }

    void Save() {
        std::lock_guard<std::recursive_mutex> lk(gMu);
        WriteBool(L"borderless", gValues.borderless);
        WriteBool(L"infinite_durability", gValues.infDur);
        WriteBool(L"infinite_ammo", gValues.infAmmo);
        WriteBool(L"infinite_stamina", gValues.infStamina);
        WritePrivateProfileStringW(L"save_helper", L"python", gValues.python.c_str(), IniPath().c_str());
    }

    void SetCheat(int cheat, bool on) {
        std::lock_guard<std::recursive_mutex> lk(gMu);
        // While the bridge is latched off after a fault, keep the user's saved choice for the next launch.
        if (ue3::Faulted()) return;
        ue3::SetCheat((ue3::Cheat)cheat, on);
        if (cheat == ue3::DURABILITY) gValues.infDur = on;
        if (cheat == ue3::AMMO)       gValues.infAmmo = on;
        if (cheat == ue3::STAMINA)    gValues.infStamina = on;
        Save();
    }

    void SetBorderless(bool on) {
        std::lock_guard<std::recursive_mutex> lk(gMu);
        gValues.borderless = on;
        Save();
    }

    void ApplyCheatsFromSettings() {
        std::lock_guard<std::recursive_mutex> lk(gMu);
        ue3::SetCheat(ue3::DURABILITY, gValues.infDur);
        ue3::SetCheat(ue3::AMMO, gValues.infAmmo);
        ue3::SetCheat(ue3::STAMINA, gValues.infStamina);
    }
}
