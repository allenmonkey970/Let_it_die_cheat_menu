#pragma once
// Save edits that must happen with the game CLOSED (permanent Coin Locker items, fighter stats).
// The overlay appends lines to lid_mod\save_queue.txt; a helper (pythonw lid_mod\tools\lid_apply_queue.py),
// started at launch, waits for this process to exit and then applies them to the save (with a backup).
// The helper holds lid_mod\save_helper.lock while applying; a new session waits for it before loading.
#include <string>
#include <vector>

namespace savequeue {
    struct Entry { std::string op, arg, label; };   // op: "locker" (part id) | "locker_item" (R&D material id)
                                                    // | "fighter_max" (current|all) | "fighter_grade" (current|all:n)
    void StartHelper();                             // spawns its own thread; returns immediately
    bool Add(const Entry& e);                       // false until the queue is loaded (Ready)
    void Remove(size_t index);
    bool Ready();
    std::vector<Entry> Pending();
    std::string HelperStatus();
    std::vector<std::string> LastResults();         // what the helper did after the previous session (shown once)
}
