# Economy: resources, gathering, income, markets, trade, population

Status: ✅ the income pipeline (`Leader::gather` → `calc_gather` → `calc_resource_caps` → `do_gather`), the per-building
rate formulas, the gatherer bookkeeping, auto-gather target choice and every `game_random` draw of the gathering code are
specified from the decompiles and, for `do_gather`, the `calc_gather` cadence, `find_gather_spot` scoring, markets,
prices, buy/sell, `calc_rare`, trade value/income, pop cap and attrition, checked against the disassembly · 🟡 the cosmetic gather cycles (`do_gather` farm walk, `do_non_flat_gather` woodcutter/miner trips) are
outlined with their exact RNG sites but should be ported line by line from the decompile · ❓ open items in §13. Nothing
is validated against a running game (no replay with economy commands exists on this Mac). **Port:** engine/sim `econ_*.cpp` (docs/sim-implementation.md
"Economy"); corrections found by the port are marked in place and collected in §13.1.

Scope: resources and gathering (this document); construction, training queues, research and `Build::process` belong to
[production-spec.md](production-spec.md) (being written in parallel); this spec only names the production calls that touch
the economy (cost payment, pop cap checks). Read first: [frame-spec.md](frame-spec.md) (frame order, object lifecycle),
[orders-spec.md](orders-spec.md) (order list, `work`, `do_job`, GATHER/GATHER_POINT group actions),
[rules-spec.md](rules-spec.md) §9.4 (goods, lands) and §12 (constants), [state-walk.md](state-walk.md) (`LeaderDataEncrypt`).

Deliverables: this document; `re/tools/gather-ref.mjs` (per-building gather rates, city base income, leader-wide
post-processing, commerce caps and the per-frame stockpile update, reading the rules from `~/Games/RiseOfNations`:
`node re/tools/gather-ref.mjs --examples | --selftest | --constants`). Decompiled C stays local (gitignored):
`re/decomp/econ/` (jobs `re/jobs/dc_econ_1..4.txt`, disassembly `da_econ_1.txt` → `re/decomp/econ/asm/`, stockpile
writer grep `ig_econ_8221.txt`, callers `ct_econ_bucket.txt`), `re/decomp/econ_mkt/` (markets, tribute, rares; jobs
`dc_econ_mkt_*.txt`), `re/decomp/econ_trade/` (caravans, supply, pop cap, attrition; jobs `dc_econ_trade_*.txt`).

## 0. Conventions

