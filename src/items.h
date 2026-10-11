#pragma once
// Item catalog for the overlay picker: lid_mod\items.tsv + lid_mod\icons_atlas.png (built by
// tools\build_item_index.py from the user's own game files; 64px cells, 40 per row).
#include <d3d9.h>
#include <string>
#include <vector>

namespace items {
    struct Item {
        std::string ptid, name, slot, reqStat;   // slot: Arm | Head | Body | Legs | Material (ptid = master_item.itemid)
        int reqValue = 0, icon = -1, rarity = 0, sort = 0;
        bool gameIcon = false;                    // UI_Icon_<ptid>_SF.upk exists: the game itself can draw it
                                                  // (picker icon may otherwise be a same-model fallback)
    };
    constexpr int kCell = 64, kCols = 40;

    void LoadFiles();                            // worker thread: parse TSV + decode PNG (WIC)
    bool EnsureTexture(IDirect3DDevice9* dev);   // render thread: upload atlas once
    void OnDeviceLost();                         // before IDirect3DDevice9::Reset
    const std::vector<Item>& All();
    IDirect3DTexture9* Atlas();
    void IconUV(int icon, float& u0, float& v0, float& u1, float& v1);
    const std::string& Error();                  // "" when everything loaded
}
