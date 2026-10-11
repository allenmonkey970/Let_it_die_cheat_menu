#include "savequeue.h"

#include <windows.h>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>

#include "settings.h"

namespace {
    std::mutex gMu;
    std::vector<savequeue::Entry> gPending;
    std::vector<std::string> gLastResults;
    std::string gHelperStatus = "save helper: starting";
    std::atomic<bool> gReady{ false };      // queue loaded (after the previous session's helper finished)

    std::wstring QueuePath()  { return settings::ModDir() + L"\\save_queue.txt"; }
    std::wstring ResultPath() { return settings::ModDir() + L"\\save_queue_result.txt"; }
    std::wstring LockPath()   { return settings::ModDir() + L"\\save_helper.lock"; }

    bool FileExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

    // Line format: op \t arg \t label   (UTF-8)
    void LoadLocked() {
        std::ifstream f(QueuePath(), std::ios::binary);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::stringstream ss(line);
            savequeue::Entry e;
            std::getline(ss, e.op, '\t'); std::getline(ss, e.arg, '\t'); std::getline(ss, e.label, '\t');
            if (!e.op.empty() && !e.arg.empty()) gPending.push_back(e);
        }
        f.close();
        // Results are shown once: move them aside so the next launch does not repeat them.
        {
            std::ifstream r(ResultPath(), std::ios::binary);
            while (std::getline(r, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) gLastResults.push_back(line);
            }
        }
        if (!gLastResults.empty())
            MoveFileExW(ResultPath().c_str(), (settings::ModDir() + L"\\save_queue_result.shown.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    void SaveLocked() {
        std::wstring tmp = QueuePath() + L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            for (auto& e : gPending) f << e.op << '\t' << e.arg << '\t' << e.label << '\n';
        }
        MoveFileExW(tmp.c_str(), QueuePath().c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    // Newest pythonw.exe under <root>\Python3<minor>\ by numeric minor version (Python314 > Python39).
    // Skips the Microsoft Store stub in WindowsApps.
    std::wstring FindPythonUnder(const std::wstring& root) {
        WIN32_FIND_DATAW fd; std::wstring best; int bestMinor = -1;
        HANDLE h = FindFirstFileW((root + L"\\Python3*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return best;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            int minor = _wtoi(fd.cFileName + 7);              // "Python314" -> 14
            std::wstring cand = root + L"\\" + fd.cFileName + L"\\pythonw.exe";
            if (FileExists(cand) && minor > bestMinor) { best = cand; bestMinor = minor; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return best;
    }

    std::wstring FindPython() {
        const std::wstring& configured = settings::Get().python;
        if (!configured.empty() && FileExists(configured)) return configured;
        wchar_t buf[MAX_PATH];
        std::wstring p = FindPythonUnder(L"C:");
        if (p.empty() && GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) p = FindPythonUnder(std::wstring(buf) + L"\\Programs\\Python");
        if (p.empty() && GetEnvironmentVariableW(L"ProgramFiles", buf, MAX_PATH)) p = FindPythonUnder(buf);
        return p;
    }

    void SetHelperStatus(const std::string& s) { std::lock_guard<std::mutex> lk(gMu); gHelperStatus = s; }

    DWORD WINAPI HelperThread(LPVOID) {
        // If the previous session's helper is still applying (quick relaunch), wait for it so we never load a
        // queue it is about to clear. A lock older than 5 minutes is stale (helper killed) and ignored.
        for (int i = 0; i < 60 && FileExists(LockPath()); ++i) {
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExW(LockPath().c_str(), GetFileExInfoStandard, &a)) {
                FILETIME now; GetSystemTimeAsFileTime(&now);
                ULARGE_INTEGER n{ now.dwLowDateTime, now.dwHighDateTime }, t{ a.ftLastWriteTime.dwLowDateTime, a.ftLastWriteTime.dwHighDateTime };
                if (n.QuadPart - t.QuadPart > 5ULL * 60 * 10000000) break;
            }
            SetHelperStatus("save helper: waiting for the previous session's changes to finish...");
            Sleep(500);
        }
        { std::lock_guard<std::mutex> lk(gMu); LoadLocked(); }
        gReady = true;

        std::wstring script = settings::ModDir() + L"\\tools\\lid_apply_queue.py";
        std::wstring py = FindPython();
        if (!FileExists(script)) { SetHelperStatus("save helper missing (lid_mod\\tools\\lid_apply_queue.py)"); return 0; }
        if (py.empty()) { SetHelperStatus("save helper: Python not found - set [save_helper] python= in lid_mod\\settings.ini"); return 0; }
        std::wstring cmd = L"\"" + py + L"\" -I \"" + script + L"\" --pid " + std::to_wstring(GetCurrentProcessId()) +
                           L" --mod \"" + settings::ModDir() + L"\"";
        std::wstring cmd2 = cmd;          // CreateProcessW may modify the command-line buffer
        STARTUPINFOW si{ sizeof si };
        PROCESS_INFORMATION pi{};
        DWORD flags = CREATE_NO_WINDOW | DETACHED_PROCESS;
        bool brokeAway = true;
        BOOL ok = CreateProcessW(py.c_str(), &cmd[0], nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB,
                                 nullptr, nullptr, &si, &pi);
        if (!ok) {
            brokeAway = false;
            ok = CreateProcessW(py.c_str(), &cmd2[0], nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi);
        }
        if (!ok) { SetHelperStatus("save helper failed to start (error " + std::to_string(GetLastError()) + ")"); return 0; }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        BOOL inJob = FALSE;
        IsProcessInJob(GetCurrentProcess(), nullptr, &inJob);
        SetHelperStatus(brokeAway || !inJob
            ? "save helper running - queued changes are applied when you quit the game"
            : "save helper running inside the game's job - if queued changes are not applied after quitting, "
              "run lid_mod\\tools\\lid_apply_queue.py --pid 0 --mod <lid_mod> with the game closed");
        return 0;
    }
}

namespace savequeue {
    void StartHelper() {
        HANDLE h = CreateThread(nullptr, 0, HelperThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }

    bool Add(const Entry& e) {
        if (!gReady) return false;
        std::lock_guard<std::mutex> lk(gMu);
        gPending.push_back(e);
        SaveLocked();
        return true;
    }
    void Remove(size_t index) {
        if (!gReady) return;
        std::lock_guard<std::mutex> lk(gMu);
        if (index < gPending.size()) { gPending.erase(gPending.begin() + index); SaveLocked(); }
    }
    std::vector<Entry> Pending() { std::lock_guard<std::mutex> lk(gMu); return gPending; }
    bool Ready() { return gReady; }
    std::string HelperStatus() { std::lock_guard<std::mutex> lk(gMu); return gHelperStatus; }
    std::vector<std::string> LastResults() { std::lock_guard<std::mutex> lk(gMu); return gLastResults; }
}
