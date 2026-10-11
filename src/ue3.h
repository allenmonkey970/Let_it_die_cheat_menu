#pragma once
// Minimal UE3 bridge for LET IT DIE 5.0.1 (BrgGame-Steam.exe). Offsets from Ghidra (MODLOG.md).
#include <string>

namespace ue3 {
    enum Cheat { DURABILITY = 1, AMMO = 2, STAMINA = 4 };

    bool Install();                         // hook ProcessEvent (game-thread pump) + ProcessInternal (cheats)
    // Queued; executed on the game thread during the player's next PlayerTick.
    // BrgGameInfo.DebugPartEquip (temporary). site: Head|Arm_R|Arm_L|Body|Legs; ptid must be PT_*. false = rejected.
    bool QueuePartEquip(const std::wstring& site, const std::wstring& ptid);
    // Currencies (BrgUserData natives, saved by the game): adds `amount`, clamped to the game's own limit.
    // Bloodnium: the native stat-distribute check spends from the native save-store soul record (+0xB4) plus the
    // current chara record, not from BrgUserData, so the Bloodnium add also writes soul+0xB4 (after verifying it).
    // A UI-only raise lets the stat screen allow an upgrade the native check then rejects with an assert (crash).
    enum Currency { CUR_KILLCOINS, CUR_SPLITHIUM, CUR_BANK, CUR_TANK, CUR_DEATHMETAL, CUR_BLOODNIUM, CUR_TDM, CUR_COUNT };
    void QueueAddCurrency(Currency c, int amount);
    const char* CurrencyName(Currency c);
    bool CurrencyAvailable(Currency c);     // its game functions were found
    bool Scanned();                         // the game-function lookup has run at least once
    // Live XP is intentionally absent: AddExp only raises the script-side value and the native stat-distribute
    // check then asserts ("body_lvl distribute failed"). Levels are edited in the save instead.

    void SetCheat(Cheat c, bool on);
    bool GetCheat(Cheat c);
    bool Faulted();                         // an engine call faulted: cheats latched off until restart
    bool Ready();                           // cheat targets resolved
    bool Unavailable();                     // gave up: targets missing in this build
    std::string CheatStats();
    std::string Status();                   // human-readable state for the overlay / RPC
    std::string Diag();
}
