# LET IT DIE cheat menu (offline)

An in-game mod menu for the **offline edition of LET IT DIE** (v5.0.1, Windows x64, Direct3D 9). It is a
`dinput8.dll` proxy that draws a Dear ImGui overlay inside the game. Single-player and offline only.

Press **Insert** in game to open or close the menu.

## Demo

https://github.com/user-attachments/assets/6b4da0d6-ae3b-4032-9eea-d55e5192b7b8

Full-quality download: [`media/LET_IT_DIE_mod_menu.mp4`](media/LET_IT_DIE_mod_menu.mp4).

## Features
- **Cheats tab**
  - Infinite durability (weapons and armor).
  - Infinite ammo.
  - Infinite stamina.
  - Add any amount of Kill Coins, SPLithium, Bank funds, SPLithium Tank, Bloodnium or TDM Points, capped at the
    game's own limits (Bank and Tank limits grow with their level).
- **Items tab:** a searchable picker with icons and names for weapons, armor and R&D materials.
  - **Send to Coin Locker** is permanent. The item is written into your save after you quit and is waiting on your next
    launch.
  - **Try it on now** is a temporary live preview.
- **Fighter tab:** raise a fighter's grade (grade sets the stat ceiling), then max the stats of your current
  fighter or every fighter up to it. Applied to the save after you quit.
- **Settings tab:** borderless window, a diagnostics panel, and settings remembered between launches.
- While the menu is open, the game gets no mouse or keyboard input, and the menu draws its own cursor.

## How it works (short)
- **Cheats:** the DLL hooks the engine's `ProcessEvent` and `ProcessInternal` (MinHook). When one of the game's
  decrement-only script functions runs for your fighter, the hook skips it: `*DurabilityDown`, `BulletConsumption` and
  `StaminaConsumption`.
- **Permanent changes:** these are done to the save file with the game closed. The overlay queues them in
  `lid_mod\save_queue.txt`. A small Python helper (`db/lid_apply_queue.py`) starts with the game, waits for it to exit,
  backs up the save, and applies the queue atomically.
- **More detail:** see `MODLOG.md` for the reverse-engineering notes (offsets, save format, gotchas).

## Requirements
- Windows 10/11 x64, and the offline edition of LET IT DIE 5.0.1 (your own copy).
- **To build:**
  - Visual Studio Build Tools with the x64 C++ toolset. `build.bat` expects the VS 18 Build Tools path; edit `VS=` if
    yours differs.
  - Dear ImGui 1.91.5 and MinHook 1.3.3 are git submodules in `third_party/`. Clone with `git clone --recursive`, or run `git submodule update --init` after cloning.
- **To run:** Python 3 on Windows (`pythonw.exe`) for the save helper. Set `[save_helper] python=` in
  `lid_mod\settings.ini` if it isn't found automatically.
- **To build the item catalog:** Python 3 with Pillow, plus the icon exporter described in `assets/README.txt`.

## Build and install
1. Build: run `build.bat`. It produces `build\dinput8.dll`.
2. Item catalog (optional; the Items tab needs it):
   - Export the `UI_Icon_*` textures from your install.
   - Run `python tools\build_item_index.py --game "<your LET IT DIE folder>"`. It writes `assets\items.tsv` and
     `assets\icons_atlas.png`.
3. Install: close the game and run `.\deploy.ps1 -Game "<your LET IT DIE folder>"`.
   - It copies `dinput8.dll` next to `BrgGame-Steam.exe` (`Binaries\Win64`), plus `lid_mod\tools\*.py` and the catalog.
4. Uninstall: delete `Binaries\Win64\dinput8.dll` and `Binaries\Win64\lid_mod\`.

## Safety
- **Backups:** every save edit makes a backup first, next to the save (`*.sav.pre-overlay-*`, `*.pre-fighter-*`).
- **Back up yourself, too:** keep your own copy of `Savedata\` before first use.
- **Offline only:** don't use this anywhere other players are affected.

## Repository notes
- **Not included:** game files and extracted assets (icons, item names). They are generated locally from your own
  install.
- **Game folder:** the scripts take it from the `LID_GAME` environment variable (or `deploy.ps1 -Game`,
  `build_item_index.py --game`). Nothing is hard-coded.

## Credits
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT) and [MinHook](https://github.com/TsudaKageyu/minhook)
  (BSD-2-Clause). Pulled in as submodules under `third_party/`, with their own licenses.
- LET IT DIE is a trademark of its owners. This project is not affiliated with them.
