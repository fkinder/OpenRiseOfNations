# Crafts and spells (0x275..0x2ab): spies, special forces, packers, conversions, city casts

Everything the original does with `SpellType::cast_*` that is not already in `docs/transport-spec.md` §4 (cast machinery, TRANSPORT)
and `docs/heroes-spec.md` §4 (DECOY, AMBUSH, FORCED MARCH, ENTRENCH, SUPPLIES, `Caster::process_spells`). Sources: decompiles in
`re/decomp/spells/` (jobs `re/jobs/spells*.txt`), asm in `re/decomp/spells_dis/`, `Data/craftrules.xml`, `rules.xml`. Pseudo-code
is C-like; `o, who` = the caster, `to, tw` = the target; coordinates are world units (tile = 0xc0); `K(x)` = a rules constant.
Marks: ✅ read from asm/decompile, ❓ unresolved (counted in §11). The decompiler names the SpellType index field `_padding_`
and mixes it up with `from` (+0x3c); every use below was checked against asm (`[ESI+4]` = spell index).

## 0. Summary

* 55 crafts. Only these can ever run in the shipped data, because `is_castable` first requires `obj.is(FROM) || obj.is(FROM2) ||
  tribe/graft variants` and **`FROM None` is type −1, which never matches** (`ObjectTypeData::is` skips `param < 0`), except the
  hard-coded exemptions 0x29a DISBAND and 0x28a TRANSPORT: BRIBE 0x275, COUNTERINTEL 0x277, DECOY/AMBUSH/MARCH/ENTRENCH
  0x27a..0x27d, INFORMER 0x27f, SABOTAGE 0x280, SNIPER 0x281, the eight PACK/UNPACK, CIVILIAN 0x293, TO ARMS 0x294, DEPOPULATE
  0x286 (a building queue item), BUILDAIR 0x295 (production-spec §6.7), DISBAND, TRANSPORT. **Dead in the data** (FROM none, no
  caller bypasses `cast`): PILFER 0x276, ASSIMILATE 0x278, RALLY 0x279 and JAM RADAR 0x283 (no `cast` case, `cast_rally` /
  `cast_jam_radar` are empty), SUPPLIES 0x27e, BLOW TREAD 0x282, PARADROP 0x284, RESEARCH PROJECT 0x285, MOVE CAPITAL 0x287
  (only the scenario editor calls `cast_move_capital` directly), LOCK/UNLOCK GATE 0x288/9 (`is_castable` returns 0 for both),
  IMMORTAL 0x297/0x298, radar UPGRADE 0x299. They are specified so a mod / scenario can still reach them.
* No craft is a nuke, terraform or armageddon trigger. 0x296 MORPH is only the stand-in for "the order's spell is a unit type"
  (`eff = 0x296` for job time / cost); 0x29b RAZE, 0x29c..0x2a3 DECLARE WAR (+target) and 0x2a4..0x2ab CANCEL ALLIANCE (+target)
  are price tags only (§6.8). Nukes are missiles (aircraft-spec §8).
* Randomness: no `game_random` in any `cast_*` except dead PILFER; the draws come from callees (§8).

## 1. The table (✅ `SpellType::init` @674a80, `SpellType::cast` @676ce0)

