#!/bin/bash
# Runs `ron-sav --replay` on every recording of re/oracle/Recorded Games (the 45 that start from an embedded save and the 36 random-map ones
# that start from the header's setup) and writes one table row per recording to re/oracle/out/replay-table.tsv.
#
#   re/tools/replay-all.sh [--frames N] [--jobs J] [--only REGEX] [--out FILE] [--net]
#
#   --frames N   stop every replay N frames after its start (default: the whole recording)
#   --jobs J     replays in parallel (default 4, the most the machine should run)
#   --only RE    only the recordings whose path matches the regex (e.g. "2026.08.30 21'2")
#   --out FILE   the table (default re/oracle/out/replay-table.tsv)
#   --net        also run `ron-net replay` on the multiplayer recordings and add a column telling whether its numbers equal ron-sav's
#
# Columns: replay (the date and time of the file name, never a player name), start (save = an embedded save, setup = generated from the header),
# mode (MP / SP), boundaries compared, fully equal boundaries, first-row (parts equal at the first CHECK_SUMS row, of 15; "-" when the recording has no
# rows or does not start at frame 0 / 1), then for each of the 15 parts the first boundary frame at which it differs, or "=" when it matches through
# the last boundary compared. Totals are printed at the end. docs/oracle-corpus.md section 8 and docs/fidelity-plan.md section 3.5 describe it.
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
frames=0
jobs=4
only=""
out="re/oracle/out/replay-table.tsv"
net=0
while (($#)); do
	case "$1" in
		--frames) frames="$2"; shift 2 ;;
		--jobs) jobs="$2"; shift 2 ;;
		--only) only="$2"; shift 2 ;;
		--out) out="$2"; shift 2 ;;
		--net) net=1; shift ;;
		*) echo "unknown option $1" >&2; exit 64 ;;
	esac
done
[[ -x build/ron-sav ]] || { echo "build/ron-sav is missing (bash build-mac.sh)" >&2; exit 1; }
[[ -f re/oracle/out/checksums.csv ]] || { echo "re/oracle/out/checksums.csv is missing (node re/tools/rcxdump.mjs --survey ... --checksums, docs/oracle-corpus.md)" >&2; exit 1; }
mkdir -p "$(dirname "$out")"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
parts=(units builds walls ammo deaths groups guys leaders cities items goods world rules scenario_data script_run_time)

