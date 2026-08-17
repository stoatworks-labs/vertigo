# demo/ — the browser demo

Intended for **https://vertigo-demo.stoatworks-labs.com** — not deployed yet, and
the project page it should link from does not exist either. See the note at the
bottom.

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
  **Do not edit these.** Fix the master and re-run its `sync.sh`; `sync.sh
  --check` reports drift.

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

## Before the first deploy

Three things are outstanding and none of them is code:

1. **The DNS record and the custom domain** for `vertigo-demo` have never
   existed. `wrangler.toml` claims the hostname, which means the first deploy
   is also the thing that creates it.
2. **`sync.sh` in `stoatworks-backend/resolume-demo` does not list this repo**,
   so a fleet-wide kit sync would skip it. The vendored files here were copied
   by hand and are byte-identical to porthole's, which sync.sh does maintain.
3. **The page links to the repository and not to a project page**, because there
   is no project page and no user guide yet. Same reasoning as the empty `guide`
   and `page` in `source/StoatworksAbout.h`: a link that 404s is worse than no
   link. Add both when the website entry exists.
