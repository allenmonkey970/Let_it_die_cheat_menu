#pragma once
// Persistent overlay settings: <game>\Binaries\Win64\lid_mod\settings.ini (next to dinput8.dll).
#include <string>

namespace settings {
    struct Values {
        bool borderless = false;
        bool infDur = false;
        bool infAmmo = false;
        bool infStamina = false;
        std::wstring python;               // pythonw.exe for the save helper ("" = auto-detect)
    };
    Values& Get();
    void Load();                           // call once at startup (worker thread)
    void Save();                           // call after any change
    const std::wstring& ModDir();          // ...\Binaries\Win64\lid_mod (created if missing)

    // Single source of truth for cheat toggles (menu, RPC, startup): updates ue3 + the ini.
    // `cheat` is a ue3::Cheat value.
    void SetCheat(int cheat, bool on);
    void SetBorderless(bool on);
    void ApplyCheatsFromSettings();        // after ue3::Install()
}
