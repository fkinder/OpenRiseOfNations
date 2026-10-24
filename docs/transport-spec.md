# Transports (water crossing)

How a land unit crosses water in the original, and what the engine still needs for it. Companion of
`docs/garrison-spec.md` (containers, `go_inside`, `eject_contents`) and `docs/movement-spec.md` (path flag 4).
Everything below comes from the exe (`re/decomp/transport*`, `re/decomp/transport_dis`, vtable dump
`re/decomp/transport_vt.txt`); nothing is checked against a recording yet.

## 0. Summary

There is no transport ship to build. A land unit that may be transported (`UNIT_CAN_TRANSPORT`, `unit_masks`
0x800000) paths across water with the transport flag (`PathData.flags & 4`) on the points that change medium. The
moment the unit is placed on a water tile, `Unit::set_new_location` does **not** move it: it queues a cast order for
spell 0x28a (TRANSPORT) at the front and returns 0. `Unit::do_cast` runs that order on the next frames and calls
`SpellType::cast_transport`, which creates a **Transport Barge** (or the current upgrade; a **Merchant Fleet** for
caravans) next to the unit, copies the unit's hit-point ratio, moves the unit's whole order list and path onto the
barge, flags the barge `UNIT_CAN_TRANSPORT`, and puts the unit (its o_down chain with it) inside the barge. The
barge then walks the remaining path. When the barge is placed on a non-water tile, `set_new_location` ejects its
contents (`Object::eject_contents(0, -1, carry_orders = 1, no_orders = 1)`: the unit comes out on land, inherits the
damage ratio and the barge's orders and path) and the barge dies when empty. The land unit then finishes the path.

The player can also order units onto an existing ship (`BOARD_SHIP` / `AWAIT_BOARD`, §8) and can build the barge
now with the TRANSPORT command (§9). The AI's civilian ferrying (`Unit::think_civilian_transport`) is §10 and is not
part of this delivery.

## 1. Data

| Thing | Where | Value |
|---|---|---|
| Spell TRANSPORT | craft 0x28a (650) in `craftrules.xml` "Transport" | COST empty, JOB_TIME 0, MANA 0, SPELL_RANGE 0, FLAGS empty |
| Spell DISBAND | 0x29a | shares the "no need to own the spell" rule of `is_castable` |
| Transport Barge / Galleon / Freighter | units 320 / 321 / 322 (`ptype::kTransportBarge`) | DOMAIN Sea, CARRY 20, POP 0, MOVES 25, HITS 50, FLAGS `h`, `JUMP` chain |
| Merchant Fleet / Modern Merchant Fleet | units 318 / 319 | the barge of a caravan |
| Continental Marines family | 119 (`is(0x77)`) | barge of a marine gets `unit_masks2 |= 0x200` (kMarineTransport) |
| Scout family | 69 (`is(0x45)`) | transport level 1 |
| Citizens 50..53, caravan 59 (`is(0x3b)`), merchants 61 / 62, fur trapper 400 | | transport level 3 (civilian) |
| Aircraft Carrier | 351 (`is(0x15f)`) | the only non-land unit `can_ever_transport` (it carries planes) |
| `unit_board_distance` | `rules.xml` UNIT_BOARD_DISTANCE "3/1 tile" | search radius for the barge spot |
| `unit_disembark_distance` | already used by `unit_come_out` | |
| `military_transport_bonus`, `americans_marine_speed_bonus`, `americans_marine_hp_bonus` | rules | speed / hits of barges (movement-spec; `stats.cpp:277`) |
| TRANSPORT_BONUS | tech 686 | the leader's prerequisite for transports (`Leader::check_transport`) |

Type fields: `UnitType +0x218` domain (0 land, 1 sea, 2 air), `+0x2b4` `unit_flags` (0x10 = amphibious, the `h`
flag of hovercraft-like units), `+0x2d4` `carry`, `+0x2d8` `carry_size`, `+0x308` `uber_size`.
Unit fields: `+0x68` `unit_masks`, `+0x6c` `unit_masks2`, `+0x86` `cara` (caravan slot), `+0x8e` `o_up`, `+0x90`
`o_down`, `+0x24` `damage`, `+0x50` `angle`, `+0x96` `mana_burn`, `+0x98` `spell_time`, `+0xb8` path stack, `+0xcc`
order list. Leader flags `0x100 / 0x200 / 0x400` = kCanTransportCiv / Mil / Sct; `leader_options +0x1c & 2` = "also
transport scouts" (`Game.options_flags & 2`).

Vtable slots used below (`re/decomp/transport_vt.txt`): `+0x8 is_active`, `+0x18 is_unit`, `+0x20 is_building`,
`+0x48 is_seen(who, 0)`, `+0xb8 is(type, strict)`, `+0xbc is_on_map`, `+0xc0 is_plane`, `+0xd0 is_caravan`,
`+0xd8 is_moving`, `+0xe4 get_captain`, `+0xe8 is_captain`, `+0x110 is_cargo`, `+0x11c hits(0)`, `+0x158 die`,
`+0x15c update_hits(0)`, `+0x174 update_seen`.

## 2. Who may be transported

### 2.1 `LeaderData::can_transport()` @6e0c60 → TransportType

```
if (flags & 0x100) return 3;      // civilian level: everything
if (flags & 0x200) return 2;      // military
return (flags >> 10) & 1;         // 1 = scouts only, 0 = none
```

### 2.2 `UnitData::transport_type()` @46f790 → TransportType

```
if (is(0x45 SCOUT, 0)) return 1;
t = type index
if (t in {50,51,52,53}) return 3;                       // citizens, scholars
if (is(0x3b CARAVAN, 0)) return 3;
if (t in {61, 62, 400}) return 3;                       // merchants, fur trapper
return 2;                                               // everything else is military
```

### 2.3 `UnitData::can_ever_transport()` @46f290

```
if (type.domain == 0) return 1;
if (!is_unit()) return 0;
if (type.carry == 0) return 0;
return is(0x15f CARRIER, 0) ? 0 : 1;      // disassembly @46f2e3: a carrying sea unit (barge) yes, the carrier no
```

### 2.4 `UnitData::can_transport()` @46f960 (already `unit_can_transport`)

`((unit_masks & 0x800000) && !(unit_masks2 & 0x2000)) || (type.unit_flags & 0x10)`.

### 2.5 `ObjectData::is_cargo()` @653600 (vslot +0x110)

`is_on_map() && is a unit type (50..413) && type.domain == 0` (buildings use `ObjectTypeData::is_cargo`, not needed).

### 2.6 `ObjectData::cargo_inside()` @64dcd0

Sum of `type.carry_size` over the objects of the inside chain (`inside_down` links, §garrison-spec 1) that are
active units **and captains** (`o_up < 0`). 0 when off map.

### 2.7 `ObjectData::can_carry(o, who)` @6483c0 — this = carrier, (o, who) = cargo

```
if (o == this.o && who == this.who) return 0;
if (who != this.who) return 0;
if (cargo.type.domain != 2) {                         // land / sea cargo
    if (cargo.type.domain == 1) return 0;
    if (!cargo.is_unit() || !this.is_unit()) return 0;
    if (this.is(0x15f CARRIER, 0)) return 0;
    return cargo_inside(this) + cargo.type.carry_size < this.type.carry;
}
// air cargo (carriers, airbases, missile silos): TODO(air), not needed here
```

### 2.8 `Leader::check_transport()` @6bc5f0 (fix of `techs.cpp` `check_transport`)

Already ported except for the per-unit test. With `want = has_preq(TRANSPORT_BONUS) ? 3 : 0` and docks present:
for every active unit with `can_ever_transport()`: skip when `!(options_flags & 2) && is(0x45 SCOUT, 0)`; then
`if (transport_type() <= want) unit_masks |= 0x800000`. (The current port grants the flag to every land unit; with
`want == 3` the only difference is the air / sea units and the carrier, so use the real predicates.) The
clear branch (`flags & 0x700`) is right.

### 2.9 `Group::action_set_transport(flag)` @7024b0 (command SET_TRANSPORT)

`level = LeaderData::can_transport()`; `on = level != 0 && flag`. For every active group member with
`can_ever_transport()`: `on ? unit_masks |= 0x800000 : unit_masks &= ~0x800000`. (Prelude: the usual
`ScenarioData::ignore_orders` kick and `action_begin` (+0x14), as the other group actions.)

## 3. Entering and leaving water: `Unit::set_new_location(x, y, p3, p4)` @5f8d20

The engine's `unit_set_new_location` (`guys.cpp:306`) has the structure; this fills its `check_transform` TODO.
With `wc` (wcoord change) and `tc` (tile change) as in the port:

```
check = wc
if (!wc && tc) {
    half = WData(old wcoord).flags & 0x100 (HALF_LAND)          // local_20 = that bit
    if (has_tribe_bonus(who, 0x12)) {                           // tribe bonus 18: forest walkers
        if (type.domain == 0)
            unit_masks = (tile(new).medium == 0x30 TREE) ? unit_masks | 0x800 : unit_masks & ~0x800;
    }
    check = half
}
if (check && (((unit_masks & 0x800000) && !(unit_masks2 & 0x2000)) || (type.unit_flags & 0x10))) {
    d = type.domain
    if (d == 0 && tile(new).medium == 0x20 WATER) {
        add_cast_order(-1, -1, -1, -1, 0x28a, QUEUE_FIRST, 0);
        // falls through: the original keeps going and moves the unit onto the water tile!
    }
    if (d == 1 && tile(new).medium != 0x20 WATER) {
        eject_contents(0, -1, 1, 1);
        if (num_inside(1) == 0) die(0, -1, 0.0);
    }
}
```

Correction (disassembly @5f8f72: `CALL add_cast_order; XOR EAX,EAX; RET`): the original **returns 0 right after
queuing the cast order** — the unit does not move onto the water tile; it stays on the shore and the barge is created
on the unit's own tile (§5) and teleported to the water spot. The barge-leaving branch also returns 0 (whether or not
contents remain: the next frame retries); the unit comes out next to the barge's position, found by `find_nearby_spot`
with the unit's own type, so on land. `do_move` already stops on a 0 from `unit_set_new_location`.