run_one() {   # run_one <index> <file>
	local i="$1" f="$2"
	local name
	name="$(basename "$f" .rcx | sed -E 's/^Playback - //')"
	[[ "$f" == */Multi/* ]] && name="Multi/$name"
	local args=(--replay)
	((frames > 0)) && args+=(--frames "$frames")
	local log="$tmp/$i.log"
	./build/ron-sav "$f" "${args[@]}" > "$log" 2>&1
	local start="save" mode="MP"
	grep -q "starts from the header's setup" "$log" && start="setup"
	[[ "$f" != */Multi/* ]] && mode="SP"
	local note=""
	if grep -q "no CHECK_SUMS rows" "$log"; then note="no CHECK_SUMS rows (single player)"
	elif grep -q "crashed" "$log"; then note="CRASHED"
	elif grep -q "Conquer the World replay" "$log"; then note="Conquer the World"
	elif grep -q "not supported here" "$log"; then note="no embedded save: the generator path is missing in this build"
	elif grep -q "replay: no embedded save and " "$log"; then note="$(grep -m1 'replay: no embedded save and ' "$log" | sed -E 's/^replay: //' | cut -c1-90)"
	elif grep -q "^replay: " "$log"; then note="$(grep -m1 '^replay: ' "$log" | cut -c1-80)"
	fi
	if [[ -z "$note" ]] && grep -q "STOPPED:" "$log"; then note="$(grep -m1 'STOPPED:' "$log" | sed -E 's/^ +//' | cut -c1-100)"; fi
	local comp="" eq=""
	comp="$(grep -m1 -oE '\([0-9]+ turn boundaries compared' "$log" | grep -oE '[0-9]+')"
	eq="$(grep -m1 -oE '[0-9]+ with all 15 parts equal' "$log" | grep -oE '^[0-9]+')"
	local firstb
	firstb="$(grep -m1 -oE 'against the first row \(boundary [0-9]+\)' "$log" | grep -oE '[0-9]+')"
	local cols="" firstrow=0 any=0
	local p
	for p in "${parts[@]}"; do
		local line v
		line="$(grep -m1 -E "^  $p +(matches|first differs)" "$log")"
		if [[ -z "$line" ]]; then v="-"
		elif [[ "$line" == *matches* ]]; then v="="; any=1; firstrow=$((firstrow + 1))
		else
			v="$(grep -oE 'boundary frame [0-9]+' <<<"$line" | grep -oE '[0-9]+')"; any=1
			[[ -n "$firstb" && "$v" != "$firstb" ]] && firstrow=$((firstrow + 1))
		fi
		cols+=$'\t'"$v"
	done
	((any)) || firstrow="-"
	if [[ "${comp:-0}" == "0" ]]; then   # nothing was compared (an error before the first boundary, or no rows): the "matches" lines mean nothing
		cols=""
		for p in "${parts[@]}"; do cols+=$'\t'"-"; done
		firstrow="-"
	fi
	local netcol=""
	if ((net)) && [[ "$mode" == "MP" ]]; then
		local nlog="$tmp/$i.net"
		local nargs=()
		((frames > 0)) && nargs+=(--frames "$frames")
		./build/ron-net replay "$f" "${nargs[@]}" > "$nlog" 2>&1
		local same=yes
		for p in "${parts[@]}"; do
			local a b
			a="$(grep -m1 -E "^  $p +(matches|first differs)" "$log" | sed -E 's/ +/ /g')"
			b="$(grep -m1 -E "^  $p +(matches|first differs)" "$nlog" | sed -E 's/ +/ /g')"
			[[ "$a" == "$b" ]] || same=no
		done
		netcol=$'\t'"$same"
	elif ((net)); then netcol=$'\t'"-"
	fi
	printf '%s\t%s\t%s\t%s\t%s\t%s%s%s\t%s\n' "$name" "$start" "$mode" "${comp:--}" "${eq:--}" "$firstrow" "$cols" "$netcol" "$note" > "$tmp/$i.row"
}

files=()
while IFS= read -r f; do
	[[ -n "$only" && ! "$f" =~ $only ]] && continue
	files+=("$f")
done < <(find "re/oracle/Recorded Games" -name '*.rcx' | sort)
echo "replay-all: ${#files[@]} recordings, $jobs at a time$( ((frames > 0)) && echo ", $frames frames each")"
i=0
for f in "${files[@]}"; do
	while (($(jobs -rp | wc -l) >= jobs)); do sleep 1; done   # bash 3.2 (macOS) has no `wait -n`
	run_one "$i" "$f" &
	i=$((i + 1))
done
wait

header="replay\tstart\tmode\tboundaries\tequal\tfirst_row_parts_equal"
for p in "${parts[@]}"; do header+="\t$p"; done
((net)) && header+="\tnet_equals_sav"
header+="\tnote"
{
	printf '%b\n' "$header"
	for ((k = 0; k < i; k++)); do cat "$tmp/$k.row"; done
} > "$out"

# totals
awk -F'\t' -v net="$net" 'NR > 1 {
		n++; kind = ($2 == "setup") ? "setup" : "save"
		cnt[kind]++
		if ($4 != "-") { cmp[kind] += $4; eq[kind] += $5; rows[kind]++ }
		if ($6 != "-") { fr[kind] += $6; frn[kind]++; if ($6 == 15) full[kind]++ }
	}
	END {
		printf "replay-all: %d recordings (%d from an embedded save, %d generated from the header)\n", n, cnt["save"], cnt["setup"]
		for (k in cnt) {
			printf "  %-6s %d recordings with CHECK_SUMS rows: %d boundaries compared, %d with all 15 parts equal", k, rows[k], cmp[k], eq[k]
			if (frn[k]) printf "; first row: %d recordings, %.1f of 15 parts equal on average, %d fully equal", frn[k], fr[k] / frn[k], full[k]
			printf "\n"
		}
	}' "$out"
echo "table: $out"
