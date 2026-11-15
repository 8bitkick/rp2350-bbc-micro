#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
: "${PICO_SDK_PATH:?Set PICO_SDK_PATH to the SDK used for the firmware}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
CC=${CC:-cc}
BT="$PICO_SDK_PATH/lib/btstack/src"
$CC -fsanitize=address,undefined -DNIMBUS_TEST=1 -I src/common tests/bluetooth/mapping_test.c src/common/controller.c -o "$TMP/mapping"
"$TMP/mapping"
$CC -fsanitize=address,undefined -I src/common tests/bluetooth/store_test.c src/common/controller_store.c -o "$TMP/store"
"$TMP/store"
$CC -fsanitize=address,undefined -I src/boards/presto tests/bluetooth/reconnect_test.c -o "$TMP/reconnect"
"$TMP/reconnect"
$CC -fsanitize=address,undefined -DBT_KEYBOARD_CLASSIC=1 -I tests/bluetooth/host_stubs -I tests/bluetooth/discovery_stubs -I src/common -I src/boards/presto -I "$BT" tests/bluetooth/keyboard_host_test.c "$BT/btstack_hid_parser.c" "$BT/btstack_util.c" -o "$TMP/keyboard"
"$TMP/keyboard"
$CC -fsanitize=address,undefined -I tests/bluetooth/discovery_stubs -I src/boards/presto -I "$BT" tests/bluetooth/discovery_test.c src/boards/presto/keyboard_discovery.c "$BT/btstack_util.c" -o "$TMP/discovery"
"$TMP/discovery"
python3 tests/bluetooth/keyboard_map_command_test.py
python3 tests/bluetooth/wifi_gate_test.py
