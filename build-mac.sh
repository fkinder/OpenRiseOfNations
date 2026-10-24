#!/bin/bash
# Build OpenRiseOfNations (the ronmap game app, the viewer and the command-line checkers) on macOS.
# Needs only Apple's Command Line Tools (no full Xcode). Package a .app / .dmg with package-mac.sh.
set -euo pipefail
cd "$(dirname "$0")"

if ! xcrun --find clang++ >/dev/null 2>&1; then
	echo "Apple's command line developer tools are needed. Install them with:"
	echo "    xcode-select --install"
	echo "then run this script again."
	exit 1
fi

CXXFLAGS=(-std=c++17 -O2 -Wall -Wextra -I engine -mmacosx-version-min=12.0)
# UNIVERSAL=1 builds Apple Silicon + Intel binaries (used by package-mac.sh); otherwise only this Mac's architecture.
if [[ "${UNIVERSAL:-0}" == "1" ]]; then
	CXXFLAGS+=(-arch arm64 -arch x86_64)
fi
rm -rf build/obj && mkdir -p build/obj

echo "Compiling engine..."
OBJS=()
for src in engine/*/*.cpp; do
	obj="build/obj/$(basename "$(dirname "$src")")_$(basename "${src%.cpp}").o"
	clang++ "${CXXFLAGS[@]}" -c "$src" -o "$obj"
	OBJS+=("$obj")
done

echo "Building ron-check..."
clang++ "${CXXFLAGS[@]}" "${OBJS[@]}" apps/check/main.cpp -lz -o build/ron-check

echo "Building ron-rules..."
clang++ "${CXXFLAGS[@]}" "${OBJS[@]}" apps/rules/main.cpp -lz -o build/ron-rules

echo "Building ron-world..."
clang++ "${CXXFLAGS[@]}" "${OBJS[@]}" apps/world/main.cpp -lz -o build/ron-world

echo "Building ron-combat-check..."
clang++ "${CXXFLAGS[@]}" "${OBJS[@]}" apps/combat-check/main.cpp -lz -o build/ron-combat-check

echo "Building ron-prod-check..."
clang++ "${CXXFLAGS[@]}" "${OBJS[@]}" apps/prod-check/main.cpp -lz -o build/ron-prod-check

echo "Building ron-econ-check..."
clang++ "${CXXFLAGS[@]}" "${OBJS[@]}" apps/econ-check/main.cpp -lz -o build/ron-econ-check

echo "Building ron-sim..."
clang++ "${CXXFLAGS[@]}" "${OBJS[@]}" apps/sim/main.cpp -lz -o build/ron-sim

echo "Building ron-script..."
clang++ "${CXXFLAGS[@]}" "${OBJS[@]}" apps/script/main.cpp -lz -o build/ron-script

echo "Building ron-ui-check..."
clang++ "${CXXFLAGS[@]}" "${OBJS[@]}" apps/ui-check/main.cpp -lz -o build/ron-ui-check

echo "Building ronviewer..."
clang++ "${CXXFLAGS[@]}" -fobjc-arc -I platform/metal "${OBJS[@]}" platform/metal/*.mm apps/viewer-mac/main.mm \
	-framework Cocoa -framework Metal -framework MetalKit -framework QuartzCore -framework CoreText -lz \
	-o build/ronviewer

echo "Building ronmap..."
clang++ "${CXXFLAGS[@]}" -fobjc-arc -I platform/metal -I platform/audio "${OBJS[@]}" platform/metal/*.mm platform/audio/*.mm \
	apps/map-mac/*.mm \
	-framework Cocoa -framework Metal -framework MetalKit -framework QuartzCore -framework CoreText -framework AVFoundation -lz \
	-o build/ronmap

echo "Building ron-mapgen..."
clang++ "${CXXFLAGS[@]}" "${OBJS[@]}" apps/mapgen/main.cpp -lz -o build/ron-mapgen

echo "Building ron-sav..."
clang++ "${CXXFLAGS[@]}" "${OBJS[@]}" apps/sav/main.cpp -lz -o build/ron-sav

echo
echo "Done. Next:"
echo "  ./build/ron-check <game dir>    # parses every unit asset; should end with 'failures: 0'"
echo "  ./build/ron-rules <game dir> --replay re/analysis/sample_replay.bin    # rules tables vs a replay's image"
echo "  ./build/ron-world <game dir> scenario/Tutorial1/tut1.scx [--json out.json]    # simulation world grid"
echo "  ./build/ron-combat-check <game dir> --dump out.tsv    # damage table (compare: node re/tools/combat-ref.mjs --dump)"
echo "  ./build/ron-prod-check <game dir>    # production prices / times (compare: node re/tools/production-ref.mjs --examples)"
echo "  ./build/ron-econ-check <game dir>    # gather rates / income (compare: node re/tools/gather-ref.mjs --examples)"
echo "  ./build/ron-sim <game dir> scenario/Tutorial1/tut1.scx --list    # headless simulation (see apps/sim/main.cpp)"
echo "  ./build/ron-script --parse-all <game dir>    # compiles every shipped .bhs script (== bhs-parse.mjs --all)"
echo "  ./build/ron-ui-check <game dir> check    # interface readers (layout, strings, fonts, cursors); 'failures: 0'"
echo "  ./build/ron-mapgen <game dir> --style 12 --size 3 --seed 1 --nations 4 --png map.png    # random-map generator (--all: matrix check)"
echo "  ./build/ron-sav re/oracle/Saves/<file>.SVX --verify    # decodes an original save (or a replay's embedded save) and recomputes its Adler-32 verify block"
echo "  ./build/ronviewer <game dir>    # opens the unit viewer"
echo "  ./build/ronmap <game dir> scenario/Tutorial1/tut1.scx    # renders a scenario's terrain"

echo "Building ron-net..."
clang++ "${CXXFLAGS[@]}" "${OBJS[@]}" apps/net/*.cpp -lz -o build/ron-net
echo "  ./build/ron-net replay \"re/oracle/Recorded Games/Multi/<file>.rcx\"    # N engine instances in lockstep, fed by one multiplayer replay; the numbers of ron-sav --replay plus the instances' agreement"
echo "  ./build/ron-net selftest    # lockstep acceptance tests on a toy simulation: stall, pause, speed, resign, drops, desync"
echo "  ./build/ron-net timing \"re/oracle/Recorded Games/Multi\"    # the turn clock reproduces the frame column of every recorded package"
echo "  ./build/ron-net host 7777 --random 18,0,1,2 --frames 600 & ./build/ron-net join 7777 --frames 600    # a LAN game of two processes"
