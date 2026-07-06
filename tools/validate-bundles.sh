#!/usr/bin/env sh
# validate-bundles.sh -- prove both bundles load the way a real host loads them.
#
# lv2_validate is too naive here: it lacks the MOD extension schemas
# (mod#, modgui#) and flags every MOD property as "undefined", drowning
# real errors in noise. What actually matters is that lilv -- the library
# MOD Desktop / mod-host use -- can discover the bundle, load its manifest,
# and enumerate every port without error. This stages a throwaway install
# and asserts exactly that.
#
# Usage: tools/validate-bundles.sh
# Env:   MACOS=true forwarded to the build (homebrew lv2 headers).
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

# Each bundle installs to $PREFIX/lib/lv2/<name>.lv2/ -- that tree is the
# LV2_PATH a host walks.
make -C "$ROOT" install PREFIX="$STAGE" DESTDIR= >/dev/null
LV2_DIR="$STAGE/lib/lv2"

if ! command -v lv2ls >/dev/null 2>&1 || ! command -v lv2info >/dev/null 2>&1; then
    echo "validate: lv2ls/lv2info not found (brew install lilv) -- skipping" >&2
    exit 0
fi

fail=0
check_bundle() {
    uri=$1
    want_ports=$2
    name=$3

    if ! LV2_PATH="$LV2_DIR" lv2ls 2>/dev/null | grep -qx "$uri"; then
        echo "FAIL $name: lilv did not discover $uri" >&2
        fail=1
        return
    fi

    # lv2info exits 0 and prints port blocks only if the manifest, the
    # plugin .ttl, and the modgui .ttl all parse and cross-reference.
    info=$(LV2_PATH="$LV2_DIR" lv2info "$uri" 2>/dev/null) || {
        echo "FAIL $name: lv2info could not load $uri" >&2
        fail=1
        return
    }
    got_ports=$(printf '%s\n' "$info" | grep -c '^	Port ')
    if [ "$got_ports" -ne "$want_ports" ]; then
        echo "FAIL $name: expected $want_ports ports, TTL declares $got_ports" >&2
        fail=1
        return
    fi
    echo "ok   $name: discovered, $got_ports ports enumerate"
}

# Port counts must equal PLUGIN_PORT_COUNT in each bundle's loopjefe.cpp:
#   mono   = 9  (input, output, state, advance, reset, undo, redo, dry_level, time_info)
#   stereo = 11 (+ input_1, output_1)
check_bundle "http://treefallsound.com/plugins/loopjefe"      9  "loopjefe (mono)"
check_bundle "http://treefallsound.com/plugins/loopjefe-2x2" 11  "loopjefe-2x2 (stereo)"

exit $fail
