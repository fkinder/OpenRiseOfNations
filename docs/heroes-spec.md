# Heroes: generals, patriots, campaign heroes, their powers and plunder

Status 2026-10-24: 🟡 decoded from the decompiles (`re/decomp/heroes`, `re/decomp/heroes2`, local) and the PDB layouts; not
checked against a recording. Companion of `docs/transport-spec.md` §4 (the cast order, `do_cast`, `is_castable`,
`pay_cast_costs`, `get_job_time`: the general powers are spells cast through the same machinery) and `docs/combat-spec.md`
(every `has_general(mask, type)` term in the stat getters, healing and attrition).

## 0. Summary

A **hero** is a unit whose type has `unit_flags2 & 0x20` (`UnitData::is_hero`, `h` of the `p`-flag family: generals 54..57,
patriots 352..357, campaign heroes 358..375). Every hero owns a `Hero` record (a `Caster`: a list of **active spells** with
expiry frames) in `heroes.lists[who]`, slot kept in `Unit +0x86` (the field the engine calls `cara`; a unit is never both a
caravan and a hero). `HeroesData::find_hero(x, y, who, mask, type, extra)` answers "is there a hero of `who` (of `type`,
with unit mask `mask`) whose **radius** covers (x, y)"; `ObjectData::has_general` wraps it for a unit (returning the unit
itself when it is the hero). The radius comes from `HeroData::get_radius`: `general_radius` scaled by the general upgrade
level, hero-specific multipliers and the patriot bonuses. The **powers** (DECOY 0x27a, AMBUSH 0x27b, FORCED MARCH 0x27c,
ENTRENCH 0x27d, SUPPLIES 0x27e) are crafts cast by the general: mana (`UnitType.mana`, 1000 for generals) is burned
(`mana_burn`) and the effect is a unit flag plus an `ActiveSpell` that `Caster::process_spells` expires. **Government
heroes** (patriots, `unit_flags & 0x4000000`, `is_gov_hero`) are trained by the Senate for the current government and
respawn after `gov_hero_respawn`. **Plunder** (`Unit::plunder`, `Build::plunder`) pays the killer / razer and is boosted by
THE DESPOT and Spitamenes in range.

## 1. Data

| Thing | Where | Value |
|---|---|---|
| General I..IV | units 54..57, `WHERE Fort`, mana 1000, flags `lmhcb` | `is(54)` is the general family (rules_units.cpp sets `unit_flags2 |= 0x20` for `is(54)`) |
| Patriots | 352 The Despot, 353 Senator, 354 Monarch, 355 President, 356 Comrade, 357 CEO; `WHERE Senate`, mana 1000, flag `p` | `unit_flags & 0x4000000` = gov hero; `is_gov_hero_type`: 352 ≤ t ≤ 357 (`TypeData::is_gov_hero_type` @470680) |
| Campaign heroes | 358 Alexander, 359 Napoleon, 360 Parmenio, 361 Antipater, 362 Ptolemy, 363 Darius, 364 Memnon, 365 Spitamenes, 366 Chandragupta, 367 Porus, 368 Wellington, 369 Archduke Charles, 370 Djezzar, 371 Fouché (spy-like, mana 3000), 372 Schwarzenberg (commando-like), 373 Blücher, 374 Kutusov, 375 Paoli | `WHERE None`: placed by scenarios / CtW; mana 2000 (4000 Alexander, Napoleon) |
| `general_radius` | rules | "6 tiles" |
| `mil_patriot_radius_bonus` / `econ_patriot_radius_bonus` | rules | 3 / 1 tiles |
| `general_speed_bonus`, `forced_march_speed`, `alexander_forced_march_speed` | rules | 3m, 42m, 3/2 (movement-spec; `movement.cpp:134` TODO) |
| `general_rally_armor`, `general_building_range`, `general_building_attack`, `general_upgrade_los` | rules | 2 armor, 1 tile, 1, 2 (combat-spec terms) |
| `parmenio_radius_adjust`, `parmenio_ambush_time`, `parmenio_ambush_cost` | rules | 8.8 fixed (×/256), 3/1, 1/1 |
| `wellington_radius`, `kutosov_radius` | rules | percent |
| `terra_cotta_range` | rules | added to the radius for the owner of wonder 0x211 (Terra Cotta Army) |
| `porus_decoys` (8.8 fixed), `kutosov_decoys` (percent), `decoy_time` (2500 frames) | rules | decoys |
| `antipater_entrench_bonus/rate` | rules | entrench |
| `gov_hero_respawn` | rules | "30 seconds" |
| `general_plunder`, `caravan_plunder`, `plunder`, `aztec_plunder`, `thedespot_plunder`, `thedespot_unit_plunder`, `inca_refund`, `aztec_slay_bonus/cap`, `russian_plunder_steal`, `lakota_raze_price`, `german_mine_cost`, `tikal_temple_cost` | rules | plunder §7 |
| Spells | crafts 0x27a DECOY, 0x27b AMBUSH, 0x27c FORCED MARCH, 0x27d ENTRENCH, 0x27e SUPPLIES (`craftrules.xml`: cost, mana, `DURATION` +0x1d4, `DURATION_UPGRADE` +0x1d8, job time) | the engine's `Type.duration`, `duration_upgrade`, `mana` |

