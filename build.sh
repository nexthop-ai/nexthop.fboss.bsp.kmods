#!/bin/bash

# Convenience script to build the BSP kernel modules.
# Usage: ./build.sh [clean]
#   ./build.sh       - runs make in kmods/
#   ./build.sh clean - runs make clean in kmods/

set -e

# Determine the make target
MAKE_TARGET="$1"
if [ "$MAKE_TARGET" = "clean" ]; then
    echo "Running make clean..."
    MAKE_CMD="make clean"
else
    echo "Running make..."
    MAKE_CMD="make"
fi

cd "$(dirname "$(readlink -f "$0")")"

echo "=== Building kmods ==="
cd kmods
$MAKE_CMD
cd ..

echo "Done!"