SpellType fields: `+4` index, `+8` JOB_TIME (frames), `+0xc` RES_TIME, `+0x3c` FROM (the casting object type = `where`),
`+0x1c8` `spell_flags`, `+0x1cc` SPELL_RANGE × 0xc0, `+0x1d0` MANA, `+0x1d4` DURATION × 15, `+0x1d8` DURATION_UPGRADE × 15,
`+0x1dc` FROM2 (default 0), `+0x1e0` `use[6]` = COST2 (the casting price; COST is the unused research price). Init side effects:
`unittypes[FROM/FROM2].unit_flags (+0x2b8) |= 2`, `buildtypes[..].build_flags (+0x2c0) |= 0x20000000`, `Type.+0x40 = FROM's +0x40`.
`FLAGS` letters (a = bit 0 .. m = bit 12): `a 1` must be researched (`has_spell`; **no craft has it**, so RES_TIME is dead), `b 2` targets
units, `c 4` buildings, `d 8` area (target = the order's x,y), `e 0x10` own objects, `f 0x20` enemy, `g 0x40` does not cancel other
orders, `h 0x80` allied, `i 0x100` vehicles only, `j 0x200` non-vehicles only, `k 0x400` at-peace, `l 0x800` does not blow the cloak,
`m 0x1000` "use craft as group". `spell_flags & 0xe != 0` = targeted (`do_cast` §3).

| idx | name | flags | `cast` handler | JOB | MANA | RNG tiles | FROM / FROM2 | DUR/upg (s) |
|---|---|---|---|---|---|---|---|---|
| 275 | Bribe | `fcbhm` 0x10a6 | cast_bribe | 100 | 1000 | 6 | Spy / The Senator | |
| 276 | Pilfer | `fcm` 0x1024 | cast_pilfer (dead) | 100 | 1000 | 1 | none | |
| 277 | Counterintel | `febchm` 0x10b6 | is_valid_target, cast_counterintel | 38 | 500 | 10 | Spy / Scout | |
| 278 | Assimilate | `ecfh` 0xb4 | cast_assimilate (dead) | 100 | 2000 | 1 | none | |
| 279 | Rally | `glm` 0x1840 | none (no case) | 0 | 1000 | 0 | none | 15 |
| 27a..27d | Decoys / Ambush / March / Entrench | `lm` / `glm` / `glm` / `lm` | heroes-spec §4 | 100/0/0/120 | 1000 | 0 | General | 0/30+15/10+5/0 |
| 27e | Restock | 0 | cast_supplies (dead) | 200 | 0 | 0 | none | |
| 27f | Informer | `fbcml` 0x1826 | cast_double_agent | 40 | 500 | 10 | Spy | |
| 280 / 281 | Sabotage / Sniper | `fcm` 0x1024 / `fbm` 0x1022 | cast_sabotage | 150 / 60 | 1000 | 1 / 12 | Commando | |
| 282 | Blow Tread | `fbim` 0x1122 | cast_blow_tread (dead) | 25 | 500 | 6 | none | |
| 283 | Jam Radar | `gm` 0x1040 | none | 0 | 1000 | 12 | none | 15 |
| 284 | Paradrop | `dlm` 0x1808 | cast_paradrop (aircraft-spec §9) | 25 | 500 | 0 | none | |
| 285 / 286 / 287 | Research Project / Raze (=DEPOPULATE) / Move Capital | 0 | research_project / depopulate / move_capital | 300 / 200 / 600 | 0 | 0 | none / Small City / none | |
| 288 / 289 | Lock / Unlock Gate | 0 | lock_gate / unlock_gate (dead) | 0 | 0 | 0 | none | |
| 28a | Transport | 0 | transport-spec §5 | 0 | | | none | |
| 28b/c, 28d/e, 28f/90, 291/2 | Pack/Deploy: artillery, machine gun, merchant, fishermen | 0 | cast_pack / cast_unpack | 80, 50, 148, 40 | 0 | 0 | Catapult+Katyusha, Machine Gun, Merchant, Fishermen | |
| 293 / 294 | Civilian / To Arms | 0 | cast_civilian / cast_to_arms | 5 | 0 | 0 | Militia / Citizen+Scholar | |
| 295 | Build Aircraft | 0 | none (`Group::action_spell` queues carrier planes) | 0 | | | Aircraft Carrier | |
| 296 | Morph | 0 | none | 150 | | | none | cost 5o/5m, RES 500 |
| 297 / 298 | Go Ranged / Go Melee | `g` 0x40 | immortal_arch / immortal_hinf (dead) | 60 | 0 | 0 | none | |
| 299 | Upgrade (radar) | 0 | `set_type(current_upgrade(AIRDEFENSE 523))` on the building (dead, §6.7) | 300 | | | none | cost 6t/8m |
| 29a / 29b | Disband / Raze | 0 | cast_disband / none | 0 | | | none | |
| 29c..2a3, 2a4..2ab | Declare War / Cancel Alliance ×8 | 0 | none | 0 | | | none | cost 8g |

COST2 (casting price): 27e 2w/2m/2c/2o/2f, 285 10g, 287 50t/50s/50g/50w, 296 5o/5m, 299 6t/8m, 29c.. 8g; all others free
(the "COST 20g/20w" of generals / informer is the unused research price). Pay / refund rules: production-spec §2.1.2 (owned spell),
transport-spec §4.3. Mana: `pay_cast_costs` adds `MANA` to `mana_burn`; a refund subtracts `min(mana_burn, MANA)`.

## 2. Predicates

### 2.1 `SpellTypeData::is_castable(o, who, p3)` @675bc0 → 3 castable ✅ (this extends transport-spec §4.2)

```
if !(obj.is(S.from,0) || obj.is(S.from2,0) || obj.is(T) || obj.is(graft(from2)),0)):   // T = S.from if tribe_can_type else its type_graft(who)
    if S.idx != 0x29a && S.idx != 0x28a: return 0
if obj.is_unit() && (unit_masks & kDecoy) && !S.is_pack() && !S.is_unpack(): return 0     // (transport-spec says 1: wrong, asm = XOR EAX)
switch S.idx:                                                                              // anything else: return 3
 0x27e: if !is_map_unit return 0; return (tile of unit has TData 0x100 && W-cell owner == who && city at (x,y) with city.race(+0x5f) == who) ? 3 : 2
 0x284: if !is_map_unit return 0; o = terr_owner(x,y); return (o >= 0 && is_ally(who, o)) ? 3 : 2
 0x286: c = build.city; if c >= 0 && p3 == 0 && !CityData::depop_can_queue(c) (a DEPOPULATE already queued): return 1
        else return is_castable(DISBAND 0x29a, o, who, p3)                                 // recursion with the DISBAND rules
 0x287: if build.is_unassimilated: return 0; if p3 == 0 && researching(0x287, o): return 0
        C = cities[build.city]; if (C.was_capital_flags(+0x68) >> C.founder(+0x60)) & 1 && C.founder != who && leaders[C.founder].active: return 0
 0x288, 0x289: return 0                                                                    // never castable (transport-spec merged them with 0x28a)
 0x28a: return obj.is_cargo() ? 3 : 0
 0x28b 0x28d 0x28f 0x291 (pack):   is_map_unit && !(unit_masks & kPacked)
 0x28c 0x28e 0x290 0x292 (unpack): is_map_unit &&  (unit_masks & kPacked)
 0x294: is_map_unit && (has_tech(tribe type of types[66]) || has_tech(types[67]) || has_tech(types[68]))   // MILITIA / MINUTEMAN / PARTISAN (or the tribe graft)
        && (terr_owner(x,y) < 0 || terr_owner == who || is_ally(who, terr_owner))
 0x297, 0x298: is_map_unit
 0x29a: if p3 == 0 && researching(0x29a, o): return 0; if !is_building return 0
        B = build; if !(B.type.build_flags(+0x2c0) & 0x10) || (B.flags & 0x20 kCity):
            c = B.city; if c >= 0:
              if !(B.flags & 0x20): if cities[c].is_disbanding() > (p3 != 0 ? 1 : 0): return 0       // disbanding = sum over the city's buildings of count_queue(DISBAND)
              else: if cities[c].flags(+4) & 0x10 CAPITAL: return 0
                    for i in 0..7: if i != who && (cities[c].was_capital_flags >> i & 1) && leaders[i].active: return 0
```
`is_map_unit` = valid, active, on map. Results 1 and 2 are "not castable now" for `cast` (needs 3) but non-zero for
`Group::action_spell` (§3.4) which only skips on 0.

### 2.2 `SpellTypeData::is_valid_target(who, o, tw, *reason)` @6763c0 ✅ (reason codes feed the UI message)

```
reason = 0; if (flags & 0xe) == 0 || (flags & 8): return 1                      // untargeted / area spells
T = objects[tw][o]
if T.is_unit() && !(flags & 2) && S.idx == 0x280 && !T.has_objmask(0x200000 vehicle): reason = 1; return 0   // SABOTAGE hits vehicles though it lacks 'b'
if T.is_building() && !(flags & 4): reason = 2; return 0
if tw == who: if !(flags & 0x10): reason = 3; return 0
elif enemy(who,tw): if !(flags & 0x20) reason 6, return 0
elif ally(who,tw):  if !(flags & 0x80) reason 4, return 0
elif peace(who,tw): if !(flags & 0x400) reason 5, return 0                        // (neither: falls through)
reason = 8
if T.is_unit(): if (flags & 0x100) && !vehicle return 0; if (flags & 0x200) && vehicle return 0
0x276: T must be an active building with kCity (dead)
0x27f: if T.is_infiltrated(who) return 0                                          // already planted
0x275: if has_general(T, 0, 0x165 CEO) >= 0 || T.is_gov_hero() || tw == who: return 0
       if ally(who,tw): return teams_locked() && !leaders[tw].active              // only a dead ally's property in locked-team games
       if !enemy(who,tw) return 0
       if T.is_unit(): not Spy(0x3a), (T.attack() != 0 || T.is_hero()), not caravan, not worker, not supply, not merchant, T.type.domain == 0
       else:           terr_owner(T) == who                                      // buildings only on the briber's own land
0x281: if has_general(T,0,0x165) >= 0 || T.is_gov_hero() || T.type.domain != 0: return 0
       if !(T.attack() || T.is_hero() || T.is_supply()) && !T.is(0x45 Scout family): return 0
       if T.is_caravan || T.is_worker || T.is_merchant: return 0
0x277: if !T.is_seen(who,0): return 0
       if tw == who:    return T.is_infiltrated(-1)                              // any plant on my object
       if ally(who,tw): return T.is_infiltrated(-1) && !T.is_infiltrated(who)
       if enemy(who,tw): return T.is(0x3a Spy)
       return 0
return 1 (reason 0)
```

### 2.3 `SpellTypeData::get_range(_, who, to, tw)` @676a80 ✅ (units of 1/192 tile; 0 = no range test)

```
R = S.spell_range; if R == 0 return 0
if idx == 0x275 || idx == 0x277:
    R += get_spy_upgrade(who) * K(spy_bribe_upgrade_range=2) * 0xc0               // get_spy_upgrade = number of owned techs 0x308..0x30a (0..3)
    if has_wonder(who, 0x211 TERRA COTTA) R += K(terra_cotta_range = 0) * 0xc0
    if to >= 0 && (T.type.is_siege || T.is_supply || T.is_building): R = 0xc0     // only adjacent
if idx == 0x27f: R += get_spy_upgrade(who) * K(spy_informer_upgrade_range=3) * 0xc0; if to >= 0 && T.is_building: R = (R + 1) / 2
if idx == 0x281 && to >= 0 && T.is_unit && T.has_objmask(0x200000 vehicle): R = 0xc0
return R
```

### 2.4 `SpellTypeData::get_job_time(o, who)` @675800 ✅ (adds the arithmetic transport-spec §4.4 only listed)

`t = S.job_time`; every `/100` is C truncating division, every `/2` and `/4` the signed shifts the asm uses.
```
0x27d: if who>=0: if tribe_bonus(who,10): t = K(french_entrench)*t/100
       if o>=0 && obj.is_active(): t = (K(antipater_entrench_rate)*t + ((.. >> 31) & 0xff)) >> 8      // ❗ the result of is(0x169) is DISCARDED in the asm:
                                                                                                 // every general's entrench is scaled (rate 128 = x1/2)
0x275, 0x27f: if who>=0 && has_preq(who, 0x304 SPIES_CRAFT_FASTER): t /= 2
0x28b, 0x28c: if who>=0 && tribe_bonus(who,8): t = K(turk_pack)*t/100
       if leaders[who].num_units[Napoleon 0x167] != 0 (+0x59cc) && obj.is_unit && has_general(obj,0,0x167) >= 0 && obj.type.is_siege: t = K(napoleon_pack)*t/100   // x50%
       if obj.is(0x10f,strict) || obj.is(0x116,strict): t /= 2          // Howitzer 271, Katyusha 278
       elif obj.is(0x111,strict): t = (t + ((t>>31)&3)) >> 2            // Rocket Artillery 273: /4
0x28d, 0x28e: if obj.is(0x7d Heavy Machine Gun, nonstrict): t /= 2
0x280, 0x281: if who>=0: if tribe_bonus(who,9): t /= 2; if o>=0 && obj.is(0x177 Commander Paoli, strict): t = 10
```
Examples: Catapult pack 80; Katyusha 40; Rocket Artillery 20; Howitzer with Napoleon in range 20; Heavy MG 25; Spy bribe with the tech 50.

## 3. `Unit::do_cast` @5ebfe0: what transport-spec §4.5 left out

### 3.1 Targeted branch (`spell_flags & 0xe != 0`, spell is a spell type) ✅ asm 5ec135..5ec9f0

```
// order: ox, whom (+8,+0xc), uid (+0x10, i16), x, y (+0x14,+0x18), paid (+0x1c), spell (+0x20)
T = objects[order.whom][order.ox]
if !(T.flags & kValid): c = T.get_captain(); if c != order.ox: order.ox = c; order.uid = objects[whom][c].uid           // follow the chain top
if !T.is_on_map(): (c, w) = T.get_inside(); order.ox = c; order.whom = w; kill order if c < 0 || w < 0; order.uid = ..   // target the container
if !is_valid_target(who, order.ox, order.whom, NULL): kill_current_order
if spell == 0x275 && self.is(0x161 Senator, strict):
    ow = terr_owner(T.x, T.y); if ow < 0 || !is_ally(who, ow): (feedback) kill_current_order                          // the Senator bribes only on own / allied land
self.spell_target_o (+0xa2) = ox; self.spell_target_who (+0xa8) = tw; self.spell_target_uid (+0xa6) = ox < 0 ? 0xffff : T.uid
if !(flags & 8):                                                   // single target
    if !(T.flags & kValid): kill; (tx, ty) = T.pos
    radius = T.is_building ? (T.type.x_size + T.type.y_size) * 0x30 : T.unit_radius (type +0x244)    // 0x238 / 0x234 / 0x244
else: (tx, ty) = (order.x, order.y), radius = 0
R = get_range(S, who, ox, tw)
if R != 0 && vector_dist(|self - t|) > R + radius:                 // walk into range
    a = find_angle(self - t)
    if find_nearby_spot(self.type, tx, ty, &nx, &ny, R + radius - 0xc0, R + radius - 0x30, 0, a, FILTER_NOT_ME, o, who, 0, 0, -1, 0, -1) != 0: return   // keeps the order
    if vector_dist(|(nx,ny) - t|) > R + radius: return
    add_move_order(nx, ny, 1, 0, QUEUE_FIRST, 0, R + radius, -1, -1); return
if tw != who && ox >= 0:                                           // reveal and face
    if !T.is_seen(who,0): kill
    self.flags |= kAttacking (0x80)
    if !(self.seen_mask(+0x40) & 1<<tw): if !world.is_seen(self.pos/384.., tw): self.seen_mask |= 1<<tw; self.update_local_seen()  else self.seen_mask |= 1<<tw
    set_angle(find_angle(T.pos - self.pos), .., 1)
anim: if self.is(0x3a Spy): 0x275 -> CHAR_ATTACK3(13,0,0); 0x27f -> ATTACKWALK(10,0,0); else T.is(0x3a) ? ATTACK2(12,0,0) : ATTACK1(11,0,0)
      elif self.is(0x45 Scout family): 0x280 -> ATTACKWALK; 0x281 -> ATTACK3; else as the spy else-case
      else: ATTACK1 (11, 0, 1)
if !(unit_masks & kCasting): (presentation: enemy-sees-you sound / message, bubble); unit_masks |= kCasting (0x20000)
if is_cloaked() && !(flags & 0x800) && spell != 0x281 && !self.is(0x3a): unit_masks |= kBlowCover|kCoverBlown (0x11000)       // every frame while casting
if spell == 0x275: unit_masks2 |= 0x60                                                                                        // ❓ both bits, every frame
spell_time += 1; if (i16)spell_time < get_job_time(S, o, who): return
if is_cloaked() && !(flags & 0x800): unit_masks |= 0x11000                                                                    // at completion: no spy / sniper exemption
spell_time = 0
(flags & 8) ? SpellType::cast(S, o, who, order.x, order.y) : SpellType::cast(S, o, who, ox, tw)
order.paid = 0; kill_current_order(0)
```
`pay_cast_costs` (once, first frame) and the failure path are transport-spec §4.5. Cloak: `UnitData::is_cloaked` @60a6a0 ✅ =
`unit_masks & 0x800 (kUnderCover) || type.unit_flags & 0x4000 || unit_masks2 & 0x8000 (kPassiveCloak) || (type.unit_flags & 0x40000 && no current order)`;
the 512-frame decay and ambush cloak are heroes-spec §6. A blown cover (0x10000) stops `process_cloak` clearing 0x1000 until the next tick.
`UnitData::is_casting(spell)` @60a530: 0x27c -> `unit_masks & 0x8000`, 0x27b -> `unit_masks & 0x2000`, anything else 0.

### 3.2 Untargeted pack / unpack branch (spell_time == 0 on the first frame, then every frame) ✅

```
first frame, pack  (spell in {28b,28d,28f,291}): if unit_masks & kPacked: kill order
    if is_merchant(): scene.recalc_builds = 1; update_gpiece();  angle := guys[0].angle;  set_anim(CHAR_PACK, 0, 1)
first frame, unpack (is_unpack): options.rebuild = 1; if !(unit_masks & kPacked): kill order
    if is_rare_collector():                                          // type 61, 62, 400 or is(317 Fishermen)
        if !good_merchant_spot(tile(x), tile(y)): kill_current_order(0); (feedback INVALID); return          // cost refunded by kill
        set_new_location((x/48)*48 + 0x18, (y/48)*48 + 0x18, 1, 1)                                           // snap to the centre of a 48-unit cell
    angle := guys[0].angle; set_anim(CHAR_UNPACK, 0, 1)
every frame (incl. the first): if S is pack or unpack && self.type.is_siege && has_general(self, 0, 0x162 THE MONARCH) >= 0:
    spell_time += 1; for each guy g (i < guy_count): g.+0x74 += 1; if g.+0x78 <= g.+0x74: g.+0x74 -= 1        // extra tick (anim clock ❓ fields)
spell_time += 1; if spell_time < get_job_time(S) return; spell_time = 0; SpellType::cast(S, o, who, 0, 0); order.paid = 0; kill_current_order(0)
```
The hero animations of §4 of heroes-spec (`is_hero` casters, ATTACK1/2/3 for 0x27b/0x27c/0x279) are already in the port.

### 3.3 Non-spell order ("upgrade by casting") ✅ asm 5ecebf..5ed03a
`order.spell` < 0x275 or > 0x2ab (a unit type): job time and cost use 0x296. At completion, if `is_captain`:
`set_type(current_upgrade(who, order.spell), 0)`; then if the unit's tile has `TData & 3 == 3` and `find_building_at(tile, SEARCH_FRIENDLY, who)`
returns an `is(0x1a4 University)` building: `go_inside(b)`, `come_out(0)`. (`Unit::set_type` RNG: production-spec §6.8.1.) ❓ the spell-type
array index used by `pay_cast_costs` for a unit-type "spell" is the unit type index itself (`spelltypes.list[order.spell]`).

### 3.4 Issuing: `Group::action_spell(spell, ox, whom, x, y)` @6fe1a0 (replay cmd 23 SPELL) 🟡
```
GroupOut::validate_spell(...) != 0 -> return        // @7095e0, also sim state: BRIBE target checks of is_valid_target repeated per member; PARADROP needs an allied-land member
if spell is a spell type and !(leader_options.+0x1c & 1 "use craft as group") and (S.flags & 0x1000):
      best = group member with is_castable==3, !is_casting(spell) and the largest mana_left()   (others skip)
unit-type spell with domain 2 (aircraft): BUILDAIR path (production-spec §6.7)
TRANSPORT: members that are land, !is_busy, castable, has_spell, mana ok, with a water spot -> add_cast_order(0x28a, QUEUE_NEW, explicit)   (transport-spec §9)
else for each member m (valid, on map):
    if spell != 0x29a && != 0x286 && is_castable(S, m, who, 0) == 0: skip
    unit: mana_left (+ refund of its current cast's mana) < S.mana -> "not enough craft": skip
          !S.can_pay_cost(who, -1, m, 1) -> skip; !is_valid_target(who, ox, whom) -> skip
          wrong packed state (pack on packed / unpack on unpacked) or already is_casting(spell): skip
          if !is_busy: if target != (order.target): spell_time = 0, store target (+0xa2/+0xa6/+0xa8); queue = (S.flags & 0x40) ? kill leading CAST orders, QUEUE_FIRST : QUEUE_NEW
                       add_cast_order(ox, whom, x, y, spell, queue, explicit = 1)
    building: if !obj.vslot(0xc) || get_job_time == 0: pay_cast_costs; cast(S, m, who, 0, 0)   else Build::queue_up(spell, 1)   // production-spec §4 queue item
```
The queue item completes in `Build::finished` (production-spec line 2619): `if is_spell_type(t) && has_spell(t): SpellType::cast(o, who, 0, 0)`.

## 4. Spies and special forces (cast bodies)

### 4.1 `cast_bribe(o, who, to, tw)` @672d00 ✅ (target conversion; `swap_team` and `capture_city` are capture-spec §3-§5)
```
if who == tw: refund (unpay_cost(who, o, -1); mana_burn -= min(mana_burn, MANA)); return
if ally(who, tw) && (!teams_locked() || leaders[tw].active): refund; return                       // gate repeats is_valid_target for allies
T = objects[tw][to]
if T.is_unit && has_general(T, 0, 0x165 CEO) >= 0: refund; return                                  // CEO radius protects (heroes-spec §3.3)
if T.is_unit:
    (console: S_SPY_*_DONE_BRIBE messages)
    leaders[who].units_bribed(+0x8f4) += 1                                                          // ❓ name
    Unit::swap_team(T, who)                                                                         // vslot +0x94: whole chain recreated for `who` (RNG: Guy::init_real per guy)
    return
if T.flags & kCity (0x20):                                                                          // a city centre: only a dead teammate's, in locked teams
    if !ally(who,tw) || leaders[tw].active || !teams_locked(): refund; (feedback "cannot"); return
    c = T.build.city; if c >= 0: Cities::capture_city(who, c, tw) [capture-spec §4]; else refund
    return
if terr_owner(T) != who: refund; (feedback); return                                                // other buildings: own land only
if !(enemy(who,tw) || (ally && !leaders[tw].active && teams_locked)): refund; return
n = Build::swap_team(T.build, who); T.build.close(); new = objects[who][n]
if !(T.flags & 4 kActive-like) && game.patch_version > 3: new.mask_me(1, REGEN_FORCE); new.mark_behind_tiles(1) else: (vslot 0x1a8) ; new.mask_me(1, REGEN_TOTAL)   // ❓ wall/pathing refresh details (capture-spec §5.2)
```
Conversion therefore needs: `unit_swap_team`, `build_swap_team`, `cities_capture_city` (all in `capture.hpp`) plus the stat. No cost beyond MANA.

### 4.2 `cast_counterintel(who, to, tw)` @673740 ✅ (note: the caster object is NOT passed; `SpellType::cast` first requires `is_valid_target` and calls it with ECX = who)
```
if !T.is_captain: to = T.get_captain()
if tw != who && tw != own-id && !(mutual_ally):               // enemy spy: kill it
    if T.is_unit && T.is_captain && T.curr_uber_size(0) == 1 && !(unit_masks & kDecoy):
        leaders[who].units_killed(+0x80c) += 1; leaders[who].score_combat(+0x44) += types[T.type].get_kill_value(tw) / 5
    T.die(2 SHOT, -1, 0.0f)                                    // no deaths_current_frame / units_lost bookkeeping here
else: for p in leaders: (messages to console.who if p valid, p != who, p != tw and T.infiltrated & 1<<p); T.infiltrated(+0x3a) = 0   // clears every plant
```
### 4.3 `cast_double_agent` (INFORMER, 0x27f) @673a80 ✅
`options.rebuild = 1; if !T.is_captain: to = T.get_captain(); T.infiltrated(+0x3a) |= 1 << who; T.update_seen(0)` (+ UI messages, and for the local
player a selection-group clear). `is_infiltrated(p)` makes the object visible to p regardless of fog (fog-spec).

### 4.4 `cast_sabotage` (SABOTAGE 0x280 / SNIPER 0x281) @6720e0 ✅
```
if !(T.flags & kValid): refund; return
if T.is_building:
    d = T.build.hits_left() / 2 (signed); d = max(d, K(commando_min_damage = 200))       // ❗ the Elite (0x4d) / Special (0x4b) branches test the SPELL's own index
                                                                                          // (SpellType::is(0x4d,1) -> idx == 0x4d): never true; special/elite_min_damage (400/800) are dead
    if T.build.take_damage(d, 0, 3 SPLODED, -1, 1 quiet, 0x55555555, -1, -1, 0) != 0:
        leaders[who].buildings_destroyed(+0x818) += 1; leaders[who].score_combat(+0x44) += types[T.type].get_kill_value(tw)/5; (Steam stat, local player only)
    return
if T.is_unit && T.is_captain && T.curr_uber_size(0) == 1 && !decoy:
    leaders[tw].deaths_current_frame(+0xa58) += 1 (u16); leaders[who].kills_current_frame(+0xa5a) += 1 (u16); leaders[who].units_killed += 1
    leaders[who].score_combat += get_kill_value(tw)/5
T.die(T.has_objmask(0x200000 vehicle) ? 3 SPLODED : 2 SHOT, -1, 0.0f)
```
### 4.5 Reactive casts (`Unit::target_opportunity` @5fffc0; combat_target.cpp TODO markers) ✅
`target_opportunity(this, atk_o, atk_who)` for a unit `U` that is attacked or sees an enemy:
* `U`'s current order is CAST_SPELL: if `order.spell != 0x280` return; if `order.ox == atk_o || order.whom == atk_who` return; else `U.spell_time /= 2` (signed). Return.
* `U` has no order and no pending action: if `U.is(0x3a Spy)` and the attacker is a unit with `type.domain == 0` and `types[0x275].can_pay_cost(U.who, -1, U.o, 1)`
  and `is_valid_target(0x275, U.who, atk_o, atk_who)` and (`MANA == 0` or `mana_left >= MANA`): `add_cast_order(U, atk_o, atk_who, U.x, U.y, 0x275, QUEUE_NEW, explicit 1)`; return.
  Else if `U.is(0x49 Commando)` with the same four tests for 0x280: cast 0x280 the same way; return. Otherwise the normal combat reaction continues
  (combat-spec §4 `target_opportunity`). The AI `think_spellcaster` (§7 of heroes-spec) is still not specified for spies / commandos ❓.

### 4.6 Dead bodies (documented so mods work)
* PILFER `cast_pilfer` @673e80: target an active city-centre building (or a type with `build_flags & 0x8000000`), `c = build.city >= 0`; pick the good `r` in 0..5
  (skip KNOWLEDGE) available to both leaders minimising `v = mine + (his < 100 ? 100 : 0) + (mine > his ? 1000 : 0)` (last tie wins); `a = his[r]`;
  if `his.+0x95c != 0`: `lvl = city.get_level(); a = lvl * a * 2 / (his.+0x95c + lvl)`; if `city.+0x61 != 0`: `a /= city.+0x61 + 1`; `h = a / 2`;
  `x = h + (h - 1 >= 1 ? game_random(0,0xffff) % h : 0)`; `mine[r] += x; his[r] -= x`. **The only direct draw of the whole file.**
* ASSIMILATE @671820: active non-city building on own land (enemy, or dead ally in locked teams): `build.swap_team(who)` + `close`; a city centre with `tw == who`:
  `city.+0x5f = who`, `Region::fix_borders` if the W-cell region < 0x40, `leaders[who].flags |= kRecompute`; a dead ally's city: `capture_city`.
* BLOW TREAD @672cb0: `if unit.damage == 0 { unit.damage = 1 }`. RALLY / JAM RADAR: no code. SUPPLIES: heroes-spec §4.1.

## 5. Packing (PACK 0x28b/d/f/91, UNPACK 0x28c/e/90/92)

`add_cast_order` remaps 0x28b/0x28c by type: `is(0x7b Machine Gun) -> 0x28d/0x28e`, type 61/62/400 -> `0x28f/0x290`, `is(0x13d Fishermen) -> 0x291/0x292`
(transport-spec §4.1). `TypeData::is_pack` / `is_unpack` = the index sets above (vslot +0x50 / +0x54).
```
cast_pack(o, who) @670be0:   options.rebuild = 1
  if obj.is_active() && on_map:
     if type in {61, 62, 400}:                                        // merchants and fur trappers, not fishermen
         (tx, ty) = (x/192, y/192); for (dx, dy) in {(0,0), (-1,0), (0,-1), (-1,-1)}: World::set_blocked_at(tx+dx, ty+dy, 0)   // free the 2x2 block
         leaders[who].flags |= kRecompute
     unit_masks |= kPacked (0x80000); unit.+0x14c = -1 (❓ name); update_los(); update_seen(0); update_gpiece(); set_new_location(x, y, 1, 1)
cast_unpack(o, who) @6709c0: same guard; unit.+0x14c = -1
     if type in {61, 62, 400}: (tx, ty) = (x/192, y/192); if !good_merchant_spot(tx, ty) return;          // stays packed, order dies in do_cast
         set_new_location(tx*192, ty*192, 1, 1); set_blocked_at(tx+dx, ty+dy, 1) for the same four; scene.recalc_builds = 1; leaders[who].flags |= kRecompute
     unit_masks &= ~kPacked; update_los(); update_seen(0); update_gpiece(); set_new_location(x, y, 1, 1)
```
Predicates (✅): `is_rare_collector` = type ∈ {61, 62, 400} or `is(0x13d)`; `UnitData::is_packing` @60aa60 / `is_unpacking` @60a4b0 = the **current order is a CAST_SPELL order**
(order vslot +0xf4 returns the CastOrder; its `+0x20` spell) whose spell `is_pack` / `is_unpack`; `is_packing_or_unpacking` @60a410 = either; `is_busy` @60a370 = either or `is_entering_or_exiting` @60a6f0 (the current order's vslot +0x11c SpecialAnimOrder has `type` 0 or 1).
(The port's `groups.cpp:682` comment says SPECIAL_ANIM: wrong.) Users: `kill_current_order` (merchant killing a pack order: `update_gpiece`, `recalc_builds`),
`Group::action_attack`, `Group::action_garrison`, `Unit::do_guard`, `Unit::think`, `detect_boat_collision` (below), `calc_gather` (`unit_collect`: a packed collector yields 0 unless called with the unpack flag).
`good_merchant_spot(tx, ty)` @6068a0: the four tiles `(tx,ty) (tx-1,ty) (tx,ty-1) (tx-1,ty-1)` all in bounds with `!(mask & 0x4000)`, `(mask & 3) != 3`, `(int8)mask >= 0`, and
`calc_gather(unit, out, p8=1, p9=1, x = tx*192, y = ty*192) != 0` (a rare / good is collectable from that corner; port: `unit_collect` needs the (x, y) override and the "count unpacking
units" flag, econ_trade.cpp:128).
`Unit::find_merchant_spot(x, y, r, *tx, *ty)` @603ab0: `calc_gather(.., x, y)` must be non-zero, then for `i < radius[r]` over the square spiral (`move_x/move_y`, compass.hpp):
`(tx, ty) = tile + (move_x[i], move_y[i])` in bounds, `good_merchant_spot`, `!invalid_loc(tx, ty)`, `!detect_unit_collision(tx*192, ty*192, 1, 1)` -> first hit.
`Unit::unpack_merchant(r)` @6038e0 (from `think_merchant` / `think`): `find_merchant_spot` fails -> 0; else `unit_masks &= ~kAutoExplore`; `add_cast_order(-1, -1, -1, -1, 0x28c, QUEUE_NEW, explicit 0)`;
then a MOVE_TO order to `(tx*192 + 0x18, ty*192 + 0x18)` is inserted in front (it runs first, UNPACK after arrival); returns 1.
Idle auto-unpack and PACKER_NEVER gating in `think_attack`: combat-spec §4 step 2 and §5 step 4; the cast is `add_cast_order(.., 652, QUEUE_FIRST, 0)`.
`Unit::do_guard` @5e5c70 (guard order idle counter `+0x24`, incremented each call): a PACKED packer (`unit_flags & 4`, `get_packer_stance() == AUTO`, not `is_unpacking`) with
`idle >= (is(0x7b Machine Gun) ? 30 : 70)` does `add_cast_order(-1,-1,-1,-1, 0x28c, QUEUE_FIRST, 0)` and returns; otherwise `set_anim(CHAR_DEFAULT, 0, 1)`.
`Group::action_attack` / `Group::action_garrison` have packer branches I only partly traced ❓: a member that `is_packing_or_unpacking` with `QUEUE_NEW` keeps its cast;
an unpacked-needed member whose target is in range gets `add_cast_order(0x28c)` before the attack; a packing member gets `add_cast_order(0x28b, QUEUE_NEW)` and a MOVE at `QUEUE_LAST`;
garrisoning packers re-queue as `QUEUE_FIRST` / `QUEUE_LAST` / `QUEUE_NEW` depending on `is_packing` / `is_unpacking` (@700a00..700b60).
**Collision**: `detect_boat_collision` @5fa8b0 (a mover shoving an own / allied blocker aside) refuses to push a blocker that is a packer (`unit_flags & 4`) unless it is PACKED, refuses
when it `is_unpacking`, is entrenched (0x2000000) or cargo. The generic soft rule in `collision.cpp` (a mover waits only for a non-pack CAST order, `spell_is_pack`) is already ported;
the remaining packer rule is exactly those two tests in the shove (`collision.cpp` has no `detect_boat_collision` port yet: add them there).

## 6. City, conversion and bookkeeping casts

### 6.1 DEPOPULATE 0x286 / DISBAND 0x29a / RAZE
`cast_depopulate(o, who)` @671e50: if the object is a valid, active building with `kCity`: `cast_disband`; else refund (`unpay_cost(who, o, -1)`).
`cast_disband(o, who)` @671ed0: if the object is valid and an active building: `Build::plunder(B, obj, who)` (own-building refund path, heroes-spec §7.2);
local player: S_ALERT_BUILDING_RAZED; `Object::disband(obj, 0)` (units draw `% 3 + 1`, combat-spec table #11; buildings none). Queue timing, prices: production-spec §4.9 / §2.1.2.
### 6.2 MOVE CAPITAL 0x287 (editor / scripts only) @671760
`if build.city >= 0: for i < leaders[who].num_cities(+0x408): if cities[who][i].flags & 1: flags &= ~0x10; cities[who][build.city].flags |= 0x10; .founder(+0x60) = who`.
### 6.3 RESEARCH PROJECT 0x285 @670990 (dead): `resources[3 KNOWLEDGE] += 50`.
### 6.4 LOCK / UNLOCK GATE 0x288/9 @671480/@671440 (dead): `build.+0x81 = 1 / 0` ❓ (readers of `+0x81` not found; `is_castable` is 0).
### 6.5 CIVILIAN 0x293 / TO ARMS 0x294 @6704a0 / @670880 ✅ (direct callers too: `Group::action_alarm`, scenario funcs `militia_to_citizen` / `citizen_to_militia`, no `is_castable`)
```
old = unit.hits(0); dmg = unit.damage (+0x24)
CIVILIAN: if unit.made_from(+0x54) == 0: made_from = tribe_can_type(types[50]) ? 50 : types[50].type_graft(who)
          set_type(current_upgrade(who, made_from), 0)           // RNG: production-spec §6.8.1
TO ARMS:  made_from = unit.type.index; set_type(current_upgrade(who, MILITIA), 0)
new = unit.hits(0); unit.damage = (new * ((dmg << 8) / old) + ((.. >> 31) & 0xff)) >> 8            // ratio kept, 1/256 resolution
unit.+0xaa = 9 (CIVILIAN) | 0 (TO ARMS) (the byte capture-spec calls `form`); options.rebuild = 1
```
`made_from` (UnitData +0x54) is also what techs.cpp:597 needs. Castable rules in §2.1 (0x294); 0x293 needs `is(0x42 Militia)` only.
### 6.6 IMMORTAL 0x297 / 0x298 @670740 / @670600 (dead): need `is(0xa2)` / `is(0xb8)`; `made_from = type`; `set_type(current_upgrade(who, IMMORTALARCHERS | IMMORTALS), 0)`; same damage ratio; no `+0xaa` write.
### 6.7 0x299: `SpellType::cast`: `B = obj.get_build(); B.set_type(current_upgrade(who, AIRDEFENSE 523), 0)`. **Contradiction with aircraft-spec §7**: `FROM none` makes `is_castable` return 0 and `queue_here` (`where < 0`) refuse it ❓ (check the shipped building queue).
### 6.8 Price tags: `Leader::pay_dow(t, d)` @6d2b10 / `LeaderData::afford_dow(t, d, *missing)` @6d5ce0 ✅
`base = d == 0 ? 0x29c : 0x2a4; cost[r] = types[base + t].get_cost(r, who, -1, -1, 0, 1, -1)` (8 gold). `afford_dow`: returns 1 (nothing to pay) if `t != who` and either diplo is war (both modes), or (cancel only) the pair is not mutually allied; else for every available good `r` with `stock[r] < cost[r]`: remember the first missing good; returns 1 if none, else 0 with `*missing`. `pay_dow`: for each available good:
`stock[r] = max(0, stock[r] - cost[r])` and `escrow[r] (+0x468) = max(0, escrow[r] - cost[r])` (ai-spec §10.1.9).

## 7. Leader / unit bookkeeping

* `LeaderData::has_spell(t)` @6e0bc0: for a spell with flag `a`: tech bit set **and** `type_avail(t,1)`; otherwise `type_avail(t,1)` (every craft, since none has `a`).
* `Leader::verify_spell_flags` @6ce190 and `Caster::process_spells` @739ad0: heroes-spec §4.4 (only 0x27b / 0x27c ever enter `active_spells`; the 0x283 branch is unreachable because JAM RADAR is never cast).
* `Specials` (spies, commandos, ... every non-citizen unit with `type.unit_flags & 0x10`, not a hero / caravan): `Specials::init_special(who, o)` @7401c0 = `init_hero` (heroes-spec §2.1) on `specials.lists[who]` (initial capacity 40,
  records 0x30 bytes: `Caster` + `slot +0x24`, `o +0x26`, `flags +0x28` bit 0 in use, `who +0x29`); `Unit.cara = slot`; `close_special(who, slot)` @740310 = `Caster::process_spells(force 1)`, clear, and shrink `L.num_specials (+0x43c)`
  while the last slot is free. A `Special`'s `active_spells` stays empty (no craft pushes one); the record exists for the save walk (`Specials::walk_data`) and the `Unit.cara` linkage.
* Unit fields touched: `spell_time (+0x98)`, `spell_target_o/uid/who (+0xa2/+0xa6/+0xa8)`, `made_from (+0x54)`, `unit_masks` kCasting/kPacked/kBlowCover/kCoverBlown/kUnderCover, `unit_masks2` 0x60, `infiltrated (+0x3a)`, `seen_mask (+0x40)`.
* `kill_current_order` of a CAST_SPELL order refunds if `paid` (orders-spec §2 / orders.cpp:184, `unpay_cast_costs`); `target_opportunity` §4.5; `UnitData::mana()` @609a50 = type mana, ×(supply_upgrade+1) for supply units, French `is(0x36, strict)` ×(100+french_special_craft)/100, aircraft with wonder 0x21e ×(100+space_air_range)/100.

## 8. Random draws, in order of occurrence

No `game_random` inside any `cast_*`, `is_*`, `get_*`, `do_cast` branch (the only one is dead PILFER, §4.6). Indirect draws (all specified elsewhere):
1. BRIBE (unit): `Unit::swap_team` -> `Objects::init_unit` -> `Guy::init_real`, one `% 100` draw per guy in guy order (production-spec table line 3117, capture-spec §5.3).
2. CIVILIAN / TO ARMS / IMMORTAL: `Unit::set_type` -> one `Guy::init_real` draw per kept squad guy and per crew guy (production-spec §6.8.1).
3. COUNTERINTEL / SNIPER kills: `die(2|3)` death-animation draws (combat-spec §6.2, table #13, #14 for SCORCH only); SABOTAGE building kill: the usual `take_damage` death path.
4. DISBAND of a unit: `Object::disband` one `% 3 + 1`; DEPOPULATE / DISBAND of a building: none; decoys: heroes-spec §4.6 (`init_unit` draws; the SoundGlobal draw is the UI stream).
5. `find_nearby_spot`, `add_move_order`, `set_new_location`, `Build::plunder`, `swap_team` of a building: none.

## 9. What the port needs

Files: extend `engine/sim/spells.cpp/.hpp` (castable / valid_target / range / job time / targeted do_cast / dispatch), new `engine/sim/spells_spy.cpp` (§4), `engine/sim/packers.cpp` (§5, `is_packing` family replaces `groups.cpp:682`, `unpack_merchant`
in `econ_trade.cpp`), `Group::action_spell` + `validate_spell` in `groups.cpp`/`commands.cpp` (replay cmd 23), conversions in `spells.cpp` (§6.5).
`TODO(spells)` / `TODO(packers)` markers to close: `spells.cpp:59/68/147/153/168/189`, `spells.hpp:30/40`, `queues.cpp:237/978`, `orders.cpp:184`, `combat_target.cpp:735/813/821/892/937/942`, `groups.cpp:682`,
`econ_trade.cpp:128/162/206`, `construct.cpp:373` (CIVILIANSPELL castable = §2.1 0x293), `garrison.cpp:588` (militia disarm = §6.5), `techs.cpp:597` (`made_from`), `transport.cpp:67`
(`is_entering_or_exiting` = SPECIAL_ANIM type 0 / 1), `script_funcs_query.cpp:721` (CastOrder spell), `ai_unit.cpp:64` / `ai_army.cpp:1295` (AI, out of scope).
Acceptance (objective):
1. `spell_is_castable` returns exactly the §2.1 value for fixtures covering: FROM mismatch (0), decoy (0), packed / unpacked × 8 pack spells, 0x294 with / without the three techs and on foreign / own / unowned land, 0x286 with a queued DEPOPULATE (1) and without (DISBAND rule), 0x29a on a capital (0), a former capital of an active rival (0), a city with a DISBAND queued (p3 0 -> 0, p3 1 -> 3), 0x288/0x289 (0).
2. `spell_job_time` matches §2.4: catapult 80 / Katyusha 40 / Rocket Artillery 20 / Howitzer + Napoleon in range 20 / Heavy MG 25 / bribe with SPIES_CRAFT_FASTER 50 / Paoli 10 / Spanish sabotage 75; entrench scaled by `antipater_entrench_rate` for any general.
3. Bribing an enemy unit with `mana_burn` accounting: `units_bribed` +1, owner becomes the briber, hit ratio kept; the same call on a CEO-covered unit, on `who == tw`, and on an ally with teams unlocked refunds exactly `min(mana_burn, 1000)`.
4. Sabotage on a 1000-hit building: damage 500; on a 100-hit building: 200; sniper on infantry `die(2)` + counters (+0xa58 / +0xa5a / +0x80c / +0x44 += value/5), on a vehicle `die(3)` and range 0xc0.
5. COUNTERINTEL kills an enemy spy with `die(2)` (no `units_lost`), clears `infiltrated` on own objects; INFORMER sets bit `who` and the fog layer shows the object.
6. A pack / unpack cycle of a catapult takes `job_time` frames (halved by an in-range Monarch: spell_time advances 2 per frame), toggles `kPacked` on completion only, blocks / frees the four tiles for merchants (not fishermen), refuses to unpack on a bad merchant spot and refunds.
7. TO ARMS then CIVILIAN restores the original type and `damage` within 1/256, with the `Guy::init_real` draws in the order of §8 (sim hash stays identical to the oracle).
8. `.rcx` oracle: replays containing command 23 (SPELL; list them with `re/tools/rcxdump.mjs`) replay with identical sync tags for 600 frames after the first cast ❓ (needs the oracle-inventory list of replays that cast).

## 10. Evidence

| Subject | Address | Where |
|---|---|---|
| `SpellType::init`, `SpellTypeData` ctor | 674a80, 6749b0 | `re/decomp/spells/SpellType_init*`, field map §1 |
| `SpellType::cast` (dispatch, arg order) | 676ce0 | `.c` + `spells_dis/SpellType_cast*.asm` (counterintel gets ECX = who) |
| `is_castable` / `is_valid_target` / `get_range` / `get_job_time` | 675bc0 / 6763c0 / 676a80 / 675800 | `.c` + `.asm` (switch on `[ESI+4]`) |
| `Unit::do_cast` | 5ebfe0 | `.c` + `spells_dis/Unit_do_cast*.asm` (targeted 5ec135..5ec9f0, pack 5eca03.., upgrade 5ecebf..) |
| bribe / counterintel / double_agent / sabotage / pilfer / assimilate / blow_tread | 672d00 / 673740 / 673a80 / 6720e0 / 673e80 / 671820 / 672cb0 | `.c` + `.asm` |
| pack / unpack / good_merchant_spot / find_merchant_spot / unpack_merchant | 670be0 / 6709c0 / 6068a0 / 603ab0 / 6038e0 | `.c` |
| is_packing / is_unpacking / is_busy / is_casting / is_cloaked / is_rare_collector | 60aa60 / 60a4b0 / 60a3a0 / 60a530 / 60a6a0 / 46fae0 | `.c` |
| depopulate / disband / move_capital / research / gates / civilian / to_arms / immortal | 671e50 / 671ed0 / 671760 / 670990 / 671480 / 6704a0 / 670880 / 670740 / 670600 | `.c` |
| `Group::action_spell`, `validate_spell`, `process_spell` | 6fe1a0, 7095e0, 948340 | `re/decomp/aircraft/Group_action_spell*`, `re/decomp/spells/` |
| `Unit::target_opportunity` (reactive casts) | 5fffc0 | `.c` lines 225-340 |
| `pay_dow`, `afford_dow`, `has_spell`, `verify_spell_flags`, `get_spy_upgrade` | see names | `.c`, `get_spy_upgrade` asm (loop 0x308..0x30a) |
| `Specials::*`, `Special::*`, `Caster::*` | 7401c0.., 73ff00.., 739980.. | `.c` |
| Callers (who reaches what) | | `re/decomp/spells/callers.txt` |
| Data | | `craftrules.xml` (55 crafts), `rules.xml` (SPY_*, COMMANDO/SPECIAL/ELITE_MIN_DAMAGE 200/400/800, TURK_PACK 100%, NAPOLEON_PACK 50%, ANTIPATER_ENTRENCH_RATE 1/2, TERRA_COTTA_RANGE 0) |

## 11. Unresolved (❓) and contradictions with the existing specs

❓ list: (1) unit `+0x14c` (set to −1 by pack / unpack); (2) guy `+0x74/+0x78` pair in the Monarch speed-up; (3) `unit_masks2` bits 0x20/0x40 (set on every bribe-cast frame, decayed by `process_cloak`); (4) names of `LeaderData +0x8f4` (bribes) and `+0x818`
(buildings destroyed); (5) the mask / REGEN calls after a building bribe (capture-spec §5.2); (6) `GroupOut::validate_spell` per-member details for ENTRENCH / PARADROP; (7) 0x299 reachability vs aircraft-spec; (8) readers of the gate flag `build +0x81`;
(9) order vslot +0xf4 = CastOrder (inferred from its use of `+0x20` as the spell); (10) `spell_target` fields' readers (UI / scenario); (11) wall / `mask_me` details of the building swap; (12) unit-type "spell" cost index (§3.3);
(13) PARADROP/area orders with `ox = −1` index `objects[whom][ox]` before any guard (§3.1); (14) AI `think_spellcaster` for spies / commandos / `Army::use_spies`; (15) oracle replays that cast (§9.8); (16) the packer branches of `Group::action_attack` / `action_garrison` (§5). **16 ❓.**

Contradictions: transport-spec §4.2 (decoy returns 0, not 1; 0x288/0x289 return 0, only 0x28a tests `is_cargo`); transport-spec §4.4 / `spells.cpp:109` (Antipater term applies to every active caster: `is(0x169)` result unused);
aircraft-spec §7 and §9 (0x299 and PARADROP have `FROM none`: never castable); heroes-spec §4 / §4.1 (SUPPLIES 0x27e also has `FROM none`: only the three-case `is_castable` branch exists, no unit can pass the owner chain); heroes-spec §4.4 (`0x283` is JAM RADAR, never active, not a shockwave; `cast_rally` / `cast_jam_radar` are empty);
`groups.cpp:682` (`is_packing` is a CAST order, not SPECIAL_ANIM); `combat_target.cpp:937` (the halving test is `order.ox == atk_o || order.whom == atk_who` -> return, only for spell 0x280).