`num_inside(1)` is garrison-spec `num_inside(p = 1)` (captains only).

## 4. The cast order

### 4.1 `Unit::add_cast_order(ox, whom, x, y, spell, pos, explicit)` @5e4a60

```
if (pos == QUEUE_NEW) { unit_masks &= ~0x4000000 (kMultimove); path.clear(); close_orders(0); clear_partial_path(); update_action(); }
if (spell == 0x28b) spell = is(0x7b,0) ? 0x28d : (type in {0x3d,0x3e,400}) ? 0x28f : is(0x13d,0) ? 0x291 : 0x28b;   // pack variants
if (spell == 0x28c) spell = is(0x7b,0) ? 0x28e : (type in {0x3d,0x3e,400}) ? 0x290 : is(0x13d,0) ? 0x292 : 0x28c;   // unpack variants
order = new CastOrder: x, y, ox, whom, uid = whom/ox valid ? objects[whom][ox].uid : 0xffff, spell, paid = 0
order.flags: explicit ? |= 4 (kExplicit) : &= ~4
orderlist.add(order)
if (pos == QUEUE_FIRST) { clear_partial_path(); orderlist.current = head; }   // the new order becomes the front
update_action()
```

`Order` needs two new fields: `int32_t spell` (+0x20) and `int32_t paid` (+0x1c). `x, y` (+0x14/+0x18), `ox, whom`
(+8/+0xc), `uid` (+0x10) are the TargetOrder fields the struct already has.

