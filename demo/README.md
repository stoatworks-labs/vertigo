# demo/ — the browser demo

Live at **https://vertigo-demo.stoatworks-labs.com**.

**This is not the plugin.** It is the GLSL from [`source/Shaders.cpp`](../source/Shaders.cpp),
copied across unedited and run in WebGL2 over clips generated in the page, with
the parameters the plugin's constructor declares. The page says so in a banner,
and lists what it does not reproduce at the foot.

## What is worth doing on it

The plugin's central claim is that one surface does not move, and unusually for
one of these pages that is checkable by a visitor rather than only by the
repository's harness. Two presets exist for it:

- **Null: no depth** — Relief flat, Dolly driven hard. The picture is untouched,
  because a scene with no depth cannot be dolly-zoomed.
- **Null: no move** — Dolly centred, Relief at full. Also untouched.

Both set Quality to Fast, and that is not tidiness: the supersample grid still
runs when the geometry is the identity, so at Good or Best the picture is
resampled and very slightly softened even though nothing moved.

## Editing it

- `plugin.js` — this plugin's parameters and its shaders. **When the shader in
  [`source/Shaders.cpp`](../source/Shaders.cpp) changes, change it here too.**
  The two copies exist because the demo cannot include a C++ file. Unlike most
  of the fleet, that is *enforced* here: `tools/check_shaders.py` compares them
  character for character and `tools/verify.sh` runs it.
- `vendor/` — the shared kit, vendored from `stoatworks-backend/resolume-demo/`.
  **Do not edit these.** Fix the master and re-run its `sync.sh`, which lists
  this repo; `sync.sh --check` reports drift.
- There is deliberately no `page` in `plugin.js` until the website project page
  exists — the kit leaves a missing link out rather than rendering one that
  404s, the same reasoning as the empty `guide` and `page` in
  `source/StoatworksAbout.h`.

## Deploying

No build step. From the repo root:

```bash
cf-run npx wrangler deploy
```

Then verify by content rather than by status code — a wrong page still answers
200:

```bash
curl -s 'https://vertigo-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'
```

`.assetsignore` keeps `README.md` and `tools/` off the public URL; both 404 in
production, which is worth re-checking after any change to what lives in here.