Leader fields: `+0x420` `num_heroes` (count of `heroes.lists[who]` slots in use, including closed ones below the high
mark), `+0x56fe` `num_units[t − 50]` (alive units per type: the "owns(t)" test), `+0xa48` `gov_hero_frame`, `+0x914`
`plunder_scale` (float, scenario `set_plunder_scale`; < 0 = none), leader flags `0x4000` kAmbushActive, `0x8000`
kMarchActive (any hero of the leader is currently ambushing / marching; `Leader::verify_spell_flags` clears them when none
is). Unit masks (objects.hpp): `kUnderCover 0x800`, `kCloakRunning 0x2000` (AMBUSH on this hero), `kMarchRunning 0x8000`,
`kEntrenched 0x2000000`, `kDecoy 0x1`; `umask2::kAntipaterEntr 0x1000`.

Structures (PDB): `HeroData` 0x30 = `Caster` (`+0x4 Array<ActiveSpell> active_spells` {count +0x8, capacity +0xc, grow
+0x10 (i16), data +0x14}) + `+0x24 i16 hero` (own slot), `+0x26 i16 o`, `+0x28 u8 hero_flags` (bit 0 = in use), `+0x29 i8
who`. `ActiveSpell` 0xc = `{TypeIndex t, int start, int frame}` (frame = expiry). `HeroesData` = `PtrArray<Hero>[8]`
(`heroes`, 20 preallocated records per leader, grows by one).

## 2. Hero records

### 2.1 `Heroes::init_hero(who, o)` @73a330 → slot (called from `Unit::init` for every `is_hero()` unit; `Unit.cara = slot`)

```
n = L.num_heroes
slot = first i < n with !(lists[who][i].hero_flags & 1), else:
    if n < lists[who].count: slot = n (a preallocated record)      // count = 20 at init
    else: append a new Hero record; slot = count − 1
L.num_heroes = max(L.num_heroes, slot + 1)
H = lists[who][slot]: H.hero = slot; H.o = o; H.who = who; H.active_spells.count = 0; H.hero_flags = 1
```

### 2.2 `Heroes::close_hero(who, slot)` @73a480 (from `Unit::close` when `is_hero() && cara >= 0`)

`Caster::process_spells(H, H.o, H.who, force = 1)` when `H.o >= 0 && H.who >= 0` (expires every active spell: §4.3), then
`hero_flags &= ~1`, `who = −1`, `o = −1`; then `L.num_heroes` shrinks while the last slot is unused (`while n > 0 && !(lists[n−1].flags & 1): n−−`).

### 2.3 `Hero::init` @739cf0 / `Hero::close` @739cc0: the same field writes for the editor / scenario path (no list bookkeeping).

### 2.4 `Hero::walk_data`: `active_spells` then the bytes `+0x24..+0x2a` (hero, o, hero_flags, who) — the save walk
(state-walk.md "Heroes").

## 3. Finding a general

### 3.1 `HeroData::get_radius()` @739e50 → tiles

