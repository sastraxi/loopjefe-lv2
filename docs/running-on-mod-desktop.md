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

## Why a special target — four macOS gotchas

The default `make install` targets a Linux/MOD-device layout
(`$(PREFIX)/lib/lv2`, `.so` binaries). MOD Desktop on macOS differs in
four ways that will prevent the plugin from loading (with no visible error in the pedalboard UI — the plugin just doesn't appear, or "Error loading effect" on drag-in; see "Reading the host log" below):

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

3. **No external dylib deps to chase.** The engine has no third-party
   libraries (the tempo-follow stretcher is hand-rolled in `src/wsola.h`).
   The bundled `loopjefe.dylib` therefore depends only on system
   frameworks, matching MOD's own `sooperlooper-2x2.dylib`. Verify with
   `otool -L loopjefe.dylib` — only `libSystem`, `libc++`, and
   `Accelerate` should appear.

4. **MOD Desktop enforces library validation.** Even with deps clean,
   the host rejects any third-party `.dylib` whose code signature lacks
   MOD's Team ID (`P3KTRVLR59`). An ad-hoc-signed dylib (`codesign -s -`)
   isn't enough — library validation compares Team IDs and ours is empty.
   The fix is to run a **re-signed copy** of MOD Desktop with the
   `com.apple.security.cs.disable-library-validation` entitlement added.
   The original in `/Applications` is SIP-protected (`com.apple.provenance`)
   and can't be re-signed in place; copy it to `~/Applications` first.
   See "Re-running MOD Desktop re-signed" below.

## Re-running MOD Desktop re-signed

`tools/resign-mod-desktop.sh` copies MOD Desktop from `/Applications` to
`~/Applications`, strips xattrs, and re-signs the whole bundle ad-hoc with
`disable-library-validation`. Run it once after installing or updating
MOD Desktop, then launch `~/Applications/MOD Desktop.app` (not the
`/Applications` original):

```sh
tools/resign-mod-desktop.sh
open ~/Applications/MOD\ Desktop.app
```

The re-signed copy survives across launches; re-run the script only after
a MOD Desktop update overwrites `/Applications/MOD Desktop.app`.

## Reading the host log

When a plugin fails to load, MOD Desktop's pedalboard UI shows only a
generic "Error loading effect" toast — the actual cause is in the
**MOD Host** log pane. Enable it via *Show Logs* (bottom-left, under GUI
Options). The host (`mod-host`) runs as a JACK internal client (`.so`
inside `jackd`), so its `fprintf(stderr, …)` lands in jackd's stderr,
which MOD Desktop merges into that pane. The two lines that matter:

- `lilv_lib_open(): error: Failed to open library …` — dylib won't
  `dlopen` (code-signature mismatch, missing external dep, or wrong
  architecture). Check `otool -L` and `codesign -dv` on the bundle's
  `.dylib`.
- `can't get lilv instance` — `lilv_plugin_instantiate()` returned NULL
  (missing required feature, or `instantiate` returned NULL/crashed).
  lilv prints a `Missing feature …` line just before this one.

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
a standards-compliant plain Object, the engine will **ignore
transport** and every beat-sync feature (record quantize, overdub wrap,
tempo-follow, tempo-abort) falls back to free-run. If tempo sync looks
dead in the app, this mismatch is the first suspect.
