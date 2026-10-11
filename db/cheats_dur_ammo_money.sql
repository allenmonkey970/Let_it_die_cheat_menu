-- LET IT DIE (Offline 5.0.1) DB cheats: infinite durability, infinite ammo, free buy/craft.
-- Target: BrgGame\Content\masters.db  (reversible: restore masters.db.pre-overlay-cheats).
-- Applied by the lid-overlay-mod project. See MODLOG.md.

-- ===== INFINITE DURABILITY (weapons PTTP_ARM + all armor) =====
-- Zero every durability-consumption field so durability never drops.
UPDATE master_part SET
  dur_hit = 0, dur_shoot = 0, dur_rage = 0,
  dur_drp = 0, dur_drp_pe = 0, dur_drp_met = 0, dur_drp_arc = 0,
  dur_drp_ams = 0, dur_drp_rft = 0, dur_drp_hzm = 0, dur_drp_hvn = 0,
  dur_dwn_fort = 0, dur_dwn_hunter = 0,
  -- also stop magazine-capacity degradation
  cap_drp = 0, cap_drp_pe = 0, cap_drp_met = 0, cap_drp_arc = 0,
  cap_drp_ams = 0, cap_drp_rft = 0, cap_drp_hzm = 0, cap_drp_hvn = 0,
  cap_dwn_fort = 0, cap_dwn_hunter = 0;

-- ===== INFINITE AMMO (ranged weapons only: capacity > 0) =====
-- Huge magazine + reserve so you effectively never run out / reload.
UPDATE master_part SET capacity = 9999, spare = 9999 WHERE capacity > 0;

-- ===== FREE MONEY (Kill Coins): buying items + crafting/upgrading costs 0 =====
UPDATE master_item SET buy_money = 0, buy_rank_money = 0;
UPDATE master_part_research SET craft_money = 0, lvup_money = 0, lvup_money_c = 0;