```
r = (general_radius * (get_general_upgrade(who) + 3)) / 2          // get_general_upgrade: stats.cpp (0x305..0x307 → 0..3)
U = units[who][o]
if U.is(0x168 PARMENIO 360, 0): r = (parmenio_radius_adjust * r + ((parmenio_radius_adjust * r) >> 31 & 0xff)) >> 8   // signed /256
if U.is(0x170 WELLINGTON 368, 0): r = (wellington_radius * r) / 100
if U.is(0x176 KUTUSOV 374, 0): r = (kutosov_radius * r) / 100
if has_wonder(who, 0x211): r += terra_cotta_range
if U.is(352,1) || U.is(354,1) || U.is(356,1): r += mil_patriot_radius_bonus      // Despot, Monarch, Comrade (strict)
if U.is(353,1) || U.is(355,1) || U.is(357,1): r += econ_patriot_radius_bonus     // Senator, President, CEO (strict)
return r
```
(Checked in the disassembly: the mil ladder jumps to the `ADD mil_patriot_radius_bonus` when one of 352 / 354 / 356
matches, the econ ladder to the `ADD econ_patriot_radius_bonus` when one of 353 / 355 / 357 matches, and a hero that
matches neither returns `r` unchanged (`JZ 0073a164` → plain return). A hero matches at most one family.)

### 3.2 `HeroesData::find_hero(x, y, who, mask, type, extra)` @73a1b0 → slot or −1

```
for i in 0 .. L(who).num_heroes − 1:
    H = lists[who][i]; if !(H.hero_flags & 1) continue
    U = units[who][H.o]; if !U.is_active() || !U.is_on_map() continue
    if type >= 0 && !U.is(type, 0) continue
    r = H.get_radius()
    if vector_dist(|x − U.x|, |y − U.y|) − extra <= r * 0xc0 && (mask == 0 || (U.unit_masks & mask)) return i
return −1
```
(`vector_dist` = the `max + min²/2max` metric of combat-spec. Walk order: slot order, first hit wins.)

### 3.3 `ObjectData::has_general(mask, type)` @646b00 → o of the hero or −1 (this = any object)

```
if is_unit() && is_hero() && (mask == 0 || (unit_masks & mask)) && (type < 0 || is(type, 0)): return o   // the unit is its own general
extra = is_building() ? (type.y_size + type.x_size) * 0x60 : 0
if type >= 0 && L(who).num_units[type − 50] == 0: return −1                                     // cheap owns(t) test first
return find_hero(x, y, who, mask, type, extra)
```
Note the return of `find_hero` is a **hero slot**, which callers only compare with −1 (`>= 0`), except `do_cast` /
`cast_disband`-style callers that use it as an index: keep the slot semantics.

### 3.4 `LeaderData::has_general(x, y, mask, type, extra)` @6e1050: `(type < 0 || num_units[type − 50] != 0) ? find_hero(x, y, who, mask, type, extra) : −1`.

### 3.5 Where it is used (all already written as `has_general` / `find_hero` TODOs in the engine):
`UnitData::speed` / `get_speed` (forced march, general speed bonus, siege ×2 with 0x162 Ptolemy… see movement-spec and
`movement.cpp:134/153/733`), `armor`, `attack`, `max_range`, `los` (combat-spec §5.1 terms), `get_damage`,
`train_time` / `construct_time` (Antipater, Djezzar…), `process_supply` / `process_healing` (attrition.cpp:359..510),
`do_move` / `do_group_move`, `do_cast`, `get_job_time`, `Build::process`, `cast_bribe`, `is_valid_target`, `take_damage`,
`capture_city` (capture.cpp:486 Spitamenes), `Object::disband`, `WallData::armor`, and `Unit::process_cloak` (§6).

## 4. The general powers (crafts 0x27a..0x27e)

All are cast through `Unit::do_cast` (transport-spec §4.5, untargeted branch: `spell_flags & 0xe == 0`) after
`SpellType::cast → is_castable == 3`. `pay_cast_costs` charges the craft cost and `mana_burn += mana` (the hero's mana is
`UnitType.mana`; `UnitData::mana()` is the current maximum; the AI casts only when `mana_burn + spell.mana <= mana()`).
Durations: `D = spell.duration (+0x1d4) + get_general_upgrade(who) * spell.duration_upgrade (+0x1d8)` frames.

### 4.1 `cast_supplies` @6714c0 (0x27e): `units[who][o].mana_burn = 0` — the caster's own mana refills. (Nothing else.)
Dead in the shipped data: craft 0x27e has `FROM None`, so `is_castable`'s owner chain never passes (spells-spec §1-§2).