### 4.2 `SpellTypeData::is_castable(o, who, p3)` @675bc0, only what spell 0x28a needs

Returns 3 = castable. The "does the leader own the spell" chain at the top (`is(spell)`, `is(from2)`,
`tribe_can_type`, graft) is skipped for 0x28a and 0x29a. Then:

```
if (obj.is_unit() && (unit_masks & 1 kDecoy) && !is_pack && !is_unpack) return 0;   // decoys cast nothing
case 0x288, 0x289, 0x28a: return obj.is_cargo() ? 3 : 0;
```

### 4.3 `SpellType::pay_cast_costs(o, who)` @676c40 → 0 paid, 1 cannot pay

```
q = can_pay_cost(spell, who, -1, 0, 1);      // Type vslot +0x84
if (q == 0) return 1;
if (q == 10 && g.editor) return 0;           // free in the editor (semaphore byte1 & 8)
pay_cost(spell, who, o, -1, 1, 0);           // vslot +0xd4
if (obj.is_unit()) unit.mana_burn += spell.mana;   // (+0x1d0 of the SpellType = mana)
return 0;
```

`unpay_cast_costs` @676ff0: `unpay_cost(who, o, -1)`; `mana_burn -= min(mana_burn, mana)`. Spell 0x28a has an
empty cost and mana 0, so this is a no-op in practice; port it faithfully anyway (`can_pay_cost` returns 10).

### 4.4 `SpellTypeData::get_job_time(o, who)` @675800