- Addresses are `riseofnations.exe` VAs, names are PDB names. Pseudocode is paraphrased, never the decompiler text.
- int32 arithmetic with C truncating division unless noted. `pct(x, p)` = `((p + 100) * x) / 100` (the binary's form of
  every "+p %" bonus; the multiply happens first, in int32). `trunc256(x)` = `x / 256` and `trunc16(x)` = `x / 16`
  truncated toward zero (the binary's add-sign-then-shift sequences).
- `rnd(n)` = `game_random.get(0, 0xFFFF) % n`; `GameAccess::rnd` @43cca0 returns 0 **without drawing** when `n ≤ 1`.
  Inline `Random::get(game_random, 0, 0xffff) % n` sites always draw.
- **Goods** (`TypeIndex`): 0 FOOD, 1 TIMBER, 2 WEALTH, 3 KNOWLEDGE, 4 METAL, 5 OIL (`NUM_GATHER = 6`), rares 6..49
  (`BASE_RARE = 6`, e.g. 28 DIAMONDS, 29 GEMS, 44 COFFEE). Gather buildings: 417 FARM, 418 WOODCUTTER, 419 MINE,
  420 UNIVERSITY, 421 OILWELL, 422 OILPLATFORM; enhancers 423 GRANARY, 424 LUMBERMILL, 425 SMELTER, 426 REFINERY;
  workers 50/51 PEASANTS(KOREAN) (citizens), 52/53 SCHOLARS(KOREAN); 61/62 MERCHANT(DUTCH), 400 FURTRAPPER,
  317 FISHERMEN. The good ↔ building mappings are hard-coded: `BuildTypeData::get_good` @63bd50 (417→0, 418→1,
  419→4, 420→3, 421/422→5), `GoodTypeData::best_gather_type` @670230 (inverse, 5→421), `GoodTypeData::get_enhancer`
  @66fcf0 (0→423, 1→424, 4→425, 5→426).
- **Tribes** (`has_tribe_bonus(n)` @6e1370, n = `TribeIndex`): 2 INCA, 6 ROMANS, 7 EGYPTIANS, 10 FRENCH, 11 BRITISH,
  12 GERMANS, 13 RUSSIANS, 15 JAPANESE, 17 MONGOLS, 18 IROQUOIS, 19 LAKOTA, 20 AMERICANS, 22 DUTCH. It is 0 when
  GameInfo flag 4 is set, when the setup has no starting town and the leader has no city yet, or `tribe < 0`; a CtW
  racial-power bit grants it; `LEADER_RACIAL_POWER_OFF` (flags2 0x40) denies the tribe's own.
- **Wonders** (`has_wonder(t)`): 526 PYRAMIDS, 527 COLOSSUS, 528 HANGINGGARDENS, 531 FORBIDDENCITY, 532 TIKAL,
  533 PORCELAIN, 535 ANGKORWAT, 538 KREMLIN, 539 TAJMAHAL, 540 EIFFELTOWER.
- **Rate units.** Every gather rate in this document is *resources per `GATHER_RATE` frames × 16* (`gather_rate` = 450
  frames; the binary multiplies every XML amount by 16 = `<< 4`). The stockpile gains `rate / (450·16)` per frame with
  the remainder carried (§2.4), so a rate of 160 is 10 resources per 450 frames.
- **`LeaderDataEncrypt`** (LeaderData +0x6eb8 → 0xf8 bytes, state-walk.md): every int is kept XORed with a fixed key in
  RAM; saves and checksums hold plain values. A port stores plain values. Fields used here (per good k = 0..5):

  | field | offset | key | meaning |
  |---|---|---|---|
  | `bucket[k]` | +0x00 | 0x8221 | **the stockpile** (what the HUD shows; costs, tribute, market and plunder change it) |
  | `leftover[k]` | +0x18 | 0x3421 | sub-unit remainder of the per-frame income (§2.4) |
  | `resource_cap[7]` | +0x30 | 0x1281 | commerce cap on the income rate, ×16 (§2.3); index 3 (knowledge) is 999·16; slot 6 is computed but unused here |
  | `over_cap[k]` | +0x4c | 0x8932 | 0 = rate under cap, 1 = capped, 2 = capped at the 999 ceiling (UI tint; also gates the over-cap message) |
  | `resources[k]` | +0x64 | 0x872 | **the gather rate** (×16 per 450 frames) produced by `calc_gather` |
  | `support[k]` | +0x7c | 0x26076 | always reset to 0 by `Leader::gather` (vestigial upkeep; `calc_support` @6ceea0 zeroes too) |
  | `income[k]` | +0x94 | 0x90236 | the capped rate shown in the HUD (before handicap and game-mode multipliers) |
  | `rate[k]` | +0xac | 0x73862 | not written by the gathering code (❓) |
  | `bonus[k]` | +0xc4 | 0x6722 | reset to 0 by every `calc_gather` recompute (❓ no writer found in the economy code) |
  | `epoch[4]` | +0xe8 | 0x63187 | epoch levels; `epoch[2]` (+0xf0) is the **commerce** level indexing `commerce_cap` |

  The combat spec's shorthand "resources `^ 0x8221`" means the stockpile `bucket`; the field literally named `resources`
  is the rate. The writes `LeaderDataEncrypt::scratch = …` that accompany some updates are a global temporary (not state).
- Other LeaderData fields: `escrow[6]` +0x468 and `escrow_rate[6]` +0x480 (AI savings, §2.4), `base_rate[6]` +0x4b0
  (added to the rate; no writer found outside `Leader::init` ❓), `known_rares` +0x6d4, `rares_collected[44]` +0x6d8,
  `gather_stamp` +0x7ac, `collected[6]` +0x874 (total gathered, statistics), `bonus_cap[7]` +0x918, `territory` +0x9d8,
  `rare`/`rare_owned`/`rare_conquest` (`BitMask<44>`, +0x6d98/+0x6dac/+0x6dc0; bit `good − 6`; the BitMask `flags`
  word at +8 is an "is empty" cache that the code clears or sets by hand). Leader flags: `LEADER_RECOMPUTE` 0x2000000,
  `LEADER_CALC_UNIT_STATS` 0x4000000, `LEADER_CALC_WALL_STATS` 0x8000000, `LEADER_IDLE_PEASANT` 0x80000.
- Object fields: `UnitData.gather_down` +0x92 (next gatherer of the same building), `good_obj` +0x94 (circle index of
  the rare/fish spot), `rare` +0x54 (union: the good being collected), `idle` +0xb0 (idle-frame counter), `unit_masks`
  +0x68 (`UNIT_BAD_FISH` 0x20, `UNIT_BUILDER` 0x400, `UNIT_AI_CONTROL` 0x40000, `UNIT_PACKED` 0x80000, `UNIT_HAS_WOOD`
  0x8000000, `UNIT_GETTING_WOOD` 0x10000000, `UNIT_HAS_METAL` 0x20000000, `UNIT_GETTING_METAL` 0x40000000,
  `UNIT_WITH_STUFF` = 0x78000000), `doober` +0x86 (held doober). `BuildData.gather_down` +0x70 (head of the gatherer
  list), `city` +0x72, `city_down` +0x74 (next building of the city), `farm` +0x78 (farm slot), `recharging` +0x7a
  (reused as a "worked" counter by gather buildings), `build_masks` +0x60 (0x800 = worked this frame), `gather_max`
  +0x80 (char), `gather_from` +0x98 (`MiningList`: `Array<TCoordData>` of claimed tiles + `mtn` +0xb4 / `cliff` +0xb5
  deposit indices), `gather` +0xb8 (rally points). `CityData` (0xb8): `trade_val` +0x52 (short, caravan income),
  `granary`/`lumber_mill`/`smelter`/`refinery` +0x56..0x59 (uchar enhancer percentages), `free` +0x5a, `gatherers` +0x5c.
- `GatherOrder` (order type 7, 0x34 bytes, orders-spec §3): `ox` +8, `whom` +0xc, `uid` +0x10, `tx`/`ty` +0x14/+0x18
  (current resource tile), `build_type` +0x1c, `wait` +0x20, `goto_build` +0x24, `non_flat_gather` +0x25, `dist_mod`
  +0x26, `been_there` +0x27.
- "every N frames, staggered" = `(frame + s) % N == 0` with the stagger `s` given each time (all operands are
  non-negative, so the binary's `& (N−1)` with sign fix-up equals `%`).

## 1. The economy in one frame

```
Game::do_frame
 ├─ 2. Leaders::process_all (leaders 0..7, ACTIVE)                                         frame-spec §4
 │     └─ Leader::gather @6ce280                                                            §2.2
 │          ├─ Leader::calc_gather @6ceee0   (recompute the rate; throttled, §3.1)         §3
 │          │    cities → City::calc_gather → calc_city_resources → BuildData::calc_gather (each city building)
 │          │    oil wells, out-of-city woodcutters/mines → BuildData::calc_gather
 │          │    idle fishermen / merchants → Unit::do_gather (rare/fish spot)            §3.7
 │          ├─ Leader::calc_resource_caps @6ce900                                          §2.3
 │          └─ Leader::do_gather @6ce450     (stockpile += rate / 7200 per frame)          §2.4
 ├─ 3. GameDaemon::process_all → GameDaemon::calc_markets (market drift, game_random)     §5
 ├─ 4. Objects::process_all
 │     ├─ units: Unit::process → work → do_job
 │     │     ├─ GATHER → Unit::do_gather @5ef2a0 (walk, slot registration, farm/oil/woodcutter/mine cycles, RNG) §4.2
 │     │     └─ NONE → do_idle → think → think_peasant → find_gather_spot (auto-gather)   §4.6
 │     │                                 think_fish (RNG), think_merchant, think_caravan  §4.7, §6
 │     └─ buildings: Build::process → gather-tile maintenance (find_gather_tiles: RNG)    §3.6
 └─ 6. Leaders::end_process_all (pop-cap warning, UI only)                                §7
```

RoN has **no resource delivery**: income is a per-building rate that depends on how many gatherers are registered (and
have arrived once) at the building, recomputed by `calc_gather` and paid every frame by `do_gather`. The woodcutter and
miner walks between the resource tile and the building (§4.3) are visual, but they run in the simulation (positions,
orders, held doobers, RNG) and must be ported for lockstep.

## 2. Resources and the stockpile

### 2.1 Where the numbers live

Per leader and good: the stockpile `bucket[k]`, the rate `resources[k]` (recomputed by `calc_gather`), the commerce cap
`resource_cap[k]`, the displayed `income[k]`, the carry `leftover[k]` and the flag `over_cap[k]` (all in
`LeaderDataEncrypt`), plus the AI's `escrow[k]`. Rates are ×16; stockpiles are whole resources.

Other writers of the stockpile (all decrypt-add-encrypt in place; found by grepping every `XOR …, 0x8221`, job
`ig_econ_8221.txt`; none draws RNG in the stockpile update itself): `Type::pay_cost` / `unpay_cost`,
`TypeData::can_pay_cost`, `Build::refund_cost` / `unpay_cost` (production-spec), `Unit::do_repair`, `Build::plunder`,
`Unit::plunder`, `Cities::capture_city`, `Object::disband`, `Object::do_damage` (plunder on kill), `Unit::explore_goody`
(goody boxes), `SpellType::cast_pilfer` / `cast_research_project`, `City::new_caravan`, `Build::do_bonus`,
`Leader::init`, `Leader::gain_tech`, `Leader::defeat`, `Leader::action_respond` / `action_offer` / `clear_agree` /
`pay_dow` (diplomacy deals, tribute §5), the AI (`Leader::use_market`, `market_speculation`, `make_stuff`,
`research_techs`, `production_ai_setup`, `diplomacy`, `process_taunt`, `consider_tribute`, `create_units`,
`create_buildings`), scenario script functions (`ScenarioFuncSet::give_good`, `take_good`, `set_good`), the cheat
`Game::action_cheat_increase_buckets`, CtW setup (`ConquestGame::*`) and the scenario loader/editor.

### 2.2 `Leader::gather` @6ce280 ✅

Called once per frame per ACTIVE leader from `Leaders::process_all` (its body is also duplicated in `Leader::process`
@6b88b0, which nothing on the frame path calls):

1. Copy the six rates `resources[0..5]` into a local array (decrypted) and call `calc_gather(local)` (§3). When the
   recompute is throttled away, `calc_gather` returns without touching the array, so the old rates are written back.
2. Write the six values back into `resources[k]`.
3. `rare = rare_owned | rare_conquest`; if that differs from the stored `rare`, store it and set
   `LEADER_CALC_UNIT_STATS | LEADER_CALC_WALL_STATS` (processed later in the same `Leaders::process_all` iteration).
4. `support[0..5] = 0`.
5. `calc_resource_caps()` (§2.3), then `do_gather()` (§2.4).

### 2.3 Commerce caps: `Leader::calc_resource_caps` @6ce900 ✅

For cap slots k = 0..6 (slot 6 is computed the same way as 0..5 and never read by the gathering code):

```
if k == 3:  cap = 999                                        // knowledge is never capped below the ceiling
else:
    cap = constants.commerce_cap[epoch[2]]                   // commerce level 0..7: 70,100,150,200,260,320,400,500
    if British (11):   cap = pct(cap, british_commerce)      // +25
    if k == 2 and inca_wealth_cap and Inca (2):        cap = pct(cap, inca_wealth_cap)
    elif k == 1 and french_timber_commerce and French (10): cap = pct(cap, french_timber_commerce)
    elif k == 0 and egyptian_food_commerce and Egyptians (7): cap = pct(cap, egyptian_food_commerce)
    if the leader's rare or rare_conquest has DIAMONDS (bit 22): cap = pct(cap, diamonds_commerce)
    if k in {0, 2} and has_wonder(PYRAMIDS):  cap += pyramids_commerce
    if k in {1, 2}:
        if has_wonder(COLOSSUS): cap += colossus_commerce
        if k == 2: { if has_wonder(TAJMAHAL): cap += taj_wealth_commerce;  goto republic }
    if k == 5: { if EIFFEL: cap += eiffel_oil_commerce;  if KREMLIN: cap += kremlin_commerce }
    elif k < 6: { if KREMLIN: cap += kremlin_commerce
                  if k == 1 and TIKAL: cap += tikal_timber_commerce
                  elif k == 4 and ANGKORWAT: cap += angkor_metal_commerce }
  republic:
    cap += republic_commerce_bonus3 if has_preq(REPUBLIC_3) else bonus2 if REPUBLIC_2 else bonus if REPUBLIC_1 else 0
    cap += bonus_cap[k]                                      // LeaderData +0x918 (techs/cards; writers in production-spec)
    cap = clamp(cap, 0, 999)
if has_preq(VIRTUAL_REALITY_BONUS 695): cap = 999            // all slots, knowledge included
resource_cap[k] = cap << 4
```

So the Kremlin raises food, timber, metal and oil caps (not wealth); the Colossus raises timber and wealth. The ceiling
999·16 = 0x3e70 is also the Dutch rate ceiling (§2.4).

### 2.4 Paying income: `Leader::do_gather` @6ce450 ✅ (arithmetic checked in the disassembly)

For each good k = 0..5 with `type_avail(k, 1)` (oil and knowledge are unavailable until their ages; skipped goods keep
their stockpile and flags untouched):

```
if setup.starting_resources == 8:                 // "unlimited resources" mode
    bucket[k] = 99999 (stored 0x104be);  escrow[k] = 0;  continue
r = resources[k] - support[k] + base_rate[k]
if r < 0: income[k] = r; over_cap[k] = 0; continue        // nothing paid
if resource_cap[k] < r:
    (UI only: if over_cap[k] was 0 and who == console.who: "X income capped" message + S_ECON_CAP sound)
    over_cap[k] = (resource_cap[k] >= 0x3e70) ? 2 : 1;  r = resource_cap[k]
else over_cap[k] = 0
if k != KNOWLEDGE and Dutch (22):                 // interest on savings
    base = starting stockpile for this mode: game_rules == 8 and get_team() == 0 → (starting_resources2 + 1)·starting[k];
           CtW (semaphore byte2 & 2) and the leader is a nomad (flags2 0x80) → starting[k]·ctw_nomad_starting_res_x;
           else starting[k]                      // Game.starting[6], Game +0x600
    excess = bucket[k] − base
    r2 = r
    if excess > 0: r2 = min(r + (dutch_interest·excess / 100)·16, resource_cap[k] + dutch_interest_cap·16)
    r = min(r2, 0x3e70)
income[k] = r
h = get_gather_handicap(); if h: r = ((h + 100)·r) / 100        // §2.5
if k == KNOWLEDGE and setup.tech_cost > 4: r = (tech_cost ≤ 6) ? (3r)/4 : r/2     // both truncating
if (GameInfo.flags & 2) or setup.game_rules == 9: r = (3r)/2
if ai_speed > 1: r = r·ai_speed                    // ai_speed is 1 in normal games
D = gather_rate·16                                 // 7200
q = r / D;  leftover[k] += r % D
while leftover[k] >= D: q++, leftover[k] −= D
bucket[k] += q;  collected[k] += q
if (leader_flags & 0xc) != 4:                      // not (HUMAN without COOP_SOLO): AI-run leaders
    D2 = D·100;  e = escrow_rate[k]·r / D2;  m = escrow_rate[k]·r % D2
    if m != 0: period = max(2, (m/2 + D2) / m);  if frame % period == 0: e++
    escrow[k] += e
```

`escrow_rate` is set by the AI (`Leader::plan_strategy` writes 0x28 = 40 for food, see `re/decomp/grid_regions/`); for
human leaders the escrow block is skipped entirely. There is no RNG anywhere in `do_gather` (the escrow "dither" is a
frame-modulo test).

### 2.5 Difficulty handicap: `LeaderData::get_gather_handicap` @6d66a0 ✅

Humans (`LEADER_HUMAN`): `get_handicap()` (the lobby handicap) in multiplayer (`semaphore byte0 & 4`), else 0.
Computers: difficulty d = `multi_diff` in multiplayer (`semaphore byte0 & 4`); in single player d =
`setup.difficulty`, except d = `multi_diff` when not CtW (`semaphore byte2 & 2` clear), `semaphore byte1 & 0x10`
(scripted game rules) is set and `multi_diff ≥ 0`. Result: d 0 → −35, 1 → −15, 2 → −7,
3 → 0, 4 → +25, 5 → +50 (percent on the paid rate; the HUD `income` is unaffected).
`LeaderData::get_mod_resource_cap` @6d65b0 (AI planning only) scales a cap by **float** 0.5 / 0.75 / 1.0 for computer
leaders at difficulty 0 / 1 / other in single player; it never feeds the stockpile (see §5 for its callers).

## 3. Income: `Leader::calc_gather` @6ceee0

### 3.1 Cadence ✅

`calc_gather(out)` recomputes `out[0..5]` only when:
- `LEADER_RECOMPUTE` is set: on frame 0, or when `(frame + who) % 8 == 0`;
- otherwise: `frame ≥ gather_stamp + 300` **and** `(frame + 8·who) % 256 == 0`. Because 256 < 300, an unflagged leader
  recomputes every **512** frames after an aligned recompute (the next aligned frame is skipped by the 300 test).

At the end of a recompute `LEADER_RECOMPUTE` is cleared and `gather_stamp = frame`. The flag is set by anything that
changes a building's gatherer count or validity: `Unit::kill_current_order` on a GATHER order (orders-spec §4.3),
the first arrival of a gatherer (`been_there` 0 → 1 in `do_gather` / `do_non_flat_gather`), `Unit::check_idle`
@6032c0 when a fisherman, merchant or fur trapper first becomes idle (it starts producing; nothing flags the reverse,
so a collector given an order keeps counting until the next recompute), and the production/territory code (building finished or lost, techs: production-spec). Port it
as a dirty flag with exactly these setters; the recompute frames are observable (rates change on them).

### 3.2 Order of contributions ✅

```
rare_owned.clear(); known_rares = Σ reg_known_rares[0..63]; rares_collected[0..43] = 0
if has_wonder(PORCELAIN 533):                         // every rare in own territory counts as owned
    for each good object g (goods list order): if g valid, g.is_seen(who), type_avail(g.type) and the W-cell owner
        (wdata.who at g's W-cell) == who: rare_owned.set(g.type − 6)
for k in 0..5: out[k] = basic_gather[k] << 4;  bonus[k] = 0          // basic_gather is all 0 in the shipped rules
if Lakota (19):    out[FOOD] += (stable_units − scout_garr − stable_garr − peasants_garr + scouts + peasants)·lakota_food·16
if Americans (20): a = (barracks_units − barracks_garr)·americans_barracks_gather·16
                   added to food, timber (if type_avail), metal (if type_avail), wealth (if type_avail), in that order
if not a scenario, or ScenarioData::buildings_gather:
    for c in 0..city_mark−1 (cities list of who): if valid: City::calc_gather(c) → add its six values (§3.3)
    for w in 0..oil_well_mark−1 (oil wells of who): if valid: its building's BuildData::calc_gather(p4 = 1) → add
    for o in 2000..build_mark−1: building valid, not in a city (city < 0), not is_neutralized, type WOODCUTTER or
        MINE: BuildData::calc_gather(p4 = 1) → add
    for o in 0..unit_mark−1: is_map_unit and (is(FISHERMEN) or is_merchant) and the unit's order type is NONE:
        Unit::do_gather(out_u, &rare_owned, rares_collected, …) → add                           (§3.7)
out[OIL] = ((num_buildings[REFINERY]·refinery_bonus + 100)·out[OIL]) / 100
if has_preq(CAPITALISM_1 805): out[OIL] += capitalism_oil_prod·16
if inca_wealth_per_miner < 0 and Inca: out[WEALTH] += out[METAL]          // dead with the shipped value 10
conquest rares (§5): unless another VALID leader has num_units[WELLINGTON 368] ≠ 0, for each bit i of rare_conquest:
    rares_collected[i]++, LeaderData::calc_rare(i + 6, tmp, 0), out += tmp
new = rare_owned | rare_conquest
if new ≠ rare: (if the GEMS bit (23) changes: Regions::fix_all_borders) rare = new;
               flags |= CALC_UNIT_STATS | CALC_WALL_STATS; Leader::calc_pop_cap()
if has_rare(COFFEE 44): out[k] = pct(out[k], coffee_income_bonus) for all six
if world.total_land (World +0x78) ≠ 0:
    tax = territory_taxes[get_taxation()]; British: tax = pct(tax, british_taxation);
    CtW and conquest bonus 0x20: tax = pct(tax, ctw_missionaries_bonus)
    out[WEALTH] += (territory·tax·16) / total_land                  // territory = LeaderData +0x9d8
    if Mongols (17) and mongol_nomadic_food: out[FOOD] += ((num_nations·territory·800) / total_land) / mongol_nomadic_food
LeaderData::calc_resource_bonuses(out)                                                         (§3.8)
flags &= ~LEADER_RECOMPUTE;  gather_stamp = frame
for k in 0..5: if !type_avail(k, 1) and has_preq(k):           // obsolete basic good (GoodType OBS_PROD)
    if obsolete_production_good ≥ 0: out[that] += trunc256(out[k]·obsolete_production_rate)
    out[k] = 0
```

Iteration orders that matter: city list order, oil-well list order and building/unit slot order (sums commute, but
`City::calc_gather` writes city enhancer bytes and `Unit::do_gather` writes unit state, so keep the order).

### 3.3 Cities: `City::calc_gather` @737c60 and `LeaderData::calc_city_resources` @6d5530 ✅

`City::calc_gather` first refreshes the city's enhancer bytes (walked state):
`granary = granary_bonus[get_granary() − 1]` if the city has a granary (city_flags 0x200) else 0;
`lumber_mill = lumbermill_bonus[lumber_level − 1]` if city_flags 0x400 else 0;
`smelter = smelter_bonus[get_smelter() − 1]` if the city has a SMELTER (`count_buildings`) else 0; `refinery = 0` (the
refinery count is evaluated and discarded; oil uses the leader-wide `refinery_bonus`). Levels come from techs:
`get_granary` 1..5 (GRANARY2..5), `get_lumbermill` / `get_smelter` 1..4, `get_university` 1..6 (UNIVERSITY2..6).
(The decompiler shows the tables shifted by one array, e.g. `fishermen_bonus[level + 4]`; that is the same address as
`granary_bonus[level − 1]`.)

Then `calc_city_resources(out, x, y, city)`:

```
out = 0;  out[WEALTH] += city.trade_val                       // caravan income (§6), already ×16
for each building b of the city (chain from the city centre o through city_down +0x74):
    if b is a live building, its type is a gather type (build_flags 0x40), not OILWELL/OILPLATFORM, and b.city == city:
        out += BuildData::calc_gather(b, p4 = 1, p5 = 0, p6 = 0)                                   (§3.4)
if the city centre is FORBIDDENCITY: out[k] = pct(out[k], forbidden_city_gather) for all six
if the leader has a CEO (num_units[THECEO 357] ≠ 0): the first live hero unit that is a map unit and is(THECEO, 1):
    if find_city_at(its position) == city: out[k] = pct(out[k], theceo_production_bonus); stop at that hero either way
for k in 0..5 with city_gather[k] ≠ 0:                       // 10 food, 10 timber per city
    out[k] += 16·(forbidden_city_base_gather if the centre is FORBIDDENCITY and that constant ≠ 0 else city_gather[k])
if Romans: out[WEALTH] += roman_city_gather·16
if Germans: out[FOOD] += german_city_gather·16; out[TIMBER] += same; if type_avail(METAL): out[METAL] += same
out[WEALTH]    += 16·get_taxes()      // village_taxes + num_buildings·building_taxes + market_taxes (if a MARKET;
                                      //   pct(…, porcelain_market) with PORCELAIN) + temple_taxes (if a TEMPLE)
out[KNOWLEDGE] += 16·get_literacy()   // village_literacy + university_literacy (if UNIVERSITY) + library_literacy (if LIBRARY)
```

Note that the Forbidden City and CEO multipliers apply to the buildings and caravan income but not to the base gather,
taxes or literacy that are added after them. Oil wells and oil platforms are counted in the oil-well loop of §3.2
(whether in a city or not), never here.

### 3.4 One building: `BuildData::calc_gather` @62d360 → `BuildTypeData::calc_gather` @639e40 ✅ (formulas) 🟡 (scan loops)

`BuildData::calc_gather(out, &max, &per, p4, p5, p6)` passes: the building's tile corner, `active` = `p5 ? 1 :
is_active()` (finished), `gatherers` = `num_gatherers(1, 1)` (§3.5), `inside` = scholars inside for universities
(unused by the formula), `city` = b.city, `who`, `apply_bonuses` = p6 (0 on every income path), and its `gather_from`
list; it clamps `max ≥ 0` (corrected by the port: the clamp is on the slot count, `*param_2`). `BuildTypeData::calc_gather(out, &max, &per, assume, city, who, apply_bonuses, tx, ty,
active, gatherers, inside, list)`:

```
for k: out[k] = 0; raw[k] = 0; enh[k] = (city < 0) ? 100 : cities[who][city].enhancer_amount(k, 100)  // = byte + 100
       bonus[k] = 256;  if who ≥ 0 and apply_bonuses: LeaderData::calc_resource_bonuses(bonus)  (8.8 multipliers)
if !active and !assume: max = 1, per = 0, return
```

Four paths, by building type:

**(A) Flat** (`is_flat`, build_flags 0x10000000 `BUILDTYPE_FLAT_GATHER`, set by `ObjectType::finalize_init_all` for
FARM, OILWELL, OILPLATFORM): `max = max_flat_gatherers() = 1`, `per = 0`. For every footprint tile (x_size × y_size from
the corner, valid, owner none/own/allied): land = `get_land(t, 1)`; for each of the land's 4 MAKEs with good ≥ 0:
`raw[good] += num` (× `river_resource_value` = 2 on river tiles). Then for each k with `active` and `gathers(k)`:
`per = max(0, trunc256(raw[k]·(k == OIL ? oil_rate : peasant_rate)))`; if `enh[k] ≠ 100`: `per = per·enh[k]/100`;
if k == FOOD and Japanese (15): `per = pct(per, japanese_fishing_boats)` (the constant's name says fishing boats, the
code applies it to farms); `out[k] += per·gatherers`; if `bonus[k] ≠ 256`: `out[k] = trunc256(out[k]·bonus[k])`,
`per = trunc256(per·bonus[k])`. Finally, if the type is FARM, the owner is Egyptian, `egyptian_farm_wealth > 0` and
`gatherers ≠ 0`: `out[WEALTH] += egyptian_farm_wealth·16`.
A farm on 16 "Land" tiles: raw food 16, per = trunc256(16·2560) = 160 (10 food / 450 frames); an oil well on 16 oil
tiles: per = 16·35 = 560.

**(B) University** (is UNIVERSITY): `max = max_knowledge_gatherers() = 7`; `n = min(max, gatherers)`;
`per = max(0, trunc256(scholar_rate[get_university() − 1]·16))` (scholar_rate is EntryFraction×256: 5,7,10,15,20,25 →
80, 112, 160, 240, 320, 400); enhancer test as above (always 100); `out[KNOWLEDGE] += per·n`; bonus.

**(C) Mine** (is MINE): `R = gather_radius()·192` (mine_radius 6 tiles). Deposit: if `list` is null or both
`list.mtn < 0` and `list.cliff < 0`: nearest mountain range (`MountainsData::find_nearest(x, y, region, &dm)`) and
nearest cliff (`CliffsData::find_nearest(…, &dc)`) in the W-cell region (`WorldData::get_region`) of the building's
centre (`corner_tile` returns the centre Coord: `(x_size + 2·tx)·96`); else the stored `list.mtn` / `list.cliff`
with distances 0. If both exist and `dc < dm`, use the cliff. If the chosen deposit is farther than R it is dropped.
Before sizing, the deposit's tiles are checked: a tile that is not a tree, is mountain (for ranges), is owned by
none/own/ally and — **only when `list` is null** (placement preview) — `is_gathered_from`, marks the deposit as taken:
`max = −1`. Otherwise `max = MountainRangeData::gather_size(…)` / `CliffsData::gather_size(…)` (§3.6). If `max ≠ 0`:
Germans +`german_miners`, TAJMAHAL +`taj_farms`, KREMLIN +`kremlin_farms` (all 0 in the shipped rules).
`per = trunc256(peasant_rate) << 4 = 160`; enhancer (smelter byte) scaling; `n = min(max(max, 0), gatherers)`;
Inca with `inca_wealth_per_miner > 0`: `out[WEALTH] += n·inca_wealth_per_miner·16`; `out[METAL] += per·n`; bonus[METAL].

**(D) Radius** (everything else that gathers: the WOODCUTTER): `R = gather_radius()` tiles (woodcutter_radius 8).
For each W-cell i in `circle_x/y[0 .. circle_radius[(R + 3)/4])` around the W-cell of the building's centre
(`corner_tile` returns the centre; corrected by the port), valid, `vector_dist` from the centre tile to the W-cell's
**centre tile** `(4wx + 2, 4wy + 2)` (`WCoord::operator TCoord` @4613b0 returns `4w + 2`) ≤ R, owner none/own/ally (Chebyshev
… `vector_dist` of the tile coordinates ≤ R, owner none/own/ally):
- with `list` (every income call): if the W-cell's centre tile `(4wx + 2, 4wy + 2)` is in `gather_from`, scan its land
  (corrected by the port: not the top-left tile);
- without `list` (placement preview): if the tile is not `is_gathered_from`: collect the W-cell's 16 tree tiles (woodcutter)
  or mountain tiles (other) into a static scratch list and scan its land if the top-left tile has a tree / mountain;
  if it is gathered from, remember `blocked = 1`.
Land scan of a W-cell: for each of the land's 4 MAKEs with `good ≥ 0`, `gathers(good)` and `num ≠ 0`:
`raw[good] += 16·num` (one `num` per tile of the W-cell); Iroquois (18) add the counted tiles to `raw[FOOD]`.
Then `rivers` = river tiles in the footprint, and for each k with `active` and (`gathers(k)` or (k == FOOD, Iroquois,
`iroquois_food ≠ 0`)):
```
raw[k] += trunc16(raw[k]·rivers)
max = trunc16(raw[k] + 8)                              // slots = forest W-cells (rounded), more with rivers
if max == 0 and blocked: max = −1, stop
if max ≠ 0 and who ≥ 0 and k == TIMBER: French +french_woodies (1); TAJ +taj_farms; KREMLIN +kremlin_farms
per = (k == FOOD and Iroquois and gathers(TIMBER)) ? per + iroquois_food·16 : trunc256(peasant_rate) << 4  (= 160)
enhancer scaling;  if k == OIL: per = per·max, max = min(max, 1)
n = min(max, gatherers);  access = 2·total_gather_access(list or scratch, corner, who)
n = min(n, access);  gatherers = n;  max = min(max, access)
out[k] += n·per;  bonus
```
`total_gather_access` @636500 counts the list's tiles that have `has_gather_access(t, who, 0, 0)` and are not
hugging the corner coordinate (`|cx − 192·tx − 96| + |cy − 192·ty − 96| > 0x240` or either term > 0x23f).

`max_gatherers` @63c430 (stored as `gather_max` +0x80 by `Build::init`, `update_max_gatherers` @623310 and
`find_gather_tiles`): 1 for flat buildings, else the `max` of a preview `calc_gather(assume = 1, apply = 0,
active = 1, gatherers = 1)` with the building's city and `gather_from` list (null list when `o < 0`), clamped ≥ 0.

### 3.5 Gatherer bookkeeping ✅

Each gather building keeps a singly linked list of the units registered to it: head `BuildData.gather_down` (+0x70),
next `UnitData.gather_down` (+0x92), −1 terminated, newest first.

- `Build::add_gatherer(o, who)` @62f640: only for the building's owner, while `num_gatherers(0, 0) < gather_max`, for a
  live on-map worker (types 50..53) not already in the list: `check_gatherers()` then push at the head.
- `Build::remove_gatherer(o)` @62f8d0: unlink `o` (walks the list; stops at a dead entry).
- `Build::check_gatherers()` @62f710: drop entries that are dead, off-map or no longer `is_gathering_at(this)` (heads
  are popped with their `gather_down` reset to −1; an entry unlinked from the middle keeps its own `gather_down`).
- `Build::all_gathering()` @62f570 (after `check_gatherers`): 1 unless some gatherer's **front** order is not a target
  order, or has `goto_build ≠ 0`, or `wait < 0`.
- `BuildData::num_gatherers(been_there, no_decoys)` @630450: OILPLATFORM: citizens inside (`count_inside`, mode
  `COUNT_TYPE`, +2 for `no_decoys`); UNIVERSITY: scholars inside (same); plus every list entry that passes
  `is_gathering_at(this, been_there)` and, if `no_decoys`, is not `UNIT_DECOY`.
- `UnitData::is_gathering_at(o, who, been_there)` @608880: a scholar inside building `(o, who)` → 1 (an inside
  scholar elsewhere → 0 when `been_there`); otherwise an on-map worker whose current action is a GATHER order on
  `(o, who)` → 1, or, with `been_there`, the order's `been_there` byte. **Income counts only gatherers that have
  arrived once** (`num_gatherers(1, 1)`), slot limits count every registered gatherer (`num_gatherers(0, 0)`).
- `UnitData::is_gathering` @6089b0 (worker with a GATHER action on an own building), `can_gather` @608850 (types 50..53).

### 3.6 Gather tiles and deposit sizes ✅ (logic) 🟡 (`find_gather_tcoords` decompile is register-confused; port from asm)

Non-flat gather buildings other than universities (woodcutters, mines) claim the tiles they work: `gather_from`
(`MiningList`) plus the tile bit `TData.mask 0x1000` ("gathered from", `World::set_gathered_at` @6b46b0,
`WorldData::is_gathered_from` @472ac0). A claimed tile cannot be claimed by another building.

`Build::find_gather_tiles` @623350 (from `Build::init` @629740 at creation, and from `Build::process` when the
building's W-region has flag 0x10):
1. `old = gather_from.count`; `BuildTypeData::find_gather_tcoords(x, y, &gather_from, who)` @63bdc0 appends tiles:
   woodcutter: every unclaimed **tree** tile (`mask & 0x30 == 0x30`) of every W-cell within woodcutter_radius (circle
   order, distance test on the W-cell, owner none/own/ally, the W-cell's **centre** tile `(4wx+2, 4wy+2)` unclaimed);
   mine: the stored or nearest deposit (mountain range or cliff, nearer wins, within mine_radius·192; stores
   `mtn`/`cliff` in the list) and every one of its tiles that is **not a tree** (`mask & 0x30 ≠ 0x30`), owned by
   none/own/ally and unclaimed. (Corrected from the asm port, engine/sim/build.cpp `find_gather_tcoords`.)
2. Mark every listed tile claimed (`mask |= 0x1000`).
3. **RNG shuffle** if tiles were added (`old < count`): repeat `4·count` times: `i = (count − 1 < 1) ? 0 :
   game_random % count` (inline draw, skipped when count ≤ 1), remove tile i and append it at the end.
4. `gather_max = max_gatherers(o, who, corner)` (§3.4).

`Build::verify_gather_tiles` @623570 (from `Build::process` when the W-region has flag 0x20): drop every listed tile
whose W-cell owner is now neither own nor mutually allied, clearing its 0x1000 bit. (Region flags: GameDaemon moves
0x20 → 0x10 each frame, frame-spec §5; 0x20 is raised by `World::compute_reg_territory` when a region's territory
pass completes, production-spec §5.10.1.)

Deposit sizes: `MountainRangeData::gather_size(wx, wy, who, list)` @89d170 counts the range's solid W-cells `total`
(W-cell flag 0x20 clear and tile class `mask & 3 == 2`) and of those the `claimable` ones (owner none/own/ally and, with a
list, the W-cell's centre tile `(4wx+2, 4wy+2)` in the list); size class by the range's tile count `mount_tx.count`:
`< mtn_tiny_size` (100) → 3, `< mtn_small_size` (210) → 5, `< 275` → 6, `< 400` → 8, else 10 citizens;
`slots = class·claimable / total` (if total ≠ 0), at least 1 if anything is claimable. `CliffsData::gather_size`
@8a8f80 is the same over the cliff's tile list (owner test per tile, list membership per tile).

### 3.7 Fishermen and merchants: `Unit::do_gather(out, …)` @5fce20 → `UnitData::calc_gather` @609180 ✅ 🟡 (crowding distance args)

Only idle (order NONE) fishermen and merchants (incl. fur trappers) produce, counted from §3.2's unit loop.
`calc_gather(&rare, &crowded, &good_obj, out, owned, collected, check_only, …)`:
- packed merchants / fishermen (`UNIT_PACKED`) produce nothing unless `check_only`;
- search radius `r = upgrade_level·4 + 4` for merchants and fishermen, `upgrade_level + 2` otherwise (the type vslot
  +0x124 is `ObjectTypeData::upgrade_level` @661090: how many types of its `upgrade` chain the type `is`); the scan is
  over **tiles** (`circle_x/y` around the unit's tile; the fast path `good_obj` indexes the same tile circle), with the
  tile's `RESOURCE` bit and `find_good_at` on that tile's W-cell;
- fast path: the cached spot `good_obj` (index into the circle tables) if still a good of the right terrain (merchants
  need land `mask & 0x30 ≠ 0x20`, fishermen water `== 0x20`; tile bit 0x200 = has a good) found by
  `ObjectsData::find_good_at(wx, wy, who)`; else scan `circle_x/y[0 .. circle_radius[r])` for the first such good and
  cache its index;
- found good g: `rare = g`, `owned.set(g − 6)`, `collected[g − 6]++`, `LeaderData::calc_rare(g, out, allied)` where
  `allied` = the good's W-cell owner is own/mutually allied (§5);
- crowding: count own live on-map units of the same type (is(type)) within the 5×5-ish W-cell circle
  (`circle_radius[2]`) whose distance to this unit is `< (their r + my r)·192` (packed ones skipped unless unpacking);
  if any: `out[k] /= count + 1` and `crowded = 1` → `Unit::do_gather` sets `UNIT_BAD_FISH` (0x20), else clears it.

### 3.8 Leader-wide multipliers: `LeaderData::calc_resource_bonuses` @6db030 ✅

Applied once to the leader's total (and, in placement previews only, to a 256 array to make 8.8 multipliers):
Russians: oil `pct(russian_oil)` (+20); PYRAMIDS food +20%; COLOSSUS wealth +30%; HANGINGGARDENS knowledge
`+hanging_gardens_knowledge·16` (flat +50); ANGKORWAT metal +50%; TAJMAHAL wealth +100%; EIFFELTOWER oil +100%;
TIKAL timber +50%; VIRTUAL_REALITY_BONUS: every good but knowledge `pct(global_prosperity)` (+25); CtW: per good,
`out[k] += num_ctw_rate_bonuses[k]·ctw_prod_rate_bonus·out[k] / 100`. There are **no** per-tech gather multipliers:
techs act through enhancer levels (§3.3), commerce caps (§2.3) and the scholar rate.

## 4. Citizens and the GATHER order

### 4.1 Issuing work ✅ (orders-spec §2.6, §2.9, §4.2)

| Command (replay-spec op) | Bytes | process_* | Effect |
|---|---|---|---|
| GATHER (19) | i32 ox, queued | `process_gather` @9488b0: if `ox < 0` or the object in the **group owner's** list is valid → `Group::action_gather(ox, q)` @700b90 | workers → nearest own gather building to the target (spiral search), `add_gather_order(b, q, explicit = 1)`; others move there |
| CITY_GATHER (18) | i32 t, queued | `process_city_gather` @948a10 → `Group::action_city_gather(t, q)` @701780 | for every member that is a finished city centre: `clear_gather`, then a rally point (`add_gather_point(b.x, b.y, 1, QUEUE_LAST)`) on each of its buildings of type t (or FARM/WOODCUTTER/MINE when t < 0) |
| GATHER_POINT (22) | i32 x, y, action, add_to_end | `process_gather_point` @948510 → `Group::action_gather_point` @6ff1b0 | rally points; trained units take them in `Unit::come_out` (frame-spec §10) |

Quirk (CITY_GATHER): when no member got a point and the group's owner is the local player, the code calls
`clear_gather` once more on the **last member processed** — a seat-local write to simulation state. It is harmless when
that member is a city centre (already cleared), but a mixed selection could desync; reproduce only if replays show it.

`Unit::add_gather_order(ox, q, explicit)` @61a5c0: QUEUE_NEW clears the orders first (`UNIT_MULTIMOVE` off,
`+0xc0 = 0`, `close_orders`, `clear_partial_path`, `update_action`); clears `UNIT_BUILDER`; GATHER order with
`ox`, `whom = who`, `uid` = building uid, `build_type` = building type, `tx = ty = −1`, `goto_build = non_flat = 0`;
if the type is a gather type: `Build::add_gatherer`, and for non-flat non-university buildings `non_flat_gather = 1`,
`dist_mod = 10` for mines, 4 otherwise; explicit flag; if the building is in another land region and the leader may
transport this unit class: `UNIT_CAN_TRANSPORT`; append; `update_action`.

### 4.2 `Unit::do_gather(order)` @5ef2a0 — the per-frame handler 🟡 (structure ✅, RNG ✅ from asm)

1. Target `(ox, whom)`; if `whom ≠ who`, or the building is not live/finished → `kill_current_order` (the unit idles and
   re-thinks next frame).
2. **Registration** while `been_there == 0`: if not in the building's list: if `num_gatherers(0,0) < gather_max`:
   OILPLATFORM targets and sea units skip registration, else `add_gatherer`; if still not registered →
   `kill_current_order` + `add_think_order` (re-search). If full → same. After a re-think, flat targets return if the
   unit already stands on the footprint, else it moves to the building (`add_move_order(…, QUEUE_FIRST)`).
3. FARM target: the farm must still be in a city (b.city ≥ 0) else drop as above; note the farm slot (+0x78).
4. WOODCUTTER / MINE: every 128 frames (`(frame + 4·o) % 128 == 0`) check over-subscription (`num_gatherers(0,0) >
   gather_max` → drop + re-think + walk-back logic as in 2); otherwise **`do_non_flat_gather`** (§4.3).
5. Farms: standing on the footprint (`covers_tile`) counts as arrived; otherwise another region without transport or a
   full farm drops the order (NEW_THINK), an unregistered land unit clears `been_there`, else it walks to the farm
   centre (QUEUE_FIRST). Other targets (oil well, university, oil platform): if not `adjacent_to(building)`: target spot = the
   building centre when it is in another land region and the unit can be transported, else `find_nearby_spot` around
   the building (radius `min(xs, ys)·96 + 48` — corrected from the asm @5ef76a: `x_size < y_size ? x_size : y_size` —
   angle `find_angle(unit − building)`, `FILTER_NOT_ME`; it returns 0 when a spot is found).
   No spot → `kill_current_order` + NEW_THINK. A unit not (or no longer) registered for a non-platform target clears
   `been_there` and returns; otherwise `add_move_order(spot, QUEUE_FIRST)`. (A unit in another region that cannot be
   transported, or a full building, also drops the order with NEW_THINK.)
6. Adjacent / on the footprint: if `been_there == 0`: if over-subscribed → `kill_current_order` + NEW_THINK; else
   `been_there = 1`, `LEADER_RECOMPUTE`; scholars and oil-platform citizens `go_inside` (a citizen entering a platform
   that can carry it dies as a unit: `go_inside`, `kill_current_order`, `die`), then `check_gatherers`, done.
7. AI-controlled units (`UNIT_AI_CONTROL`) every 256 frames (`(o + frame + who) % 256 == 0`) on difficulty > 1 repair a
   damaged non-city-captured building of their city (`add_repair_order(QUEUE_NEW)`).
8. Non-farm (oil well): mark the building worked (`build_masks |= 0x800`, `+0x7a++` once per frame), and unless guy 0
   already plays CHAR_FARM: `set_anim(CHAR_FARM)`, face 0x40000000, snap to `(b.x + 0x120, b.y)`.
9. Farm work (`FarmsData::get_farm_type(farm)`):
   - farm type 1 (paddy-style): `set_anim(CHAR_SOW)`; every 256 frames (`(7·o + frame + who) % 256 == 0`):
     **`dx = rnd(x_size / 2)`, then `dy = rnd(y_size / 2)`** (GameAccess::rnd: no draw when the argument ≤ 1), move to
     tile `(corner_x + 1 + dx, corner_y + 1 + dy)` centre (`·192 + 96`), QUEUE_FIRST;
   - otherwise the farm cell under the unit (`unit tile − corner`, must be inside the footprint, else an assert and cell
     (1,1)) by status (farms-spec §3.3): 0 bare → sow unless guy 0 already reaps ('$'); 1 growing → `set_anim(CHAR_SOW)`,
     `Farms::grow(farm, cy, cx)`; 2 ripe → `set_anim(CHAR_REAP)`, `Farms::snip` unless guy 0 already sows ('#');
     3 → `set_anim(CHAR_REAP)`; the fall-through cases pick a new cell: **`dx = rnd(x_size)`, then `dy = rnd(y_size)`**,
     move to `(corner_x + dx, corner_y + dy)` centre, QUEUE_FIRST.

### 4.3 Woodcutters and miners: `Unit::do_non_flat_gather` @5f0170 🟡

A trip cycle driven by the order's `goto_build` / `wait` / `tx,ty` (port line by line; the RNG sites are exact):
- every call with `been_there`: building worked mark (as §4.2 step 8); `unit.group (+0x80) = −1`.
- **Choose the next tile** (`goto_build == 1`, `wait < 0`, after a dump): drop held doobers; if the stored tile is
  invalid or lost gather access: pick from `gather_from` the tile minimising `max(3, dist(tile, building)) · dist_mod +
  index/4` among tiles with mask 0x4000 and `has_gather_access(t, who, 1, 0)` (`dist_mod` ≤ 3 for tiny deposits),
  then **move that tile to the end of the list** (round robin); then **`wait = game_random % 200 + 400`**,
  `goto_build = 0`; woodcutters `UNIT_GETTING_WOOD`; miners `UNIT_GETTING_METAL` and `wait = 1000000` (miners never
  run out of work: they stay at the ore).
- **At / going to the tile** (`goto_build == 0`): clear `UNIT_WITH_STUFF`; first time sets `been_there` +
  `LEADER_RECOMPUTE`.
  - `wait < 0` (done): find a spot by the building (radius `min(xs, ys)·96 + 48`); none → `set_anim(CHAR_CHOP_WOOD)`,
    `wait = 20`; else walk back (`goto_build = 1`, `wait = 32`, `UNIT_HAS_WOOD` / `UNIT_HAS_METAL`).
  - guy 0 playing CHAR_MINE_ORE: nothing. Guy 0 playing CHAR_CHOP_WOOD: `wait−−`; at 0: if the building's gatherers are all
    busy (`Build::all_gathering`) `wait = −1`, else **`wait = game_random % 100 + 300`**.
  - otherwise, within 0x140 of the tile centre: `wait−−`, at 0 same test with **`wait = game_random % 50 + 100`**; face
    the tile; create a held doober if none (wood at the tile centre, ore halfway); `set_anim(CHAR_CHOP_WOOD / MINE_ORE)`.
  - farther: `set_anim(CHAR_DEFAULT)`; spot near the tile (`find_nearby_spot` radius 0xc0 …); if none, or the spot is the
    current position or the current destination: give up the tile (`tx = ty = −1`, `goto_build = 1`, `wait = −1`,
    `dist_mod−−` if > 0, drop the doober); if no longer registered: `been_there = 0`; else move there (QUEUE_FIRST),
    `UNIT_GETTING_*`.
- **Returning** (`goto_build == 1`, `wait ≥ 0`): adjacent to the building → `wait−−`, face it, `set_anim(CHAR_DUMP_WOOD /
  DUMP_ORE)`; else re-register check, then a spot by the building (another region + transportable → the building
  centre) and move; first trip farther than 0x600 with no spot within 0x600 → `kill_current_order`.

### 4.4 Drop-off ✅

There is no carried amount: `UNIT_HAS_WOOD/METAL`, the held doobers (`Doober::add_hold_doober` /
`remove_hold_doobers`, walked state) and the dump animations are presentation that lives in the simulation. Income comes
only from §3.4 with `num_gatherers(1, 1)`; a woodcutter counts from its first arrival until its order ends.

### 4.5 Full or destroyed sites ✅

- Full building at order start: `add_gather_order` does not register; `do_gather` then drops the order and queues a
  THINK order (`add_think_order`) → `do_think_order` (orders-spec §6: `think_peasant(1)` immediately).
- Over-subscribed later (e.g. `gather_max` shrank because trees were lost): woodcutters/miners check every 128 frames,
  others on arrival; the extra unit drops its order (NEW_THINK) and re-thinks.
- Destroyed / unfinished / captured target: `do_gather` step 1 kills the order; `kill_current_order` removes the unit
  from the list and sets `LEADER_RECOMPUTE`; the unit idles and `think_peasant` finds new work. `Build::close`
  (frame-spec §8) clears the gatherer list and the claimed tiles.
- Exhaustion: resources never run out (forests and mountains are permanent; trees removed by other code shrink
  `gather_from` via `verify_gather_tiles`/rebuilds).

### 4.6 Idle citizens: auto-gather ✅

`Unit::think` @5f6e40 (orders-spec §5) on an idle worker: types 50/51 clear `UNIT_WITH_STUFF` and set
`LEADER_IDLE_PEASANT` (cleared at the end of each `Leaders::process_all` iteration; read by UI/AI); past the second
idle frame, and unless NEW_THINK is set, think continues only when `(o + frame) % 16 == 0`; it returns for
`LEADER_UNIT_AI_OFF` or `UNIT_AI_OFF`; only ACTIVE leaders reach `think_peasant` (after the military/merchant checks).

`Unit::think_peasant(force)` @5f5760:
1. Unless `force`: threshold `T` = 1 for AI-controlled units, else by the leader option `peasants_wait` (+8; NOT `peasants`, which is the citizens' stance; LEADER_OPTIONS
   command, replay-spec op 73): 0 → 2, 1 → 7, 2 → 12, 3 → 17, 4 → 32, 5 → 62 idle frames; return unless
   `idle == T` or (`idle > T` and `(idle − 2) % 5 == 0`).
2. AI citizens: `think_civilian_transport(1)`.
3. Builders (`UNIT_BUILDER`, or worker stance 1/2) other than scholars: `find_build_spot()` (help a construction:
   production-spec).
4. Worker stance check (`get_worker_stance` / stance type), then **`find_gather_spot(r)`** with
   `r = unit_gather_respond_range·192` (32 tiles) for humans' units, −1 (unlimited) for AI-controlled units.
5. Else repair (`find_repair_spot`), clear `UNIT_BUILDER`; AI units with no city in their region may `think_scout`.

`Unit::find_gather_spot(r)` @5f5170 — the auto-job target (scholars ignore `r`):
```
region = tregion(unit); ucity = find_city_at(unit, who)
best = −1, bestscore = 0
for b in 2000 .. build_mark−1 (own buildings, slot order):
    skip unless live, finished, gather type, not neutralized
    skip unless (b is UNIVERSITY) == (unit is a scholar)
    skip unless num_gatherers(0,0) < gather_max or the unit already gathers at b
    skip unless tregion(b) == region
    if not scholar and ucity ≥ 0 and b.city ≠ ucity:
        u = cities[ucity].gatherers + cities[ucity].free
        skip if u < 2, or (b.city ≥ 0 and u ≤ cities[b.city].free + 2 + cities[b.city].gatherers)
    d = vector_dist(unit, b);  skip if r ≥ 1 and d > r
    room = Σ over goods k with type_avail(k) and b.is(best_gather_type(k)) and over_cap[k] == 0:
               resource_cap[k] − income[k]  (+1600 for Dutch when k ≠ KNOWLEDGE)
    score = room·500 / (d/192 + 2)
    if score > bestscore: best = b, bestscore = score
if best ≥ 0 and not already gathering at best: add_gather_order(best, QUEUE_LAST, explicit 0); return 1
```
No RNG; ties keep the lowest slot; a site whose resource is at its cap scores 0 and is never chosen.

### 4.7 Fishermen and merchants ✅

`Unit::think` sends AI-controlled fishermen to `think_fish` @5f4c60 and merchants to `think_merchant` @5f4740; idle
rare collectors of human leaders are re-evaluated every 32 frames (`(o + frame) % 32`) with `do_gather(…, check_only)`
and unpacked (`unpack_merchant(4)`) where they stand.

`think_fish`: boats with type flag `+0x2b8 & 4` that are unpacked re-check their spot every 1024 frames (`(o + frame) %
1024`) with `calc_gather(check_only)` (updates `UNIT_BAD_FISH`) and stay if it still yields. Otherwise scan the 289 W-cells
`move_x/y[0..0x120]` around the unit: for each water W-cell (`wdata` flag 0x100 clear, terrain class 1 or 2) in the
unit's region: `v = 0` without a good; with a good, skip it if another unit (W-cell head `o ≥ 0`, `who < 8`, not self)
stands there, else `v = 1000000`; `v /= max(1, manhattan(unit W-cell, cell))`; **`v += game_random % 60 + k`** (one
inline draw per candidate, in k order); keep the strictly best. (A fallback `rnd(0x79) + 0x19` exists for "best index >
0x120", which cannot happen.) Best = own cell: packers cast UNPACK (spell 652, QUEUE_FIRST); else a fresh MOVE_TO order
(QUEUE_NEW) to the cell centre.

`think_merchant`: packed merchants first try `unpack_merchant(3)`; then over the leader's `new_rares` list (+0x6e6c) in
order, with a priority starting at 200 and dropping by 10 per entry: rares still valid and visible to the leader, owner
none/ally, on the same terrain class as the merchant, with no own same-type unit within 0x300 (two searches) and no enemy
combat unit within 0xc00: `score = max(1, prio (+100 in the merchant's region) − danger(leader, rare cell))` where danger
is the leader's per-cell danger map (`World +0x13c`, see GameDaemon::calc_danger); best → removed from and re-appended
to `new_rares`, MOVE_TO the rare (QUEUE_NEW). No RNG.

### 4.8 Animations (UnitAnim) ✅

CHAR_SOW 35 / CHAR_REAP 36 (farms), CHAR_FARM 37 (oil wells), CHAR_CHOP_WOOD 25 / CHAR_DUMP_WOOD 27, CHAR_MINE_ORE 29 /
CHAR_DUMP_ORE 31, CHAR_DEFAULT 0 between. `set_anim(anim, 0, 1)`; the handlers test guy 0's current animation byte
(`guys[0] + 0x9c`) to avoid restarting a playing one. Animation variant draws inside `Guy::set_anim` consume RNG
(anim-spec, sim-implementation); CHAR_WALK_WITH_WOOD / WALK_TO_WOOD / WALK_WITH_ORE / WALK_TO_ORE exist but are chosen by
the movement code from the `UNIT_*_WOOD/METAL` masks (❓ confirm in `Guy`/`Unit::update_gpiece`).


## 5. Markets, buying and selling, tribute, rares

Evidence `re/decomp/econ_mkt/` (decompiles + `.asm`, jobs `re/jobs/dc_econ_mkt_*.txt`); prices and stockpile updates ✅
(checked in the disassembly), treaty/alliance branches of `action_respond` 🟡.

### 5.1 Market state and initialisation ✅

Game fields (all inside the 0x194-byte Game block that saves and replay headers carry, replay-spec): `market_tick`
+0x564, `market[6]` +0x568 (supply/demand level, the base price), `market_flux[6]` +0x580, `next_flux[6]` +0x598,
`delta_flux[6]` +0x5b0, `flux_length[6]` +0x5c8, `starting[6]` +0x600 (§5.7). `Setup::build_game` @5ac190 sets
`market_tick = 0` and per good `market = 50, market_flux = 15, next_flux = 15, flux_length = 1, delta_flux = 0`.
Only `do_buy`, `do_sell` and `calc_market(s)` write them afterwards.

### 5.2 `GameDaemon::calc_markets` @732180 (every frame, after `Leaders::process_all`) ✅

```
if frame != 0 and market_cycle_rate >= 2 and frame % market_cycle_rate != 0: return     // shipped rate 1: every frame
t = market_tick
for i in 0..5:
    if t != 0 and (t + i) % 8 != 0: continue                    // good i processed when (t + i) ≡ 0 (mod 8)
    if ((t + i) & 0xff) == 0:                                    // drift toward equilibrium every 256 ticks
        m = market[i]; e = market_equilibrium (65)
        if m > e: market[i] = (e == 0) ? m − 1 : m − m / e
        elif m < e: market[i] = m + 1; if m + 1 < market_basement (10): market[i] = m + 2
    if t == 0: calc_market(i)
    else: flux_length[i]−−; if flux_length[i] <= 0: calc_market(i)
    market_flux[i] += delta_flux[i]
market_tick++
```

At tick 0 all six goods redraw (18 draws); afterwards at most one good per tick, and it redraws only when its
`flux_length` runs out. Ticks ≡ 1, 2 (mod 8) process nothing.

### 5.3 `GameDaemon::calc_market(i)` @732270 ✅

```
v = max(market[i] / 2, market_min_variance (2));  half = (v + 1) / 2
a, b = (half > 0) ? rnd(half + 1), rnd(half + 1) : 0, 0          // M1, M2 (in this order)
next_flux[i] = a + b − (half + 1)                                 // mean −1 (sic)
r = market_trend_range (16);  tr = (r − 1 > 0) ? rnd(r) : 0      // M3
len = market_min_trend (8) + tr;  flux_length[i] = len
d = next_flux[i] − market_flux[i];  s = d > 0 ? 1 : (d < 0 ? −1 : 0)
delta_flux[i] = (d + (len − 1)·s) / len                           // rounds away from zero; can overshoot
```

### 5.4 Prices: `LeaderData::calc_market_prices(g, &buy, &sell)` @6dc2a0 ✅

Wealth per lot of 100:
```
sell = market[g] + market_flux[g];  buy = 2·market[g] + market_flux[g]
if nubian_market_prices (20) and Nubians (4): sell += 20, buy −= 20
elif AMBER (good 17) in rare or rare_conquest: sell += amber_market (10), buy −= amber_market      // no stacking
if CtW and n = num_bonus_cards[8] > 0: n × (buy = buy·100 / (ctw_market_bonus_buy + 100));
                                       sell += ((ctw_market_bonus_sell·sell) / 100)·n
sell = max(sell, 1);  buy = max(buy, 2·market_basement);  buy = max(buy, sell + 10)
if has_wonder(SUPERCOLLIDER 541): buy = min(buy, super_buy 125); sell = min(sell, super_buy − 1)
                                  buy = max(buy, super_sell 50 + 1); sell = max(sell, super_sell)
if russian_communism (0) and Russians and ages >= 5: buy = sell = 100
```

### 5.5 Buying and selling ✅ (no RNG, no floats)

Commands BUY (46) / SELL (47): `u8 op, i32 who, i32 good, i32 flags` (13 bytes, replay-spec unchanged).
`process_buy` @946a20 → `Leader::action_buy` @6cfa20: lots `n = (flags & 1) ? 5 : 1`; `flags & 4` → `n = (flags & 1)
? 100 : 10`; `flags & 2` → 99999. `process_sell` @9468a0 (loop inlined; `action_sell` @6cfa90 is the same): `n =
(flags & 1) ? 5 : 1`, `flags & 2` → 99999 (no ×10 case). Gate: `can_buy_sell` @6d53e0 = `has_bonus(BUY_SELL 685)`
(Nubians, or the tech) and `has_market` @6d5410 (a market exists and a live own city with flag 0x800); a nuclear
embargo (`get_nuke_embargo` @6d52c0, waived by SPACEPROGRAM) blocks trading (UI message only). Then one `do_buy` /
`do_sell` per lot until one fails — prices are recomputed per lot. The good and leader indices are not validated.

```
do_buy(g) @6cfbd0:  if bucket[WEALTH] < buy: fail
    bucket[WEALTH] −= buy;  escrow[WEALTH] = max(0, escrow[WEALTH] − 100)     // −100, not −buy (sic)
    bucket[g] += 100;  market[g] += market_supply_demand (3)                    // no upper clamp
do_sell(g) @6cfc60: if bucket[g] < 100: fail
    bucket[g] −= 100;  escrow[g] = max(0, escrow[g] − 100);  bucket[WEALTH] += sell
    market[g] = max(0, market[g] − market_supply_demand)
```
Stockpiles are never capped (commerce caps limit only rates). Scripts reach the same code (`ScenarioFuncSet::buy_good`
/ `sell_good`).

### 5.6 Tribute ✅ (resource paths) 🟡 (treaty branches)

Commands: TRIBUTE (43) / DEMAND_TRIBUTE (44): `i32 who, whom, good, amount` → `Leader::action_offer(whom, good,
±amount)` @6d1780 (`action_tribute` @6d1770 is a jump to it); CLEAR_TRIBUTES (39): `who, whom` →
`action_clear_tributes` @6d1690; ACCEPT (41) → `action_respond(whom, 1)` @6d03c0; REJECT (42) → `action_respond(whom,
agree == 1 ? 0 : 2)` (0 withdraws an agreement, 2 rejects; corrected from `process_reject` @946e90).

State: `dip[8]` (`Diplomacy`, +0x692c, stride 0x5c: `agree` +0, `any_offer` +4, `treaty` +8, `offers[6]` +0xc
(antisymmetric between the two leaders, positive = gives), `dows[6]` +0x24, `attacks[8]` +0x3c), `tributes[6]` +0x498
(held while a deal is pending), `tribute_stamp[8]`, `gift_stamp[8]`, `tribute_sent` / `tribute_received`.

- `action_offer(B, g, n)`: (UI click stamp); if `n > 0` and `bucket[g] < offers[g] + n`: refuse (UI sound); else
  `any_offer = 1`, `clear_agree` both ways (which resets `any_offer` to 0 again — quirk), `A.offers[g] += n`,
  `B.offers[g] −= n`.
- `clear_agree(X, Y)` @6d1af0: if `agree == 1` refund held amounts (`r = min(dows[g] or offers[g] > 0, tributes[g])`,
  `tributes −= r`, `bucket += r`), clear dows; then `agree = any_offer = 0`.
- `action_respond(A, B, 1)`: on the first agreement, check A can afford its positive offers plus any declare-war costs
  (the DoW cost is **assigned** per third party k, last k wins — quirk), then debit them into `tributes` (and `escrow =
  max(0, escrow − x)`), `agree = 1`. A pure gift (no treaty, no attacks, nothing asked from B) executes immediately:
  per good both leaders get their held DoW costs back (the second clear writes `B.dip[B].dows` instead of
  `B.dip[A].dows` — quirk), then `bucket[receiver] += giver.scale_tribute(amount)`, `tributes[giver] −= amount`,
  statistics, `consider_tribute` (AI stamps, no RNG); treaties, peace and DoW deals follow (🟡).
- `scale_tribute(amount)` @6d5240 on the **giver**: `p = epoch[2]·commerce_tribute (7) + base_tribute (51)`, `p ≥ 1`;
  `p ≥ 100` → amount; else `amount < 20` → `amount·p/100`, `< 100` → `(amount·p + 50)/100`, else `(amount·p + 99)/100`.
- `Leader::defeat` @6ecb00 (🟡): allies of a defeated leader receive `scale_tribute(bucket[g] / nAllies)` for g ≠
  knowledge; the defeated leader's stockpiles are zeroed.
- `is_tribute_period` @6d5b90 is AI-only (diplomacy, armies).

### 5.7 Starting stockpiles ✅

`Game::init_starting_resources` @58a500 (from `Game::init_rules_and_teams` in `Game::run`, before `Leaders::init`; its
draws come after `game_random := GameInfo.seed` and before `Setup::build_game` reseeds, so they never reach the stream
the simulation sees — only the chosen values stay):
```
if setup.starting_resources == 12 (Random): setup.starting_resources = rnd(12)              // S1; can become 7 or 8
for g in 0..5:
    base = starting_goods[g] (200, 200, 100, 100, 100, 100);  if setting == 7 (Deathmatch) and g != 3: base = starting_goods[0]
    lo, hi = category "startingresources"[setting].data[0..1]
    if lo == 0: starting[g] = base / 2
    else: span = hi − lo;  r = (span != 0 and span·base − 1 > 0) ? rnd(span·base) : 0       // S2, per good
          starting[g] = lo·base + r
    if setting == 8 (Infinite): starting[g] = 99999
```
Settings: 0 Low (0,0), 1 Standard (1,1), 2 ×2, 3 ×3, 4 ×5, 5 ×10, 6 ×20, 7 Deathmatch (100,100), 8 Infinite,
9 Variable Low (1,2), 10 Variable Medium (3,5), 11 Variable High (5,20), 12 Random (1,20).
`LeaderDataEncrypt::init` @6d9c60 stores the keys (all fields 0). `Leader::init` @6e3930 zeroes the economy arrays,
sets `resource_cap[k] = commerce_cap[0]·16`, `escrow_rate[k] = 40` when `city_num + village_num > 2`, and per
available good `bucket_add(g, amount)` with `amount = (starting_resources2 + 1)·starting[g]` (game_rules 8, team 0),
`starting[g]·3` (CtW nomad), else `starting[g]`; Greeks with `greek_delay_knowledge` at age 0: knowledge 0; Persians:
food `pct(persians_bonus_food)`; Infinite: 99999. (`Leader::init` also draws one unrelated `game_random` at @6e4eb6.)

### 5.8 Rares ✅ (`calc_rare`) 🟡 (bookkeeping)

- Bitmasks `rare` / `rare_owned` / `rare_conquest` (bit good − 6); `has_rare` @6e0770 tests `rare | rare_conquest`.
  Owned rares come from merchants on them (§3.7), Porcelain territory (§3.2) and CtW conquest; `rare` is refreshed by
  `calc_gather` and `Leader::gather` (§2.2, §3.2). A change recomputes unit/wall stats and the pop cap; the GEMS bit
  (23) also re-runs `Regions::fix_all_borders` (gems_territory_bonus).
- `Leader::new_rare` @6d9e70 (from `World::compute_reg_territory`, AI/co-op leaders only, no RNG): appends a newly seen
  rare to `new_rares` of the leader and its mutual allies (skips duplicates, unavailable goods, FISH, WHALES).
- `LeaderData::calc_rare(r, out, in_territory)` @6e08d0 (income of one collected rare, ×16):
  ```
  out = 0; mult[k] = 100; M = merchants_bonus[get_merchants_level()]       // 100,120,150,200,300 (MERCHANTS_1..4)
  for i in 0,1 with bonus_type[i] in 0..5: out[t] += bonus_num[i]·16
        mult[t] = M if in_territory or (r in {FISH 6, WHALES 31} and t != FOOD)
  FISH/WHALES: F = fishermen_bonus[level] (FISHERMEN1..3 → 1..3; 0,50,100,200,200), B = japanese_fishing_boats if Japanese
  other rares in own/allied territory: F = 0, B = porcelain_rare (if PORCELAIN) + nubian_rare (if Nubians); else F = B = 0
  out[FOOD] = (F + B + mult[0])·out[0] / 100   if F + B ≠ 0 or mult[0] > 100
  out[k]    = (mult[k] + B)·out[k] / 100       for k = 1..5 if B ≠ 0 or mult[k] > 100
  ```
  The other effects of rares (unit stats, costs, caps: diamonds, coffee, amber) are applied by their own consumers.

### 5.9 AI economy (strategy_all only)

`Leader::use_market` @6c91c0 (from `make_stuff ← production_ai ← plan_strategy ← Leaders::strategy_all`, and from
`found_cities ← ScenarioFuncSet::place_city_with_cost`): sells/buys single lots to cover its make list; draws **`rnd(6)`**
(start of the sell scan, M4) on each pass that needs wealth. `Leader::market_speculation` @6c8110 (from
`production_ai_setup`): escrow clamp and price-threshold trades, no RNG. `get_mod_resource_cap` (float × 0.5 / 0.75)
is used only by `create_buildings`, `create_units`, `research_techs`, `production_ai_setup`; the float products are exact
and equal `cap/2`, `3·cap/4` (truncated) for caps below 2²², so a port may use integers.


## 6. Caravans and trade routes

Evidence `re/decomp/econ_trade/` (jobs `re/jobs/dc_econ_trade_*.txt`). ✅ = checked in the disassembly unless marked 🟡.

### 6.1 Data

- `TradeOrder` (TRADE_ROUTE 15, a TargetOrder, 0x34): A building `ox`/`whom`/`uid` +8/+0xc/+0x10, B building
  `oxx`/`whose` +0x14/+0x18 (−1 = pick a partner), `started` +0x1c, `loaded` +0x20, `uid2` +0x24.
- `Caravan` (0x50, one per **land** caravan unit, `caravans.lists[who]`, count `caravan_mark` LeaderData +0x438):
  `city2`/`whom` +0/+2 (A), `city3`/`whose` +4/+6 (B), `cara` +8, `o` +0xa, `caravan_flags` +0xc (1 in use, 2 route
  assigned, 4 a round trip completed = counts toward income), `who` +0xd, `road` +0x10 (`Stack<PathData>`),
  `making_road` +0x20, `reset_road` +0x24, saved road-A* state +0x28..+0x40 (movement-spec §12.2). Unit +0x86 holds the
  caravan slot.
- City: `traded_with[8]` +0x2c (bitmask per partner player), `trade_val` +0x52 (short), `vans` +0x74
  (`Array<CaravanLink{cara, who}>`).
- Slots (Caravans, Supplies, OilWells share the scheme) 🟡: `init_*` takes the first unused index below the mark, else
  index `mark`, else appends; `mark = max(mark, idx + 1)`; `close_*` clears the in-use bit, `who = o = −1` and shrinks
  the mark over trailing unused records. Caravans are created in `Unit::init` @612100 for non-hero `is_caravan`
  (`unit_flags2 & 8`) units with `domain == 0`, closed in `Unit::close`.
- **Merchant fleets** (318/319) are `is_caravan` but sea units: no Caravan record exists, `do_trade` asserts and
  `Unit::work` reads `caravans[who][−1]`. They are not buildable in the shipped rules; guard or drop sea trade.

### 6.2 Assigning a route

- TRADE (op 17: `ox, whom, oxx, whose, queued`) → `process_trade` @948b20 (endpoints negative or alive) →
  `Group::action_trade` @701cc0 🟡: target must be active and a VILLAGE-line city (`is_trade`) or, for fleets, a coastal
  DOCK; QUEUE_FIRST uses the insert pattern; each eligible member gets `Unit::add_trade_order(ox, whom, oxx, whose,
  q == QUEUE_LAST ? QUEUE_LAST : QUEUE_NEW, explicit 1)` @5e4dc0 (QUEUE_NEW clears orders; clears UNIT_TRADE_ROUTE;
  `uid2` is read when `oxx ≥ 0 && whom ≥ 0` — tests `whom`, sic; UNIT_CAN_TRANSPORT as for gather orders).
- **Auto-trade:** `Unit::think_caravan(force)` @5f5650 (from `think` for ACTIVE leaders, after `think_peasant`): same
  idle threshold as §4.6 but from `leader_options.peasants_wait` (+8); then `find_city(x, y, SEARCH_ALLIED, who, …,
  FILTER_CAN_TRADE)` @65ba90 — players 0..7 × cities ascending, **last** city at the minimum distance (`≤`) — and
  `add_trade_order(city.o, city.who, −1, −1, QUEUE_NEW, 0)`.
- FILTER_CAN_TRADE (`valid_filter` entry 17, jump table @67e57c -> @67e204): the candidate is a city centre (flag 0x20, city ≥ 0) and some
  other alive city (own, or an ally's when the candidate's owner `has_preq(FOREIGN_TRADE 684)`) that is **`is_seen` by the candidate's owner**
  has an empty route with it both ways.
  `CityData::get_empty_trade_routes(c, p)` @7395c0 = `1 − (links in vans whose caravan has flag 2 and joins (c, p))`:
  one caravan per city pair.

### 6.3 `Unit::do_trade(order)` @5ed270 (each frame) ✅

Port: `econ_trade.cpp do_trade` (2026-10-25: re-checked against the decompile and the disassembly, `re/decomp/caravan_fix/`). The locals keep the
original's roles: (`a_city`, `a_pl`) = local_18 / param_1, (`b_city`, `b_pl`) = local_1c / local_34, the objects `ox` / `oxx` = local_24 / local_30.

1. `set_anim(CHAR_DEFAULT, 0, 1)` (may draw a Guy idle variant, §8); no caravan slot → assert, return; `making_road` → return
   (the sliced road search continues in `Unit::work`, §6.4).
2. A's city (fail if none); before starting, retarget `ox` to the city centre. Region check (unit region against A's, `can_transport` otherwise).
3. **Partner choice** (`oxx < 0`): for p 0..7 allied with the unit's owner (own, or foreign with FOREIGN_TRADE and `who == pa`), cities c
   ascending (not A itself, alive, **`CityData::is_seen(c, unit.who)`**, empty route both ways, reachable by region or `can_transport`):
   `v = (who == pa) ? trade_value(a, pa, c, p) << 2 : trade_value(c, p, a, pa)` — the first pair is the *first* (bonus) end (asm @5ed665:
   `ECX/EDX = a, pa`, the candidate on the stack; the engine had the two pairs swapped before 2026-10-25); keep the first strict maximum.
   None → (UI message) `idle = 99`, `kill_current_order`; AI units then `go_to_city(a + 1, pa, 1)` if that city exists.
4. Both buildings must be active (`flags & 4`), B's `city` ≥ 0 and both cities valid; **then** the region test again with B's region
   (`(unit ≠ A or A ≠ B region) and !can_transport` → fail).
5. **Start** (`!started`): permission (own route, or FOREIGN_TRADE), empty-route checks; **if the unit is nearer city B than city A**
   (`dist(unit, City B.x/y) < dist(unit, City A.x/y)`, asm @5edaf7) swap the local (a, pa) ↔ (b, pb) **but not `ox`/`oxx`** (quirk: on a foreign
   route started nearer B the start-frame arrival test then reads a wrong object); append `{cara, who}` to the near then the far city's `vans`;
   set the Caravan's cities (near = `city2`/`whom`), flags |= 1|2, `started = 1`; `compute_trade` of both; `Caravan::build_road()` (§6.4) — if it
   returns < 0 (still searching / failed) walk to the near city (QUEUE_FIRST, `City.x/y`) and return.
6. **Arrival** (`LAB_005edd56`, same frame after a start with a road, else every frame):
   - `loaded`: test the building A (`objects[a_pl][ox]`, `R = max(xs, ys)·96`): within `R + 0x306` on both axes → `loaded = 0`, UNIT_TRADE_ROUTE;
     unless a decoy: flags |= 4, `compute_trade(A)`, `compute_trade(B)`, `cities[pa][a].new_caravan(pb, b)`. The target is B in any case.
   - `!loaded`: B (`objects[b_pl][oxx]`) must be an active **trade building (VILLAGE line), else the route ends** (fail); within `R + 0x306` →
     `loaded = 1`; unless a decoy `cities[pb][B.city].new_caravan(pa, a)`. The target is A in any case.
   - So **without a road a caravan walks to the building it is not testing and a route never completes** (the quirk); every working route has
     a road, and the road movement (below) is what alternates the ends.
7. **Movement:** with a road (`road.length != 0`): invert the stored road if its *bottom* end is nearer to the unit than its top, copy it
   into the unit path (`Stack::operator=`: the unit's allocation grows to the road's), lane offsets (for i ≥ 1 with |dx|, |dy| to the previous
   *unshifted* point ≤ 192: `x += dy / 3`, `y += ((dx·0x55555555) >> 32) − dx) / 2`), pop the top, `add_move_order(bottom point,
   pathed, QUEUE_FIRST)`, flag 4 on the new top when its region differs from the unit's, then **invert the stored road unconditionally**
   (its orientation toggles on every use — keep). Without a road: `find_nearby_spot` around the target (radius `R·96 .. R·96 + 0xc0`, `R` of
   the building that was *tested*) and move if within `R + 0xc6`.
8. Fail: `kill_current_order(0)` (→ `end_trade_route` via close_order); with no order left `think_caravan(1)`.

`Unit::work` @60d180 (`caravan_work`, called from `work` in orders.cpp): for a caravan with `caravan_flags & 6` and an order that is not
CAST_SPELL: the route is kept only while `get_action()` is TRADE_ROUTE (else `end_trade_route`); `making_road` → `Caravan::build_road` (one
slice per frame); a current order that is not TRADE_ROUTE needs the *next* order to be (else `end_trade_route`).
`Unit::end_trade_route` @5e3bd0: only while playing (not loading); clears UNIT_TRADE_ROUTE, `caravan_flags` 2 and 4, the suspended search
and the road; for a TRADE_ROUTE action sets `loaded = 0` and removes the `{cara, who}` link from the **order's** two cities (`ox`/`oxx`, if
active, with a valid city), re-running `compute_trade` on each (not the record's: a route started nearer B swapped them).
`City::close` repaths its caravans (🟡 cities.cpp TODO).

### 6.4 Roads ✅ (ported 2026-10-25; replay-checked)

`Caravan` (0x50) also holds `road` (`Stack<PathData>` +0x10: list, `size` +0x14, `length` +0x18, increment −1 so `increase_size` doubles 0 → 1 → 2 →
4 …), `making_road` +0x20, `reset_road` +0x24, the suspended search +0x28..+0x40 (trees, first direction, goal, expansions). The original's saves
walk only the road, `making_road` and `reset_road` (so a loaded game restarts a suspended search). Engine: `CaravanRec` (game.hpp), native save
version 11 (the search is saved with `PathNode::z_val`).

`Caravan::build_road` @73db10 (`caravan_build_road`): `clear_road` (length 0, the allocation stays); cities invalid → `clear_temp_road`, 0;
resume (`making_road` and not `reset_road` and trees present) → `find_road_restore`, else (`reset_road` or no trees) `clear_temp_road`,
`reset_road = 0`, `find_road(A.o, A.who, B.o, B.who, unit, unit.who)`. Result < 0 → −1 (suspended or bad endpoints), 0 → 0 (no tiles), else
the stack (top = the node next to A) is popped into a temporary stack and **laid**: a land tile (`TData & 0x30 != 0x20`) → `World::set_road_at`,
flags |= 0x20, thinning counter reset; a water tile is kept when the counter is 0 or fewer than 5 tiles remain on either side (`tolerance = 0x180`,
counter = 4) else dropped (counter − 1); the first and last kept node get flag 1; the nodes are pushed back (same orientation), `reset_road =
making_road = 0`, `Caravans::reset_paths` @73e060 (every in-use caravan that is searching gets `reset_road = 1`), return 1.
`PathFinder::find_road` @688a40 + `astar_caravan_road` @685990 with `cara ≥ 0` (movement-spec §12): 8-connected, the first direction as for buildings,
**heuristic `vector_dist(N.x − gx, −gy)·40/16`** (asm @685fd9: the y argument is `−gy`, no abs — an original bug), `can_transport` from the unit
(`unit_can_transport`), 3,200 valid expansions per call; at the budget (or at the goal with ≥ 3,200) the popped node goes back to the open
list, the trees, direction and goal are stored in the record, `making_road = 1`, −1; `kill_lists(pathfinder)` after every search (also for
building roads). Replay check: `21'56'12 -> 21'56'43`: 8 of 9 caravans at the next save's position/orders (3 of 9 before), the stored
roads equal node for node (39 nodes, allocation 64).
`Caravan::verify_road` @73d950 (`caravan_verify_road`, called from `Unit::do_move` when a path point with flag 0x20 is reached and its
tile is not a road: a building (`TData & 0x80`, `(& 3) == 3`) on the road rebuilds it) and `Caravans::new_danger(x, y, pl)` @73e0c0
(`caravans_new_danger`, from `Object::take_damage` when a caravan is killed outside friendly territory or by a building: every allied in-use
route whose road passes within 0x600 — or that is searching — restarts: clear road and trees, flags &= ~4, `repath`, `build_road`;
`trade_val` is not recomputed) are ported but **not called** (movement.cpp / combat_damage.cpp are other owners' files).

### 6.5 Trade value and income ✅

```
Caravan::trade_value(cA, pA, cB, pB) @73d9d0:                         // bonuses use pA
    d = Caravan::distance(W-cells of A and B)    // 0 if D < xs/4, 1 if < xs/2, 3 if >= 4·xs/5, else 2 (D = vector_dist, xs = W width)
    v = A.get_trade_value() + B.get_trade_value()       // num_buildings + 2·level − 2; level 1 village, 2 town, 3 metropolis/Forbidden City
    if d: v = (d + 3)·v / 3
    if pA != pB: v = v·3/2
    if Indians (21): v = pct(v, indians_caravan)        // 15
    if SPICE (good 12) in rare|rare_conquest: v = pct(v, spice_caravan_income)   // 20
City::compute_trade @739640:
    trade_val = Σ over vans links with caravan flag 4 and both cities alive of (short)((v·16)/2)    // v from the city's side
    if trade_val changed: leaders[city.who] |= LEADER_RECOMPUTE
```
`trade_val` is added to the city's wealth rate in `calc_city_resources` (§3.3): one completed route yields
`trade_value·16` of rate split over its two cities (rates are paid continuously, not per trip). It is refreshed only by
`compute_trade` (route start, completion at A, route end) — city growth does not update it until then.

Lump sum, `City::new_caravan(pl, c)` @739750: the first time player `pl`'s city `c` (bit `c & 31` of `traded_with[pl]`)
delivers to this city: `bucket[WEALTH of pl] += (epoch[2](pl) + 1)·10`, doubled when `pl` is foreign (no cap). Later
arrivals pay nothing.

`LeaderData::get_caravan_limit(pairs)` @6dca50: `n = epoch[2] + 1` + `colossus_caravan` (COLOSSUS) + `taj_caravan`
(TAJMAHAL; reads −1, the XML element name differs, rules-spec §12.3) + `silk_caravan` (SILK) + `nubian_caravan_limit`
(Nubians), ≤ 99; with `pairs`: ≤ c(c−1)/2 where c = own cities (+ mutual allies' with FOREIGN_TRADE). Training uses
`pairs = 0` (production-spec). Nubian MARKETs train a free caravan on activation (`Build::activate`).

## 7. Population cap, supply, attrition, oil wells

### 7.1 Population cap: `Leader::calc_pop_cap` @6dc490 ✅

```
L = category pop_limits[setup.pop_limit].data[0]          // 50, 75, 100, 125, 150, 200
misery = 0
if (scripted rules or CtW) and ScenarioData::pop_cap[who] >= 0: pop_cap = that; goto tail
if LEADER_IGNORE_POPCAP: pop_cap = L; goto tail
e = epoch[0] (military);  pop_cap = pop_cap_const[e]       // 25, 50, …, 200
if L > pop_cap_const[7]: …                                 // unreachable with the shipped data (L ≤ 200)
if objects.wall_mark[who] != 0: pop_cap += Σ alive cities CityData::pop_cap()   // village_pop·level = 0 shipped
if Bantu (3): pop_cap = pct(pop_cap, bantu_pop_cap); L = pct(L, bantu_final_pop_cap)
pop_cap = min(pop_cap, L);  if VIRTUAL_REALITY_BONUS: pop_cap = L
tail: if PEACOCKS (good 25): pop_cap = pct(pop_cap, peacocks_pop);  if COLOSSUS: pop_cap += colossus_pop_cap
LeaderData.pop_cap (+0x7e4) = pop_cap
```
The tail also applies to the scripted and IGNORE_POPCAP paths (the cap may exceed L). Called from `gain_tech` (epoch
techs), `calc_gather` (rare change), city init/close/upgrade/capture, `Build::activate/finished/close` (cities,
Colossus), scripts and the editor. Units count through `LeaderData.control` (+0x940, `+= control_cost` in
`Unit::set_type` for non-decoy captains); `check_population(type)` @6e1330 = `control + control_cost > pop_cap`
(training blocks on it and counts `pop_issues`, production-spec). `CityData::get_pop_value` (1/3/5) feeds
`LeaderData.pop` (+0x95c), a city-population statistic for score, not the cap. `min_pop_limit`, `max_pop_limit`,
`military_pop`, `granary_pop` have no simulation readers.

### 7.2 Supply

`Supply` records (0x10; `supply_mark` +0x434; unit +0x84) for `is_supply` units (`unit_flags2 & 0x40`: supply wagons
and three gov heroes). `SupplyData::get_radius` @73b560 = `supply_radius + supply_radius_upgrade·get_supply_upgrade()
(+ terra_cotta_range with TERRACOTTA)` tiles; `get_supply_upgrade` counts SUPPLY_WAGONS_1..3. `Supplies::find_supply`
@73aba0 returns the **first** live on-map wagon within radius·192 (index order). `Unit::process_supply` @5e0560
(attrition ticks, combat-spec §7) and `UnitData::in_supply` @609ef0 (recharge) use it; supply has **no economic
effect** (no consumption: `supplies_left` returns 1, `Supply::deplete` is empty, `think_supply` has no callers).

### 7.3 Attrition, leader side (floats ✅ SSE single precision)

`Leader::calc_attrition` @6cdea0 (int, `att` +0x7f0): 0 unless ATTRITION1..4 techs (consecutive count n → 
`attrition_improved[n − 1]`); then COLOSSEUM, Russians, CtW byte +0x6900, KREMLIN each `pct(att, …)` with 0 → 1;
`give_att_disabled` forces 0. `Leader::calc_anti_attrition` @6cdcc0 (float `anti_att` +0x7f4): 256.0f;
`take_att_disabled` → 0; then for FORAGING level (`attrition_upgrade[k]`), STATUEOFLIBERTY, Mongols, TITANIUM in that
order: `u ≥ 100` → 0 and stop, else `anti = (anti·100.0f) / (float)(100 − u)` (two roundings, that order). Both run in
`Leader::calc_unit_stats` (LEADER_CALC_UNIT_STATS, set e.g. by rare changes, `set_age`, `set_epoch`) and `Leader::init`.
`UnitData::get_attrition(p)` @608fd0 (combat-spec §7.4) multiplies `(float)v · anti_att · 0.00390625f` and truncates
(`CVTTSS2SI`). `Leader::calc_misery` @6cdff0 only zeroes `misery` and has no callers; misery is always 0.

### 7.4 Oil wells

`OilWell` records (0x10: slot, `o`, `reg` = W-cell region at creation, flags, who; `oil_well_mark` +0x430), created by
`Build::activate` for OILWELL/OILPLATFORM (slot stored in Build +0x78), closed by `Build::close`. Used only by the
income loop of §3.2.


## 8. Every `game_random` draw of the economy

One simulation stream (replay-spec §12). Draws in the gathering code, in the order they happen within a frame (§1):

| # | Site | Draw | When | Iteration context |
|---|---|---|---|---|
| G1 | `Build::find_gather_tiles` @623350 (from `Build::init` @629740) | `4·count` × inline `game_random % count` (none when `count ≤ 1`) | a woodcutter/mine is created and its tile scan added tiles | object creation order (scenario load, `Objects::init_build` from BUILD / production) |
| G2 | `Build::find_gather_tiles` (from `Build::process`) | same | the building's W-region has flag 0x10 and new tiles were found | building pass of `Objects::process_all` (frame-spec §6) |
| G3 | `Unit::do_gather` @5ef2a0, farm type 1 | `GameAccess::rnd(x_size/2)` then `rnd(y_size/2)` (each skipped when the argument ≤ 1) | `(7·o + frame + who) % 256 == 0` | unit pass |
| G4 | `Unit::do_gather`, other farms | `rnd(x_size)` then `rnd(y_size)` | the farmer's cell needs no work (status fall-through) | unit pass |
| G5 | `Unit::do_non_flat_gather` @5f0170 | inline `% 200 + 400` | a new resource tile is chosen | unit pass |
| G6 | `Unit::do_non_flat_gather` | inline `% 100 + 300` | guy 0 plays CHAR_CHOP_WOOD and `wait` reaches 0 while some gatherer of the building is idle | unit pass |
| G7 | `Unit::do_non_flat_gather` | inline `% 50 + 100` | at the tile (not chopping yet), `wait` reaches 0, same condition | unit pass |
| G8 | `Unit::think_fish` @5f4c60 | inline `% 60`, once per scanned water W-cell of the region (≤ 289) | AI fishing boat re-thinks (idle, not still yielding) | unit pass, `k = 0..0x120` order |
| G9 | `Farms::inc_time` | farms-spec §3.3 | every frame per farm | `Objects::inc_time` |
| F1 | `Farms::add` @8d8a40 | `game_random & 3` (3 = pen) | a farm whose city has n = w other farms (not 4, no nearest farm within 0x480, not 3/3) | `Build::init` (creation order) |
| F2 | `Farms::add` | two draws (ambience position `% 3`, `% 3`) | a new wheat farm in a city with ≥ 2 other wheat farms none of which has the ambience bit 4 | `Build::init` |
| F3 | `Farms::add_animals` @8d8f30 | per animal: type `& 1`, then y `% 0x180`, x `% 0x180` (5 animals) | a pen farm activates (`Build::activate` of every FARM calls it) | `Build::activate` |
| G10 | `Guy::set_anim` behind every `set_anim(CHAR_SOW/REAP/FARM/CHOP_WOOD/…)` | animation variant draws (anim-spec) | when an animation (re)starts | unit pass |
| M1–M3 | `GameDaemon::calc_market(i)` @732270 | `rnd(half + 1)`, `rnd(half + 1)`, then `rnd(trend_range)` (if `trend_range ≥ 2`) | tick 0: goods 0..5 (18 draws); then when a good's `flux_length` expires on its tick | `GameDaemon::process_all` step 4, every frame |
| M4 | `Leader::use_market` @6c91c0 (AI) | `rnd(6)` | each pass needing wealth | `Leaders::strategy_all`; also script `place_city_with_cost` |
| S1 | `Game::init_starting_resources` @58a500 | `rnd(12)` | setting 12 (Random) | `Game::init` |
| S2 | same, per good 0..5 | `rnd(span·base)` | `lo ≠ 0`, `span ≠ 0`, `span·base > 1` | `Game::init` |
| T1 | `PathFinder::calc_road_cost` @686328 | `rnd(20)` per expanded neighbour | caravan road searches: route start, resume in `Unit::work`, `verify_road`, `new_danger` restarts | unit pass / `take_damage` |
| T2 | `Guy::set_anim` via `set_anim(CHAR_DEFAULT)` at the top of every `do_trade` | idle variant `% 100` per guy | anim-spec conditions | unit pass |

No draws in `do_buy`/`do_sell`, `calc_market_prices`, tribute, `calc_rare`, `trade_value`, `compute_trade`,
`new_caravan`, `get_caravan_limit`, `think_caravan`, `find_city`, the supply functions, `calc_pop_cap`, the attrition
functions or the oil-well records. (`Leader::init` draws one unrelated value at @6e4eb6.) **No draws** in
`Leader::gather`, `calc_gather`, `calc_resource_caps`, `do_gather`, `calc_city_resources`, `BuildTypeData::calc_gather`,
`max_gatherers`, `add_gather_order`, `find_gather_spot`, `think_merchant`, `UnitData::calc_gather` or the gatherer list
functions.

Iteration-order dependencies (besides the RNG): `find_gather_spot` keeps the first best building in slot order;
`think_merchant` walks `new_rares` in list order and rotates the chosen rare to the end; `do_non_flat_gather` rotates the
chosen tile to the end of `gather_from`; `Build::add_gatherer` pushes at the list head (newest first), which is the order
`check_gatherers` / `all_gathering` walk.

## 9. Rules data read

From `rules.xml` CONSTANTS (rules-spec §12, all loaded by `engine/sim/rules_constants*`): `starting_goods[6]`,
`basic_gather[6]`, `city_gather[6]`, `gather_rate`, `peasant_rate` (frac×256), `scholar_rate[6]` (frac×256), `oil_rate`
(frac×256), `granary/lumbermill/smelter_bonus[5]`, `refinery_bonus`, `commerce_cap[8]`, `woodcutter_radius`,
`mine_radius`, `river_resource_value`, `village/building/market/temple_taxes`, `territory_taxes[5]`,
`village/university/library_literacy`, `mtn_{tiny,small,medium,large,huge}_{size,gather}`, `unit_gather_respond_range`,
the wonder constants (`pyramids_food/_commerce`, `colossus_wealth/_commerce`, `hanging_gardens_knowledge`,
`forbidden_city_gather/_base_gather`, `tikal_timber/_timber_commerce`, `porcelain_market`, `angkor_metal/_metal_commerce`,
`taj_wealth/_wealth_commerce/_farms`, `kremlin_commerce/_farms`, `eiffel_oil/_oil_commerce`, `global_prosperity`), the
tribe constants (`inca_wealth_cap`, `inca_wealth_per_miner`, `roman_city_gather`, `egyptian_food_commerce`,
`egyptian_farm_wealth`, `french_timber_commerce`, `french_woodies`, `british_commerce`, `british_taxation`,
`german_city_gather`, `german_miners`, `russian_oil`, `japanese_fishing_boats`, `mongol_nomadic_food`, `iroquois_food`,
`lakota_food`, `americans_barracks_gather`, `dutch_interest`, `dutch_interest_cap`), `coffee_income_bonus`,
`diamonds_commerce`, `capitalism_oil_prod`, `republic_commerce_bonus{,2,3}`, `theceo_production_bonus`,
`ctw_prod_rate_bonus`, `ctw_missionaries_bonus`, `ctw_nomad_starting_res_x`, plus the §5–§7 constants.

`rules.xml` LANDS (9 × 4 MAKE: Land → 1 knowledge + 1 food, Forest → 1 timber, Mountains / Cliffs → 1 metal, Oil → 1
oil; loaded as `Rules::lands`). Goods: `obsolete_production_good/_rate` (§3.2), `bonus_type/num` (rares, §5). Buildings:
`build_flags` (0x40 gather type, 0x10000000 flat, set for 417/421/422 by `finalize_init_all`), `x_size`/`y_size`,
`domain`. Units: the type vslot +0x124 used as the rare/fish search radius (❓ identify the field), type flag `+0x2b8 & 4`
(packer boats). Techs read by `has_preq`: GRANARY2..5 (699–702), LUMBERMILL2..4, SMELTER2..4, UNIVERSITY2..6
(737–741), REPUBLIC_1..3 (797–799), CAPITALISM_1 (805), VIRTUAL_REALITY_BONUS (695). Category list `pop_limits` (§7).

**What `engine/sim/rules*.hpp` already has:** every constant (by offset; `constant_offset(name)`), `lands`, goods' obsolete
and bonus fields, `build_flags`, sizes, tribes, categories — enough for this spec. **Missing** (runtime, not rules):
`LeaderDataEncrypt` / economy fields of `Leader`, `CityData`, gather fields of `Build` (`gather_down`, `gather_max`,
`gather_from` + `mtn`/`cliff`, `city`, `city_down`, `farm`) and `Unit` (`gather_down`, `good_obj`, `rare`, held doober),
the GatherOrder fields beyond `objects.hpp`'s TargetOrder, `GameInfo` setup bytes (`starting_resources`,
`starting_resources2`, `tech_cost`, `game_rules`, `difficulty`, `pop_limit`), `Game.starting[6]` and the market arrays,
leader options (`peasants`), mountain-range / cliff deposit tables with `solid_mount_wx/wy`, `mount_tx` (partly in
`engine/sim/mountains.*`), the goods object list and `ObjectsData::find_good_at`, and a named accessor for
`TypeIndex` tech constants (GRANARY2 …) — add them to `rules.hpp`'s enum list rather than hard-coding numbers.

## 10. Worked examples (`node re/tools/gather-ref.mjs --examples`)

With the shipped rules (gather_rate 450, peasant_rate 2560, oil_rate 8960):

| # | Case | Result (rate ×16 per 450 frames) |
|---|---|---|
| E1 | farm on 16 Land tiles, 1 farmer, no granary | per 160, food 160 (10 / 450 frames) |
| E2 | same farm with 4 river tiles, granary level 3 (+100%) | raw 20 → per 200 → ×2 = 400 |
| E3 | oil well on 16 oil tiles | per 560 (35 / 450 frames) |
| E4 | woodcutter, 5 claimed forest W-cells, 7 woodcutters, lumber mill level 2 (+50%) | 5 slots, per 240, timber 1200; 2 woodcutters idle-count |
| E5 | E4 as French | 6 slots, timber 960 (no mill) |
| E6 | mine on a 150-tile range, 6 of 8 solid W-cells claimable, 5 miners, smelter level 1 (+50%) | slots 5·6/8 = 3, per 240, metal 720 |
| E7 | university, 4 scholars, level 1 / 3 | per 80 / 160, knowledge 320 / 640 |
| E8 | capital: 3 × E1 farms, a 5-man woodcutter, market, university | food 640, timber 960, wealth 160, knowledge 160 |
| E9 | E8 + territory 600 of 12000 land tiles, taxation level 2 (100%) | wealth 240 |
| E10 | commerce caps at commerce level 1 | 1600 each, knowledge 15984 |
| E11 | food rate 690, 450 frames | +43 resources, leftover 900 |
| E12 | rate 2000 against cap 1600 | +100 per 450 frames, `over_cap` 1, `income` 1600 |

`--selftest` checks that 7200 frames of any rate ≤ 15984 pay exactly that rate (the leftover returns to 0), the mine
slot floor, and the knowledge cap.

## 11. Player commands (replay-spec opcodes)

| op | Command | Layout (after the u8 op) | `process_*` → handler | Section |
|---|---|---|---|---|
| 17 | TRADE | i32 ox, whom, oxx, whose, queued | `Group::action_trade` (caravan routes) | §6 |
| 18 | CITY_GATHER | i32 t, queued | `process_city_gather` @948a10 → `Group::action_city_gather` @701780 | §4.1 |
| 19 | GATHER | i32 ox, queued | `process_gather` @9488b0 → `Group::action_gather` @700b90 → `Unit::add_gather_order` | §4.1, orders-spec §2.6 |
| 22 | GATHER_POINT | i32 x, y, action, add_to_end | `process_gather_point` @948510 → `Group::action_gather_point` @6ff1b0 → `Build::add_gather_point` | orders-spec §2.9 |
| 39 | CLEAR_TRIBUTES | i32 who, whom | `process_clear_tributes` @9471b0 → `Leader::action_clear_tributes` | §5.6 |
| 41 / 42 | ACCEPT / REJECT | i32 who, whom | `process_accept` @946fb0 / `process_reject` @946e90 → `Leader::action_respond` | §5.6 |
| 43 / 44 | TRIBUTE / DEMAND_TRIBUTE | i32 who, whom, good, amount | `process_tribute` @946d70 / `process_demand_tribute` @946c50 → `Leader::action_offer(±amount)` | §5.6 |
| 46 / 47 | BUY / SELL | i32 who, good, flags (1: ×5, 4: ×10/×100, 2: all) | `process_buy` @946a20 → `action_buy`; `process_sell` @9468a0 (inline) | §5.5 |
| 73 | LEADER_OPTIONS | i32 who, peasants, peasants_wait, buildings, BitMask<32> flags | leader options: `peasants` = auto-gather delay (§4.6) | §4.6 |

There is no "auto-gather on/off" command: idle workers always auto-gather after the `peasants` delay; worker stances
(STANCE command, orders-spec) and `UNIT_AI_OFF` can suppress it. Rally points (GATHER_POINT) decide what newly trained
citizens do (`Unit::come_out`, frame-spec §10).

## 12. Port plan (ordered)

1. **Market drift first (RNG parity).** `calc_markets` draws `game_random` from frame 0 (18 draws at tick 0, then 3
   per redraw) whether or not anyone trades, so every replay's RNG stream depends on it: port `Game` market arrays,
   `Setup::build_game` init, `calc_markets` / `calc_market` (§5.1–5.3) into `GameDaemon::process_all` step 4 now, and
   `Game::init_starting_resources` (§5.7, draws S1/S2 when the setting is random/variable) in `Game::init` order.
2. **Leader economy state.** Plain-value `LeaderEcon` per leader (bucket, leftover, resource_cap[7], over_cap,
   resources, support, income, rate, bonus, epoch[4], escrow, escrow_rate, tributes, base_rate, collected, rare masks,
   rares_collected, known_rares, gather_stamp, bonus_cap, territory) with the XOR keys applied only in the DataWalk /
   checksum writer; `Leader::init` economy resets and starting stockpiles (§5.7).
3. **Income pipeline skeleton.** `Leader::gather` → `calc_gather` (cadence, `LEADER_RECOMPUTE` as a dirty flag with
   the setters of §3.1) → `calc_resource_caps` → `do_gather` in `Leaders::process_all`; with zero buildings this
   already pays nothing but sets caps/income exactly. Check against `gather-ref.mjs` (`doGatherFrame`,
   `resourceCaps`).
4. **Building rates.** `CityData` economy fields and `City::calc_gather` / `calc_city_resources`; `BuildData` gather
   fields; `BuildTypeData::calc_gather` paths A–D, `max_gatherers`, `total_gather_access`; deposits
   (`MountainRangeData::gather_size`, `CliffsData::gather_size`, `find_nearest` — extend `engine/sim/mountains.*`).
   Unit-test each path against `gather-ref.mjs` (flat/radius/mine/university helpers) with hand-built tile sets.
5. **Gather tiles.** `find_gather_tcoords` (port from the disassembly), `find_gather_tiles` with its RNG shuffle in
   `Build::init` (creation order matters: the scenario loader must create buildings in file order, see
   sim-implementation "units and buildings are created in separate passes"), `verify_gather_tiles`, tile bit 0x1000,
   region flags 0x20/0x10 in `Build::process`.
6. **Gatherer lists and the GATHER order.** `add_gatherer` / `remove_gatherer` / `check_gatherers` / `num_gatherers` /
   `is_gathering_at`; `add_gather_order`; the GATHER hooks of `kill_current_order` (orders-spec §4.3); GATHER and
   CITY_GATHER commands (`Group::action_gather`, orders-spec §2.6).
7. **`Unit::do_gather` and `do_non_flat_gather`** line by line (§4.2–4.3), with `Farms::grow/snip` (farms-spec), held
   doobers, the walking/dump animations and RNG G3–G7. This is the part most likely to drift; compare positions and
   RNG counts per frame in `ron-sim` digests.
8. **Auto-gather.** `think_peasant` gating, `find_gather_spot` scoring (§4.6), LEADER_OPTIONS `peasants`,
   `add_think_order` / `do_think_order`; `LEADER_IDLE_PEASANT`.
9. **Fishermen, merchants, rares.** `UnitData::calc_gather` (crowding), `Unit::do_gather(out)`, `calc_rare`, Porcelain
   and conquest rares, `think_fish` (RNG G8), `think_merchant`, `new_rares` (needs `World::compute_reg_territory`).
10. **Leader-wide terms.** Refinery, capitalism, coffee, territory taxes (needs territory from the borders code),
    `calc_resource_bonuses`, obsolete goods.
11. **Markets and tribute commands.** BUY/SELL (`do_buy`/`do_sell`, §5.5), TRIBUTE/DEMAND/CLEAR/ACCEPT/REJECT
    (§5.6, the resource paths first; treaty branches with diplomacy).
12. **Trade, supply, population** (§6–§7).
13. **Harness.** Add `LeaderEcon` (stockpiles, rates, leftovers, caps) and the gather lists to the `ron-sim` digest;
    `--gather WHO:o..@B` and `--buy/--sell` options; a JS/C++ parity run of `gather-ref.mjs --examples`; the first real
    oracle is a replay with citizens gathering (CHECK_SUMS in multiplayer cover LeaderData, which includes the decrypted
    economy fields, state-spec).


## 13. Open questions

1. `bonus[k]` (LeaderDataEncrypt +0xc4) is reset by every recompute and `rate[k]` (+0xac) is never written by the
   gathering code; `base_rate[k]` (+0x4b0) is only zeroed by `Leader::init`. Find their writers (production, scripts) or
   confirm they are vestigial.
2. Who raises the W-region flag 0x20 that triggers `verify_gather_tiles` / `find_gather_tiles` (territory changes,
   tree removal?). Until known, gather tiles only refresh at building creation.
3. `find_gather_tcoords` and the scan loops of `BuildTypeData::calc_gather` path D are register-confused in the
   decompile (the W-cell circle coordinates and the 16-tile `traverse_x/y` loops); port from `re/decomp/econ/asm/`.
4. The exact cosmetic state machines of `do_gather` (farm) and `do_non_flat_gather` (§4.2–4.3) need a line-by-line
   port and a replay check; in particular that miners never return (`wait = 1000000`, early return while CHAR_MINE_ORE
   plays) and the role of `Build.recharging` (+0x7a) / `build_masks` 0x800 for gather buildings (who clears 0x800?).
5. The rare/fish search radius (unit type vslot +0x124) and the crowding distance arguments of `UnitData::calc_gather`
   (lost in the decompile): read from the asm.
6. Walking-with-stuff animations (CHAR_WALK_WITH_WOOD etc.): chosen where? (`Guy` / `update_gpiece` from
   `UNIT_*_WOOD/METAL`.)
7. `GameAccess::ai_speed` > 1 in a synced game? (multiplies the paid rate). `GameInfo.flags & 2` and game_rules 8/9
   meanings (×1.5 income, defender stockpiles).
8. Treaty / alliance / declare-war branches of `Leader::action_respond` and the type vslot +0x78 cost formula (DoW
   costs) are decompile-only.
9. Trade: `Unit::work`'s "previous order is TRADE_ROUTE" test (list semantics), the two caravan quirks (§6.3 steps 5
   and 6) and the unused 7th argument of `add_move_order` need a replay; `Object::take_damage`'s caravan-danger gate
   tests `domain == 0` (combat-spec §6 says "armed" — correct it there).
10. CITY_GATHER's seat-local `clear_gather` (§4.1) — confirm it cannot touch a non-city building in practice.
11. Who else sets `LEADER_CALC_UNIT_STATS` (refreshing `att` / `anti_att`), e.g. wonder completion.
12. Nothing here is validated: the first oracles are a replay with citizens gathering (income), one with market trades
    (price drift is already exercised by every replay through M1–M3) and one with a caravan route.

### 13.1 Resolved or corrected by the port (engine/sim econ_*.cpp)

- World +0x78 (`total_land` above) is `land_size`, set by `World::analyze_map` @6b58b0 (from `Setup::build_game`, before
  `compute_all_territory`): the sum of the land regions' sizes (regions 1..63).
- `ScenarioData::buildings_gather` gates the buildings / cities / units part of `calc_gather` in scenarios; its static
  is 0 but stock scripts call `buildings_gather_disable()` (ceraiding1.bhs), so a scenario starts with it on (the
  port's default; scripts are not run yet).
- Unit +0x54 (`rare`) is 0 after `Unit::init` (not −1); `Group::action_gather` reads it as "previous type" (scholar
  militia) and falls back to the unit type only when negative.
- `Unit::find_gather_spot` returns 0 (no order) when the best building is the one the unit already gathers at.
- `Unit::do_think_order` returns after `kill_current_order` when any order is left; only then `think_peasant(1)`.
- Human rare collectors that are packed: `Unit::think` re-checks them on the first idle frame or every 32 frames and
  increments `idle` (+0xb0) after the check.
- `LeaderData::get_diff` @6ec000 (the AI repair test of `do_gather`) = `multi_diff` in multiplayer or under scripted
  rules outside CTW when ≥ 0, else `GameInfo.difficulty`.
- `Build::activate` calls `Farms::add_animals` for every FARM (it returns at once for wheat farms); `Build::close`
  removes the farm record and kills the gaia animals whose AnimalData `ox/whom` (+0x150/+0x152) point at the farm.
- `CityData::count_farms` @7368c0 counts live farms of the city with a farm slot whose type is not a pen (finished or
  not); `Farms::add`'s n is `count_buildings(FARM, exact, any state) − 1`.

## Evidence

| Function(s) | Address | Section |
|---|---|---|
| `Leaders::process_all`, `Leader::gather`, `calc_gather`, `calc_resource_caps`, `do_gather`, `calc_support` | 6ed2a0, 6ce280, 6ceee0, 6ce900, 6ce450, 6ceea0 | §2–§3 |
| `LeaderData::get_gather_handicap`, `get_mod_resource_cap`, `calc_resource_bonuses`, `calc_city_resources`, `get_granary/lumbermill/smelter/university` | 6d66a0, 6d65b0, 6db030, 6d5530, 6db340/6db3a0/6db3f0/6db1f0 | §2.5, §3 |
| `City::calc_gather`, `CityData::enhancer_amount`, `get_taxes`, `get_literacy` | 737c60, 738360, 737b50, 737c00 | §3.3 |
| `BuildData::calc_gather`, `BuildTypeData::calc_gather`, `max_gatherers`, `total_gather_access`, `gather_radius`, `get_good`, `GoodTypeData::get_enhancer/best_gather_type` | 62d360, 639e40, 63c430, 636500, 63bc60, 63bd50, 66fcf0/670230 | §3.4 |
| `Build::add_gatherer/remove_gatherer/check_gatherers/all_gathering`, `BuildData::num_gatherers/is_gathered_by`, `UnitData::is_gathering_at/is_gathering/can_gather` | 62f640/62f8d0/62f710/62f570, 630450/62f520, 608880/6089b0/608850 | §3.5 |
| `Build::find_gather_tiles/verify_gather_tiles`, `BuildTypeData::find_gather_tcoords`, `World::set_gathered_at`, `MountainRangeData::gather_size`, `CliffsData::gather_size`, `WorldData::has_gather_access` | 623350/623570, 63bdc0, 6b46b0, 89d170, 8a8f80, 6b4e50 | §3.6 |
| `Unit::do_gather(out)`, `UnitData::calc_gather`, `Unit::check_idle` | 5fce20, 609180, 6032c0 | §3.7, §3.1 |
| `Unit::add_gather_order`, `do_gather`, `do_non_flat_gather`, `think_peasant`, `find_gather_spot`, `think_fish`, `think_merchant`, `think` | 61a5c0, 5ef2a0, 5f0170, 5f5760, 5f5170, 5f4c60, 5f4740, 5f6e40 | §4 |
| `CommandPackage::process_gather/city_gather/gather_point`, `Group::action_city_gather` | 9488b0/948a10/948510, 701780 | §4.1 |
| `GameDaemon::calc_markets/calc_market`, `LeaderData::calc_market_prices`, `Leader::action_buy/do_buy/do_sell`, `process_buy/sell` | 732180/732270, 6dc2a0, 6cfa20/6cfbd0/6cfc60, 946a20/9468a0 | §5.1–5.5 |
| `Leader::action_offer/clear_agree/action_respond/consider_tribute`, `LeaderData::scale_tribute/is_tribute_period` | 6d1780/6d1af0/6d03c0/6d1360, 6d5240/6d5b90 | §5.6 |
| `Game::init_starting_resources`, `Setup::build_game`, `Leader::init`, `LeaderDataEncrypt::init` | 58a500, 5ac190, 6e3930, 6d9c60 | §5.1, §5.7 |
| `LeaderData::calc_rare/has_rare`, `Leader::new_rare/use_market/market_speculation` | 6e08d0/6e0770, 6d9e70/6c91c0/6c8110 | §5.8–5.9 |
| `Unit::do_trade/think_caravan/add_trade_order/end_trade_route`, `Group::action_trade`, `Caravan::trade_value/distance/build_road/verify_road`, `City::compute_trade/new_caravan`, `CityData::get_empty_trade_routes/get_trade_value`, `Caravans::new_danger`, `LeaderData::get_caravan_limit` | 5ed270/5f5650/5e4dc0/5e3bd0, 701cc0, 73d9d0/73d300/73db10/73d950, 739640/739750, 7395c0/7363f0, 73e0c0, 6dca50 | §6 |
| `Leader::calc_pop_cap`, `CityData::pop_cap/get_pop_value`, `LeaderData::check_population`, `Supplies::find_supply`, `SupplyData::get_radius`, `Unit::process_supply`, `UnitData::in_supply`, `Leader::calc_attrition/calc_anti_attrition/calc_misery`, `UnitData::get_attrition`, `OilWells::init_oil_well/close_oil_well` | 6dc490, 737d40/738450, 6e1330, 73aba0, 73b560, 5e0560, 609ef0, 6cdea0/6cdcc0/6cdff0, 608fd0, 73f6f0/73f670 | §7 |
