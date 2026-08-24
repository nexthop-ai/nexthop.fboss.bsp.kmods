#!/bin/sh
# Publish a revision-independent handle for the CPU card IDPROM: it is located
# on a different CPU I2C controller depending on the board revision.
set -eu

PLATFORM_DIR=/sys/devices/platform
I2C_DIR=/sys/bus/i2c/devices
LINK_DIR=/run/nexthop_bsp/eeproms
LINK_NAME=SCM_IDPROM
ADDR=0x50
KERNEL_DEVICE=24c64
EEPROM_MODULE=at24

# Instantiating the EEPROM only binds a driver, and so only grows an "eeprom"
# attribute, once at24 is registered. This unit runs early in boot, before
# anything else has needed at24, so load it up front rather than racing the
# modalias autoload.
modprobe "$EEPROM_MODULE" 2>/dev/null || :

# A Meta/FBOSS EEPROM starts with 0xfb 0xfb, anything else at 0x50 is not the IDPROM.
is_idprom() {
    eeprom="$1/eeprom"
    [ -r "$eeprom" ] || return 1
    magic=$(od -An -tx1 -N2 "$eeprom" 2>/dev/null | tr -d ' \n') || return 1
    [ "$magic" = "fbfb" ]
}

wait_for_eeprom() {
    i=0
    while [ "$i" -lt 20 ]; do
        if [ -r "$1/eeprom" ]; then
            return 0
        fi
        i=$((i + 1))
        sleep 0.1
    done
    return 1
}

publish() {
    mkdir -p "$LINK_DIR"
    ln -sfn "$1" "$LINK_DIR/$LINK_NAME"
    echo "$LINK_DIR/$LINK_NAME -> $1"
}

for ctrl in "$PLATFORM_DIR"/AMDI0010:*; do
    [ -d "$ctrl" ] || continue
    for bus in "$ctrl"/i2c-*; do
        [ -d "$bus" ] || continue
        dev="$I2C_DIR/${bus##*/i2c-}-0050"

        # Never disturb a device somebody else already instantiated at 0x50 --
        # use it if it is the IDPROM, otherwise leave this controller alone.
        if [ -e "$dev" ]; then
            if is_idprom "$dev"; then
                publish "$(readlink -f "$dev")"
                exit 0
            fi
            continue
        fi

        echo "$KERNEL_DEVICE $ADDR" >"$bus/new_device" 2>/dev/null || continue
        wait_for_eeprom "$dev" || :
        if is_idprom "$dev"; then
            publish "$(readlink -f "$dev")"
            exit 0
        fi
        echo "$ADDR" >"$bus/delete_device" 2>/dev/null || :
    done
done

echo "no IDPROM found at $ADDR on any CPU I2C controller" >&2
exit 0