`t = job_time` of the spell, then modifiers for 0x27d (entrench: `french_entrench`, Antipater
`antipater_entrench_rate`), 0x275 / 0x27f (spies: SPIES_CRAFT_FASTER halves), 0x28b / 0x28c (pack: `turk_pack`,
Napoleon `napoleon_pack`, light artillery /4, then /2), 0x28d / 0x28e (/2 for `is(0x7d)`), 0x280 / 0x281 (sabotage:
tribe bonus 9 halves, `is(0x177,1)` → 10). None applies to 0x28a: **job time 0**.

### 4.5 `Unit::do_cast(order)` @5ebfe0 — the untargeted branch (spell flags & 0xe == 0, which is 0x28a's case)

```
spell = order.spell; S = spelltypes[spell]; eff = is_spell_type(spell) ? spell : 0x296
if (order.paid == 0) {
    if (pay_cast_costs(S, o, who) != 0) { (console feedback); goto kill }   // cannot pay: the order dies
    order.paid = 1
}
if (spell_time == 0) {                                           // first frame of the cast
    (pack / unpack anim and relocation branches: not 0x28a)
    set_anim(CHAR_DEFAULT, 0, 1)
    if (spell == 0x28a &&
        find_nearby_spot(unittypes[320 BARGE], x, y, &sx, &sy, 0, K(unit_board_distance), 0, 0x55555555, FILTER_NOT_ME,
                         -1, -1, 0, 0, -1, 0, -1) != 0)
        goto kill;                                               // no water spot for a barge: give up
}
(pack with a general nearby: not 0x28a)
spell_time += 1
if (spell_time < get_job_time(S_eff, o, who)) return;            // 1 < 0 is false: casts on the first frame
if (spell == 0x28a && !is_captain()) {
    cap = get_captain()
    if (order_type(units[who][cap]) == CAST_SPELL) { spell_time -= 1; return; }   // followers wait for the captain
}
spell_time = 0
if (is_spell_type(spell)) SpellType::cast(S, o, who, 0, 0);
else (unit upgrade path: not relevant)
if (spell == 0x28a) return;                                      // the order now lives on the barge (§5)
order.paid = 0 (the order object is about to die)
kill: kill_current_order(0)
```

`SpellType::cast(o, who, p3, p4)` @676ce0: `if (is_castable(o, who, 1) != 3) return;` then the `switch` on the spell:
`0x28a → cast_transport(o, who)`. The other cases are the other spells (TODO elsewhere).

## 5. `SpellType::cast_transport(o, who)` @670db0

```
U = units[who][o]
if (!U.is_active() || !U.is_on_map()) return;
if (U.type.domain == 1 || U.type.domain == 2) return;
B = U.is_caravan() ? current_upgrade(who, 318 MERCHANTFLEET) : current_upgrade(who, 320 TRANSPORTBARGE)
if (B < 0) B = the base type (318 / 320)
if (find_nearby_spot(unittypes[B], U.x, U.y, &sx, &sy, 0, K(unit_board_distance), 0, 0x55555555, FILTER_NOT_ME,
                     -1, -1, 0, 0, -1, 0, -1) != 0) {
    if (who == console) sound S_NOT_NEAR_OCEAN_FOR_TRANSPORT
    return;                                                      // the cast order is killed by do_cast
}
b = init_unit(who, B, U.x, U.y, -1, -1, -1)                      // created ON the unit's position
if (b < 0) return;
Bg = units[who][b]
(sound S_TRANSPORTLOAD when U is on screen and seen by the console)
if (U.is(0x77 MARINES, 0)) { Bg.unit_masks2 |= 0x200; Bg.update_hits(0); update_speed(Bg); }
same_damage(Bg, o, who)                                          // §7.2
set_new_location(Bg, sx, sy, 1, 1)                               // teleport to the found spot
set_angle(Bg, U.angle, 1)
if (U.is_caravan()) {
    Bg.cara = U.cara; caravans[who][U.cara].o = b;               // the caravan record now points at the fleet
    Bg.unit_masks = (U.unit_masks & 0x200) ? Bg.unit_masks | 0x200 : Bg.unit_masks & ~0x200;   // kTradeLoaded
}
while (U.orderlist not empty) {                                  // every order moves, front first
    U.orderlist.current = head; take it out; Bg.orderlist.add(it)
}
kill_current_order(Bg, 0)                                        // kills the CAST order itself (it was first)
Bg.path = U.path (same stack order: the original inverts U.path and pops into Bg.path)
Bg.unit_masks |= 0x800000
if (!Bg.path.empty()) {                                          // the next point loses flag 4 when it is in the barge's region
    p = Bg.path.pop()
    if (tregion(p.x, p.y) == tregion(sx, sy)) p.flags &= ~4
    Bg.path.push(p)
}
clear_orders(U)
(console selection: U out, Bg in; replace_hotunit — presentation)
go_inside(U, b, who, 0)                                          // U and its o_down chain enter the barge
```

