# vertigo

The dolly zoom as an FFGL effect for Resolume Arena/Avenue and an OpenFX effect
for Resolve/Nuke/Natron/Vegas. C++/GLSL, CMake MODULE → universal `.bundle`
(macOS) + Windows `.dll`. Public MIT repo.

Read `AGENTS.md` before changing the dolly maths.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render a frame offline: `./build/vgtest --out /tmp/frame.png`
- List parameters: `./build/vgtest --list`
- Put real footage through the real shader:
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/vgtest --pipe --width W --height H [--script cues.txt] | ffmpeg …`

## Verify
- Everything (~20s): `tools/verify.sh`
- GLSL vs C++ maths: `./build/vgtest --probe`
- The anchor really holds: `./build/vgtest --anchor --set "Anchor=0.5"`
- The depth solve: `./build/vgtest --depth`
- No dead controls: `python3 tools/sweep.py`

## OpenFX build
- `source/ofx/VertigoOFX.cpp` → `build/Vertigo.ofx.bundle` (target `VertigoOFX`,
  `-DBUILD_OFX=OFF` to skip). It links `Dolly.cpp` directly — the model has one
  home — but mirrors the fragment shader's pixel machinery (edge modes, the
  supersample grid, the 5-tap depth blur, the solve loop) on the CPU. Change the
  shader's pixel machinery, change this too.
- OFX SDK subset (BSD-3) vendored under `external/openfx`.
- Smoke test: `../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.vertigo`
- Null proof (**needs `quality=0`** — supersampling resamples even when the
  geometry is the identity): `--set relief=0.5 --set quality=0` → 0 bytes differ,
  and the same for `--set dolly=0.5`.
- Preset proof: `--edit preset=N` (a real user edit, so the preset logic runs)
  must be byte-identical to the same values set by hand, and must NOT match the
  null.
- Install for Resolve: copy the bundle into `/Library/OFX/Plugins`.

## Browser demo (demo/)
- Intended for `vertigo-demo.stoatworks-labs.com`. **Not deployed** — the custom
  domain has never existed, so the first `cf-run npx wrangler deploy` creates it.
- No build step. Serve locally with
  `python3 -m http.server 8792 --directory demo`.
- `demo/plugin.js` carries a **second copy of the GLSL**, because the page cannot
  include a C++ file. Unlike most of the fleet that is enforced here:
  `python3 demo/tools/check_shaders.py` compares it to `source/Shaders.cpp`
  character for character, and `tools/verify.sh` runs it.
- `demo/vendor/` is the shared kit from
  `stoatworks-backend/resolume-demo/`. **Do not edit.** It was copied by hand and
  is byte-identical to porthole's; `sync.sh` does not list this repo yet.
- Two presets exist to make a claim checkable by a visitor rather than only by
  the harness: **Null: no depth** and **Null: no move**. Both set Quality to Fast,
  because the supersample grid still resamples when the geometry is the identity.
- State goes in the query string (`?dolly=0.66&relief=0.25&clip=grid`), which is
  what "Copy link" produces.
- Verifying it in the Browser pane: a **hidden pane never fires
  `requestAnimationFrame`**, so take a screenshot first or nothing renders. And
  the context is `preserveDrawingBuffer: false`, so `readPixels` from outside the
  render callback returns zeros — it looks like a passing byte-comparison and is
  not one. Byte-exact null proofs come from `ofxprobe`, not from the page.

## Notes
- One shader pass, no intermediate buffers. The effect is the GLSL; the C++ is
  host glue plus the model.
- The model is written in **disparity**, not depth: `m = (1-σd_a)/(1-σd)` with
  `d` in 0..1 and `σ < 1`, so the denominator is unconditionally positive. There
  is no divide-by-zero guard and there should not be one.
- The Dolly slider is geometric in the parallax ratio `P = 1/(1-σ)`, so the two
  directions are reciprocal. Dolly at 0.5 and Relief at 0.5 are both exact nulls.
- **Relief scales the field about the middle of the range, not about the
  anchor** — the anchor is held by putting it through the same map. Scaling about
  the anchor makes the control silently dead over its negative half. See AGENTS.md.
- The maths exists twice, in `Dolly.cpp` and `Shaders.cpp`. `--probe` measures one
  against the other. Change one, change both, run it. `--probe` cannot catch a
  model that is wrong in both copies; `--anchor` is what covers that.
- Radial depth is evaluated in **output** space (closed form, one fetch); Luma
  and Alpha are solved for in **source** space (three fixed iterations).
- The depth fetch deliberately bypasses the edge mode — off-frame depth extends
  from the edge, or Transparent would read as "infinitely far".
- All host parameters are 0..1 and mapped internally. `SetParamInfo` clamps a
  standard default into 0..1 before `SetParamRange` can widen it.
- `SetTextParameter` must be overridden for the About block or `FF_INSTANTIATE_GL`
  fails for the whole plugin, invisibly to the harness.
- Geometry is in picture space; `MaxUV` is applied only at the fetch, and every
  fetch stays half a texel inside the picture.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log.
- `smooth` is a GLSL reserved word — the uniform is `DepthSmooth`. Shader errors
  surface only at runtime, in the diagnostics log.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It exists for the one failure that actually
happens: a shader that will not compile, which otherwise looks like "the effect
does nothing" with no message anywhere. It logs the GL vendor/renderer/version
next to it. `~/Library/Logs/vertigo/`, or `VERTIGO_LOG_DIR`.
