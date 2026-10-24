#!/bin/bash
# Acceptance fixtures of the aircraft leftovers (docs/aircraft-spec.md 13-16): the debris of a crashing plane, the strafing
# impact slide, bird flocks, UNITMASK (AUTO_EXPLORE, a carrier's REPEAT). Usage: re/tools/air-fixtures.sh <game dir>
# (uses build/ron-sim, or $RON_SIM). Exit status 0 when every check passes.
set -uo pipefail
game="${1:?usage: air-fixtures.sh <game dir>}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
sim="${RON_SIM:-$root/build/ron-sim}"
korea="$game/conquest/ColdWar/korea.scx"
tut1="$game/scenario/Tutorial1/tut1.scx"
fail=0
check() { # name, condition result (0 = pass)
	if [[ $2 -eq 0 ]]; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi
}

# 1. Dead crafts: two airborne Fighter Bombers of the carrier shot down at frame 70 leave debris (Ammo::init_crash: one game_random draw
#    each) that is gone a few frames later (impact on the terrain test of Ammo::inc_time).
out="$(RON_AIR_TRACE=1 "$sim" "$game" "$korea" --frames 120 --digest-every 0 --combat-every 5 --scramble 0:113@20 --crash 0:123,124@70 2>&1)"
n="$(grep 'crashes: debris slot' <<<"$out" | sort -u | wc -l | tr -d ' ')"
check "crash: two guys turned into debris ($n)" $((n == 2 ? 0 : 1))
grep -E '^  f(75|80|85|90|95|100|105|110|115|120) ' <<<"$out" | grep -q 'debris [1-9]'
check "crash: debris in flight after the crash" $?
grep -E '^  f120 ' <<<"$out" | grep -q 'debris'
check "crash: no debris left at frame 120" $((! $?))

# 2. The strafing impact slide: the two shots of one release (nodes of both parities) land on either side of the heading, no longer
#    on the same point.
out="$(RON_COMBAT_TRACE=1 "$sim" "$game" "$korea" --frames 380 --digest-every 0 --combat-every 1000 --scramble 0:113@20 --flight 0:123,124@103:6@60 2>&1)"
pairs="$(grep -E '^f[0-9]+ ammo [0-9]+ from 0:12[34] -> 6:103 piece 23 ' <<<"$out" | head -4 | sed -E 's/.* end \(([0-9]+,[0-9]+)\).*/\1/' | sort -u | wc -l | tr -d ' ')"
check "slide: shots of one strafing run end on different points ($pairs distinct)" $((pairs >= 3 ? 0 : 1))

# 3. Bird flocks: Objects::add_flock makes the asked number of flock birds, they fly their four waypoints and are gone after landing at the last.
out="$("$sim" "$game" "$tut1" --frames 600 --digest-every 0 --combat-every 25 --flock -1:40,40,4@50 2>&1)"
grep -q 'FLOCK 4 birds' <<<"$out"
check "flock: add_flock ran" $?
grep -E '^  f(75|100) ' <<<"$out" | grep -q 'flock birds 4'
check "flock: four flock birds flying" $?
grep -E '^  f600 ' <<<"$out" | grep -q 'flock birds'
check "flock: all landed by frame 600" $((! $?))
out="$("$sim" "$game" "$tut1" --frames 440 --digest-every 0 --combat-every 25 --flock 0:40,40,3@300 --flock 0:40,40,3@400 2>&1)"
grep -E '^  f425 ' <<<"$out" | grep -q 'flock birds 3'
check "flock: a leader's second flock within 225 frames is refused (3 birds at 425, not 6)" $?

# 4. UNITMASK: AUTO_EXPLORE and a carrier's REPEAT toggle the bit on the group's units; a second command clears it.
out="$("$sim" "$game" "$korea" --frames 30 --digest-every 0 --carrier-repeat 0:113@5 --auto-explore 0:6@5 2>&1)"
grep -q 'unitmask 200000 on 0: 113=set' <<<"$out"
check "unitmask: carrier REPEAT set" $?
grep -q 'unitmask 100 on 0: 6=set' <<<"$out"
check "unitmask: AUTO_EXPLORE set" $?
out="$("$sim" "$game" "$korea" --frames 30 --digest-every 0 --carrier-repeat 0:113@5 --carrier-repeat 0:113@10 2>&1)"
grep -q 'unitmask 200000 on 0: 113=clear' <<<"$out"
check "unitmask: second toggle clears" $?
exit $fail
