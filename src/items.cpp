#include "items.h"

#include <windows.h>
#include <wincodec.h>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "settings.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {
    std::vector<items::Item> gItems;
    std::vector<unsigned char> gPixels;     // BGRA, freed after upload to a managed texture
    UINT gW = 0, gH = 0;                     // atlas image size
    UINT gTexW = 0, gTexH = 0;               // texture size (padded to a power of two if the GPU needs it)
    std::atomic<bool> gFilesReady{ false };
    IDirect3DTexture9* gTex = nullptr;
    bool gTexManaged = false;
    std::string gFileError = "item catalog not loaded yet";   // set once by LoadFiles
    std::string gTexError;                                      // render thread only
    std::string gError;                                         // combined view returned by Error()
    int gTexFailures = 0;
    DWORD gNextTexTry = 0;
    void UpdateError() { gError = gFileError.empty() ? gTexError : (gTexError.empty() ? gFileError : gFileError + "; " + gTexError); }

    std::string Narrow(const std::wstring& w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        return s;
    }

    bool LoadTsv(const std::wstring& path, std::string& err) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { err = "missing " + Narrow(path); return false; }
        std::string line;
        std::getline(f, line);                               // header
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::vector<std::string> c;
            std::stringstream ss(line);
            std::string cell;
            while (std::getline(ss, cell, '\t')) c.push_back(cell);
            if (c.size() < 8) continue;
            items::Item it;
            it.ptid = c[0]; it.name = c[1]; it.slot = c[2]; it.reqStat = c[3];
            it.reqValue = atoi(c[4].c_str()); it.icon = atoi(c[5].c_str());
            it.rarity = atoi(c[6].c_str()); it.sort = atoi(c[7].c_str());
            gItems.push_back(std::move(it));
        }
        if (gItems.empty()) { err = "items.tsv has no rows"; return false; }
        // ...\LET.IT.DIE\Binaries\Win64\lid_mod -> ...\LET.IT.DIE\BrgGame\CookedPCConsole
        std::wstring root = settings::ModDir();
        for (int i = 0; i < 3; ++i) root = root.substr(0, root.find_last_of(L"\\/"));
        std::wstring cooked = root + L"\\BrgGame\\CookedPCConsole\\UI_Icon_";
        for (auto& it : gItems) {
            std::wstring pkg = cooked + std::wstring(it.ptid.begin(), it.ptid.end()) + L"_SF.upk";
            it.gameIcon = GetFileAttributesW(pkg.c_str()) != INVALID_FILE_ATTRIBUTES;
        }
        return true;
    }

    UINT NextPow2(UINT v) { UINT p = 1; while (p < v) p <<= 1; return p; }

    bool CreateAtlasTexture(IDirect3DDevice9* dev) {
        // Try exact size, then power-of-two padding; managed pool first (survives Reset), then default pool.
        const UINT sizes[2][2] = { { gW, gH }, { NextPow2(gW), NextPow2(gH) } };
        HRESULT hr = E_FAIL;
        for (int s = 0; s < 2 && FAILED(hr); ++s) {
            gTexW = sizes[s][0]; gTexH = sizes[s][1];
            hr = dev->CreateTexture(gTexW, gTexH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &gTex, nullptr);
            gTexManaged = SUCCEEDED(hr);
            if (FAILED(hr))
                hr = dev->CreateTexture(gTexW, gTexH, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &gTex, nullptr);
        }
        if (FAILED(hr)) { gTex = nullptr; gTexError = "could not create icon texture"; return false; }
        D3DLOCKED_RECT lr;
        if (FAILED(gTex->LockRect(0, &lr, nullptr, gTexManaged ? 0 : D3DLOCK_DISCARD))) {
            gTex->Release(); gTex = nullptr; gTexError = "could not lock icon texture"; return false;
        }
        for (UINT y = 0; y < gTexH; ++y) {
            unsigned char* row = (unsigned char*)lr.pBits + (size_t)y * lr.Pitch;
            memset(row, 0, (size_t)gTexW * 4);
            if (y < gH) memcpy(row, gPixels.data() + (size_t)y * gW * 4, (size_t)gW * 4);
        }
        gTex->UnlockRect(0);
        if (gTexManaged) { gPixels.clear(); gPixels.shrink_to_fit(); }   // managed pool keeps its own copy
        return true;
    }

    bool LoadPng(const std::wstring& path, std::string& err) {
        HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* fac = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* conv = nullptr;
        bool ok = false;
        do {
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac)))) { err = "WIC unavailable"; break; }
            if (FAILED(fac->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) { err = "missing " + Narrow(path); break; }
            if (FAILED(dec->GetFrame(0, &frame)) || FAILED(fac->CreateFormatConverter(&conv))) { err = "bad atlas png"; break; }
            if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) { err = "atlas convert failed"; break; }
            conv->GetSize(&gW, &gH);
            if (!gW || !gH || gW > 4096 || gH > 4096) { err = "atlas size out of range"; break; }
            gPixels.resize((size_t)gW * gH * 4);
            if (FAILED(conv->CopyPixels(nullptr, gW * 4, (UINT)gPixels.size(), gPixels.data()))) { err = "atlas decode failed"; break; }
            ok = true;
        } while (false);
        if (conv) conv->Release();
        if (frame) frame->Release();
        if (dec) dec->Release();
        if (fac) fac->Release();
        if (SUCCEEDED(hrInit)) CoUninitialize();
        return ok;
    }
}

namespace items {
    void LoadFiles() {
        std::string err;
        const std::wstring& dir = settings::ModDir();
        if (!LoadTsv(dir + L"\\items.tsv", err)) { gFileError = err; UpdateError(); return; }
        if (!LoadPng(dir + L"\\icons_atlas.png", err)) gFileError = "names loaded, icons missing: " + err;
        else gFileError.clear();
        UpdateError();
        gFilesReady = true;
    }

    bool EnsureTexture(IDirect3DDevice9* dev) {
        if (gTex) return true;
        if (!gFilesReady || gPixels.empty()) return false;
        // Transient failures (device lost) are retried every 2 s, at most 5 times.
        if (gTexFailures >= 5 || (gTexFailures && (int)(GetTickCount() - gNextTexTry) < 0)) return false;
        bool ok = CreateAtlasTexture(dev);
        if (ok) { gTexError.clear(); gTexFailures = 0; }
        else { ++gTexFailures; gNextTexTry = GetTickCount() + 2000; }
        UpdateError();
        return ok;
    }

    void OnDeviceLost() {
        if (gTex && !gTexManaged) { gTex->Release(); gTex = nullptr; }  // default pool must go before Reset
    }

    const std::vector<Item>& All() { static const std::vector<Item> empty; return gFilesReady ? gItems : empty; }
    IDirect3DTexture9* Atlas() { return gTex; }
    void IconUV(int icon, float& u0, float& v0, float& u1, float& v1) {
        float x = (float)(icon % kCols) * kCell, y = (float)(icon / kCols) * kCell;
        u0 = x / gTexW; v0 = y / gTexH; u1 = (x + kCell) / gTexW; v1 = (y + kCell) / gTexH;
    }
    const std::string& Error() { return gError; }
}