Notes for the port:
- The barge is created with `init_unit` at `U.x, U.y` (a water tile: U already stands there, §3) and then
  teleported to the spot. `init_unit`'s draws, if any, happen here, so keep its order of calls.
- `go_inside` is `unit_go_inside` (queues.cpp). It calls `Object::insert_inside` on the captain, then the follower
  chain, resets `kMultimove` / closes orders when the container is a unit and `uber_size > 1` — already ported.
- A chain (uber_size > 1): only the captain runs `do_cast` to the end; followers hit the "captain is casting" wait
  (§4.5) and are taken inside with the captain by `go_inside`. Their own cast orders are cleared by
  `clear_orders(U)` — no: the followers are distinct units with their own order lists; the original leaves those
  orders and the units stop processing while inside (frame-spec: units inside skip `process`). Port as the
  original: do not touch the followers' orders.
- `kill_current_order(Bg, 0)` on the barge: the moved CAST order is the barge's current order at that point, so the
  barge's front order after this is the unit's old MOVE (or whatever came next).

## 6. The barge under way

- Passability: `invalid_loc` (passability.cpp) already lets a domain-1 unit onto a non-water tile when the path step
  carries flag 4 and `can_transport(u)` is true, and a domain-0 unit onto water the same way. No change.
- Collision: `collision.cpp:534` — in `detect_unit_collision`, when the blocking unit B's front order is CAST_SPELL
  with `spell == 0x28a`, `wait_case = true` (the mover waits for the transforming unit like it waits for a mover).
  `collision.cpp:382` is a different rule: a blocker casting a **pack** spell (0x28b / 0x28d / 0x28f / 0x291) is a
  soft collider.
- `check_target_path` (combat_orders.cpp:307) for CAST_SPELL: `SpellTypeData::is_valid_target` — untouched (0x28a has
  no target: `ox < 0` returns 0 before it).
- Speed (`movement.cpp:93`): `+= LeaderDataEncrypt.epoch[0] (the military library level; leader +0x6eb8 → +0xe8 ^
  0x63187) * K(military_transport_bonus)` for barges (domain 1 with unit_flags 0x10), and `+= epoch[0] *
  K(americans_marine_speed_bonus)` for marine transports. (Both constants are 0 in the stock rules.)
- The AI `is_entering_or_exiting` (garrison.cpp:479): a unit whose front order is BOARD_SHIP / AWAIT_BOARD, or a
  CAST_SPELL 0x28a. Fill when convenient.

## 7. Landing

### 7.1 `Object::eject_contents(kill_if_stuck, type, carry_orders, no_orders)` @64cd20 — the unit-source branch

The port (`garrison.cpp:558`) has the loop. For each contained captain that `come_out(0)` returned 0 for, when
**this is a unit** (`is_unit()`), after the building-only block:

```
(sound S_TRANSPORTUNLOAD when this is on screen and seen by the console)
same_damage(U, this.o, this.who)                                 // U takes the barge's damage ratio
if (this.is_caravan()) {
    U.cara = this.cara; caravans[who][U.cara].o = U.o; this.cara = -1;
    U.unit_masks = (this.unit_masks & 0x200) ? U.unit_masks | 0x200 : U.unit_masks & ~0x200;
}
if (carry_orders) {
    if (U.type.uber_size == 1) {
        while (this.orderlist not empty) { current = head; take it out; U.orderlist.add(it) }
        U.path = this.path (same stack order, via invert + pops)
        if (!U.path.empty()) { p = U.path.pop(); p.flags &= ~4; U.path.push(p) }   // the next point: no transport flag
    } else {
        reset_move_orders(this)                                  // Unit::reset_move_orders
        if (!(this.unit_masks2 & 0x40000) || get_army(this) < 0) {
            // a temporary group: the barge's orders are re-issued to the chain through Group::set_up_insert /
            // Group::kill(barge) / Group::add(U) / Groups::push_group / finish_insert
            ❓ port as: copy the barge's order list to the captain U as in the uber_size == 1 case (the group
            machinery re-creates the same orders as group orders). Mark TODO(groups) with this note.
        }
    }
}
level = LeaderData::can_transport(who)
if (transport_type(U) <= level && (!U.is(0x45,0) || (options_flags & 2))) U.unit_masks |= 0x800000;
(console selection: this out, U in; replace_hotunit — presentation)
```

