#!/bin/sh
# Builds the launcher (this repo) plus both Translation Core pieces (the
# sibling VNX-Translation-Core repo) and arranges them into testingbuild/
# matching the real SD card layout — drag the contents straight onto your
# SD card:
#   testingbuild/Viridite.nro                                 (the launcher)
#   testingbuild/Viridite/Viridite-Translation-Core-x64.nro   (real engine)
#   testingbuild/Viridite/Viridite-Translation-Core-x32.nro   (placeholder)
#
# Expects https://github.com/Viridite/VNX-Translation-Core to be cloned as a
# SIBLING of this repo (../VNX-Translation-Core) — that's the only thing this
# script assumes about your layout:
#   git clone https://github.com/Viridite/Viridite.git
#   git clone --recurse-submodules https://github.com/Viridite/VNX-Translation-Core.git
#   cd Viridite && ./build_all.sh
#
# Copy testingbuild/Viridite.nro to sdmc:/switch/ and testingbuild/Viridite/
# to sdmc:/switch/Viridite/ to match what the launcher expects (see
# CORE_X64_PATH/CORE_X32_PATH in source/main.cpp).
set -e
cd "$(dirname "$0")"

CORE_DIR="../VNX-Translation-Core"
if [ ! -d "$CORE_DIR" ]; then
  echo "error: expected the VNX-Translation-Core repo cloned at $CORE_DIR (as a sibling of this repo)." >&2
  echo "       git clone --recurse-submodules https://github.com/Viridite/VNX-Translation-Core.git $CORE_DIR" >&2
  exit 1
fi

echo "=== Launcher (this repo) ==="
make

echo "=== Translation Core (x64) ==="
make -C "$CORE_DIR"

echo "=== Translation Core (x32, placeholder) ==="
make -C "$CORE_DIR/core32"

rm -rf testingbuild
mkdir -p testingbuild/Viridite
cp Viridite.nro testingbuild/Viridite.nro
cp "$CORE_DIR/Viridite-Translation-Core-x64.nro" testingbuild/Viridite/Viridite-Translation-Core-x64.nro
cp "$CORE_DIR/core32/Viridite-Translation-Core-x32.nro" testingbuild/Viridite/Viridite-Translation-Core-x32.nro

echo
echo "=== testingbuild/ layout (drag this onto your SD card's /switch/ folder) ==="
find testingbuild -type f