### 4.2 `cast_march` @671500 (0x27c)
```
H = heroes[who][U.cara]; H.active_spells.push({0x27c, start = frame, frame = frame + D})
U.unit_masks |= 0x8000 (kMarchRunning); L.flags |= 0x8000 (kMarchActive); (sound)
```

### 4.3 `cast_ambush` @671610 (0x27b)
```
D as above; if U.is(0x168 PARMENIO, 0): D = (parmenio_ambush_time * D + ((...) >> 31 & 0xff)) >> 8
H.active_spells.push({0x27b, frame, frame + D})
U.unit_masks |= 0x2800 (kCloakRunning | kUnderCover); L.flags |= 0x4000 (kAmbushActive); (sound)
```

### 4.4 `Caster::process_spells(o, who, force)` @739ad0 — every frame for every hero (`Unit::process`), and with `force = 1`
from `close_hero` / `unit_stop_effect`
```
changed = false
for i = count − 1 down to 0:                                      // newest first
    S = active_spells[i]
    if !force:
        if S.frame >= frame:                                      // not expired
            if S.t == 0x283 && ((S.start − frame + 1) & 0x1f) == 0: (shockwave graphics)   // craft 0x283 JAM RADAR: never active in the shipped data (spells-spec)
            continue
    // expired (or forced):
    if S.t == 0x27b: U.unit_masks &= ~0x2800; (sound AMBUSH_OFF when console && !force); changed = true
    if S.t == 0x27c: U.unit_masks &= ~0x8000; (sound MARCH_OFF when console && !force); changed = true
                     // (both paths share the `changed = true` label in the asm, @739c45)
    active_spells.remove(S)
if changed: Leader::verify_spell_flags(L); (options rebuild)
```
`Leader::verify_spell_flags` @6ce190: clears `L.flags & 0x4000` when no in-use hero of the leader has `unit_masks & 0x2000`,
and `0x8000` when none has `0x8000`.

### 4.5 `cast_entrench` @672820 (0x27d) — entrenches idle friendly land combat units in the general's radius
```
r = heroes[who][U.cara].get_radius()
find_units(U.x, U.y, SEARCH_FRIENDLY, who, r * 0xc0, 0x200, FILTER_ALL, 0, 0, ?, 1)     // the found list in g.objects (search.hpp)
for each found unit F (o, w):
    T = F.type: require T.has_objmask(0x20) (land combat), T.attack (+0x1e8) != 0, T.unit_flags2 (+0x2c8) & 0x10000
    require F.is_captain(); require attack_dist(U → F) <= r * 0xc0; require F.is_idle()
    require no enemy combat unit within 0x600 of F (find_unit SEARCH_ENEMY, 0x600, 0x2000, FILTER_COMBAT)
    if !field_battle (semaphore byte2 & 0x10):
        if !U.is(0x169 ANTIPATER, 0): owner = WData(F).owner; require owner >= 0 && is_ally(w, owner)   // only on own / allied land
    for each unit C of F's o_down chain (captain first), while active && on map:
        if C's action is ATTACK (type 10): (tx, tw) = its target
        kMultimove off, path cleared, close_orders(0), clear_partial_path, update_action
        if tx >= 0: add_attack_order(C, tx, tw, QUEUE_NEW, 0, 0)
        if !(C.unit_masks & 0x2000000): (entrench graphic event)
        C.unit_masks |= 0x2000000 (kEntrenched)
        if U.is(0x169 ANTIPATER, 0): C.unit_masks2 |= 0x1000 (kAntipaterEntr)
        C.des_angle (+0x5c) = C.angle (+0x50)
```
No draws. (Entrenchment's effect on armor is combat-spec's `entrenchment_modifier` term.)

