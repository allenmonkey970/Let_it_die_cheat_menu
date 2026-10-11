# LET IT DIE — in-game overlay cheat menu — MODLOG

Working journal (reverse-engineering skill). Append facts as verified. Never commit game files.

## Target
- Game: **LET IT DIE: Offline Edition**, PC/Steam (cracked RUNE repack), AppId 794600.
- Install: `C:\Users\antho\Downloads\LET.IT.DIE-SteamRIP.com\LET.IT.DIE`
- Game exe: `Binaries\Win64\BrgGame-Steam.exe` (x64, PE link ts 2026-08-31).
- Engine: **Unreal Engine 3** ("BrgEngine", GungHo/Grasshopper fork). Native C++ + UnrealScript.
- Version: **5.0.1** (`masters.db master_const_str CLIENT_VERSION`; `TITLE_VERSION = 5.0.1.0.0 - 1.85`).
- Offline/single-player only. No live anti-cheat in this configuration. Server service ended Sep 2026.

## Goal
In-game overlay menu (hotkey) over the live game: infinite weapon/armor durability, infinite ammo,
infinite money, and an item-spawn picker showing item icon PNGs.

## Architecture (planned)
Injected DLL → hook D3D present → Dear ImGui overlay → cheats via memory hooks/writes → item-spawn grid.
- Compiler: MSVC (user has VS). Hook lib: MinHook. UI: Dear ImGui.
- Dynamic RE: Cheat Engine (user has it). Static RE: Ghidra (to install). Icons: umodel/FModel (to install).

## Tools confirmed on user's machine
- Cheat Engine ✅
- Visual Studio / MSVC C++ ✅
- User OK'd installing the remaining free tools (ask before each).