`come_out` of a unit held by a unit container (`unit_come_out`, queues.cpp:587): `get_inside` gives the barge; the
spot comes from `find_nearby_spot` with the **unit's** type around the barge, so it lands on a land tile; the
existing port handles a unit container (it only special-cases buildings). Verify, do not rewrite.

### 7.2 `Unit::same_damage(o, who)` @5f9400 — this takes the damage ratio of (o, who)

```
cap = last unit of this's o_down chain? No: walk this → o_down while the next is active; L = the LAST active unit
     of the chain (the original loop advances `this` down the chain and keeps the last one)                       ❓
     (for a single unit L = this)
per = hits(L, 0) / L.type.uber_size                                    // hit points per member
r_this = (total_damage(L, 0, 0) << 8) / hits(L, 0)
r_src  = (total_damage(src, 0, 0) << 8) / hits(src, 0)
r = max(r_this, r_src)
extra = ((hits(L,0) * r + ((hits(L,0) * r) >> 31 & 0xff)) >> 8) - total_damage(L, 0, 0)     // round toward zero
if (extra < 0) extra = 0
if (extra == 0) return
walk u = L's chain from the captain (`this.o`) down:
    if (u.o_down < 0) {                                              // last member
        cap = hits(u,0) - total_damage(u,0,0) - 1; u.damage = min(extra, cap); return
    }
    if (extra < per) { u.damage = extra; return }
    die(u, 0, -1, 0.0); extra -= per; if (extra == 0) return
```

