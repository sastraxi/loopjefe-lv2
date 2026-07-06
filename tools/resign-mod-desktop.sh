#!/usr/bin/env sh
# resign-mod-desktop.sh -- make MOD Desktop load unsigned third-party LV2s.
#
# MOD Desktop is a signed, hardened-runtime app with library validation
# enabled (Team ID P3KTRVLR59). macOS rejects any dlopen'd .dylib whose
# signature lacks that Team ID, so self-built LV2s like loopjefe won't
# load -- lilv_lib_open() fails with "different Team IDs" and the plugin
# is dropped from the browser / "Error loading effect" on drag-in.
#
# This script copies MOD Desktop from /Applications (SIP-protected,
# com.apple.provenance -- can't re-sign in place) to ~/Applications,
# strips the restricted xattrs, and re-signs the whole bundle ad-hoc with
# the com.apple.security.cs.disable-library-validation entitlement added.
# Launch the copy afterwards:
#
#   open ~/Applications/MOD\ Desktop.app
#
# Re-run after each MOD Desktop update (which overwrites /Applications).
# Re-signing is idempotent. The original /Applications copy is untouched.
set -eu

SRC="/Applications/MOD Desktop.app"
DST="$HOME/Applications/MOD Desktop.app"

if [ ! -d "$SRC" ]; then
    echo "resign: $SRC not found -- is MOD Desktop installed?" >&2
    exit 1
fi

ENT_FILE=$(mktemp -t resign-mod)
trap 'rm -f "$ENT_FILE"' EXIT
cat > "$ENT_FILE" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.cs.allow-jit</key>
    <true/>
    <key>com.apple.security.cs.allow-unsigned-executable-memory</key>
    <true/>
    <key>com.apple.security.device.audio-input</key>
    <true/>
    <key>com.apple.security.cs.disable-library-validation</key>
    <true/>
</dict>
</plist>
EOF

mkdir -p "$HOME/Applications"
echo "resign: copying $SRC -> $DST"
rm -rf "$DST"
ditto "$SRC" "$DST"

echo "resign: stripping restricted xattrs"
xattr -cr "$DST" 2>/dev/null || true

echo "resign: re-signing ad-hoc with disable-library-validation"
codesign --remove-signature "$DST" 2>/dev/null || true
codesign --force --deep --sign - --entitlements "$ENT_FILE" "$DST"

echo "resign: verifying"
codesign -d --entitlements - "$DST" 2>&1 | grep -q disable-library-validation \
    && echo "ok: $DST re-signed (disable-library-validation present)" \
    || { echo "FAIL: entitlement not applied" >&2; exit 1; }

echo
echo "Done. Launch with:"
echo "  open ~/Applications/MOD\\ Desktop.app"
echo "(Not the /Applications original -- that one still enforces library validation.)"