### 4.6 `cast_create_decoy` @674370 (0x27a) — copies of nearby units with `kDecoy`
```
n = (get_general_upgrade(who) + 2) * 5
if U.is(0x16f PORUS 367, 0): n = (porus_decoys * n + (… >> 31 & 0xff)) >> 8
if U.is(0x176 KUTUSOV 374, 0): n = (kutosov_decoys * n) / 100
existing = number of active captain units of `who` with kDecoy
army = U's army when (U.unit_masks2 & 0x40000) else −1       // Object::get_army
made = false
for o2 = unit_mark − 1 down to 0 (reverse slot order):
    V = units[who][o2]: active, captain, !(V.unit_masks & kDecoy), V.type.domain == 0, V.is_combat (vslot +0x120) != 0,
        type not 50..53, !V.is_caravan(), !V.is(0x3d MERCHANT, 0), o2 != o
    require vector_dist(|U − V|) < heroes[who][U.cara].get_radius() * 0xc0
    if find_nearby_spot(U.type, U.x, U.y, &sx, &sy, 0x180, −1, 0, 0, FILTER_NOT_ME, U.o, who, 0, 0, −1, 0, −1) != 0: break
    d = init_unit(who, V.type.index, sx, sy, −1, −1, −1); if d < 0: break
    if army >= 0: Army::add_unit(army, d)
    made = true
    if types[d].pop (+0x2f0) != 0: Leader::track_unit_type(who, V.type, −1, d); L.pop (+0x940) −= (decoy ? 0 : pop)… 
        (the decompile: `+0x940 -= (unit_masks & 1 ? 0 : type.pop)` evaluated BEFORE the kDecoy bit is set, so the full pop is
        removed again: a decoy costs no pop); L.+0x93c −= 1 (active count); L.+0x808 −= 1 (units_built)   ❓ exact field names
    for each unit C of d's chain (captain down): C.unit_masks |= kDecoy; C.mana_burn = 0; C.update_hits(0); C.update_los
        (vslot +0x160); update_speed(C); update_armor(C)
    existing++; if existing >= n: goto done
if !made:                                                     // nothing created: refund
    unpay_cost(spell, who, o, −1) (vslot +0xd8); mana_burn −= min(mana_burn, spell.mana)
done: (sound, console only; SoundGlobal::random is the UI stream, not game_random)
```
Decoys die after `decoy_time` frames (`Unit::process`: TODO elsewhere; `kDecoy` units deal no damage and are excluded from
most AI counts — see combat-spec).

### 4.7 `Unit::think_spellcaster()` @5f27a0 (AI; `ai_unit.cpp:62` TODO) — one `game_random` draw per call on the general path
For a general (`is_hero()` and not special): when idle (`!is_moving()`): count own decoys; if `mana_burn + mana(0x27a) <=
mana()`: cast DECOY; else when moving: `if !(L.flags & 0x8000) && (rnd16 & 1) == 0 (draw: Random::get(0, 0xffff), test
`& 0x80000001` != 0 → odd)`: FORCED MARCH if affordable, else AMBUSH when on own land and affordable. Then
`Random::get(0, 0xffff) % 3 != 0 && get_army() < 0 → return` (second draw) before the spy branches (COUNTERINTEL 0x277,
BRIBE 0x275, SABOTAGE 0x281 for spies / commandos; `is(0x3a SPY)`). Exact structure in the decompile; port with the AI
phase, keeping both draws.

## 5. Government heroes (patriots)

### 5.1 `LeaderData::get_gov_hero()` @6e0600 → type or −1
```
if L.flags2 & 0x1000: return −1                                    // no patriots (scenario / option)
gov = get_gov(L)                                                     // the current government tech
for t in 50 .. 401: if unittypes[t].unit_flags & 0x4000000: for i in 0..2: if get_preq(t, i) == gov: return t
return −1
```
(The engine has `get_gov_hero` already; check it follows this loop order.)

### 5.2 Spawn and respawn
- `Build::activate` @623e20 (Senate 0x1b6 activated, not loading): if `L.gov_hero_frame >= 0` and no friendly gov hero on the
  map (`find_unit(x, y, SEARCH_FRIENDLY, who, −1, 0x20000, FILTER_GOV_HERO, …) < 0`) and `L.gov_hero_frame <= frame` and
  `get_gov_hero() >= 0`: `train(get_gov_hero())` (build.cpp:687 TODO).
- `Build::finished` @628490 (a Senate finishing, after `gain_tech`): `t = get_gov_hero()`; if `t >= 0`: if
  `L.gov_hero_frame < 0`: `train(t)` and return 1; else the same "none on the map and frame reached" test → `train(t)`.
- `Build::process` @61edf0: when `L.gov_hero_frame == frame + 1`: a Senate (0x1b6) trains `get_gov_hero()`; a building
  with `flags & 0x20` (capital?) ❓ when the leader has no senates (`num_buildings[SENATE] + upgraded == 0`) trains it instead
  (construct.cpp:752 has the frame test).
