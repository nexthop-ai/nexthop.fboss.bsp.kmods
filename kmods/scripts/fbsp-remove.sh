#!/bin/bash
# Script to unload kernel modules defined in kmods.json

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
KMODS_JSON="${SCRIPT_DIR}/kmods.json"

if [ ! -f "$KMODS_JSON" ]; then
    echo "Error: kmods.json not found at $KMODS_JSON"
    exit 1
fi

unload_modules() {
    local module_list="$1"
    local list_name="$2"

    echo "Unloading $list_name modules..."
    for module in $module_list; do
        echo "  Unloading $module..."
        if lsmod | grep -q "^$module"; then
            rmmod "$module" && echo "    Successfully unloaded $module" || echo "    Failed to unload $module"
        else
            echo "    Module $module is not loaded"
        fi
    done
}

# Extract module lists from kmods.json using jq utility
BSP_KMODS=$(jq -r '.bspKmods[]' "$KMODS_JSON" 2>/dev/null)
SHARED_KMODS=$(jq -r '.sharedKmods[]' "$KMODS_JSON" 2>/dev/null)

# Unload bsp modules first
unload_modules "$BSP_KMODS" "bsp"

# Then unload shared modules
unload_modules "$SHARED_KMODS" "shared"

echo "Kernel module unloading complete"