`hits(u, 0)` is `obj_hits` (stats), `total_damage(u, 0, 0)` the unit's damage plus the fraction (combat-spec §5.1 /
`UnitData::total_damage`). The decompile is in `re/decomp/transport/Unit_same_damage@005f9400.c`; keep its order of
operations exactly (the `>> 31 & 0xff` rounding is the compiler's signed `/ 256`).

## 8. Boarding an existing ship: BOARD_SHIP (8) and AWAIT_BOARD (9)

### 8.1 `Group::action_board_ship(ship_o, pos)` @700010 (command BOARD_SHIP: ox = ship, queued)

```
prelude (ignore_orders, action_begin)
if (pos == QUEUE_FIRST) { set_up_insert; action_halt(0); action_board_ship(ship_o, QUEUE_NEW); re-add; finish_insert; return }
who = group.who; S = objects[who][ship_o]; rs = tregion(S)
for each member M (active, on map, type.domain != 2, !is_busy):
    rm = tregion(M); if (!Region::is_coast(rm, rs)) continue;        // the member's land region must touch the ship's sea region
    if (!can_carry(S, M.o, who)) full++;
    add_board_order(M, ship_o, who, pos == QUEUE_LAST ? QUEUE_LAST : QUEUE_NEW, ?)
    if (first one) clear_orders(S)                                   // ❓ decompile: `if (local_2c == 0) clear_orders(ship)` before count++
    count++
    add_await_board_order(S, M.o, who, pos_of_ship, 1)
(console: "ship full" message when full != 0)
```

### 8.2 `Unit::add_board_order(o, who, pos, p4)` @5e4d10 / `add_await_board_order` @5e4c80

`add_board_order`: `QUEUE_NEW` prelude as in §4.1; `order = BOARD_SHIP {ox = o, whom = who, uid}`; `flags |= 4`;
add; `update_action()`. `add_await_board_order`: `order = AWAIT_BOARD {ox, whom, uid}`; `flags = p4 ? |4 : &~4`;
add; `update_action()` (no QUEUE_NEW prelude).

### 8.3 `Unit::do_board(order)` @5ed1f0

```
S = (order.ox, order.whom); set_anim(CHAR_DEFAULT, 0, 1)
if (check_meet_ship(S) == 0) { kill_current_order(0); if (can_carry(S, o, who)) go_inside(S, 0); }
```

### 8.4 `Unit::check_meet_ship(so, swho)` @604550 → 0 = met (board now), else keeps walking

```
if (!can_carry(S, o, who)) return 0;
d = vector_dist(|x - S.x|, |y - S.y|)
if (d <= K(unit_board_distance)) return 0;                           // close enough: board
if (d >= 0x900) {                                                    // far: both walk to the shore midpoint
    pick the ship's wcoord neighbour (orthog_x/y[1..3]) where the ship is not invalid_loc; target = that cell centre
    mid = ((S.x + x) / 2, (S.y + y) / 2)
} else if (d < 0xc0) { mid = ((S.x + x)/2, (S.y + y)/2) }
else {
    if (!Region::is_coast(tregion(me), tregion(S))) kill_current_order(0);
    mid = ((S.x + x)/2, (S.y + y)/2); search the 0x51 move_x/move_y offsets around mid for a Region::coast_here cell
    of my region facing the ship's region where neither I nor the ship is invalid_loc; set my/ship targets there
    (ring of 4 cells for even coast directions); if none: kill_current_order(0)
}
if (swho == who && (ship has no action || its action type is 0 (NONE))) {
    push a MOVE_TO(mid, angle to the ship, tol 0, flags cleared) in front of my orders (QUEUE_FIRST by hand)
    ship: kMultimove off, path cleared, close_orders, clear_partial_path, update_action; push the same MOVE_TO;
    add_await_board_order(ship, o, who, _, 0)
}
push MOVE_TO(mid) in front of my orders (QUEUE_FIRST by hand), update_action
if (swho == who && order_type(ship) == AWAIT_BOARD && that order's (ox, whom) == (o, who))
    add_move_order(ship, target, 1, 0, QUEUE_FIRST, 0, ...)
return 1
```

This is a 335-line function with many coordinate fiddles (`re/decomp/transport/Unit_check_meet_ship@00604550.c`).
Port it as read; it is exercised only by the BOARD_SHIP command, which no stock scenario issues, so it cannot break
the self-checks. Mark the unclear coordinate cases ❓ in code comments rather than inventing.

### 8.5 `Unit::do_await_board(order)` @5ed040 — the ship waits for (order.ox, order.whom)

```
set_anim(CHAR_DEFAULT, 0, 1)
if (order.ox == o && order.whom == who) { kill; return }              // waiting for itself
if (whom == who) {
    T = units[who][ox]
    if (T active && on map && can_carry(this, ox, whom) && T's action type == BOARD_SHIP(8)) {
        b = update_action(T)'s BOARD order; if (b.ox == o) { if (b.whom == who) return;   // still coming: keep waiting
                                                              kill; return }
    }
}
T = objects[whom][ox]
if (T active && on map && T's action type == 8) {
    b = T's BOARD order; if (b.ox == o && b.whom == who) { repath(T); kill_current_order(T, 0); }
}
kill_current_order(0)
```

## 9. The TRANSPORT command: `Group::action_transport(force)` @702620 (CommandPackage::process_transport → force 0)

For each group member M (active, on map, captain, type.domain not 1 / 2, `!is_busy`): `B` as in §5 (caravan →
merchant fleet); `b = init_unit(who, B, M.x, M.y)`; `if (b < 0) continue`;
`if (find_nearby_spot(units[b].type, M.x, M.y, &sx, &sy, 0, K(unit_board_distance), 0, 0x55555555, FILTER_NOT_ME, b, who, 0,0,-1,0,-1) == 0 || force) { set_new_location(Bg, sx, sy, 1, 1); clear_orders(M); go_inside(M, b, who, 0); } else die(Bg, 0, -1, 0.0)`.
(No same_damage, no order transfer: this is the "board now" button.) Commands: TRANSPORT (group), SET_TRANSPORT
(group, flag), BOARD_SHIP (group, ox, queued) — add them to `commands.hpp` / the dispatcher as the GARRISON command
was (garrison-spec §4), and to `ron-sim` as `--transport WHO:o1,o2@FRAME`, `--set-transport WHO:o1,..@FRAME/0|1`,
`--board WHO:o1,..@SHIP_O@FRAME`.

## 10. AI (not in this delivery)

`Unit::think_civilian_transport(p)` @5f40d0 (civilians looking for a coast cell of a region worth settling, then
`Group::action_move_near` to it) and the army `do_transporting` keep their TODOs. They only pick destinations; the
crossing itself is §3–§7.

## 11. Randomness

None of the functions above draw `game_random` themselves. `init_unit` (already ported, with its draws if any),
`set_new_location` → `explore_goody` (G1 draws, ported) and `find_nearby_spot` (no draws) are the only callees that
may. Keep the call order of §5 so the draws stay in sequence.

## 12. What the port needs

Files: `engine/sim/transport.cpp` + `transport.hpp` (new: §2.1–2.9 predicates, `cast_transport`, `same_damage`,
`check_meet_ship`, `do_board`, `do_await_board`, `add_board_order`, `add_await_board_order`, `action_transport`,
`action_set_transport`, `action_board_ship`), `engine/sim/spells.cpp` + `spells.hpp` (new: `add_cast_order`,
`do_cast` with only the untargeted branch and a `TODO(spells)` for the targeted one, `spell_is_castable` for 0x28a
with the others returning the current behaviour, `pay/unpay_cast_costs`, `spell_job_time`, `spell_cast` dispatch
with 0x28a wired and the rest TODO), `objects.hpp` (`Order.spell`, `Order.paid`), `guys.cpp` (§3),
`garrison.cpp` (§7.1), `techs.cpp` (§2.8), `collision.cpp` (§6), `movement.cpp` (§6 speed), the order dispatcher
(`kCastSpell`, `kBoardShip`, `kAwaitBoard` handlers; today `kCastSpell` is killed), `commands.*` and `apps/sim/main.cpp`
(§9 options), `apps/map-mac` (the TRANSPORT / BOARD_SHIP verbs on the right-click menu are optional: units already
cross when ordered to move across water).

Acceptance (all objective):
1. `./build-mac.sh` with zero warnings; `ron-check` 0 failures.
2. `re/tools/sim-all.sh 600 --ai-every 8` on the 156 stock scenarios: self-check OK everywhere, and the digests of
   scenarios without transport-capable units crossing water are unchanged (list the ones that changed and why: each
   must be a unit that now reaches a target across water).
3. A crossing test: pick a stock scenario with a land unit whose owner has TRANSPORT_BONUS and docks (or set the
   `UNIT_CAN_TRANSPORT` bit through the SET_TRANSPORT command), order it with `--move` to a tile across water, run
   enough frames, and show in the dump: the CAST order appearing, the barge (type 320–322) created with the unit
   inside, the barge walking the path, the barge dying on the far shore, the unit standing on the far shore with
   the damage ratio preserved (damage a unit first with `--attack` or pick a damaged one) and its remaining orders
   intact. Put the command line in `docs/sim-implementation.md` under a "Transports" section.
4. A save / load round trip (`--roundtrip`, once phase 3 lands) across a frame where the unit is inside the barge.

## 13. Evidence

| Function | Address | Source |
|---|---|---|
| `Unit::set_new_location` | 5f8d20 | `re/decomp/transport/Unit_set_new_location@005f8d20.c` |
| `Unit::add_cast_order` | 5e4a60 | decomp |
| `Unit::do_cast` | 5ebfe0 | decomp (648 lines stripped) |
| `SpellType::cast` | 676ce0 | decomp |
| `SpellType::cast_transport` | 670db0 | decomp |
| `SpellType::pay_cast_costs` / `unpay_cast_costs` | 676c40 / 676ff0 | decomp + disasm (return values) |
| `SpellTypeData::is_castable` | 675bc0 | decomp + disasm (`re/decomp/transport_dis`) |
| `SpellTypeData::get_job_time` | 675800 | decomp |
| `LeaderData::can_transport`, `UnitData::transport_type`, `can_transport`, `can_ever_transport`, `is_casting` | 6e0c60, 46f790, 46f960, 46f290, 60a530 | disasm |
| `ObjectData::is_cargo`, `cargo_inside`, `can_carry(o,who)` | 653600, 64dcd0, 6483c0 | disasm / decomp |
| `Leader::check_transport` | 6bc5f0 | decomp |
| `Object::eject_contents` (unit branch) | 64cd20 | decomp lines 150–360 |
| `Unit::same_damage` | 5f9400 | decomp |
| `Unit::go_inside` | 61a2e0 | decomp (matches `unit_go_inside`) |
| `Unit::come_out` (barge of an oil platform, distances) | 617c10 | decomp |
| `Group::action_transport` / `action_set_transport` / `action_board_ship` | 702620 / 7024b0 / 700010 | decomp |
| `Unit::add_board_order` / `add_await_board_order` / `do_board` / `do_await_board` / `check_meet_ship` | 5e4d10 / 5e4c80 / 5ed1f0 / 5ed040 / 604550 | decomp |
| `CommandPackage::process_transport` / `process_set_transport` / `process_board_ship` | 949050 / 948f60 / 948e00 | decomp |
| `Unit::think_civilian_transport` | 5f40d0 | decomp (§10, not ported) |
| Unit / Object vtables | b417d0 | `re/decomp/transport_vt.txt` |