- `City::assimilate` @738e90: the newly assimilated city's building trains the gov hero when none is on the map and the
  frame is reached (cities.cpp:524 TODO).
- Writers of `gov_hero_frame`: `Unit::close` @60ee50 (a dying / closed unit with `is_gov_hero()`): `L.gov_hero_frame =
  frame + gov_hero_respawn * 15` (the constant is in seconds, 15 frames per second); `Leader::gain_tech` @6dcb60 sets it to
  `1` when a government tech is gained (so the Senate trains the new patriot at once; techs.cpp:875 has the frame-0 case).
- Call site of `Caster::process_spells(o, who, 0)`: `Unit::process` @610bc0, every frame, right after the unit's order
  processing prelude, for every unit with `is_hero()` (record `heroes[who][cara]`) or `is_special()` (record
  `specials[who][cara]`: spies / commandos have the same `Caster` list for their own spells).

### 5.3 THE CEO (`umask2::kTheCeo`, `update_ceo_position`) and the economic patriots' effects are in economy-spec / fog-spec.

## 6. Cloaking and cover (`Unit::process_cloak` @5e1c20)

Called from `Unit::process` when `((frame + o) & 0xf) == 0` (every 16 frames, right after the `+0x3d` byte decays by
`/4` and before the every-32-frames `process_attrition`). Inside, the "tick" below is `((frame + o) & 0x1ff) == 0`
(every 512 frames).
```
on the tick: if !(unit_masks & kCoverBlown 0x10000): unit_masks &= ~kBlowCover 0x1000; (particles when cloaked);
             unit_masks2 = (unit_masks2 & 0x40) ? &~0x40 : &~0x20     ❓ two umask2 bits (0x20 / 0x40) not named in the engine
             else: unit_masks2 &= ~0x40; unit_masks &= ~0x10000
unit_masks2 &= ~kPassiveCloak 0x8000
if (unit_masks2 & 0x80 (i8 sign)) && owner(WData) >= 0 && (owner == who || mutual ally (diplo 2 both ways)): unit_masks2 |= 0x8000
if !(unit_masks & kCloakRunning):
    if !(L.flags & 0x4000 kAmbushActive): unit_masks &= ~kUnderCover 0x800
    else: unit_masks |= / &= ~ 0x800 according to "some other in-use hero of `who` (not this unit) with (unit_masks & 0x3000) == 0x2000,
          on the map, within its get_radius() * 0xc0 of this unit" (slot order, first hit)
if has_tribe_bonus(who, 0x12) && type.domain == 0: unit_masks = (tile medium == 0x30 TREE) ? |0x800 : &~0x800
```
So an ambushing general hides every friendly unit inside its radius (`kUnderCover`); `kCloakRunning` is the general's own
flag. No draws.

## 7. Plunder

### 7.1 `Unit::plunder(o_killer, who_killer, aztec, despot)` @605b00 — this = the dying unit (called from `Object::take_damage`
on a kill and `Object::disband`; combat_damage.cpp:601 TODO)
```
if this.unit_masks & kDecoy: return
if who_killer != who && thedespot_unit_plunder != 0 && despot == 0:
    despot = find_hero(x, y, who_killer, 0, 0x160 THEDESPOT, 0) >= 0 || find_hero(x, y, who_killer, 0, 0x16d SPITAMENES, 0) >= 0
    (each find_hero only when L(who_killer).num_units[type] != 0)
V = this; if type.unit_flags & 0x10 (amphibious / barge) and inside_up >= 0: V = the carried unit (inside_up / inside_up_who)   ❓ (+0x28/+0x3e = inside_up)
if (o_killer, who_killer) == (o, who):                                       // self-disband (Inca refund)
    T = V.type; if V.is(0x42 MILITIA family, 0): T = unittypes[V.militia_from (+0x54) or the peasant base]
    for good 0..5 where type_avail(who, good): r = (inca_refund * T.cost(good, who, −1, −1, 1, 1, −1)) / 100; resources[good] += r (console message)
patch = get_patch_version() (≥ 9 in the EE)
amount = 0; good = WEALTH
hero = V.is_hero(); supply = V.is_supply() (unit_flags2 & 0x40); cara = V.is_caravan()
if hero: amount = general_plunder; else if supply || cara: amount = caravan_plunder
if aztec || despot:
    // the poorest good: for good 0..5 (not KNOWLEDGE) with type_avail(who_killer): key = resources[good] + rnd(25)  — DRAW per good
    //   (Random::get(0, 0xffff) % 0x19; strict '<' keeps the first minimum; start 99999999, default good 2 ❓ (param_1 = 2))
    bonus = aztec ? min((units_killed (+0xdc ^ 0x62766) + 1) * aztec_slay_bonus, aztec_slay_cap) : 0
    if despot: bonus += thedespot_unit_plunder
    if amount < bonus: amount = bonus; good = poorest
if !aztec && aztec_plunder != 0 && has_tribe_bonus(who_killer, 0 AZTEC): amount = (aztec_plunder * amount) / 100
    // (disassembly: the aztec == 0 path alone reaches the multiplier @6061df; a slay-bonus kill skips it)
if (scripted_rules || ctw) && !ScenarioData::plunder: return
if L(who_killer).plunder_scale >= 0: amount = (int)((float)amount * plunder_scale)
resources[who_killer][good] += amount; (console message)
```
The pre-patch-9 branch (same logic, different nesting) is dead in the EE (`get_patch_version()` ≥ 9).