## Backups (restore with `um backup restore <name>`)
- `lid-savedata` — `Savedata\` (the .sav)
- `lid-config` — `BrgGame\Config\` (8 inis)
- `lid-content-db` — `BrgGame\Content\` (masters.db + .backup + .original, 170 MB)
- Pristine DB also in-place: `BrgGame\Content\masters.db.original` (Sep 2, 59,510,784 B).

## Key data facts (from masters.db — 59 MB SQLite, 223 tables, read-only inspection)
- Economy/balance tunables: `master_const_int` (722 rows), `master_const_float`, `master_const_str`.
- Item master: `master_item`, `master_item_gen`. Shop: `master_shop_appearance`, costs in const_int.
- Durability hints: `ARMOUR_DURABILITY_2..5` consts; weapon durability tunable (manager had multipliers).
- Currency hints: `BLOODNIUM_POINT_MAX=999999`, money drop freq consts, shop costs.
- Install already modded earlier by LET IT DIE Mod Manager (`_lid_mod_manager` table, 10 mods, 2026-10-09,
  manager_version 0.11.0): armor/weapon durability x2, magazine/ammo x2, nitro-exp, tdm-rewards x2,
  storage/bank/reward-box limits.

## Community references (do NOT download game files from these)
- s3er0i9ng/let-it-die-mod-releases — Mod Manager (Python), build-locked (v1.3.25 → build 25767944/5.0.2).
- Deadscale/LET-IT-DIE-Mods — SQLite `.sql` edits (master_const_int / master_shop_appearance).
- TrustworthyAndy/* — .upk weapon/hat reskins (proves upk edits load → integrity beatable per-build).
- xCENTx/LetItDie-BorderlessFullscreen-Mod — external process tool (OpenProcess + window-style). C++/VS.

## Version gap (resolved)
Game is 5.0.1; latest public Mod Manager targets 5.0.2/build 25767944 and does exact-build checks → would
refuse this install. Chosen approach: build cheats DIRECTLY (route 2), no manager dependency → gap irrelevant.

## RE unknowns to resolve (in order)
1. [x] **Render API = Direct3D 9.** `BrgEngine.ini`: `AllowD3D11=False`, `AllowOpenGL=False`,
   `bAllowD3D9MSAA=False`. Exe statically imports d3d9+d3d11+dxgi (UE3 ships both) but D3D11/GL are
   disabled → **active renderer is D3D9**. Hook `IDirect3DDevice9::EndScene` (vtable idx 42) or
   `Present` (17). Use Dear ImGui **DX9** backend.
2. [x] **Not packed.** dumpbin shows a normal import table; CRT = MSVCR110/MSVCP110 (VS2012, orig UE3
   build). RUNE crack only replaces steam_api64 (steam_api64.rne). Renderer imports intact.
   **Injection vector = proxy `dinput8.dll`** next to the exe (exe imports DINPUT8.dll; not a KnownDLL →
   local dir wins). Alt candidates also imported: xinput1_3, winmm, d3d9/dxgi. Forward the 5 dinput8
   exports to the real `C:\Windows\System32\dinput8.dll`.
3. [ ] Durability address + writer (Cheat Engine).
4. [ ] Ammo/magazine address + writer.
5. [ ] Money/SPLithium address + writer.
6. [ ] "Add item to inventory" function (Ghidra) + item-id enumeration (master_item).
7. [ ] Item icon textures → PNG (umodel/FModel).

## Build milestones (vertical slice order)
- M0 [x] **DONE 2026-10-09.** Overlay infra: dinput8 proxy + MinHook + Dear ImGui(DX9) hooking EndScene;
  INSERT toggles a menu drawn over the live game. Verified in game (shots/02_overlay.png): menu renders,
  Renderer=D3D9, 59.9 FPS. Injection confirmed (our DINPUT8.dll loaded from Binaries\Win64, forwards to
  System32). Build: `C:\Users\antho\lid-overlay-mod\build.bat` -> build\dinput8.dll (x64, /MT, 515 KB).
  Deploy: copy to Binaries\Win64\dinput8.dll. Toggle key: VK_INSERT (0x2D).
- M0.5 [x] **Borderless window** toggle added to overlay. NOTE: the game's "windowed" 1920x1080 mode is
  ALREADY borderless by default — window style 0x14000000 (no WS_CAPTION/THICKFRAME/POPUP) at (0,0)
  covering the full monitor. So default already looks borderless; the toggle forces WS_POPUP for cases
  where the user runs a bordered/non-native window. Overlay re-verified this launch (shots/12_menu.png).
- M1-tooling [ ] In-DLL memory scanner controlled via file-RPC (rpc\cmd.txt / rpc\result.txt) so the
  agent operates the RE while the user just plays. Scans i32/f32, exact + dec/inc/changed filters,
  list/read/freeze. Runs on a worker thread (never the render thread). ← BUILDING
- M1 [ ] One cheat wired to a checkbox (durability), found via Cheat Engine.
  - IDEA to try first: UE3 tilde console + `[GameFramework.GameCheatManager]` may be live (ConsoleKey=Tilde
    in BrgInput.ini). If console works, drive game cheat/exec funcs from the overlay instead of per-value
    memory scans. Test console in-game before committing to CE pointer scans.
- M2 [ ] Ammo + money toggles.
- M3 [ ] Item-spawn grid with icon PNGs.

## DB cheats (route: "DB now + Ghidra later", user choice 2026-10-10)
`master_part` = weapons (type PTTP_ARM) + armor (PTTP_BODY/HEAD/LEGS/MASK/PANTS). Key cols:
`dur`(max), `dur_hit/dur_shoot/dur_rage`(consume per action), `dur_drp*`/`dur_dwn_fort/hunter`(drain),
`capacity`(mag), `spare`(reserve), `cap_drp*`(mag degrade). Costs: `master_item.buy_money`,
`master_part_research.craft_money/lvup_money`.
- Applied `db/cheats_dur_ammo_money.sql` to masters.db (2026-10-10), verified 0 rows remain non-cheated:
  - Infinite durability: all consume/drain cols = 0 (weapons + armor).
  - Infinite ammo: capacity=spare=9999 where capacity>0.
  - Free money (Kill Coins): buy_money/buy_rank_money=0, craft_money/lvup_money/_c=0.
- NOT yet done (money): SPLithium/medal/bloodnium buy costs, master_part.price_b, raising the actual
  money COUNTER (needs save edit -> Ghidra-later). Durability/ammo are the clean wins.
- Reversible: `BrgGame\Content\masters.db.pre-overlay-cheats` (or masters.db.original), or um backup.
- [ ] VERIFY IN GAME: durability doesn't drop after many hits; gun shows huge ammo; shop/craft free.

## SAVE FORMAT (reverse-engineered 2026-10-10) — the real fix for owned gear + money
DB edits DON'T affect owned gear/money: those are cached in the SAVE. Save = `Savedata\76561197960271872.sav`.
Format:
- Header (16 bytes): `"BRG\0"` + version u32(=2) + total_uncompressed_size u32(LE) + `"ZLIB"`.
- Body: repeated chunks `[usize u32][csize u32][zlib stream of csize bytes]`; chunks concat = one big JSON.
- Terminator: `00 00 00 00` (usize=0). **No checksum / no integrity check** → freely editable + repackable.
- Payload = UTF-8 JSON, 51 top-level keys. Key fields:
  - `soul.free_money` (Kill Coins), `soul.paid_money`, `soul.spirit` (SPLithium), `soul.bloodnium_point`,
    `soul.recycle_point`; `user.free_medal`/`paid_medal` (Death Metals).
  - `part.pts["1"|"-1"]` = list of 189 owned part instances: `{ptid, dur (current durability),
    rest (mag ammo), spare (reserve ammo), lvl, grade, eid, ...}`. Max durability/capacity per ptid is in
    masters.db master_part (dur, capacity). The on-screen "68" earlier was `rest` (ammo), not durability.
  - `item.items` = consumable items list.
Editor plan: decompress -> json edit (free_money/spirit big; each part dur=master max; ranged rest=capacity,
spare=big) -> recompress chunks -> write header+chunks+terminator. GAME MUST BE CLOSED (it overwrites on exit).
Backup the .sav first (lid-savedata + a dated copy).

## Save editor (lid_save.py codec + edit) — 2026-10-10
- Codec round-trips exactly (decode(encode(jb))==jb). Only serialize diff = 6 pause floats 0.0000->0.0 (harmless).
- Applied: free_money=999999999, spirit=999999999; all 189 parts dur=9999999; 16 ranged rest=spare=9999.
- Backups: Savedata\76561197960271872.sav.pre-saveedit + um backup lid-savedata (20261010-124454).
- [x] VERIFIED IN GAME 2026-10-10 (shots/35,36): save loaded (fighter Adolph Lv11), ammo slot 9999/9999,
  durability bars full. Shop currency HUD: Kill Coins 500000/500000, SPLithium 500000/500000.
  **DISCOVERY: both currencies hard-cap at 500,000** — my 999999999 clamped to 500000 on load. So "max money"
  = 500K (can't exceed via save). Death Metals (medal) separate, still 0.
  OPEN: whether durability STAYS full during combat (dur drain) — needs a play test; if it drains, live-mem freeze.
- NOTE for "always 500K": save sets it once; spending reduces it. Permanent top-off = live-memory freeze (overlay/Ghidra).

## GHIDRA PHASE (started 2026-10-10)
Tools installed: Ghidra 12.1.4 at `C:\ghidra_12.1.4_PUBLIC`; Temurin JDK 21 at `C:\jdk21\jdk-21.0.12.1+1`.
Env persisted: GHIDRA_INSTALL_DIR, JAVA_HOME (for REA after restart + headless). REA MCP tools still need a
Claude Code RESTART to be callable; using Ghidra HEADLESS directly (no restart needed).
Headless analysis of BrgGame-Steam.exe running in bg (task b6qf5oov1), postScript FindLidTargets.py,
Ghidra project C:\lid-ghidra\lidproj.

KEY LEAD — binary is NOT stripped of UnrealScript names (exec* native thunks present as strings):
- Inventory/add-item: `UBrgInventoryItemStore`, `AInventoryManagerexecInventoryActors`,
  `AControllerexecRouteCache_AddItem`, `UBrgUIMenuPart_ShopLimitedItemsexecAddItem`,
  `UBrgRandomGenerateLevelManagerexecAddPartsToCell`, `UBrgUIHUD_MessageWindowContentsexecAddParts`.
- Item DB access: `UBrgDatabaseexecGetItemList/GetItemPart/GetItemIndexFromId/...`.
- Durability: `UBrgUIMenuPart_ItemInfoPanelexecDrawDurability`; column strings `dur_hit`,`dur_shoot` present.
- Money column strings `buy_money`,`free_money` present. (No literal `master_part` SQL strings — queries built
  dynamically; the game's own SQL on master_* not plain-text, but column names are.)
NOTE: calling UE3 `exec` natives from our DLL is hard (they read params off the UnrealScript FFrame stack).
Easier live route may be the non-exec C++ impl, or the console/cheat-manager (ConsoleKey=Tilde).
ALTERNATIVE for item-spawn (no native RE): inject into the SAVE — add entries to part.pts / item.items with
a fresh GUID + ptid/itemid from masters.db. Reliable; requires reload. PNG picker = UI over master_item/part.

## Ghidra results (2026-10-10)
- Ghidra 12 has NO Jython: Python scripts need PyGhidra. Use Java GhidraScripts (scripts/*.java) with
  `analyzeHeadless C:/lid-ghidra lidproj2 -process BrgGame-Steam.noexc.exe -noanalysis -readOnly -postScript X.java`.
- Import crashed on PE exception dir (ExceptionDataDirectory.markup). Workaround: analysis copy
  `C:\lid-ghidra\BrgGame-Steam.noexc.exe` with data-dir[3] (Exception, file off 0x210) zeroed. Project lidproj2.
- UE3 native lookup tables = {char* name, void* func} pairs. Resolved:
  ShopLimitedItems execAddItem -> FUN_14104aa80 (UI list), HUD execAddParts -> FUN_140af7480 (UI msg),
  AddPartsToCell -> FUN_141070b70 (level gen), ItemStore execShowStore -> FUN_141089d20, Database execGetItemPart
  -> FUN_140ffcb60. All are thin exec thunks: pop params off FFrame (Code ptr at Stack+0x24, GNatives @DAT_1428360a0)
  then call a vtable method. NONE grants inventory.
- Wide strings DeathBag_AddPart/AddItem, Add{Part,Item}EventData only referenced by FUN_1410a3640 = static
  initializer of ~500 FName/FString constants (not logic).
- Conclusion: item-grant logic lives in UnrealScript bytecode in BrgGame.upk (179 MB). dur_hit refs in
  FUN_140e68050/FUN_140f3e3d0/FUN_141591740; free_money refs in FUN_140ea3a10/FUN_140e7fd20/FUN_140f58450.
LIVE-SPAWN PLAN: (1) decompile UnrealScript (UELib) -> find the func that adds a part/item to the Death Bag;
(2) in overlay DLL find GNames/GObjects/ProcessEvent at runtime, look up that UFunction by name, call ProcessEvent
with params; (3) test one item, then icon grid.

## UnrealScript RE (2026-10-10) — LIVE SPAWN RECIPE FOUND
- BrgGame.upk is LZO-compressed (CompressionFlags=2, 287 chunks). Decompressed with own tool
  `C:\lid-ghidra\uedump` (`dotnet bin/out/uedump.dll decompress <in> <out>`; lzo.net) -> BrgGame.decomp.upk (365 MB).
  UELib 1.13.7 then decompiles: 173,678 exports, 24,462 functions. Dumps in C:\lid-ghidra\uedump\out*.
- Inventory API: `BrgCommonPawn_CustomCharaNative.DeathBag_AddPart(EBrgLocalItemType type, optional BrgDbPsPart,
  ..., optional int inTargetStockIndex=-1, optional bool inAddQuickUseHolder, inIsBuy, inIsPickup, inIsForce)`
  (native event) and `DeathBag_AddItem(BrgLocalItemInfo, ...)`.
- Dev console command `BrgGameInfo.DebugPartEquip(string inPartName, string inDbPartId)` (exec):
    SetNoneEquipBrgDbPsPart(site, ps); ps.mPtid=id; ps.mLvl=1; ps.mDur=dbPart.mDur; ps.mRest=dbPart.mCapacity;
    ps.mSpare=dbPart.mSpare; ps.mEptid=GetDebugEntityId(); mCommonPawn.DeathBag_AddPart(0, ps); ChangeEquipPart(site, ps)
  inPartName: Head(0) Arm_R(1) Arm_L(2) Body(3) Legs(4) | "All" resets. Pawn: BrgGameInfo(WorldInfo.Game).mPlayerPawn.
- CAVEAT: debug eptid; IsDebugEntityId() special-cased elsewhere -> debug parts may not persist. Overlay version
  should use a real GUID eptid.
- Routes: (A) tilde console `DebugPartEquip Arm_R <ptid>`; (B) overlay via ProcessEvent with same recipe.

## LIVE SPAWN WORKS (2026-10-10)
Runtime-verified engine internals (RVA from module base, ASLR on):
- ProcessEvent = 0x10E770 (FUN_14010e770; in 1,827 vtables) `void(UObject*, UFunction*, void* parms, void*)`.
  Found by scanning for funcs that MOV/CALL [reg+0xF0] (UFunction::Func) AND read word [reg+0xD4] (iNative).
- GNames = 0x2949C98 (TArray<FNameEntry*>, name @+0x14, bit0 of int@+0x08 = wide) — 259,207 names.
- GObjects = 0x2949CE0 (found at runtime: TArray where Data[i]->Index(@+0x38)==i) — ~448k objects.
- UObject: Index +0x38, Outer +0x40, Name +0x48, Class +0x50. UFunction: FunctionFlags +0xD0, iNative +0xD4,
  PropertiesSize +0x88, Func +0xF0. CallFunction = 0x103610, ProcessInternal = 0x10EC70, UFunction::Bind = 0x1032E0.
- Bridge `src/ue3.cpp`: hooks ProcessEvent (game thread pump), queues requests, calls
  BrgGameInfo.DebugPartEquip(FString site, FString ptid) on the live BrgGameInfo. Parms = 2x FString{Data,Num,Max}.
- RPC: `rpc.sh spawn_part <ptid> [site]`, `rpc.sh ue3_diag`, `rpc.sh ue3_status`. Overlay: Part ID + Slot + "Spawn & Equip".
- TEST: PT_ARM_WP038_0H4 -> Arm_R in hub: status "spawned", right-hand slot icon changed to new weapon, no crash
  (shots/52_spawned.png). A weapon model dropped in front of the player with a pickup prompt (TBD what).
OPEN: (1) does the debug-eptid part persist across save/reload? (2) what was dropped? (3) icon grid UI.

## Persistence investigation (2026-10-10)
- IsDebugEntityId(id) = id.empty() || id[0]=='-'. UpdateUserData (script) skips debug ids when saving.
- GUID redirect (swap UFunction GenerateDebugEntityId->Func during our call; FString::op= @0x19850) + call
  BrgPawn_CustomCharaPlayer.UpdateUserData => part survives hand-switch BUT CRASH on tower/quit:
  FUN_1412b0b40 (integrity check "[cheat]part dur over! fix max dur...") looks up every user-data part in the
  native save-DB registry @0x1427D0C70 (part.pts) and null-derefs [+0x4C] when missing. Native DB insert NOT found
  (InsertWeaponIntoDeathbag impl @0x156FBD0 only bumps a counter + UI notify). => kRealId=false (disabled).
- Registry vtable 0x14214F3D0 (+0x30 find-or-create by uid, +0x48 serialize, +0x50 parse);
  per-uid list vtable 0x14214F330; part entries stride 0x68 @+0x50.
- BUG FIXED: stale rpc\cmd.txt replayed at startup before gBase init -> AV in DINPUT8+0x6B51. Now stale cmds
  ignored at scanner start + lazy EnsureBase(). Verified by planting the stale cmd.
- NEXT: save-injection delivery to Coin Locker: add {uid:1,eid:GUID,owner:"COIN_LOCKER",ptid,...} to
  part.pts["1"]. Test injection of PT_ARM_WP038_0H4 done (backup .pre-lockerinject) — awaiting in-game check.

## Log
- 2026-10-09: Recon + research done. Backups made. Approach + architecture chosen.
- 2026-10-09: RE#1 render API = **D3D9** (ini). RE#2 not packed; inject via proxy dinput8.dll.
- 2026-10-09: M0 overlay + M0.5 borderless built & verified in-game. M1 scanner (file-RPC) built & tested.
- 2026-10-10: User chose "DB now + Ghidra later". Applied DB cheats (dur/ammo/free-buy). Pending in-game verify.

## 2026-10-10 QA round 2 (user report: level-up crash, no gun in locker, dur/ammo still drain)
- **Level-up crash** = engine assert after `body_lvl distribute failed! cid=c827.., grade=1, exp=14, nec_exp=138`
  (validator FUN_14118e9c0, caller FUN_141366480). Live `AddExp` only raised the script-side XP; the native
  pool stayed 14. GM natives (DebugGmToolSetCurrentCharaExp_NT -> vtbl+0xB38 etc.) are compiled-out folded
  stubs in shipping. XP buttons REMOVED from the menu.
- XP/levels in save: `soul.chr.chrs["1"][i]` {total_exp, rest_exp, gain_exp, start_exp, hp, type, grade,
  limit_break}; stat levels in `bodyuser["1"][i]` {lvl, hp..luk}; lvl = 1 + points spent.
  Cap per stat = master_body_detail.param_lv_max (BAL grade1 = 5); HP value = master_bodylvl_status_value.
  New tool `db/lid_fighter.py list|max <name|cid|all>` (game closed, auto-backup .pre-fighter-*).
  Applied: Adolph 11 -> 25, all stats 5, hp 320.
- **Durability/ammo**: DB edits can't stop drain: script `WeaponDurabilityDown` = dur_hit*rate*... + MaintenanceValue.
  Only [cheat] checks in exe are part dur/rest/spare "over max" clamps. Fix = MinHook UObject::ProcessInternal
  (0x10EC70; CallFunction calls it directly for script funcs, line `FUN_14010ec70(this,&frame,result)`).
  FFrame: Node +0x14, Object +0x1C, Code +0x24, Locals +0x2C. Skip {Weapon,WeaponShot,Armor}DurabilityDown
  for BrgPawn_CustomCharaPlayer; drop SetPartRest/SetPartSpare when new < GetPartRest/GetPartSpare.
  Overlay checkboxes + RPC `inf_dur 1|0`, `inf_ammo 1|0`; `ue3_status` shows blocked counters. UNTESTED in game.
- Locker gun: save still holds 9430ce93 = PT_ARM_WP017_0A1 "Demon Gun 1" in soul.cl slot 3 (owner COIN_LOCKER);
  game re-saved it intact; why it is not displayed is unknown (WP025 in the same slot did display).
- Ghidra: new scripts StrRefDecomp.java (string -> decompile refs), SlotAcrossVtables.java. Headless .bat
  splits args on '=' -> DecompAt now accepts `max:N`.

## 2026-10-10 Round 3: code-review fixes + remembered settings + item picker + save queue + stamina
- Code review (15 findings) addressed: ammo now skips BrgWeapon_*.BulletConsumption (9 overrides, void/no parms)
  instead of blocking setters; ProcessEvent parm buffers are 512 B zeroed (engine copies ParmsSize back);
  add_exp RPC removed; single-pass resolver keyed by FName indices with 3s/30s retry backoff; request pump
  gated to PlayerTick (3s any-event fallback); ImGui input queued on window thread and replayed in EndScene;
  atomic save writes (lid_save.save_json: tmp -> verify -> os.replace); caught engine faults latch cheats off
  + banner; checkboxes bound to ue3 state via settings::SetCheat; strict on/off RPC parsing; dead GUID code gone.
- BUG found while fixing: {Weapon,WeaponShot,Armor}DurabilityDown run on the BrgCommonPawn_CustomChara helper
  object, NOT the pawn -> the old "is player" check could never match. Now: owner = this.mPawn (property
  offset read from the UProperty object, Offset @+0x94); weapons: this.mOwnerPawn (BrgWeapon_Base).
- UProperty/UStruct layout (from CallFunction 0x103610): Children @+0x80, Next @+0x60, PropertyFlags @+0x70,
  Offset @+0x94. CallFunction calls ProcessInternal(0x10EC70) DIRECTLY for script funcs (Func swap insufficient).
- Stamina: BrgPawn_CustomCharaPlayer.StaminaConsumption(float) (calls CommonPawn + BrgPawn_Base versions).
- Save queue: overlay -> lid_mod\save_queue.txt (op\targ\tlabel); DLL starts pythonw lid_apply_queue.py
  --pid <game> at launch; after exit: backup .pre-overlay-*, locker delivery (record identical to game-made,
  first type -1 cl slot), fighter_max current|all; results -> save_queue_result.txt shown next launch.
  Verified on a scratch copy of the save.
- Settings: lid_mod\settings.ini ([overlay] borderless/infinite_*; [save_helper] python=).
- Deploy: deploy.ps1 (dll + lid_mod\tools\*.py + assets). Item icons: UI_Icon_<ptid>_SF.upk per item.

## Item icons (2026-10-10) — RE unknown #7 done
- Packages: `BrgGame\CookedPCConsole\UI_Icon_<ID>_SF.upk`; 1022 `UI_Icon_PT_*` (parts). UE3 v861/19, engine 9953,
  LZO (flags=2). Each holds 1 `Texture2D TX_UI_Image_<ID>`, **inline** (not TFC), 1 mip (TMGS_NoMipmaps), **108x132**,
  `PF_A8R8G8B8` (BGRA in memory) x1018 + `PF_DXT5` x14 (*_TOPS_031/032/131/132). Group TEXTUREGROUP_UI, SRGB=false.
  Exceptions: WP000_001_L/_R and PT_NONE_* hold 3 textures (288x144 + _F/_M 108x132); PT_ORB is 288x144.
- **umodel (build 1590, 2023-07-07, `C:\lid-tools\umodel\`) FAILS** on these: "Serializing behind stopper" in
  FByteBulkData. Cause: **Brg FByteBulkData header = 24 bytes: flags u32, elementCount i32, sizeOnDisk i64,
  offsetInFile i64** (stock UE3 = 32-bit size/offset). Applies to any Brg asset with bulk data (meshes/sounds too).
- Extractor = own: `C:\lid-ghidra\uedump` `icons` command (Icons.cs: in-memory LZO decompress, name/import/export
  tables, Texture2D tagged props, 64-bit bulk, BGRA/DXT1/3/5 decode, minimal PNG writer):
  `dotnet C:\lid-ghidra\uedump\bin\out\uedump.dll icons <CookedPCConsole> C:\lid-tools\icons_raw "UI_Icon_PT_*.upk"`
  -> 1032 PNGs in ~2 s, 0 failures, log `_extract_log.tsv`. Build: `dotnet build -c Release -o bin/out`.
- Index/atlas: `C:\Python314\python.exe -I tools\build_item_index.py` -> `assets\icons_atlas.png` (2560x1664, 1004
  cells 64x64, 40 cols), `assets\items.tsv` (1355 rows, 1323 with icon), `assets\README.txt`. Deterministic output.
- Names: `master_part.name` = "<sct>.<txtid>" -> `master_text(sct, id, lang='int')` (id alone is ambiguous: same
  TXT id also used by RAGEMOVE_NAME/DES, WEAPON_CTGRY). 12 parts have no text; PT_NONE_* are named "None" (skipped).
- Game's own icon lookup (Ghidra): natives FUN_140c36480/35270/34e40/37360 (`UBrgUIResource_ItemIconManager::
  execGetPartIconImage` -> vtbl+0x2C0) build `Texture2D'UI_Icon_<X>.TX_UI_Image_<X>'` with X = ptid, or (bool arg)
  X = TMap<FName,FString> lookup at obj+0x54C (FUN_140fd8f20). **No grade/series fallback in the game** -> parts
  without a package simply have no icon in-game (likely why Demon Gun 1 / WP017_0A1 doesn't show in the locker).
- Fallback chosen for the picker (data-driven, first hit wins): exact 1003 | alias Fists->WP000_001_R_M 1 |
  strip `_G` 193 (identical master_part_asset) | same 3D asset set (master_part_asset) 81 (e.g. *_005 -> *_004,
  *_0N5 -> *_0N4, MIL_*_1001 -> *_1G1) | asset-group majority `master_part.series` 30 (WoT MIL 1005..1108 ->
  MIL_*_005/007; approx) | weapons: last digit g -> PT_ARM_WPnnn_00g/lower/_001 15 (Demon Gun, GLIDER, KAMAS-WoT;
  approx, different model) | none 32 (Kat/Tank Commander collab armor, Landmine, Sand).
- Gotchas: Pillow is in the *user* site-packages, which `-I` hides -> script appends `site.getusersitepackages()`;
  `-I` also ignores PYTHONIOENCODING (script sets stdout errors=replace). Names contain U+2019 (’): add to ImGui
  glyph ranges. Downscale done in premultiplied alpha (RGBa) to avoid dark fringes. Assets are game IP: never commit.

## 2026-10-10 Round 3b: second code review (15 findings) addressed
- Spawn parm strings now engine-owned (FString::op= 0x19850; tiny leak per call by design); QueuePartEquip
  whitelists Head/Arm_R/Arm_L/Body/Legs and PT_* ids (no "All"/"NONE"/empty).
- Requests run ONLY inside PlayerTick (any-event fallback only if PlayerTick never seen for 10 s after install).
  Live GameInfo = PlayerController.WorldInfo(Actor) .Game(WorldInfo); UserData = GameInfo.mUserData. No scans.
- DebugPartEquip sets GameInfo.mbDeadInfoOutputCancel: DWORD at its offset snapshotted/restored around the call.
- Resolver: in-world attempts capped at 10 then "unavailable"; GNames ids cached.
- Borderless applied on the worker (window-ops loop + event), never on the render thread (WM_SIZE deadlock).
- Input: own WM_* -> ImGui translation; Ctrl/Shift/Alt captured with GetKeyState on the window thread;
  buttons/keys reset when the menu opens.
- Settings: recursive mutex; no persistence while faulted; ModDir init in DllMain; scanner starts after Load.
- Save queue: helper takes lid_mod\save_helper.lock (O_EXCL, stale > 5 min) from game exit until done and
  re-checks game_running() right before writing; DLL waits for the lock on its own thread before loading the
  queue (buttons disabled until ready). Results file renamed to .shown.txt after display. Locker retry is a
  structured flag. pythonw picked by numeric minor version; status says if the helper is stuck in the game's job.
- lid_save: asserts -> SaveFormatError; fsync before os.replace. Already-maxed fighter now reports OK.
- Item picker: gameIcon flag = UI_Icon_<ptid>_SF.upk exists (the game has NO icon fallback; picker hides parts
  without one by default, warns in detail panel). Atlas texture: exact size, else pow2-padded; managed pool.
