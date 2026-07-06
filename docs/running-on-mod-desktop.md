# Running loopjefe on MOD Desktop (macOS)

MOD Desktop is the Mac application build of the MOD stack (mod-host +
mod-ui + JACK). It runs the same LV2 plugins a MOD device does, so it's
the fastest way to actually *hear* loopjefe and exercise the modgui —
which the in-process engine tests (`tests/`) can't touch. This is the
first real host the plugin has ever loaded in.

## TL;DR

```sh
make mod-desktop        # build .dylib bundles, install to the search path
# then restart MOD Desktop so it rescans
```

Both bundles appear in the plugin list under the **TreeFallSound** brand.

## Why a special target — two macOS gotchas

The default `make install` targets a Linux/MOD-device layout
(`$(PREFIX)/lib/lv2`, `.so` binaries). MOD Desktop on macOS differs in
two ways that will silently prevent the plugin from loading:

1. **Binary extension must be `.dylib`, not `.so`.** MOD Desktop's host
   `dlopen`s a `.dylib` (its own bundled plugins ship as e.g.
   `sooperlooper-2x2.dylib`). A `.so` is simply not discovered. The
   Makefiles now switch `LIB_EXT` to `dylib` under `MACOS=true`, and the
   `install` target **rewrites the `lv2:binary` line** in `manifest.ttl`
   (`sed 's/…\.so/…\.dylib/'`) so the binary name and the manifest never
   drift. The checked-in `manifest.ttl` still says `.so` (the source of
   truth for the Linux build); only the *installed* copy is rewritten.

2. **Search path is the standard macOS LV2 dir**, not `/usr/…/lib/lv2`.
   MOD Desktop scans, in addition to its own bundled
   `MOD Desktop.app/Contents/LV2`:

   ```
   ~/Library/Audio/Plug-Ins/LV2
   /Library/Audio/Plug-Ins/LV2
   ```

   The `mod-desktop` target installs to the per-user one via the
   `LV2DIR` override:

   ```sh
   make MACOS=true install -C loopjefe    LV2DIR="$HOME/Library/Audio/Plug-Ins/LV2"
   make MACOS=true install -C loopjefe-2x2 LV2DIR="$HOME/Library/Audio/Plug-Ins/LV2"
   ```

## Verifying a bundle loads without launching the app

`make validate` (root) stages a throwaway install and uses **lilv** — the
same library MOD Desktop/mod-host use — to confirm each bundle is
discovered and every port enumerates. This catches TTL/manifest breakage
and any C++-enum-vs-`.ttl` port drift that the engine tests can't see:

```sh
make validate MACOS=true
# ok   loopjefe (mono): discovered, 9 ports enumerate
# ok   loopjefe-2x2 (stereo): discovered, 11 ports enumerate
```

`tools/validate-bundles.sh` hard-codes the expected port counts (mono 9,
stereo 11); bump them there if you add a port. Note that `lv2_validate`
is **not** used — it lacks the MOD extension schemas (`mod#`, `modgui#`)
and drowns real errors in "undefined property" noise for every MOD
property. lilv discovery is the signal that matters.

## Known landmine to check once you're in the app

`readTimeInfo` (`src/transport.h`) matches the transport atom on
`ev->body.type == time:Position` rather than the conventional
`atom:Object` + `otype` pair. The engine tests forge an atom shaped to
match, so all the beat-sync tests pass — but if MOD Desktop's host sends
a standards-compliant plain Object, the engine will **silently ignore
transport** and every beat-sync feature (record quantize, overdub wrap,
tempo-follow, tempo-abort) falls back to free-run. If tempo sync looks
dead in the app, this mismatch is the first suspect.