### 7.2 `Build::plunder(o_attacker?, who_razer)` @623660 — this = the building being razed / destroyed (from
`Object::do_damage` when the last hit point goes, and `cast_disband`; combat_damage.cpp:422 TODO)
```
if who_razer == who && has_tribe_bonus(who, 0x13 LAKOTA) && lakota_raze_price != 0: return
if type.plunder_value (+0x2d4) == 0: return;  good = type.plunder_good (+0x2d8); if good < 0 || !type_avail(who_razer, good): return
v = plunder_value
if has_tribe_bonus(who, 0xc GERMAN) && type == 0x1a3 (mine): v = ((100 − german_mine_cost) * v) / 100; if v == 0 return
if who_razer == who:
    if !(build_masks & 0x1000 kComplete?): return                                             ❓ bit name
    if type == 0x1b5 (temple) && has_wonder(who, 0x214 TIKAL): v = ((100 − tikal_temple_cost) * v) / 100
if !(build_masks & 0x1000): v = (int)((float)v * min(1.0f, (float)construct_hits (+0x4c) / (float)hits(0)))   // partial refund while building
if who_razer == who: (own city building: full value only when the town's city is own: get_town → city.race == who; else falls to the enemy formula)
else: v = (plunder * v) / 100
if v != 0:
    if aztec_plunder && who_razer != who && has_tribe_bonus(who_razer, 0): v = ((aztec_plunder + 100) * v) / 100
    despot = 0
    if who_razer != who: if owns(THEDESPOT) && find_hero(x, y, who_razer, 0, 0x160, (y_size/2 + x_size) * 0xc0) >= 0: despot = (thedespot_plunder * v) / 100
                         if owns(SPITAMENES) && find_hero(…, 0x16d, …) >= 0: despot = (thedespot_plunder * v) / 100
    if is_scenario && !ScenarioData::plunder: return
    if plunder_scale(who_razer) >= 0: v = (int)(v * scale); despot = (int)(despot * scale)
    if has_tribe_bonus(who, 0xd RUSSIAN) && russian_plunder_steal && who_razer != who:
        bucket_add(who, good, v); bucket_add(who_razer, good, despot)        // the victim keeps the plunder, the razer only the despot share
    else: bucket_add(who_razer, good, v + despot)
    (console messages)
```
No draws. `bucket_add` is the leader's income bucket (economy-spec).

## 8. Randomness
- `Unit::plunder`: one `rnd(25)` per available good (0..5 except KNOWLEDGE, in good order) when `aztec || despot` — up to 5
  draws per killed unit of an Aztec or despot-assisted killer.
- `think_spellcaster`: two draws per call on the general path (§4.7).
- Decoys: `init_unit` draws, as in transport-spec §11. `find_hero`, `get_radius`, `has_general`, the casts and
  `process_spells`: none.

## 9. What the port needs
1. `engine/sim/heroes.hpp/.cpp` (new): the `Hero` record array per leader (20 preallocated, grow by one), `init_hero` /
   `close_hero` wired into `init_unit` / `unit_close` (the engine's equivalents), `Unit.cara` reused as the slot (document it,
   or add `hero_slot` and keep `cara` for caravans — the original shares the field; sharing keeps the save walk identical),
   `hero_get_radius`, `find_hero(x, y, who, mask, type, extra)`, the real `has_general` (replace the −1 stubs in `groups.cpp`,
   `fog.cpp`, `capture.cpp`, `stats.cpp:56`) and `leader_has_general`, `process_spells` per hero per frame (frame-spec: where
   `Unit::process` calls `Caster::process_spells(o, who, 0)` — ❓ confirm the call site order inside `Unit::process`),
   `verify_spell_flags`.
2. `spells.cpp` (from the transport port): the five casts of §4; `is_castable` paths for 0x27a..0x27e (transport-spec §4.2 lists
   only 0x28a; decompile `re/decomp/transport/SpellTypeData_is_castable` lines 60–62 for the generic unit test and the
   `switch` for 0x27b..0x27e — most have no case: castable = 3 after the ownership chain).
3. Gov heroes: §5.2 in `build.cpp:687`, `construct.cpp:752`, `cities.cpp:524`, plus the `gov_hero_frame` writer (❓).
4. Plunder: §7 in `combat_damage.cpp:422/601` and `Object::disband`; `capture.cpp:481` Spitamenes / Despot test via `find_hero`.
5. Cloak / cover: §6 in the unit frame (`Unit::process_cloak` call site: frame-spec).
6. Every `TODO(heroes)` in `movement.cpp` (134, 153, 733), `attrition.cpp`, `construct.cpp:729/793`, `econ_income.cpp:682`
   now resolves through `has_general` / `find_hero`.
7. Acceptance: zero warnings; `ron-check` 0; `sim-all 600 --ai-every 8` 156/156 self-check OK; digests change only in
   scenarios with heroes present (list them: generals in Fort scenarios, patriots after a Senate, campaign heroes in
   Alexander / Napoleon / New World / Cold War scenarios) and each change is explained (speed bonus, armor, healing,
   plunder); a `--roundtrip` across a frame with an active AMBUSH / MARCH; `ron-sim` dump shows a general's radius and the
   `kUnderCover` units while ambushing.

## 10. Evidence

| Function | Address | Use |
|---|---|---|
| `UnitData::is_hero` / `is_gov_hero` / `TypeData::is_gov_hero_type` | 46ce60 / 46ce70 / 470680 | §0, §1 |
| `Heroes::init` / `init_hero` / `close_hero` / `close`, `Hero::init` / `close` / `walk_data` | 73a970 / 73a330 / 73a480 / 73a8e0 / 739cf0 / 739cc0 / 739e20 | §2 |
| `Unit::init` (hero registration, `+0x86`) / `Unit::close` (close_hero) | 612100 / 60ee50 | §2 |
| `HeroData::get_radius` | 739e50 | §3.1 (❓ exit path) |
| `HeroesData::find_hero` / `ObjectData::has_general` / `LeaderData::has_general` | 73a1b0 / 646b00 / 6e1050 | §3 |
| `Caster::process_spells` / `Caster::init` / `Leader::verify_spell_flags` | 739ad0 / 7399a0 / 6ce190 | §4.3 |
| `SpellType::cast_supplies` / `cast_march` / `cast_ambush` / `cast_entrench` / `cast_create_decoy` | 6714c0 / 671500 / 671610 / 672820 / 674370 | §4 |
| `Unit::think_spellcaster` | 5f27a0 | §4.7 |
| `LeaderData::get_gov_hero`; `Build::activate` / `finished` / `process`; `City::assimilate` | 6e0600; 623e20 / 628490 / 61edf0; 738e90 | §5 |
| `Unit::process_cloak` | 5e1c20 | §6 |
| `Unit::plunder` / `Build::plunder` (callers: `Object::take_damage` 652020, `Object::disband` 6455c0, `Object::do_damage` 64a480, `SpellType::cast_disband` 671ed0) | 605b00 / 623660 | §7 |
| PDB: `HeroData`, `Caster`, `CasterData`, `ActiveSpell`, `HeroesData` | `re/decomp/heroes_types*.txt` | §1 |
| Callers lists | `re/decomp/heroes_callers*.txt` | §3.5 |
